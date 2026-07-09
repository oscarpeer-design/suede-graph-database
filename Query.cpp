// Query.cpp
//
// Implementation of the lightweight SQL-like query parser/executor.
// This file contains detailed, line-by-line comments explaining what each
// statement does to make the control flow and data transformations easy to
// understand.

#include "Query.h"
#include "BFS_Searcher.h"                      // Live MATCH traversals (LIVE mode)
#include "CSR_Representation.h"                 // Point-in-time CSR snapshot type
#include "CSR_Searcher.h"                       // Snapshot MATCH traversals (SNAPSHOT mode)

#include <algorithm>                            // std::transform, std::find
#include <cctype>                               // std::toupper, std::isspace, std::isdigit
#include <cstdlib>                              // std::strtoull
#include <unordered_map>                        // used for mapping field names to values

namespace {                                    // anonymous namespace for internal helpers

    // Return true if the character is considered part of a comparison operator.
    bool isOperatorChar(char c) {
        return c == '=' || c == '!' || c == '<' || c == '>'; // operator chars: =, !, <, >
    }

} // anonymous namespace

// Constructor that stores the raw query string; the string is moved in to avoid copy.
Query::Query(std::string rawQuery) : raw_(std::move(rawQuery)) {}

// --------------------------- small helpers ------------------------------------

// Convert string `s` to uppercase and return it.
std::string Query::toUpper(const std::string& s) const {
    std::string out = s;                         // make a mutable copy of the input
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); }); // uppercase each char
    return out;                                  // return the transformed string
}

// Remove matching single or double quotes from start and end of `s`, if present.
std::string Query::stripQuotes(const std::string& s) const {
    if (s.size() >= 2) {                         // only possible to have quotes if length >= 2
        char first = s.front();                  // first character
        char last = s.back();                    // last character
        if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
            return s.substr(1, s.size() - 2);    // return the substring without the outer quotes
        }
    }
    return s;                                    // return original if no matching quotes found
}

// Parse an unsigned integer from the token; reject empty or non-digit tokens.
bool Query::parseUInt(const std::string& token, uint64_t& out) const {
    if (token.empty()) return false;             // reject empty token immediately
    for (char c : token) {                       // validate every character is a digit
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    out = std::strtoull(token.c_str(), nullptr, 10); // convert decimal string to uint64_t
    return true;                                 // success
}

// Record an error message and mark the parse as failed.
void Query::setError(const std::string& msg) {
    errorMessage_ = msg;                         // store human-readable message
    parsed_ = false;                             // mark parse state as failed
}

// --------------------------- tokenizer ---------------------------------------

// Tokenize `text` into identifiers, operators, quoted strings, parentheses, and commas.
std::vector<std::string> Query::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;             // output token list
    size_t i = 0;                                // current index into the input
    const size_t n = text.size();                // store input length for loop bounds

    while (i < n) {                              // loop until we consume entire string
        char c = text[i];                        // current character

        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; } // skip whitespace

        // Handle quoted string tokens ('...' or "...")
        if (c == '\'' || c == '"') {
            char quote = c;                      // remember which quote char we saw
            size_t start = i;                    // token start index includes the opening quote
            ++i;                                 // advance past opening quote
            while (i < n && text[i] != quote) ++i; // find matching closing quote (no escape handling)
            if (i < n) ++i;                      // include closing quote if it exists
            tokens.push_back(text.substr(start, i - start)); // push the entire quoted token
            continue;                            // continue scanning after the quoted token
        }

        // Parentheses and commas are single-character tokens
        if (c == '(' || c == ')' || c == ',') {
            tokens.push_back(std::string(1, c)); // push the punctuation as a token
            ++i;                                 // advance past the punctuation
            continue;
        }

        // Operators: =, !=, <=, >=, <, >
        if (isOperatorChar(c)) {
            size_t start = i;                    // operator token start
            ++i;                                 // consume the first operator char
            if (i < n && text[i] == '=') ++i;   // allow two-character ops like <=, >=, !=
            tokens.push_back(text.substr(start, i - start)); // push operator token
            continue;
        }

        // Identifiers, keywords, and numbers: read maximal run of non-special chars
        size_t start = i;                        // token start
        while (i < n) {
            char cc = text[i];                   // peek next char
            if (std::isspace(static_cast<unsigned char>(cc)) || cc == '(' || cc == ')' ||
                cc == ',' || cc == '\'' || cc == '"' || isOperatorChar(cc)) {
                break;                          // stop when encountering delimiter
            }
            ++i;                                 // otherwise include char in current token
        }
        tokens.push_back(text.substr(start, i - start)); // push identifier/number token
    }

    return tokens;                               // return accumulated tokens
}

// --------------------------- parse orchestration ------------------------------

// Overload that sets raw_ and invokes the parameterless parse().
bool Query::parse(const std::string& rawQuery) {
    raw_ = rawQuery;                             // replace stored raw input
    return parse();                              // call the main parse implementation
}

// Main parse function: reset state, tokenize, dispatch to clause-specific parser.
bool Query::parse() {
    parsed_ = false;                             // clear parse success flag
    errorMessage_.clear();                       // clear any existing error text
    conditions_.clear();                         // clear parsed WHERE conditions
    values_.clear();                             // clear parsed INSERT / UPDATE values
    filePath_.clear();                           // clear parsed LOAD / SAVE path
    operation_ = QueryOperation::Unknown;        // reset operation type
    target_ = QueryTarget::Unknown;              // reset target type
    executionMode_ = ExecutionMode::Live;        // reset to the default (live) view
    projection_.clear();                         // reset projection (empty == "*")
    hasLimit_ = false;                           // reset row limit
    limit_ = 0;

    std::vector<std::string> tokens = tokenize(raw_); // produce tokens from the raw input
    if (tokens.empty()) {                        // handle empty input case
        setError("Empty query.");                // set error and mark parse failure
        return false;
    }

    // Optional trailing execution-mode keyword. A statement may end with the
    // reserved word LIVE (the default) or SNAPSHOT (read against a frozen,
    // point-in-time copy). We consume it here, before dispatch, so the
    // individual clause parsers never see it and their "unexpected trailing
    // tokens" checks keep working unchanged. Only a bare, final LIVE/SNAPSHOT
    // token is treated as a mode keyword -- a quoted value such as 'SNAPSHOT'
    // tokenizes with its quotes and is left untouched.
    if (tokens.size() >= 2) {
        std::string lastUpper = toUpper(tokens.back());
        if (lastUpper == "SNAPSHOT") {
            executionMode_ = ExecutionMode::Snapshot;
            tokens.pop_back();
        }
        else if (lastUpper == "LIVE") {
            executionMode_ = ExecutionMode::Live;
            tokens.pop_back();
        }
    }

    std::string keyword = toUpper(tokens[0]);    // read the first token as a case-insensitive keyword

    // SNAPSHOT selects a point-in-time view. It is meaningful for reads (SELECT
    // and MATCH) but not for mutations: INSERT and DELETE always apply to the
    // live graph, so SNAPSHOT on them is rejected up front with a clear error.
    // SELECT ... SNAPSHOT resolves against the MVCC history at the snapshot's
    // version; MATCH ... SNAPSHOT traverses the frozen CSR snapshot.
    if (executionMode_ == ExecutionMode::Snapshot &&
        keyword != "MATCH" && keyword != "SELECT") {
        setError("SNAPSHOT applies only to reads (SELECT, MATCH); INSERT, DELETE, and UPDATE always run against the live graph.");
        return false;
    }

    bool ok = false;                             // will hold result of clause-specific parsing

    if (keyword == "SELECT") {                   // SELECT statement dispatch
        operation_ = QueryOperation::Select;     // set operation type
        ok = parseSelect(tokens, 1);             // parse the remainder starting at token index 1
    }
    else if (keyword == "INSERT") {            // INSERT statement dispatch
        operation_ = QueryOperation::Insert;
        ok = parseInsert(tokens, 1);
    }
    else if (keyword == "DELETE") {            // DELETE statement dispatch
        operation_ = QueryOperation::Delete;
        ok = parseDelete(tokens, 1);
    }
    else if (keyword == "MATCH") {             // MATCH traversal dispatch
        operation_ = QueryOperation::Match;
        ok = parseMatch(tokens, 1);
    }
    else if (keyword == "UPDATE") {            // UPDATE statement dispatch
        operation_ = QueryOperation::Update;
        ok = parseUpdate(tokens, 1);
    }
    else if (keyword == "LOAD") {              // LOAD FILE dispatch
        operation_ = QueryOperation::Load;
        ok = parseLoad(tokens, 1);
    }
    else if (keyword == "SAVE") {              // SAVE FILE dispatch
        operation_ = QueryOperation::Save;
        ok = parseSave(tokens, 1);
    }
    else {                                     // unknown top-level keyword
        setError("Unrecognized query keyword: " + tokens[0]); // error out with original token shown
        return false;
    }

    parsed_ = ok;                                // record parse success/failure
    return ok;                                   // return result to caller
}

