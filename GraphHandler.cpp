#include "GraphHandler.h"

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