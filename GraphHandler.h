// GraphHandler.h
#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <cstdint>

#include "Graph.h"
#include "CSR_Representation.h"
#include "StorageEngine.h"

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
    void persistSnapshot(uint64_t snapshotId, const std::string& filename);
    void loadSnapshot(const std::string& filename);

    // Manual flush to storage
    void flush();

private:
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<StorageEngine> storage_;  // Can be null if no persistence

    std::unordered_map<uint64_t, std::unique_ptr<CSR_Representation>> snapshots_;
    uint64_t nextSnapshotId_ = 1;

    mutable std::shared_mutex mutex_;
};