// Map textual token to QueryTarget enum for "NODES" / "EDGES".
bool Query::parseTarget(const std::string& token, QueryTarget& outTarget) const {
    std::string t = toUpper(token);              // compare case-insensitively
    if (t == "NODES") { outTarget = QueryTarget::Nodes; return true; } // match nodes
    if (t == "EDGES") { outTarget = QueryTarget::Edges; return true; } // match edges
    return false;                                // unknown target
}

// --------------------------- clause parsers ----------------------------------

// parse SELECT [TOP <n>] <* | col1, col2, ...> FROM <NODES|EDGES> [WHERE ...]
//
// Projection: '*' selects all columns (the default); a comma-separated list of
// column names selects only those. Column names are node property keys plus the
// reserved names "id" and "label". Projection is NODES-only -- an explicit
// column list against EDGES is rejected (edges have no property map to trim).
//
// TOP <n>: an optional leading row limit (T-SQL style), applied to the result.
bool Query::parseSelect(const std::vector<std::string>& tokens, size_t pos) {
    // Optional leading TOP <n>.
    if (pos < tokens.size() && toUpper(tokens[pos]) == "TOP") {
        ++pos;                                    // consume TOP
        uint64_t n = 0;
        if (pos >= tokens.size() || !parseUInt(tokens[pos], n)) {
            setError("SELECT TOP: expected a non-negative integer row count after TOP.");
            return false;
        }
        hasLimit_ = true;
        limit_ = static_cast<size_t>(n);
        ++pos;                                    // consume the count
    }

    // Projection: '*' or a comma-separated column list.
    if (pos >= tokens.size()) { setError("SELECT: expected '*' or a column list."); return false; }
    if (tokens[pos] == "*") {
        ++pos;                                    // '*' -> projection_ stays empty (all columns)
    }
    else {
        // Read identifiers separated by commas until FROM.
        bool expectColumn = true;                 // grammar: col (, col)*
        while (pos < tokens.size() && toUpper(tokens[pos]) != "FROM") {
            if (tokens[pos] == ",") {
                if (expectColumn) { setError("SELECT: unexpected ',' in column list."); return false; }
                expectColumn = true;
                ++pos;
                continue;
            }
            if (!expectColumn) { setError("SELECT: expected ',' between columns."); return false; }
            // A bare '*' mixed into a list is not allowed.
            if (tokens[pos] == "*") { setError("SELECT: cannot mix '*' with named columns."); return false; }
            projection_.push_back(tokens[pos]);   // column name, case-preserved
            expectColumn = false;
            ++pos;
        }
        if (projection_.empty() || expectColumn) { setError("SELECT: expected a column name."); return false; }
    }

    if (pos >= tokens.size() || toUpper(tokens[pos]) != "FROM") { setError("SELECT: expected FROM."); return false; } // require FROM
    ++pos;                                        // advance past 'FROM'

    if (pos >= tokens.size() || !parseTarget(tokens[pos], target_)) { // read target token and set target_
        setError("SELECT: expected NODES or EDGES after FROM.");
        return false;
    }
    ++pos;                                        // advance past target token

    // Selective projection is only supported for NODES (edges have no property
    // map). EDGES must use '*'.
    if (target_ == QueryTarget::Edges && !projection_.empty()) {
        setError("SELECT: selective columns are only supported for NODES; use '*' for EDGES.");
        return false;
    }

    if (pos < tokens.size()) {                    // if there are more tokens, expect a WHERE clause
        if (toUpper(tokens[pos]) != "WHERE") { setError("SELECT: unexpected token '" + tokens[pos] + "'."); return false; } // unexpected token
        return parseWhereClause(tokens, pos + 1); // delegate WHERE parsing starting after 'WHERE'
    }

    // A WHERE-less SELECT is a full scan. Against the live graph this is rejected
    // (the live path is index-driven and needs an ID/LABEL predicate), but a
    // SELECT ... SNAPSHOT materializes the whole point-in-time set, so an
    // unfiltered snapshot scan is well-defined and permitted.
    if (executionMode_ == ExecutionMode::Snapshot) {
        return true;                              // no conditions: full snapshot scan
    }

    setError("SELECT: a WHERE clause is required (WHERE ID = ... or WHERE LABEL = ...)."); // require explicit WHERE
    return false;
}

// parse INSERT INTO <NODES|EDGES> (col1, col2, ...) VALUES (val1, val2, ...)
// Columns and values are matched by position and stored into values_ map.
bool Query::parseInsert(const std::vector<std::string>& tokens, size_t pos) {
    if (pos >= tokens.size() || toUpper(tokens[pos]) != "INTO") { setError("INSERT: expected INTO."); return false; } // require INTO
    ++pos;                                        // advance past INTO

    if (pos >= tokens.size() || !parseTarget(tokens[pos], target_)) { // parse target after INTO
        setError("INSERT: expected NODES or EDGES after INTO.");
        return false;
    }
    ++pos;                                        // advance past target

    if (pos >= tokens.size() || tokens[pos] != "(") { setError("INSERT: expected '(' to begin column list."); return false; } // column list start
    ++pos;                                        // advance into column list

    std::vector<std::string> columns;             // temporary storage for parsed column names
    while (pos < tokens.size() && tokens[pos] != ")") { // collect tokens until ')' encountered
        if (tokens[pos] != ",") columns.push_back(tokens[pos]); // ignore commas, add names
        ++pos;                                    // advance to next token
    }
    if (pos >= tokens.size()) { setError("INSERT: unterminated column list."); return false; } // unmatched '('
    ++pos;                                        // skip the closing ')'

    if (pos >= tokens.size() || toUpper(tokens[pos]) != "VALUES") { setError("INSERT: expected VALUES."); return false; } // expect VALUES
    ++pos;                                        // advance past VALUES

    if (pos >= tokens.size() || tokens[pos] != "(") { setError("INSERT: expected '(' to begin value list."); return false; } // value list start
    ++pos;                                        // advance into values

    std::vector<std::string> vals;                // store parsed values as strings
    while (pos < tokens.size() && tokens[pos] != ")") { // collect until ')'
        if (tokens[pos] != ",") vals.push_back(stripQuotes(tokens[pos])); // strip quotes from string literals
        ++pos;                                    // advance token pointer
    }
    if (pos >= tokens.size()) { setError("INSERT: unterminated value list."); return false; } // unmatched '(' in values
    ++pos;                                        // skip ')'

    if (columns.size() != vals.size()) { setError("INSERT: column count does not match value count."); return false; } // mismatch check

    for (size_t i = 0; i < columns.size(); ++i) values_[columns[i]] = vals[i]; // populate values_ map by column name

    if (pos != tokens.size()) { setError("INSERT: unexpected trailing tokens."); return false; } // no trailing tokens allowed

    if (target_ == QueryTarget::Nodes) {         // additional validation for node inserts
        if (values_.find("label") == values_.end()) {
            setError("INSERT INTO NODES requires a 'label' column.");
            return false;
        }
    }
    else {                                      // validation for edge inserts
        // Edges must have exactly from, to, label and no other columns.
        if (values_.find("from") == values_.end() || values_.find("to") == values_.end() ||
            values_.find("label") == values_.end()) {
            setError("INSERT INTO EDGES requires 'from', 'to', and 'label' columns.");
            return false;
        }
        if (values_.size() != 3) {
            setError("INSERT INTO EDGES only supports 'from', 'to', and 'label' columns (edges have no properties).");
            return false;
        }
    }

    return true;                                  // insert parse succeeded
}

