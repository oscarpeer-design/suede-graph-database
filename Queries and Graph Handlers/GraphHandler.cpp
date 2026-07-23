#include "GraphHandler.h"

// commandArgument
// Returns the trimmed argument that follows a leading keyword in a command,
// e.g. commandArgument("FLUSH  /tmp/g.bin", "FLUSH") -> "/tmp/g.bin".
// If the keyword has no argument (bare "FLUSH"), returns "". This lets FLUSH /
// LOAD carry an explicit path so persistence is driven by the command itself,
// with no reliance on a pre-initialised storage engine.
static std::string commandArgument(const std::string& command, const std::string& keyword) {
    // argument begins right after the keyword
    if (command.size() <= keyword.size())
        return std::string();
    std::string rest = command.substr(keyword.size());
    // trim leading whitespace
    size_t begin = rest.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return std::string();
    // trim trailing whitespace
    size_t end = rest.find_last_not_of(" \t\r\n");
    return rest.substr(begin, end - begin + 1);
}

// explicit constructor
GraphHandler::GraphHandler(
    std::unique_ptr<Graph> graph,
    std::unique_ptr<StorageEngine> storage
)
    : graph_(std::move(graph)), storage_(std::move(storage)), nextSnapshotId_(1)
{
}

// Let the unique_ptr members clean up automatically.
GraphHandler::~GraphHandler() = default;

// executeQueryLive
QueryResult GraphHandler::executeQueryLive(const std::string& queryStr) {
    // Determine if read or write based on query operation
    Query query(queryStr);
    if (!query.parse(queryStr)) {
        return badParsingResult(query);
    }

    // IMPORT CSV / EXPORT CSV are storage operations the Query layer parses but
    // cannot execute (it owns no StorageEngine). This coordinator does: route
    // them to importCSV/exportCSV, which own the CSV I/O. These take their own
    // exclusive lock, so we must NOT hold a lock here when calling them
    // (std::shared_mutex is non-recursive).
    const QueryOperation op = query.operation();
    if (op == QueryOperation::Import || op == QueryOperation::Export) {
        const std::string& path = query.filePath();
        const bool isImport = (op == QueryOperation::Import);
        const bool ok = isImport ? importCSV(path) : exportCSV(path);
        const std::string verb = isImport ? "IMPORT" : "EXPORT";
        return { ok, verb + " CSV " + (ok ? "succeeded: " : "failed: ") + path };
    }

    // check read-only
    if (isReadOnly(query.operation())) {
        // Shared lock for reads (SELECT, MATCH)
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return query.execute(*graph_);
    }
    else {
        // Exclusive lock for writes (INSERT, DELETE, UPDATE, LOAD, SAVE)
        std::lock_guard<std::shared_mutex> lock(mutex_);
        return query.execute(*graph_);
    }
}

// executeQuerySnapshot
QueryResult GraphHandler::executeQuerySnapshot(uint64_t snapshotId, const std::string& queryStr) {
    // Parse query string
    Query query(queryStr);
    if (!query.parse(queryStr)) {
        return badParsingResult(query);
    }
    // Acquire shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // check if snapshot id is valid
    auto it = snapshots_.find(snapshotId);
    if (it == snapshots_.end())
        return { false, "Parse error: snapshot ID: " + std::to_string(snapshotId) + " could not be matched against a snapshot version." };
    // get pointer to the snapshot representation object (do NOT move)
    CSR_Representation* csr_snapshot = it->second.get();
    return query.execute(*graph_, *csr_snapshot);
}

// createSnapshot
uint64_t GraphHandler::createSnapshot() {
    // Acquire exclusive lock to prevent mutations while we capture the snapshot
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Build a new CSR_Representation from the current graph state. The
    // constructor already registers this point-in-time view in the graph's MVCC
    // registry (owner_->CaptureSnapshot()), so we must NOT call CaptureSnapshot()
    // again here -- doing so would leak a second, never-released version.
    std::unique_ptr<CSR_Representation> csr_snapshot =
        std::make_unique<CSR_Representation>(*graph_);
    // Populate the CSR index (node/edge mappings, row offsets, columns). Without
    // this the snapshot's node mapping is empty and SELECT ... SNAPSHOT would see
    // zero members -- the CSR mapping is the membership authority.
    csr_snapshot->Load_CSR();
    // Store the snapshot under a fresh id.
    uint64_t snapshotId = nextSnapshotId_;
    snapshots_[snapshotId] = std::move(csr_snapshot);
    nextSnapshotId_++;
    // Return the id the caller uses for executeQuerySnapshot / release / persist.
    return snapshotId;
}

