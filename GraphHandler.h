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

    // Manual flush to storage
    bool flush();

    // Load live graph
    bool loadLive();

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