// parse DELETE FROM <NODES|EDGES> WHERE ID = <id>
// Only single-ID deletes are supported; the WHERE clause must be a single ID equality.
bool Query::parseDelete(const std::vector<std::string>& tokens, size_t pos) {
    if (pos >= tokens.size() || toUpper(tokens[pos]) != "FROM") { setError("DELETE: expected FROM."); return false; } // require FROM
    ++pos;                                        // advance past FROM

    if (pos >= tokens.size() || !parseTarget(tokens[pos], target_)) { // read target token (NODES/EDGES)
        setError("DELETE: expected NODES or EDGES after FROM.");
        return false;
    }
    ++pos;                                        // advance past target

    if (pos >= tokens.size() || toUpper(tokens[pos]) != "WHERE") {
        setError("DELETE: a WHERE ID = <id> clause is required."); // require WHERE clause for deletes
        return false;
    }
    if (!parseWhereClause(tokens, pos + 1)) return false; // parse WHERE starting after 'WHERE'

    // Only single equality on ID is allowed for DELETE
    if (conditions_.size() != 1 || toUpper(conditions_[0].property) != "ID" || conditions_[0].op != "=") {
        setError("DELETE: only WHERE ID = <id> is supported.");
        return false;
    }

    return true;                                  // delete parse validated
}

// parse MATCH [TOP <n>] <mode> FROM <src> ...  (SHORTEST_PATH, REACHABLE, KHOP)
bool Query::parseMatch(const std::vector<std::string>& tokens, size_t pos) {
    // Optional leading TOP <n> caps the number of nodes returned.
    if (pos < tokens.size() && toUpper(tokens[pos]) == "TOP") {
        ++pos;                                    // consume TOP
        uint64_t n = 0;
        if (pos >= tokens.size() || !parseUInt(tokens[pos], n)) {
            setError("MATCH TOP: expected a non-negative integer row count after TOP.");
            return false;
        }
        hasLimit_ = true;
        limit_ = static_cast<size_t>(n);
        ++pos;                                    // consume the count
    }

    if (pos >= tokens.size()) { setError("MATCH: expected SHORTEST_PATH, REACHABLE, or KHOP."); return false; } // require mode token
    std::string mode = toUpper(tokens[pos]);       // read mode in uppercase
    if (mode == "SHORTEST_PATH") matchMode_ = CSR_Mode::SHORTEST_PATH; // set match mode
    else if (mode == "REACHABLE") matchMode_ = CSR_Mode::REACHABLE;
    else if (mode == "KHOP") matchMode_ = CSR_Mode::KHOP;
    else { setError("MATCH: unknown mode '" + tokens[pos] + "'."); return false; } // unknown mode
    ++pos;                                        // advance past mode token

    if (pos >= tokens.size() || toUpper(tokens[pos]) != "FROM") { setError("MATCH: expected FROM."); return false; } // expect FROM
    ++pos;                                        // advance past FROM

    if (pos >= tokens.size() || !parseUInt(tokens[pos], matchFromRaw_)) { // parse source id
        setError("MATCH: expected a numeric node id after FROM.");
        return false;
    }
    ++pos;                                        // advance after source id

    if (matchMode_ == CSR_Mode::SHORTEST_PATH) {   // SHORTEST_PATH requires a TO <id>
        if (pos >= tokens.size() || toUpper(tokens[pos]) != "TO") { setError("MATCH SHORTEST_PATH: expected TO."); return false; }
        ++pos;                                    // skip 'TO'
        if (pos >= tokens.size() || !parseUInt(tokens[pos], matchToRaw_)) { // parse target id
            setError("MATCH SHORTEST_PATH: expected a numeric node id after TO.");
            return false;
        }
        ++pos;                                    // advance after target id
    }
    else if (matchMode_ == CSR_Mode::KHOP) {     // KHOP expects STEPS <k>
        if (pos >= tokens.size() || toUpper(tokens[pos]) != "STEPS") { setError("MATCH KHOP: expected STEPS."); return false; }
        ++pos;                                    // skip 'STEPS'
        uint64_t k = 0;                           // temporary numeric storage
        if (pos >= tokens.size() || !parseUInt(tokens[pos], k)) { // parse k
            setError("MATCH KHOP: expected a numeric step count after STEPS.");
            return false;
        }
        matchK_ = static_cast<size_t>(k);         // store step count into matchK_
        ++pos;                                    // advance past steps value
    }

    if (pos != tokens.size()) { setError("MATCH: unexpected trailing tokens."); return false; } // no trailing tokens allowed

    return true;                                  // match parse succeeded
}

// parse a WHERE clause of the shape: property op value [AND property op value ...]
// Supports only the operators explicitly in validOps and only AND logic.
bool Query::parseWhereClause(const std::vector<std::string>& tokens, size_t pos) {
    conditions_.clear();                          // clear any pre-existing conditions

    while (pos < tokens.size()) {                 // keep parsing condition groups until end or error
        Condition cond;                           // reusable condition struct to populate
        cond.property = tokens[pos];              // read property name token
        ++pos;                                    // advance after property

        if (pos >= tokens.size()) { setError("WHERE: expected operator."); return false; } // require operator token
        cond.op = tokens[pos];                    // read operator token
        static const std::vector<std::string> validOps = { "=", "!=", "<", ">", "<=", ">=" }; // allowed ops
        if (std::find(validOps.begin(), validOps.end(), cond.op) == validOps.end()) { // validate op
            setError("WHERE: invalid operator '" + cond.op + "'.");
            return false;
        }
        ++pos;                                    // advance past operator

        if (pos >= tokens.size()) { setError("WHERE: expected value."); return false; } // require RHS value
        cond.value = stripQuotes(tokens[pos]);    // store RHS, stripping surrounding quotes
        ++pos;                                    // advance past value

        if (pos < tokens.size()) {                 // check for optional logic connector
            std::string next = toUpper(tokens[pos]); // read connector
            if (next == "AND") { cond.logicOp = next; ++pos; } // accept only AND
            else { setError("WHERE: unexpected token '" + tokens[pos] + "' (only AND is supported)."); return false; } // unexpected token
        }

        conditions_.push_back(cond);              // append parsed condition to conditions_ vector
        if (cond.logicOp.empty()) break;          // if no AND followed, break out of loop; otherwise continue to next condition
    }

    if (conditions_.empty()) { setError("WHERE: no conditions found."); return false; } // ensure at least one condition parsed
    return true;                                  // WHERE clause parsed successfully
}