// releaseSnapshot
void GraphHandler::releaseSnapshot(uint64_t snapshotId) {
    // Acquire exclusive lock
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Look up snapshotId in snapshots_
    auto it = snapshots_.find(snapshotId);
    if (it != snapshots_.end()) {
        // Erasing the unique_ptr runs the CSR_Representation destructor, which
        // calls owner_->ReleaseSnapshot(snapshotVersion_) with the correct MVCC
        // version. Do NOT call graph_->ReleaseSnapshot(snapshotId) here: snapshotId
        // is the handler's map key, not the MVCC version, and releasing it again
        // would be a wrong/double release.
        snapshots_.erase(it);
    }
}

// activeSnapshotCount
size_t GraphHandler::activeSnapshotCount() const {
    // Acquire shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // Return snapshots_.size() and release lock
    return snapshots_.size();
}

// persistSnapshot
bool GraphHandler::persistSnapshot(uint64_t snapshotId, const std::string& filename) {
    // Acquire exclusive lock
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Look up snapshotId in snapshots_ and check the id exists
    auto it = snapshots_.find(snapshotId);
    if (it == snapshots_.end())
        return false;
    // get pointer to the snapshot representation object (do NOT move)
    CSR_Representation* csr_snapshot = it->second.get();
    // Serialize CSR SNAPSHOT and write to file
    bool saved = storage_->SaveSnapshot(*csr_snapshot, filename);
    return saved;
}

// loadSnapshot
bool GraphHandler::loadSnapshot(const std::string& filename) {
    // check if storage is null
    if (!storage_)
        return false;
    // Acquire exclusive lock
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // create a new CSR_Representation and load from file
    std::unique_ptr<CSR_Representation> csr_snapshot = std::make_unique<CSR_Representation>(*graph_);
    bool loaded = storage_->LoadSnapshot(*csr_snapshot, filename);
    if (loaded) {
        // Store loaded snapshot in snapshots_
        uint64_t snapshotId = nextSnapshotId_;
        snapshots_[snapshotId] = std::move(csr_snapshot);
        nextSnapshotId_++;
    }
    return loaded;
}

// flush Graph to storage
bool GraphHandler::flush() {
    // check if storage is null
    if (!storage_)
        return false;
    // Acquire EXCLUSIVE lock (Save reads entire graph, must be consistent)
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Flush Graph to storage
    return storage_->Save(*graph_);
}

// read Graph from storage
bool GraphHandler::loadLive() {
    // check if storage is null
    if (!storage_)
        return false;
    // Acquire exclusive lock
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Load live graph from storage
    return storage_->Load(*graph_);
}

// flush Graph to an explicit runtime-chosen path
bool GraphHandler::flush(const std::string& path) {
    // Acquire EXCLUSIVE lock (Save reads the entire graph, must be consistent)
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Create a StorageEngine on demand if this session had no persistence, so the
    // user can save to a brand-new file they just picked.
    if (!storage_)
        storage_ = std::make_unique<StorageEngine>(path);
    // Save the live graph to the explicit path (retargets the engine's file).
    return storage_->Save(*graph_, path);
}

// load live Graph from an explicit runtime-chosen path
bool GraphHandler::loadLive(const std::string& path) {
    // Acquire exclusive lock
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Create a StorageEngine on demand if this session had no persistence.
    if (!storage_)
        storage_ = std::make_unique<StorageEngine>(path);
    // Load the live graph from the explicit path (retargets the engine's file).
    return storage_->Load(*graph_, path);
}

// importCSV: read a CSV file into the live graph (IMPORT CSV '<path>').
bool GraphHandler::importCSV(const std::string& path) {
    // Acquire EXCLUSIVE lock: ImportCSV mutates the graph (creates nodes/edges).
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Create a StorageEngine on demand if this session had no persistence, so
    // CSV import works without a storage choice up front.
    if (!storage_)
        storage_ = std::make_unique<StorageEngine>(path);
    // Delegate to the engine's existing CSV importer.
    return storage_->ImportCSV(*graph_, path);
}

