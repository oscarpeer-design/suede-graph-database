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
    Match,
    Update,
    Load,
    Save,
    Import,   // IMPORT CSV '<path>' -- parsed here, executed by a coordinator
    Export    // EXPORT CSV '<path>' -- parsed here, executed by a coordinator
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

    // Operation / target of the parsed statement, the LOAD/SAVE file path, and the
    // last parse error. Exposed so an outer coordinator (e.g. a GraphHandler that
    // owns a StorageEngine) can route LOAD/SAVE and inspect parse failures.
    QueryOperation operation() const { return operation_; }
    QueryTarget target() const { return target_; }
    const std::string& filePath() const { return filePath_; }
    const std::string& lastError() const { return errorMessage_; }

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
    bool parseUpdate(const std::vector<std::string>& tokens, size_t pos);
    bool parseLoad(const std::vector<std::string>& tokens, size_t pos);
    bool parseSave(const std::vector<std::string>& tokens, size_t pos);
    // IMPORT CSV '<path>' / EXPORT CSV '<path>'. Like LOAD/SAVE these only parse
    // the statement and record the path in filePath_; a coordinator that owns a
    // StorageEngine performs the actual CSV read/write.
    bool parseImport(const std::vector<std::string>& tokens, size_t pos);
    bool parseExport(const std::vector<std::string>& tokens, size_t pos);
    bool parseWhereClause(const std::vector<std::string>& tokens, size_t pos);

    // Parse the SET key=value[, key=value ...] list of an UPDATE. Fills values_.
    // `pos` should point at the first token after the SET keyword.
    bool parseSetClause(const std::vector<std::string>& tokens, size_t pos);

    // execution helpers
    QueryResult executeSelectNodes(Graph& graph) const;
    QueryResult executeSelectEdges(Graph& graph) const;

    // Snapshot-mode SELECT resolves against a frozen CSR snapshot.
    //
    // NODES: the CSR snapshot is the authority for *which* nodes are visible --
    //   its captured node-id set (GetCSRNodeMapping) defines membership. The node
    //   payloads (label / properties) are read from the graph's MVCC history at the
    //   snapshot's captured version, so filtering sees point-in-time property
    //   values. This honours the design where CSR holds the raw index and the graph
    //   holds the properties.
    // EDGES: the CSR snapshot carries no edge identity (it indexes node adjacency
    //   only), so edges resolve against the graph's MVCC history at the snapshot's
    //   captured version; the snapshot is used only to supply that version.
    //
    // Both support the same WHERE forms as the live path (ID / LABEL filters, plus
    // property filters for nodes).
    QueryResult executeSelectNodesSnapshot(Graph& graph, const CSR_Representation& snapshot) const;
    QueryResult executeSelectEdgesSnapshot(Graph& graph, const CSR_Representation& snapshot) const;
    QueryResult executeInsertNodes(Graph& graph) const;
    QueryResult executeInsertEdges(Graph& graph) const;
    QueryResult executeDeleteNodes(Graph& graph) const;
    QueryResult executeDeleteEdges(Graph& graph) const;
    QueryResult executeMatch(Graph& graph) const;
    QueryResult executeUpdateNodes(Graph& graph) const;
    QueryResult executeUpdateEdges(Graph& graph) const;

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
    propertiesMap values_;                     // parsed INSERT / UPDATE values (column -> value)
    std::string filePath_;                     // parsed path for LOAD / SAVE FILE

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