// --------------------------- execute() --------------------------------------

// Single-graph execute: no CSR snapshot available. A MATCH ... SNAPSHOT query
// has nothing frozen to read, so it falls back to a live BFS traversal.
QueryResult Query::execute(Graph& graph) const {
    return executeResolved(graph, nullptr);
}

// CSR-snapshot execute: a MATCH ... SNAPSHOT traversal runs on CSR_Searcher over
// `snapshot`; LIVE reads and all mutations target `live`.
QueryResult Query::execute(Graph& live, CSR_Representation& snapshot) const {
    return executeResolved(live, &snapshot);
}

// Execute previously parsed statement. Dispatch based on operation_. SELECT,
// INSERT and DELETE always run against the live graph (SNAPSHOT on these is
// rejected during parsing). A MATCH traversal runs on the CSR snapshot when
// Snapshot mode is requested and a snapshot is supplied; otherwise on live BFS.
QueryResult Query::executeResolved(Graph& live, CSR_Representation* snapshot) const {
    QueryResult result;                           // accumulator for execution result

    if (!parsed_) {                               // guard: cannot execute if not parsed successfully
        result.success = false;                   // indicate failure
        result.message = errorMessage_.empty() ? "Query has not been successfully parsed." : errorMessage_; // choose message
        return result;                            // return immediately on error
    }

    switch (operation_) {                         // dispatch by operation type
    case QueryOperation::Select:
        // SELECT ... SNAPSHOT resolves against MVCC history at the snapshot's
        // captured version, when a CSR snapshot is available to supply it.
        // Without a snapshot there is no captured version, so we fall back to
        // the live graph (documented behaviour, mirroring MATCH ... SNAPSHOT).
        if (executionMode_ == ExecutionMode::Snapshot && snapshot != nullptr) {
            const uint64_t ver = snapshot->GetSnapshotVersion();
            return (target_ == QueryTarget::Nodes)
                ? executeSelectNodesSnapshot(live, ver)
                : executeSelectEdgesSnapshot(live, ver);
        }
        return (target_ == QueryTarget::Nodes) ? executeSelectNodes(live) : executeSelectEdges(live);
    case QueryOperation::Insert:
        return (target_ == QueryTarget::Nodes) ? executeInsertNodes(live) : executeInsertEdges(live);
    case QueryOperation::Delete:
        return (target_ == QueryTarget::Nodes) ? executeDeleteNodes(live) : executeDeleteEdges(live);
    case QueryOperation::Match:
        // Route to the CSR snapshot when SNAPSHOT was requested and one is
        // actually available; otherwise fall back to the live BFS traversal.
        if (executionMode_ == ExecutionMode::Snapshot && snapshot != nullptr)
            return executeMatchCSR(*snapshot);
        return executeMatch(live);
    case QueryOperation::Update:
        return (target_ == QueryTarget::Nodes) ? executeUpdateNodes(live) : executeUpdateEdges(live);
    case QueryOperation::Load:
    case QueryOperation::Save:
        // LOAD / SAVE are storage operations, not graph operations. The Query
        // layer only parses them (path via filePath()); a coordinator that owns a
        // StorageEngine performs the actual I/O. Bare execution against a Graph
        // alone cannot reach storage, so report that.
        result.success = false;
        result.message = (operation_ == QueryOperation::Load)
            ? "LOAD must be executed by a coordinator that owns a StorageEngine."
            : "SAVE must be executed by a coordinator that owns a StorageEngine.";
        return result;
    case QueryOperation::Unknown:
    default:
        result.success = false;               // unknown operation is an execution error
        result.message = "Cannot execute a query with an unknown operation.";
        return result;
    }
}

// Convenience method: parse the provided rawQuery then execute it.
QueryResult Query::run(const std::string& rawQuery, Graph& graph) {
    if (!parse(rawQuery)) {                       // attempt to parse; on failure return the parse error
        QueryResult result;
        result.success = false;
        result.message = errorMessage_;
        return result;
    }
    return execute(graph);                        // on success execute the parsed query
}

// --------------------------- SELECT execution --------------------------------
static bool executeCondition(const Condition& e, const std::string& actual, bool& matches) {
    bool condResult;
    if (e.op == "=") // equality
        condResult = (actual == e.value);
    else if (e.op == "!=") // inequality
        condResult = (actual != e.value); // inequality
    else if (e.op == "<") // lexicographic comparison
        condResult = (actual < e.value);
    else if (e.op == ">") // greater than lexicographic comparison
        condResult = (actual > e.value);
    else if (e.op == "<=") // less than or equal to
        condResult = (actual <= e.value);
    else if (e.op == ">=") // greater than or equal to
        condResult = (actual >= e.value);
    else
        condResult = false;        // unknown operator (should not happen due to parse validation)

    matches = condResult;          // report the result through the out-parameter
    return condResult;             // and as the return value used by the caller
}

// Trim each node's property map to the projected columns. A "*" projection
// (projection_ empty) is a no-op. The reserved columns "id" and "label" are
// structural fields on the Node (not entries in the property map), so they are
// always available and are simply skipped here; every other projected column is
// looked up as a property key and kept when present.
void Query::projectNodes(std::vector<Node>& nodes) const {
    if (projection_.empty()) return;              // '*' -> keep all columns

    for (Node& node : nodes) {
        propertiesMap trimmed;
        for (const std::string& col : projection_) {
            std::string upper = toUpper(col);
            if (upper == "ID" || upper == "LABEL") continue; // structural, always kept
            auto it = node.properties.find(col);  // property keys are case-sensitive
            if (it != node.properties.end())
                trimmed.emplace(col, it->second);
        }
        node.properties = std::move(trimmed);
    }
}



// Execute a SELECT query targeting nodes.
QueryResult Query::executeSelectNodes(Graph& graph) const {
    QueryResult result;                           // prepare result container

    // Fast-path: WHERE ID = <id>
    for (const Condition& c : conditions_) {      // iterate conditions to find ID equality
        if (toUpper(c.property) == "ID" && c.op == "=") {
            uint64_t idVal;                       // numeric id storage
            if (!parseUInt(c.value, idVal)) { result.success = false; result.message = "Invalid node id."; return result; } // validate id
            Node node;                            // temporary Node container
            if (graph.GetNode(NodeId(idVal), node)) { // attempt to fetch node by id
                result.nodes.push_back(node);    // add found node to result set
            }
            result.success = true;               // an id lookup always succeeds (0 or 1 rows)
            projectNodes(result.nodes);          // trim to selected columns (no-op for '*')
            applyLimit(result.nodes);            // apply TOP <n> (only matters for TOP 0 here)
            result.message = (result.nodes.size() == 1)
                ? "Found 1 row."
                : "Found " + std::to_string(result.nodes.size()) + " row(s).";
            return result;                       // return early after ID lookup
        }
    }

    // Label-based selection: WHERE LABEL = '...' [AND property filters ...]
    for (const Condition& c : conditions_) {      // search for a LABEL condition
        if (toUpper(c.property) == "LABEL" && c.op == "=") {
            int warning = operationSuccessful;    // Graph API uses an integer warning code pattern
            std::vector<Node> found;              // temporary container for nodes returned by graph
            graph.FindNodes(found, c.value, warning); // ask Graph for nodes matching label
            if (warning != operationSuccessful) { // treat graph warning as query-level failure
                result.success = false;
                result.message = "No nodes found with that label.";
                return result;
            }

            // Build a list of extra property filters (exclude the LABEL condition itself)
            std::vector<Condition> extra;
            for (const Condition& other : conditions_) {
                std::string p = toUpper(other.property);
                if (p != "LABEL") extra.push_back(other); // keep other conditions for client-side filtering
            }

            // Apply client-side property filters to each node returned by Graph::FindNodes
            for (const Node& node : found) {
                bool matches = true;                // optimistic match flag
                for (const Condition& e : extra) {
                    auto it = node.properties.find(e.property); // find property in node's property map
                    std::string actual = (it != node.properties.end()) ? it->second : ""; // default empty if absent
                    bool condResult = executeCondition(e, actual, matches); // execute the condition
                    // break early if any filter fails
                    if (!condResult) {
                        matches = false;
                        break;
                    }
                }
                if (matches) result.nodes.push_back(node); // keep nodes that match all extra filters
            }

            result.success = true;
            projectNodes(result.nodes);              // trim to selected columns (no-op for '*')
            applyLimit(result.nodes);                // apply TOP <n> row cap
            result.message = (result.nodes.size() == 1)
                ? "Found 1 row."
                : "Found " + std::to_string(result.nodes.size()) + " row(s).";
            return result;                           // done processing LABEL-based SELECT
        }
    }

    // If neither ID nor LABEL were provided, return an informative error.
    result.success = false;
    result.message = "SELECT FROM NODES requires WHERE ID = ... or WHERE LABEL = ....";
    return result;
}