// exportCSV: write the live graph to a CSV file (EXPORT CSV '<path>').
bool GraphHandler::exportCSV(const std::string& path) {
    // Acquire EXCLUSIVE lock so the export sees a consistent whole-graph view
    // (ExportCSV reads every node and edge).
    std::lock_guard<std::shared_mutex> lock(mutex_);
    // Create a StorageEngine on demand if this session had no persistence.
    if (!storage_)
        storage_ = std::make_unique<StorageEngine>(path);
    // Delegate to the engine's existing CSV exporter.
    return storage_->ExportCSV(*graph_, path);
}

/**********************************
Getter Methods Wrapped around Graph
***********************************/

// getNodeCount
size_t GraphHandler::getNodeCount() const {
    // get shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // get all nodeIds in order
    std::vector<NodeId> nodeIds;
    graph_->GetNodeIdOrder(nodeIds);
    return nodeIds.size();
}

// getEdgeCount
size_t GraphHandler::getEdgeCount() const {
    // get shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // get all edgeIds
    std::vector<EdgeId> edgeIds;
    graph_->GetAllEdgeIds(edgeIds);
    return edgeIds.size();
}

// getNode
bool GraphHandler::getNode(NodeId id, Node& out) const {
    // get shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // get node
    return graph_->GetNode(id, out);
}

// getEdge
bool GraphHandler::getEdge(EdgeId id, Edge& out) const {
    // get shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // get edge
    return graph_->GetEdge(id, out);
}

// check if there is storage
bool GraphHandler::hasStorage() const {
    return storage_ != nullptr;
}

// getGraphVersion
uint64_t GraphHandler::getGraphVersion() const {
    // get shared lock
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // return graph version
    return graph_->GetVersion();
}

// executeCommand
// this is the interface for all users
QueryResult GraphHandler::executeCommand(const std::string& commandStr) {
    std::string command = commandStr;

    // trim leading whitespace
    size_t start = command.find_first_not_of(" \t");
    if (start != std::string::npos)
        command = command.substr(start);
    // get command type
    CommandType type = parseCommandType(command);
    // check each type and execute query based on said type
    switch (type) {
        // create snapshot
    case CommandType::SNAPSHOT_CREATE: {
        uint64_t id = createSnapshot();
        // return snapshot result
        return { true, "Snapshot created with ID: " + std::to_string(id) };
    }
                                     // release snapshot from memory
    case CommandType::SNAPSHOT_RELEASE: {
        size_t idStart = command.find_last_of(" ") + 1;
        uint64_t id = std::stoull(command.substr(idStart));
        releaseSnapshot(id);
        // return snapshot release result
        return { true, "Snapshot " + std::to_string(id) + " released" };
    }
                                      // flush raw graph to storage
    case CommandType::FLUSH: {
        // A path may be given inline: "FLUSH <path>". If so, save to that path
        // (creating a StorageEngine on demand -- no pre-initialised engine
        // needed). Bare "FLUSH" uses the engine's existing path, if any.
        std::string path = commandArgument(command, "FLUSH");
        bool success = path.empty() ? flush() : flush(path);
        // return graph flush result
        return { success,
                 success ? ("Graph flushed to storage" + (path.empty() ? std::string() : (": " + path)))
                         : "Flush failed" };
    }
                           // load live graph
    case CommandType::LOAD: {
        // Same as FLUSH: "LOAD <path>" loads from an explicit path (creating a
        // StorageEngine on demand); bare "LOAD" uses the engine's existing path.
        std::string path = commandArgument(command, "LOAD");
        bool success = path.empty() ? loadLive() : loadLive(path);
        // return graph loading result
        return { success,
                 success ? ("Graph loaded from storage" + (path.empty() ? std::string() : (": " + path)))
                         : "Load failed" };
    }
                          // get node count
    case CommandType::NODE_COUNT: {
        size_t count = getNodeCount();
        // return node count
        return { true, "Node count: " + std::to_string(count) };
    }
                                // get edge count
    case CommandType::EDGE_COUNT: {
        size_t count = getEdgeCount();
        // return edge count
        return { true, "Edge count: " + std::to_string(count) };
    }
                                // query operation (includes SELECT, SNAPSHOT, LIVE, MATCH, ect)
    case CommandType::QUERY: {
        // execute live query
        return executeQueryLive(command);
    }
    default:
        // unknown command error
        return { false, "Unknown command" };
    }
}