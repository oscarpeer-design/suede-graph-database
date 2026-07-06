#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include "Graph.h"         // Node, Edge, propertiesMap, Graph API
#include "Types.h"         // NodeId, EdgeId, EdgeDirection, CSR_Mode
#include "ErrorCodes.h"    // operationSuccessful

// Forward declaration: the CSR snapshot type used to back SNAPSHOT-mode MATCH
// traversals. Only a reference/pointer is needed here, so the full definition
// (CSR_Representation.h) is included in Query.cpp rather than in this header.
class CSR_Representation;

// Query operation and target types
enum class QueryOperation {
    Unknown,
    Select,
    Insert,
    Delete,
    Match
};

enum class QueryTarget {
    Unknown,
    Nodes,
    Edges
};

// Execution mode: which view of the graph a query resolves against.
//
//   Live     -- (default) the query sees the current, mutable graph. Reads
//               reflect every insert/delete applied so far; INSERT and DELETE
//               are permitted and modify this graph.
//   Snapshot -- the query resolves against a frozen, point-in-time CSR snapshot
//               of the graph (a CSR_Representation, see
//               Query::execute(Graph&, CSR_Representation&)). The traversal
//               runs on CSR_Searcher rather than the live BFS_Searcher, and
//               observes the graph as it was when the snapshot was built,
//               independent of later live mutations.
//
// Because the CSR snapshot only indexes adjacency (not labels or properties),
// Snapshot mode is meaningful only for MATCH traversals. SELECT, INSERT, and
// DELETE with SNAPSHOT are rejected during parsing.
//
// A statement selects Snapshot mode by ending with the reserved keyword
// SNAPSHOT (LIVE is accepted as an explicit no-op for the default). See
// QUERY_REFERENCE.md for the full semantics.
enum class ExecutionMode {
    Live,
    Snapshot
};

// Simple Condition representation used by WHERE parsing
struct Condition {
    std::string property;   // left-hand-side property name (case-preserved)
    std::string op;         // operator token ("=", "!=", "<", ">", "<=", ">=")
    std::string value;      // right-hand-side value (quotes stripped)
    std::string logicOp;    // connector following this condition (e.g. "AND") or empty
};

// Result structure returned by Query::execute / run
struct QueryResult {
    bool success = false;
    std::string message;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<NodeId> traversalResult; // used by MATCH queries
};

// Query class: parsing and execution of lightweight SQL-like statements.
class Query {
public:
    explicit Query(std::string rawQuery = "");

    // high-level parse + execute
    bool parse(const std::string& rawQuery);
    bool parse();

    // Execute against a single live graph, with no CSR snapshot available.
    // LIVE queries read/mutate this graph via BFS_Searcher. A MATCH ... SNAPSHOT
    // query has nothing frozen to read here, so it gracefully falls back to a
    // live BFS traversal. Most callers use this form.
    QueryResult execute(Graph& graph) const;

    // Execute with a CSR snapshot available. A MATCH ... SNAPSHOT traversal runs
    // on CSR_Searcher over `snapshot` (built earlier from the graph), observing
    // that point-in-time state while `live` keeps changing. LIVE reads and all
    // mutations still target `live`.
    QueryResult execute(Graph& live, CSR_Representation& snapshot) const;

    QueryResult run(const std::string& rawQuery, Graph& graph);

    // Which view this parsed statement will resolve against (Live by default,
    // Snapshot when the statement ended with the SNAPSHOT keyword).
    ExecutionMode executionMode() const { return executionMode_; }

    // The projected columns for a SELECT (empty means "*" -- all columns). Only
    // meaningful for SELECT FROM NODES; property keys plus the reserved names
    // "id" and "label" are valid columns. Exposed for testing.
    const std::vector<std::string>& projection() const { return projection_; }

    // Row limit set by a leading TOP <n> (SELECT) / MATCH TOP <n>. hasLimit() is
    // false when no TOP was given; limit() is the cap when it is. Exposed for testing.
    bool hasLimit() const { return hasLimit_; }
    size_t limit() const { return limit_; }

private:
    // small helpers
    std::string toUpper(const std::string& s) const;
    std::string stripQuotes(const std::string& s) const;
    bool parseUInt(const std::string& token, uint64_t& out) const;
    void setError(const std::string& msg);

    // tokenization / parsing
    std::vector<std::string> tokenize(const std::string& text) const;
    bool parseTarget(const std::string& token, QueryTarget& outTarget) const;

    // clause parsers
    bool parseSelect(const std::vector<std::string>& tokens, size_t pos);
    bool parseInsert(const std::vector<std::string>& tokens, size_t pos);
    bool parseDelete(const std::vector<std::string>& tokens, size_t pos);
    bool parseMatch(const std::vector<std::string>& tokens, size_t pos);
    bool parseWhereClause(const std::vector<std::string>& tokens, size_t pos);

    // execution helpers
    QueryResult executeSelectNodes(Graph& graph) const;
    QueryResult executeSelectEdges(Graph& graph) const;
    QueryResult executeInsertNodes(Graph& graph) const;
    QueryResult executeInsertEdges(Graph& graph) const;
    QueryResult executeDeleteNodes(Graph& graph) const;
    QueryResult executeDeleteEdges(Graph& graph) const;
    QueryResult executeMatch(Graph& graph) const;

    // CSR-backed MATCH execution: runs the traversal on CSR_Searcher over the
    // supplied point-in-time snapshot.
    QueryResult executeMatchCSR(CSR_Representation& snapshot) const;

    // Trim each node's property map to the projected columns (no-op for "*").
    // The reserved columns "id"/"label" are structural and always retained.
    void projectNodes(std::vector<Node>& nodes) const;

    // Truncate a result vector to the TOP <n> limit, if one was given.
    template <typename T>
    void applyLimit(std::vector<T>& rows) const {
        if (hasLimit_ && rows.size() > limit_) rows.resize(limit_);
    }

    // Shared execution core for both execute() overloads. `snapshot` is null
    // when no CSR snapshot is available. MATCH reads route to the CSR snapshot
    // in Snapshot mode (when one is supplied); everything else uses `live`.
    QueryResult executeResolved(Graph& live, CSR_Representation* snapshot) const;

private:
    // parse/execution state
    std::string raw_;                          // raw query text
    bool parsed_ = false;                      // whether parse succeeded
    std::string errorMessage_;                 // last parse error

    std::vector<Condition> conditions_;        // parsed WHERE conditions
    propertiesMap values_;                     // parsed INSERT values (column -> value)

    QueryOperation operation_ = QueryOperation::Unknown;
    QueryTarget target_ = QueryTarget::Unknown;

    // SELECT projection: the requested columns. Empty means "*" (all columns).
    // For NODES these are property keys and/or the reserved names "id"/"label".
    std::vector<std::string> projection_;

    // Row limit from a leading TOP <n>. When hasLimit_ is true, at most limit_
    // rows are returned by SELECT / MATCH.
    bool hasLimit_ = false;
    size_t limit_ = 0;

    // Execution mode selected by an optional trailing LIVE / SNAPSHOT keyword.
    ExecutionMode executionMode_ = ExecutionMode::Live;

    // MATCH-specific
    CSR_Mode matchMode_ = CSR_Mode::REACHABLE;
    uint64_t matchFromRaw_ = 0;
    uint64_t matchToRaw_ = 0;
    size_t matchK_ = 0;
};