// Execute a SELECT query targeting edges.
QueryResult Query::executeSelectEdges(Graph& graph) const {
    QueryResult result;                           // prepare result container

    std::unordered_map<std::string, std::string> field; // map stringified field names to values
    for (const Condition& c : conditions_) {      // collect only equality conditions into the map
        if (c.op == "=") field[toUpper(c.property)] = c.value;
    }

    int warning = operationSuccessful;            // warning code used by Graph APIs
    std::vector<Edge> found;                      // temporary storage for edges returned by Graph

    // ID lookup fast-path: WHERE ID = <id>
    if (field.count("ID")) {
        uint64_t idVal;                           // parsed edge id storage
        if (!parseUInt(field["ID"], idVal)) { result.success = false; result.message = "Invalid edge id."; return result; } // validate ID
        Edge edge;                                // temporary Edge holder
        if (graph.GetEdge(EdgeId(idVal), edge)) { // fetch edge by id
            result.edges.push_back(edge);        // add to result if found
        }
        result.success = true;                   // an id lookup always succeeds (0 or 1 rows)
        applyLimit(result.edges);                // apply TOP <n> (only matters for TOP 0 here)
        result.message = (result.edges.size() == 1)
            ? "Found 1 row."
            : "Found " + std::to_string(result.edges.size()) + " row(s).";
        return result;                           // return after ID lookup
    }

    // Determine direction for FROM-based queries (default to OUTGOING)
    EdgeDirection dir = OUTGOING;                 // default behavior matches "edges FROM x"
    if (field.count("DIRECTION")) {               // if DIRECTION was provided parse it
        std::string d = toUpper(field["DIRECTION"]);
        if (d == "OUTGOING") dir = OUTGOING;
        else if (d == "INCOMING") dir = INCOMING;
        else if (d == "BOTH") dir = BOTH;
        else { result.success = false; result.message = "Invalid DIRECTION (expected OUTGOING, INCOMING, or BOTH)."; return result; } // invalid direction
    }

    // FROM-based query: WHERE FROM = <nodeId> [AND LABEL = '...' ] [AND TO = <id>]
    if (field.count("FROM")) {
        uint64_t fromVal;
        if (!parseUInt(field["FROM"], fromVal)) { result.success = false; result.message = "Invalid FROM node id."; return result; } // validate FROM
        NodeId from(fromVal);                     // wrap raw id into NodeId

        if (field.count("LABEL")) {               // if LABEL provided, use label-aware fetch
            graph.FindEdgesByNodeAndLabel(from, field["LABEL"], found, dir, warning);
        }
        else {                                  // otherwise request all edges for the node in the requested direction
            graph.FindEdgesByNodeId(from, found, dir, warning);
        }

        if (warning != operationSuccessful) {      // Graph reported an error/failure
            result.success = false;
            result.message = "No edges found for that node.";
            return result;
        }

        // Optional post-filter on TO: if present keep only edges with matching endpoint
        if (field.count("TO")) {
            uint64_t toVal;
            if (!parseUInt(field["TO"], toVal)) { result.success = false; result.message = "Invalid TO node id."; return result; } // validate TO
            NodeId to(toVal);                     // wrap raw id into NodeId
            std::vector<Edge> filtered;           // temporary container for filtered edges
            for (const Edge& e : found) {         // iterate edges returned by Graph
                if (e.to == to || e.from == to) filtered.push_back(e); // keep edges that have the target endpoint
            }
            found = filtered;                     // replace found with filtered list
        }

        result.edges = found;                     // set result edges
        result.success = true;
        applyLimit(result.edges);                 // apply TOP <n> row cap
        result.message = "Found " + std::to_string(result.edges.size()) + " row(s).";
        return result;                            // done with FROM-based SELECT
    }

    // LABEL-only query: WHERE LABEL = '...'
    if (field.count("LABEL")) {
        graph.FindEdgesByLabel(found, field["LABEL"], warning); // fetch edges by label
        if (warning != operationSuccessful) {      // handle graph error
            result.success = false;
            result.message = "No edges found with that label.";
            return result;
        }
        result.edges = found;                     // set result edges
        result.success = true;
        applyLimit(result.edges);                 // apply TOP <n> row cap
        result.message = "Found " + std::to_string(result.edges.size()) + " row(s).";
        return result;
    }

    // If none of the supported WHERE forms were present return an error message.
    result.success = false;
    result.message = "SELECT FROM EDGES requires WHERE ID = ..., WHERE LABEL = ..., or WHERE FROM = ....";
    return result;
}

// ----------------------- SELECT ... SNAPSHOT execution -----------------------
//
// These resolve the query against the graph's MVCC history at a fixed
// point-in-time version, so the result is unaffected by inserts or deletes
// applied after the snapshot was captured. The visible set is materialized once
// via Graph::GetNodesAtVersion / GetEdgesAtVersion, then filtered in-process by
// the parsed WHERE conditions. Because the set is already in hand, a SELECT with
// no WHERE is permitted here (a full point-in-time scan) -- unlike the live
// path, which requires an ID or LABEL predicate.

