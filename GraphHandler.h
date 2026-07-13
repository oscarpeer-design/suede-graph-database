// GraphHandler.h
#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <cstdint>

#include "Graph.h"
#include "CSR_Representation.h"
#include "StorageEngine.h"
#include "Query.h"

// commandType 
// This is separate from query type because queries handle execution and this matches parsing strategy
enum class CommandType {
    SNAPSHOT_CREATE,
    SNAPSHOT_RELEASE,
    FLUSH,
    LOAD,
    NODE_COUNT,
    EDGE_COUNT,
    QUERY,
    UNKNOWN
};

// Compares the beginning of str against prefix, ignoring case.
// Returns true if str starts with prefix (case-insensitive).
static bool startsWithNoCase(const std::string& str, const std::string& prefix) {
    // check tring length discrepancies
    if (str.size() < prefix.size())
        return false;
    // check if each character is equal or not
    // do this in a case-insensitive manner
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(str[i]) != std::tolower(prefix[i]))
            return false;
    }
    return true;
}

// Parse command string into CommandType
static CommandType parseCommandType(const std::string& command) {
    if (startsWithNoCase(command, "SNAPSHOT CREATE"))
        return CommandType::SNAPSHOT_CREATE;
    if (startsWithNoCase(command, "SNAPSHOT RELEASE"))
        return CommandType::SNAPSHOT_RELEASE;
    if (startsWithNoCase(command, "FLUSH"))
        return CommandType::FLUSH;
    if (startsWithNoCase(command, "LOAD"))
        return CommandType::LOAD;
    if (startsWithNoCase(command, "NODE COUNT"))
        return CommandType::NODE_COUNT;
    if (startsWithNoCase(command, "EDGE COUNT"))
        return CommandType::EDGE_COUNT;

    // If it's not a special command, treat it as a query
    return CommandType::QUERY;
}

class GraphHandler {
public:
    // Constructor: provide both graph and storage engine
    explicit GraphHandler(
        std::unique_ptr<Graph> graph,
        std::unique_ptr<StorageEngine> storage = nullptr
    );

    ~GraphHandler();

    // Query execution (thread-safe)
    QueryResult executeQueryLive(const std::string& queryStr);
    QueryResult executeQuerySnapshot(uint64_t snapshotId, const std::string& queryStr);

    // Snapshot management
    uint64_t createSnapshot();
    void releaseSnapshot(uint64_t snapshotId);
    size_t activeSnapshotCount() const;

    // Persistence operations
    bool persistSnapshot(uint64_t snapshotId, const std::string& filename);
    bool loadSnapshot(const std::string& filename);

    // Manual flush to storage (to the path the StorageEngine was built with)
    bool flush();

    // Load live graph (from the path the StorageEngine was built with)
    bool loadLive();

    // Flush / load the live graph to an EXPLICIT path chosen at runtime (e.g. a
    // file the interactive user picked from a dialog). If no StorageEngine exists
    // yet (an in-memory session with no persistence chosen up front), one is
    // created on demand targeting this path -- so the interactive user can save a
    // graph to a brand-new file without having answered a storage prompt.
    bool flush(const std::string& path);
    bool loadLive(const std::string& path);

    // Graph information / introspection
    size_t getNodeCount() const;
    size_t getEdgeCount() const;

    // Direct graph access (read-only)
    bool getNode(NodeId id, Node& out) const;
    bool getEdge(EdgeId id, Edge& out) const;

    // Storage status
    bool hasStorage() const;

    // Get graph version (useful for snapshot tracking)
    uint64_t getGraphVersion() const;

    // Execute any command: queries, snapshots, persistence
    QueryResult executeCommand(const std::string& commandStr);

private:
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<StorageEngine> storage_;  // Can be null if no persistence

    std::unordered_map<uint64_t, std::unique_ptr<CSR_Representation>> snapshots_;
    uint64_t nextSnapshotId_ = 1;

    // mutex for read and write operations
    mutable std::shared_mutex mutex_;

    // check if operation is read only
    bool isReadOnly(QueryOperation operation) const {
        // only allow SELECT and MATCH
        return operation == QueryOperation::Select || operation == QueryOperation::Match;
    }

    // returns bad result if parsing fails
    QueryResult badParsingResult(const Query& query) const {
        return { false, "Parse error: " + query.lastError() };
    }
};