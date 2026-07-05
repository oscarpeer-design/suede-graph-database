#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include "Graph.h"         // Node, Edge, propertiesMap, Graph API
#include "Types.h"         // NodeId, EdgeId, EdgeDirection, CSR_Mode
#include "ErrorCodes.h"    // operationSuccessful

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
    QueryResult execute(Graph& graph) const;
    QueryResult run(const std::string& rawQuery, Graph& graph);

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

private:
    // parse/execution state
    std::string raw_;                          // raw query text
    bool parsed_ = false;                      // whether parse succeeded
    std::string errorMessage_;                 // last parse error

    std::vector<Condition> conditions_;        // parsed WHERE conditions
    propertiesMap values_;                     // parsed INSERT values (column -> value)

    QueryOperation operation_ = QueryOperation::Unknown;
    QueryTarget target_ = QueryTarget::Unknown;

    // MATCH-specific
    CSR_Mode matchMode_ = CSR_Mode::REACHABLE;
    uint64_t matchFromRaw_ = 0;
    uint64_t matchToRaw_ = 0;
    size_t matchK_ = 0;
};