// Execute SELECT ... SNAPSHOT targeting nodes.
QueryResult Query::executeSelectNodesSnapshot(Graph& graph, uint64_t snapshotVersion) const {
    QueryResult result;

    // Materialize the nodes visible at the snapshot version (insertion order).
    std::vector<Node> visible;
    graph.GetNodesAtVersion(visible, snapshotVersion);

    // Partition conditions: an optional ID equality, an optional LABEL equality,
    // and any remaining property filters. All are applied against the frozen set.
    bool haveIdFilter = false;
    uint64_t idFilter = 0;
    bool haveLabelFilter = false;
    std::string labelFilter;
    std::vector<Condition> propFilters;

    for (const Condition& c : conditions_) {
        const std::string p = toUpper(c.property);
        if (p == "ID" && c.op == "=") {
            uint64_t idVal;
            if (!parseUInt(c.value, idVal)) {
                result.success = false; result.message = "Invalid node id."; return result;
            }
            haveIdFilter = true; idFilter = idVal;
        }
        else if (p == "LABEL" && c.op == "=") {
            haveLabelFilter = true; labelFilter = c.value;
        }
        else {
            propFilters.push_back(c);   // property predicate, applied client-side
        }
    }

    // Apply the filters to each visible node.
    for (const Node& node : visible) {
        if (haveIdFilter && node.id.value() != idFilter) continue;
        if (haveLabelFilter && node.label != labelFilter) continue;

        bool matches = true;
        for (const Condition& e : propFilters) {
            auto it = node.properties.find(e.property);
            std::string actual = (it != node.properties.end()) ? it->second : "";
            bool condResult = executeCondition(e, actual, matches);
            if (!condResult) { matches = false; break; }
        }
        if (matches) result.nodes.push_back(node);
    }

    result.success = true;
    projectNodes(result.nodes);              // trim to selected columns (no-op for '*')
    applyLimit(result.nodes);                // apply TOP <n> row cap
    result.message = (result.nodes.size() == 1)
        ? "Found 1 row."
        : "Found " + std::to_string(result.nodes.size()) + " row(s).";
    return result;
}

// Execute SELECT ... SNAPSHOT targeting edges.
QueryResult Query::executeSelectEdgesSnapshot(Graph& graph, uint64_t snapshotVersion) const {
    QueryResult result;

    // Materialize edges visible at the snapshot version.
    std::vector<Edge> visible;
    graph.GetEdgesAtVersion(visible, snapshotVersion);

    // Collect equality predicates into a field map (same shape as the live path).
    std::unordered_map<std::string, std::string> field;
    for (const Condition& c : conditions_) {
        if (c.op == "=") field[toUpper(c.property)] = c.value;
    }

    // Optional endpoint / direction filters.
    bool haveId = field.count("ID") != 0;
    uint64_t idVal = 0;
    if (haveId && !parseUInt(field["ID"], idVal)) {
        result.success = false; result.message = "Invalid edge id."; return result;
    }

    bool haveFrom = field.count("FROM") != 0;
    uint64_t fromVal = 0;
    if (haveFrom && !parseUInt(field["FROM"], fromVal)) {
        result.success = false; result.message = "Invalid FROM node id."; return result;
    }

    bool haveTo = field.count("TO") != 0;
    uint64_t toVal = 0;
    if (haveTo && !parseUInt(field["TO"], toVal)) {
        result.success = false; result.message = "Invalid TO node id."; return result;
    }

    bool haveLabel = field.count("LABEL") != 0;
    std::string labelVal = haveLabel ? field["LABEL"] : "";

    EdgeDirection dir = OUTGOING;
    if (field.count("DIRECTION")) {
        std::string d = toUpper(field["DIRECTION"]);
        if (d == "OUTGOING") dir = OUTGOING;
        else if (d == "INCOMING") dir = INCOMING;
        else if (d == "BOTH") dir = BOTH;
        else { result.success = false; result.message = "Invalid DIRECTION (expected OUTGOING, INCOMING, or BOTH)."; return result; }
    }

    for (const Edge& e : visible) {
        if (haveId && e.id.value() != idVal) continue;
        if (haveLabel && e.label != labelVal) continue;
        // FROM with a direction: match the requested endpoint role.
        if (haveFrom) {
            NodeId anchor(fromVal);
            bool ok = false;
            if (dir == OUTGOING)      ok = (e.from == anchor);
            else if (dir == INCOMING) ok = (e.to == anchor);
            else                      ok = (e.from == anchor || e.to == anchor);
            if (!ok) continue;
        }
        if (haveTo) {
            NodeId target(toVal);
            if (!(e.to == target || e.from == target)) continue;
        }
        result.edges.push_back(e);
    }

    result.success = true;
    applyLimit(result.edges);                // apply TOP <n> row cap
    result.message = "Found " + std::to_string(result.edges.size()) + " row(s).";
    return result;
}

// --------------------------- INSERT / DELETE ---------------------------------

// Execute INSERT INTO NODES: uses values_ to create a Node via Graph::CreateNode.
QueryResult Query::executeInsertNodes(Graph& graph) const {
    QueryResult result;                           // result container

    propertiesMap properties = values_;           // copy parsed values
    std::string label = properties.at("label");   // extract 'label' column (throws if missing)
    properties.erase("label");                    // remove label from properties map

    NodeId id = graph.CreateNode(label, properties); // create node and obtain NodeId

    Node node;                                    // temporary to re-read inserted node
    if (graph.GetNode(id, node)) {                // read back created node to include in result
        result.nodes.push_back(node);
        result.success = true;
        result.message = "Inserted node.";
    }
    else {
        result.success = false;
        result.message = "Node insert reported success but could not be re-read.";
    }
    return result;                                // return insert result
}

// Execute INSERT INTO EDGES: parse numeric from/to and call Graph::CreateEdge.
QueryResult Query::executeInsertEdges(Graph& graph) const {
    QueryResult result;                           // result container

    uint64_t fromVal, toVal;                      // raw numeric endpoints
    if (!parseUInt(values_.at("from"), fromVal) || !parseUInt(values_.at("to"), toVal)) { // validate both
        result.success = false;
        result.message = "INSERT INTO EDGES: 'from' and 'to' must be numeric node ids.";
        return result;
    }

    int warning = operationSuccessful;            // Graph returns a warning code
    EdgeId id = graph.CreateEdge(NodeId(fromVal), NodeId(toVal), values_.at("label"), warning); // create edge

    if (warning != operationSuccessful) {         // if creation failed because endpoints missing
        result.success = false;
        result.message = "INSERT INTO EDGES failed: one or both endpoint nodes do not exist.";
        return result;
    }

    Edge edge;                                    // read back created edge for result payload
    if (graph.GetEdge(id, edge)) {
        result.edges.push_back(edge);
        result.success = true;
        result.message = "Inserted edge.";
    }
    else {
        result.success = false;
        result.message = "Edge insert reported success but could not be re-read.";
    }
    return result;                                // return insertion result
}

// Execute DELETE FROM NODES WHERE ID = <id>
QueryResult Query::executeDeleteNodes(Graph& graph) const {
    QueryResult result;                           // result container
    uint64_t idVal;                               // raw id storage
    if (!parseUInt(conditions_[0].value, idVal)) { result.success = false; result.message = "Invalid node id."; return result; } // validate id

    int warning = operationSuccessful;            // Graph uses warning codes
    graph.DeleteNode(NodeId(idVal), warning);     // attempt delete

    result.success = (warning == operationSuccessful); // success if graph reported operationSuccessful
    result.message = result.success ? "Deleted node." : "DELETE failed: node does not exist.";
    return result;                                // return deletion result
}

// Execute DELETE FROM EDGES WHERE ID = <id>
QueryResult Query::executeDeleteEdges(Graph& graph) const {
    QueryResult result;                           // result container
    uint64_t idVal;                               // raw id storage
    if (!parseUInt(conditions_[0].value, idVal)) { result.success = false; result.message = "Invalid edge id."; return result; } // validate id

    int warning = operationSuccessful;            // Graph APIs use warning codes
    graph.DeleteEdge(EdgeId(idVal), warning);     // attempt delete

    result.success = (warning == operationSuccessful); // success if graph reported operationSuccessful
    result.message = result.success ? "Deleted edge." : "DELETE failed: edge does not exist.";
    return result;                                // return deletion result
}

// --------------------------- MATCH (traversals) -------------------------------

