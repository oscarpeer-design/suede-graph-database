#include "GraphHandler.h"

// Let the unique_ptr members clean up automatically.
GraphHandler::~GraphHandler() = default;

// executeQueryLive
QueryResult GraphHandler::executeQueryLive(const std::string& queryStr) {
    // Determine if read or write based on query operation
    Query query(queryStr);
    if (!query.parse()) {
        return badParsingResult(query);
    }
    // check read-only
    if (isReadOnly(query.operation())) {
        // Shared lock for reads (SELECT, MATCH)
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return query.execute(*graph_);
    }
    else {
        // Exclusive lock for writes (INSERT, DELETE, UPDATE, LOAD)
        std::lock_guard<std::shared_mutex> lock(mutex_);
        return query.execute(*graph_);
    }
}

// executeQuerySnapshot
QueryResult GraphHandler::executeQuerySnapshot(uint64_t snapshotId, const std::string& queryStr) {
    // Parse query string
    Query query(queryStr);
    if (!query.parse()) {
        return badParsingResult(query);
    }
    // Acquire shared lock 
    std::shared_lock<std::shared_mutex> lock(mutex_);

}