// Execute MATCH queries (REACHABLE, SHORTEST_PATH, KHOP) by invoking BFS_Searcher.
QueryResult Query::executeMatch(Graph& graph) const {
    QueryResult result;                           // result container

    NodeId source(matchFromRaw_);                 // wrap raw numeric source into NodeId

    Node sourceNode;                              // storage for verifying source exists
    if (!graph.GetNode(source, sourceNode)) {     // verify source node exists
        result.success = false;
        result.message = "MATCH: source node does not exist.";
        return result;
    }

    BFS_Searcher searcher(graph, source);         // construct BFS searcher with graph and source
    searcher.Run_BFS(BOTH);                       // execute BFS across both directions by default

    if (matchMode_ == CSR_Mode::REACHABLE) {      // REACHABLE: return visited node order
        std::vector<NodeId> visitOrder;           // container for visit order
        searcher.GetVisitedNodeIdOrder(visitOrder); // obtain visit order from searcher
        result.traversalResult = visitOrder;      // populate result
        result.success = true;
        applyLimit(result.traversalResult);       // apply TOP <n> row cap
        result.message = "Found " + std::to_string(result.traversalResult.size()) + " reachable node(s).";
        return result;
    }

    if (matchMode_ == CSR_Mode::SHORTEST_PATH) {  // SHORTEST_PATH: return node path to target
        NodeId target(matchToRaw_);               // wrap target id
        if (!searcher.HasVisited(target)) {       // if target not visited there is no path
            result.success = true;
            result.traversalResult.clear();
            result.message = "No path found between the given nodes.";
            return result;
        }
        std::vector<NodeId> path;                 // container for path
        searcher.GetShortestPath(target, path);   // ask searcher for shortest path back to source
        result.traversalResult = path;            // set result path
        result.success = true;
        applyLimit(result.traversalResult);       // apply TOP <n> (caps returned path length)
        result.message = "Path found with " + std::to_string(result.traversalResult.size()) + " node(s).";
        return result;
    }

    // KHOP: include nodes whose parent-chain distance to source is <= matchK_
    std::vector<NodeId> visitOrder;              // get visit order for walking parents
    searcher.GetVisitedNodeIdOrder(visitOrder);   // populate visitOrder

    for (const NodeId& node : visitOrder) {      // iterate all nodes visited by BFS
        size_t hops = 0;                         // count hops from node back to source
        NodeId current = node;                   // current node in the parent chain
        bool withinRange = false;                // whether node is within matchK_ hops
        while (true) {                           // walk parent chain
            if (current == source) { withinRange = (hops <= matchK_); break; } // reached source, check hop count
            NodeId parent = searcher.GetParentOfVisited(current); // get parent from searcher
            if (parent == NodeIdInvalid) break; // safety: parent invalid means no path (shouldn't happen)
            current = parent;                    // step to parent
            ++hops;                              // increment hop counter
            if (hops > matchK_) break;           // early out if hops exceed matchK_
        }
        if (withinRange) result.traversalResult.push_back(node); // keep nodes within range
    }

    result.success = true;                        // match executed successfully
    applyLimit(result.traversalResult);           // apply TOP <n> row cap
    result.message = "Found " + std::to_string(result.traversalResult.size()) + " node(s) within " +
        std::to_string(matchK_) + " hop(s)."; // human-readable summary
    return result;                                // return final result
}

// --------------------------- MATCH over a CSR snapshot ------------------------

// Execute MATCH queries against a frozen CSR snapshot using CSR_Searcher. This
// mirrors executeMatch()'s result shape and messages exactly, so a query
// returns the same kind of QueryResult whether it ran LIVE (BFS) or SNAPSHOT
// (CSR) -- only the engine and the point-in-time it observes differ.
QueryResult Query::executeMatchCSR(CSR_Representation& snapshot) const {
    QueryResult result;                           // result container

    NodeId source(matchFromRaw_);                 // wrap raw numeric source into NodeId

    // Verify the source exists in the snapshot's CSR index space. A node that
    // was created after the snapshot was built simply isn't mapped.
    int mapWarning = operationSuccessful;
    snapshot.MapGraphNodeToCSR(source, mapWarning);
    if (mapWarning != operationSuccessful) {
        result.success = false;
        result.message = "MATCH: source node does not exist.";
        return result;
    }

    // For SHORTEST_PATH, guard the target too. CSR_Searcher maps a missing
    // target to CSR index 0, which would otherwise be misread as "found"; we
    // treat an unmapped target as simply unreachable, matching the BFS path.
    if (matchMode_ == CSR_Mode::SHORTEST_PATH) {
        int targetWarning = operationSuccessful;
        snapshot.MapGraphNodeToCSR(NodeId(matchToRaw_), targetWarning);
        if (targetWarning != operationSuccessful) {
            result.success = true;
            result.traversalResult.clear();
            result.message = "No path found between the given nodes.";
            return result;
        }
    }

    // Run the CSR traversal. target is used for SHORTEST_PATH; matchK_ for KHOP.
    CSR_Searcher searcher(source, snapshot);
    searcher.Run(NodeId(matchToRaw_), matchMode_, matchK_);
    result.traversalResult = searcher.GetResult();
    applyLimit(result.traversalResult);           // apply TOP <n> row cap (uniform with BFS)

    // Compose the mode-specific success message, identical to the BFS variant.
    if (matchMode_ == CSR_Mode::REACHABLE) {
        result.success = true;
        result.message = "Found " + std::to_string(result.traversalResult.size()) + " reachable node(s).";
        return result;
    }

    if (matchMode_ == CSR_Mode::SHORTEST_PATH) {
        result.success = true;
        if (result.traversalResult.empty())
            result.message = "No path found between the given nodes.";
        else
            result.message = "Path found with " + std::to_string(result.traversalResult.size()) + " node(s).";
        return result;
    }

    // KHOP
    result.success = true;
    result.message = "Found " + std::to_string(result.traversalResult.size()) + " node(s) within " +
        std::to_string(matchK_) + " hop(s).";
    return result;
}
// --------------------------- UPDATE / LOAD / SAVE -----------------------------

// parseUpdate: UPDATE NODES|EDGES WHERE <conditions> SET key=value[, key=value ...]
// The WHERE region runs from just after the target up to (not including) SET;
// the SET region is everything after SET.
bool Query::parseUpdate(const std::vector<std::string>& tokens, size_t pos) {
    if (pos >= tokens.size() || !parseTarget(tokens[pos], target_)) {
        setError("UPDATE: expected NODES or EDGES.");
        return false;
    }
    ++pos;                                        // advance past target

    if (pos >= tokens.size() || toUpper(tokens[pos]) != "WHERE") {
        setError("UPDATE: a WHERE clause is required.");
        return false;
    }
    ++pos;                                        // first WHERE condition token
    const size_t whereStart = pos;

    // find the SET keyword that terminates the WHERE region
    size_t setPos = whereStart;
    while (setPos < tokens.size() && toUpper(tokens[setPos]) != "SET") ++setPos;
    if (setPos >= tokens.size()) {
        setError("UPDATE: a SET clause is required.");
        return false;
    }
    if (setPos == whereStart) {
        setError("WHERE: no conditions found.");
        return false;
    }

    // parse WHERE from the slice [whereStart, setPos)
    std::vector<std::string> whereTokens(tokens.begin() + whereStart, tokens.begin() + setPos);
    if (!parseWhereClause(whereTokens, 0)) return false;

    // parse the SET assignments after SET
    if (!parseSetClause(tokens, setPos + 1)) return false;

    return true;
}

// parseSetClause: key=value[, key=value ...] from tokens[pos]. The tokenizer
// splits on '=' and ',', so each assignment arrives as <key> '=' <value>.
bool Query::parseSetClause(const std::vector<std::string>& tokens, size_t pos) {
    values_.clear();
    if (pos >= tokens.size()) {
        setError("SET: expected at least one key=value assignment.");
        return false;
    }

    bool expectPair = true;
    while (pos < tokens.size()) {
        if (!expectPair) {
            if (tokens[pos] != ",") { setError("SET: expected ',' between assignments."); return false; }
            ++pos;
        }
        if (pos >= tokens.size() || tokens[pos] == "," || tokens[pos] == "=") {
            setError("SET: expected a column name."); return false;
        }
        std::string key = tokens[pos];
        ++pos;
        if (pos >= tokens.size() || tokens[pos] != "=") {
            setError("SET: expected '=' after column name."); return false;
        }
        ++pos;
        if (pos >= tokens.size() || tokens[pos] == "," || tokens[pos] == "=") {
            setError("SET: expected a value after '='."); return false;
        }
        values_[key] = stripQuotes(tokens[pos]);
        ++pos;
        expectPair = false;
    }
    if (values_.empty()) {
        setError("SET: expected at least one key=value assignment.");
        return false;
    }
    return true;
}

// parseLoad: LOAD FILE '<path>'
bool Query::parseLoad(const std::vector<std::string>& tokens, size_t pos) {
    if (pos >= tokens.size() || toUpper(tokens[pos]) != "FILE") {
        setError("LOAD: expected FILE (syntax: LOAD FILE '<path>')."); return false;
    }
    ++pos;
    if (pos >= tokens.size()) { setError("LOAD FILE: expected a file path."); return false; }
    filePath_ = stripQuotes(tokens[pos]);
    ++pos;
    if (filePath_.empty()) { setError("LOAD FILE: file path cannot be empty."); return false; }
    if (pos != tokens.size()) { setError("LOAD FILE: unexpected trailing tokens."); return false; }
    return true;
}

// parseSave: SAVE FILE '<path>'
bool Query::parseSave(const std::vector<std::string>& tokens, size_t pos) {
    if (pos >= tokens.size() || toUpper(tokens[pos]) != "FILE") {
        setError("SAVE: expected FILE (syntax: SAVE FILE '<path>')."); return false;
    }
    ++pos;
    if (pos >= tokens.size()) { setError("SAVE FILE: expected a file path."); return false; }
    filePath_ = stripQuotes(tokens[pos]);
    ++pos;
    if (filePath_.empty()) { setError("SAVE FILE: file path cannot be empty."); return false; }
    if (pos != tokens.size()) { setError("SAVE FILE: unexpected trailing tokens."); return false; }
    return true;
}

// updateUpper: uppercase a property name for reserved-word comparison (ID/LABEL).
static std::string updateUpper(const std::string& s) {
    std::string out(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i)
        out[i] = static_cast<char>(std::toupper((unsigned char)s[i]));
    return out;
}

// updateParseUInt: parse a base-10 unsigned integer (file-local).
static bool updateParseUInt(const std::string& token, uint64_t& out) {
    if (token.empty()) return false;
    for (char c : token)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = std::strtoull(token.c_str(), nullptr, 10);
    return true;
}

// nodeMatchesConditions: evaluate parsed WHERE conditions against one node.
// Supports ID (=, !=), LABEL (=, !=), and property comparisons (all operators),
// combined with AND.
static bool nodeMatchesConditions(const Node& node, const std::vector<Condition>& conditions) {
    for (const Condition& c : conditions) {
        const std::string upper = updateUpper(c.property);
        bool ok = false;
        if (upper == "ID") {
            uint64_t idVal = 0;
            if (!updateParseUInt(c.value, idVal)) return false;
            bool eq = (node.id == NodeId(idVal));
            if (c.op == "=") ok = eq; else if (c.op == "!=") ok = !eq; else ok = false;
        } else if (upper == "LABEL") {
            if (c.op == "=") ok = (node.label == c.value);
            else if (c.op == "!=") ok = (node.label != c.value);
            else ok = false;
        } else {
            auto it = node.properties.find(c.property);
            std::string actual = (it != node.properties.end()) ? it->second : "";
            if (c.op == "=") ok = (actual == c.value);
            else if (c.op == "!=") ok = (actual != c.value);
            else if (c.op == "<") ok = (actual < c.value);
            else if (c.op == ">") ok = (actual > c.value);
            else if (c.op == "<=") ok = (actual <= c.value);
            else if (c.op == ">=") ok = (actual >= c.value);
            else ok = false;
        }
        if (!ok) return false;
    }
    return true;
}

// edgeMatchesConditions: ID and LABEL (= / !=) for edges; properties never match.
static bool edgeMatchesConditions(const Edge& edge, const std::vector<Condition>& conditions) {
    for (const Condition& c : conditions) {
        const std::string upper = updateUpper(c.property);
        bool ok = false;
        if (upper == "ID") {
            uint64_t idVal = 0;
            if (!updateParseUInt(c.value, idVal)) return false;
            bool eq = (edge.id == EdgeId(idVal));
            if (c.op == "=") ok = eq; else if (c.op == "!=") ok = !eq; else ok = false;
        } else if (upper == "LABEL") {
            if (c.op == "=") ok = (edge.label == c.value);
            else if (c.op == "!=") ok = (edge.label != c.value);
            else ok = false;
        } else {
            return false;
        }
        if (!ok) return false;
    }
    return true;
}

// executeUpdateNodes: apply SET assignments to every node matching WHERE.
QueryResult Query::executeUpdateNodes(Graph& graph) const {
    QueryResult result;
    if (conditions_.empty()) {
        result.success = false;
        result.message = "UPDATE NODES requires a WHERE clause.";
        return result;
    }
    std::vector<NodeId> ids;
    graph.GetNodeIdOrder(ids);
    size_t updated = 0;
    for (NodeId id : ids) {
        Node node;
        if (!graph.GetNode(id, node)) continue;
        if (!nodeMatchesConditions(node, conditions_)) continue;
        int warning = operationSuccessful;
        graph.UpdateNodeProperties(id, values_, warning);
        if (warning == operationSuccessful) ++updated;
    }
    result.success = true;
    result.message = "Updated " + std::to_string(updated) + " node(s).";
    return result;
}

// executeUpdateEdges: change the label of every edge matching WHERE. Only the
// 'label' column may be assigned.
QueryResult Query::executeUpdateEdges(Graph& graph) const {
    QueryResult result;
    if (conditions_.empty()) {
        result.success = false;
        result.message = "UPDATE EDGES requires a WHERE clause.";
        return result;
    }
    for (const auto& kv : values_) {
        if (kv.first != "label") {
            result.success = false;
            result.message = "UPDATE EDGES: only the 'label' column can be updated.";
            return result;
        }
    }
    auto labelIt = values_.find("label");
    if (labelIt == values_.end()) {
        result.success = false;
        result.message = "UPDATE EDGES: SET label = <value> is required.";
        return result;
    }
    std::vector<EdgeId> ids;
    graph.GetAllEdgeIds(ids);
    size_t updated = 0;
    for (EdgeId id : ids) {
        Edge edge;
        if (!graph.GetEdge(id, edge)) continue;
        if (!edgeMatchesConditions(edge, conditions_)) continue;
        int warning = operationSuccessful;
        graph.UpdateEdgeLabel(id, labelIt->second, warning);
        if (warning == operationSuccessful) ++updated;
    }
    result.success = true;
    result.message = "Updated " + std::to_string(updated) + " edge(s).";
    return result;
}
