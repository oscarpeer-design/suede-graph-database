#include "test_graph_suite.h"

#include "Graph.h"
#include "Query.h"
#include "StorageEngine.h"
#include "CSR_Representation.h"
#include "GraphHandler.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <thread>
#include <functional>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <algorithm>

// Test suite covering: GetEdges (via FindEdgesByNodeId / FindEdgesByNodeAndLabel),
// and GetNeighbours (via GetNeighboursById) across every direction, for nodes
// that have edges in both/one/neither direction.
//
// The bug class under test: an unordered_map adjacency list only gains a key
// for a node once CreateEdge pushes into it. A node that legitimately has zero
// edges in a given direction has no key at all -- that must be treated as a
// valid empty result, not an error.

static int pass = 0, fail = 0;

static void check(const std::string& name, bool condition) {
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << name << "\n";
    if (condition) ++pass; else ++fail;
}

int test_graph() {
    Graph graph;

    // a -> b (FRIENDS). a has outgoing only, b has incoming only.
    // c has no edges at all.
    NodeId a = graph.CreateNode("Person", { {"name", "Alice"} });
    NodeId b = graph.CreateNode("Person", { {"name", "Bob"} });
    NodeId c = graph.CreateNode("Person", { {"name", "Carol"} });

    int warning = operationSuccessful;
    graph.CreateEdge(a, b, "FRIENDS", warning);
    assert(warning == operationSuccessful);

    std::vector<Edge> edges;
    std::vector<Node> neighbours;

    std::cout << "== FindEdgesByNodeId / GetEdges ==\n";

    // --- BOTH direction ---
    graph.FindEdgesByNodeId(a, edges, BOTH, warning);
    check("a BOTH: success", warning == operationSuccessful);
    check("a BOTH: count == 1 (outgoing only)", edges.size() == 1);

    graph.FindEdgesByNodeId(b, edges, BOTH, warning);
    check("b BOTH: success", warning == operationSuccessful);
    check("b BOTH: count == 1 (incoming only)", edges.size() == 1);

    graph.FindEdgesByNodeId(c, edges, BOTH, warning);
    check("c BOTH (no edges at all): success", warning == operationSuccessful);
    check("c BOTH (no edges at all): count == 0", edges.size() == 0);

    // --- single direction: the node HAS edges in that direction ---
    graph.FindEdgesByNodeId(a, edges, OUTGOING, warning);
    check("a OUTGOING (has 1): success", warning == operationSuccessful);
    check("a OUTGOING (has 1): count == 1", edges.size() == 1);

    graph.FindEdgesByNodeId(b, edges, INCOMING, warning);
    check("b INCOMING (has 1): success", warning == operationSuccessful);
    check("b INCOMING (has 1): count == 1", edges.size() == 1);

    // --- single direction: the node has ZERO edges in that direction (the core bug) ---
    graph.FindEdgesByNodeId(b, edges, OUTGOING, warning);
    check("b OUTGOING (has 0): success", warning == operationSuccessful);
    check("b OUTGOING (has 0): count == 0", edges.size() == 0);

    graph.FindEdgesByNodeId(a, edges, INCOMING, warning);
    check("a INCOMING (has 0): success", warning == operationSuccessful);
    check("a INCOMING (has 0): count == 0", edges.size() == 0);

    graph.FindEdgesByNodeId(c, edges, OUTGOING, warning);
    check("c OUTGOING (never indexed): success", warning == operationSuccessful);
    check("c OUTGOING (never indexed): count == 0", edges.size() == 0);

    graph.FindEdgesByNodeId(c, edges, INCOMING, warning);
    check("c INCOMING (never indexed): success", warning == operationSuccessful);
    check("c INCOMING (never indexed): count == 0", edges.size() == 0);

    std::cout << "\n== FindEdgesByNodeAndLabel ==\n";

    graph.FindEdgesByNodeAndLabel(a, "FRIENDS", edges, BOTH, warning);
    check("a BOTH label=FRIENDS: success", warning == operationSuccessful);
    check("a BOTH label=FRIENDS: count == 1", edges.size() == 1);

    graph.FindEdgesByNodeAndLabel(b, "FRIENDS", edges, OUTGOING, warning);
    check("b OUTGOING label=FRIENDS (has 0 outgoing): success", warning == operationSuccessful);
    check("b OUTGOING label=FRIENDS (has 0 outgoing): count == 0", edges.size() == 0);

    graph.FindEdgesByNodeAndLabel(c, "FRIENDS", edges, BOTH, warning);
    check("c BOTH label=FRIENDS (no edges at all): success", warning == operationSuccessful);
    check("c BOTH label=FRIENDS (no edges at all): count == 0", edges.size() == 0);

    std::cout << "\n== GetNeighboursById / GetNeighbours / GetEdgeIds ==\n";

    graph.GetNeighboursById(a, neighbours, BOTH, warning);
    check("a neighbours BOTH: success", warning == operationSuccessful);
    check("a neighbours BOTH: count == 1", neighbours.size() == 1);

    graph.GetNeighboursById(b, neighbours, OUTGOING, warning);
    check("b neighbours OUTGOING (has 0 outgoing): success", warning == operationSuccessful);
    check("b neighbours OUTGOING (has 0 outgoing): count == 0", neighbours.size() == 0);

    graph.GetNeighboursById(c, neighbours, BOTH, warning);
    check("c neighbours BOTH (no edges at all): success", warning == operationSuccessful);
    check("c neighbours BOTH (no edges at all): count == 0", neighbours.size() == 0);

    graph.GetNeighboursById(a, neighbours, INCOMING, warning);
    check("a neighbours INCOMING (has 0 incoming): success", warning == operationSuccessful);
    check("a neighbours INCOMING (has 0 incoming): count == 0", neighbours.size() == 0);

    std::cout << "\n== Regression: genuinely nonexistent edge label / edge id still errors ==\n";

    graph.FindEdgesByLabel(edges, "NO_SUCH_LABEL", warning);
    check("FindEdgesByLabel unknown label: still errors", warning != operationSuccessful);

    Edge e;
    check("GetEdge invalid id: still returns false", !graph.GetEdge(EdgeId(9999), e));

    std::cout << "\n" << pass << " passed, " << fail << " failed.\n";
    return fail == 0 ? 0 : 1;
}

// =====================================================================
// Query layer test suite
// =====================================================================
//
// This suite drives the public surface of the Query class end-to-end:
//
//   1. Parsing  -- Query::parse() for every statement kind (SELECT, INSERT,
//      DELETE, MATCH) plus the shared WHERE-clause grammar. Both the happy
//      path and the rejection path (malformed statements) are asserted so
//      that the tokenizer and each clause parser are covered.
//
//   2. Execution -- Query::run() (parse + execute in one call) against a
//      small, deterministic fixture graph. Node ids and edge ids are
//      sequential starting at 1 (see Graph::CreateNode / CreateEdge), so the
//      expected ids below are known ahead of time.
//
// Ordering note: the read-only SELECT and MATCH checks run first, then the
// mutating INSERT checks, then the DELETE checks last, so that earlier
// assertions are never disturbed by later graph mutations.

// Small helper: build the fixture graph shared by the execution tests.
//
// Nodes (ids are assigned sequentially by the Graph):
//   1: Person { name=Alice, age=30 }
//   2: Person { name=Bob,   age=25 }
//   3: City   { name=Sydney }
// Edges:
//   1: 1 --KNOWS-->    2
//   2: 2 --LIVES_IN--> 3
static void buildQueryFixture(Graph& graph) {
    NodeId alice = graph.CreateNode("Person", { {"name", "Alice"}, {"age", "30"} });
    NodeId bob = graph.CreateNode("Person", { {"name", "Bob"},   {"age", "25"} });
    NodeId city = graph.CreateNode("City", { {"name", "Sydney"} });

    int warning = operationSuccessful;
    graph.CreateEdge(alice, bob, "KNOWS", warning);
    graph.CreateEdge(bob, city, "LIVES_IN", warning);
}

// The query-layer suite is split into two helpers (part1 / part2) so that no
// single function accumulates a huge stack frame from all the per-block Graph /
// CSR_Representation locals -- that summed frame tripped a compiler stack-size
// warning. The pass/fail counters are file-static, so the split is transparent;
// test_query_layer() below simply runs both halves.
static int test_query_layer_part1() {

    // -----------------------------------------------------------------
    // 1. PARSING -- statements that must parse successfully
    // -----------------------------------------------------------------
    std::cout << "\n== Parsing: well-formed statements accepted ==\n";
    {
        Query q;
        check("parse SELECT by id", q.parse("SELECT * FROM NODES WHERE ID = 1"));
        check("parse SELECT by label", q.parse("SELECT * FROM NODES WHERE LABEL = 'Person'"));
        check("parse SELECT with AND", q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' AND age > 20"));
        check("parse SELECT with OR", q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' OR LABEL = 'City'"));
        check("parse SELECT with AND + OR mix", q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' AND age > '20' OR LABEL = 'City'"));
        check("parse SELECT COUNT", q.parse("SELECT COUNT * FROM NODES WHERE LABEL = 'Person'") && q.isCount());
        check("parse SELECT COUNT no WHERE", q.parse("SELECT COUNT * FROM NODES") && q.isCount());
        check("parse SELECT (not count) reports isCount=false",
            q.parse("SELECT * FROM NODES WHERE ID = 1") && !q.isCount());
        check("parse SELECT edges by from", q.parse("SELECT * FROM EDGES WHERE FROM = 1"));
        check("parse INSERT nodes", q.parse("INSERT INTO NODES (label, name) VALUES ('Person', 'Zed')"));
        check("parse INSERT edges", q.parse("INSERT INTO EDGES (from, to, label) VALUES (1, 2, 'KNOWS')"));
        check("parse DELETE nodes", q.parse("DELETE FROM NODES WHERE ID = 1"));
        check("parse DELETE edges", q.parse("DELETE FROM EDGES WHERE ID = 1"));
        check("parse MATCH reachable", q.parse("MATCH REACHABLE FROM 1"));
        check("parse MATCH shortest_path", q.parse("MATCH SHORTEST_PATH FROM 1 TO 3"));
        check("parse MATCH khop", q.parse("MATCH KHOP FROM 1 STEPS 2"));
        // keywords are case-insensitive (toUpper on the first token / clauses)
        check("parse is case-insensitive", q.parse("select * from nodes where id = 1"));
    }

    // -----------------------------------------------------------------
    // 1b. PARSING -- statements that must be rejected
    // -----------------------------------------------------------------
    std::cout << "\n== Parsing: malformed statements rejected ==\n";
    {
        Query q;
        check("reject empty query", !q.parse(""));
        check("reject unknown keyword", !q.parse("FROBNICATE NODES"));
        check("reject SELECT with empty projection", !q.parse("SELECT FROM NODES WHERE ID = 1"));
        check("reject SELECT without WHERE", !q.parse("SELECT * FROM NODES"));
        check("reject SELECT bad target", !q.parse("SELECT * FROM PLANETS WHERE ID = 1"));
        check("reject INSERT NODES no label", !q.parse("INSERT INTO NODES (name) VALUES ('Zed')"));
        check("reject INSERT count mismatch", !q.parse("INSERT INTO NODES (label, name) VALUES ('Person')"));
        check("reject INSERT EDGES missing col", !q.parse("INSERT INTO EDGES (from, to) VALUES (1, 2)"));
        check("reject INSERT EDGES extra col", !q.parse("INSERT INTO EDGES (from, to, label, weight) VALUES (1, 2, 'K', 5)"));
        check("reject DELETE non-id filter", !q.parse("DELETE FROM NODES WHERE LABEL = 'Person'"));
        check("reject DELETE without WHERE", !q.parse("DELETE FROM NODES"));
        check("reject MATCH unknown mode", !q.parse("MATCH WARP FROM 1"));
        check("reject MATCH KHOP without STEPS", !q.parse("MATCH KHOP FROM 1"));
        check("reject MATCH non-numeric source", !q.parse("MATCH REACHABLE FROM abc"));
        check("reject WHERE invalid operator", !q.parse("SELECT * FROM NODES WHERE ID ~ 1"));
        // OR is now supported, but an unknown connector (not AND/OR) is still rejected.
        check("reject WHERE bad connector", !q.parse("SELECT * FROM NODES WHERE age > 20 XOR age < 10"));
        // COUNT must be followed by '*'.
        check("reject SELECT COUNT without star", !q.parse("SELECT COUNT name FROM NODES WHERE LABEL = 'Person'"));
        // A WHERE-less non-count live SELECT is still rejected (index-driven path).
        check("reject SELECT without WHERE (non-count)", !q.parse("SELECT * FROM NODES"));
        check("reject trailing tokens", !q.parse("MATCH REACHABLE FROM 1 EXTRA"));
    }

    // -----------------------------------------------------------------
    // 2. EXECUTION -- SELECT FROM NODES
    // -----------------------------------------------------------------
    std::cout << "\n== Execute: SELECT FROM NODES ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        r = q.run("SELECT * FROM NODES WHERE ID = 1", graph);
        check("select node by id: success", r.success);
        check("select node by id: 1 row", r.nodes.size() == 1);
        check("select node by id: correct id", r.nodes.size() == 1 && r.nodes[0].id == NodeId(1));

        r = q.run("SELECT * FROM NODES WHERE ID = 999", graph);
        check("select missing node id: success (no rows)", r.success);
        check("select missing node id: 0 rows", r.nodes.size() == 0);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person'", graph);
        check("select nodes by label: success", r.success);
        check("select nodes by label: 2 rows", r.nodes.size() == 2);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Ghost'", graph);
        check("select unknown label: reports failure", !r.success);

        // Property filtering on top of a label match. Alice is age 30, Bob is 25,
        // so exactly one row should survive the `age = 30` filter.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '30'", graph);
        check("select label + property filter: success", r.success);
        check("select label + property filter: 1 row (Alice)", r.nodes.size() == 1);
    }

    // -----------------------------------------------------------------
    // 2b. EXECUTION -- SELECT FROM EDGES
    // -----------------------------------------------------------------
    std::cout << "\n== Execute: SELECT FROM EDGES ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        r = q.run("SELECT * FROM EDGES WHERE ID = 1", graph);
        check("select edge by id: success", r.success);
        check("select edge by id: 1 row", r.edges.size() == 1);
        check("select edge by id: label KNOWS", r.edges.size() == 1 && r.edges[0].label == "KNOWS");

        r = q.run("SELECT * FROM EDGES WHERE ID = 999", graph);
        check("select missing edge id: success (no rows)", r.success);
        check("select missing edge id: 0 rows", r.edges.size() == 0);

        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'KNOWS'", graph);
        check("select edges by label: success", r.success);
        check("select edges by label: 1 row", r.edges.size() == 1);

        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'NOPE'", graph);
        check("select unknown edge label: reports failure", !r.success);

        // FROM defaults to OUTGOING: node 1 has a single outgoing edge (KNOWS).
        r = q.run("SELECT * FROM EDGES WHERE FROM = 1", graph);
        check("select edges from node (outgoing default): success", r.success);
        check("select edges from node (outgoing default): 1 row", r.edges.size() == 1);

        // Node 2 has one incoming (KNOWS) and one outgoing (LIVES_IN): BOTH => 2.
        r = q.run("SELECT * FROM EDGES WHERE FROM = 2 AND DIRECTION = 'BOTH'", graph);
        check("select edges direction=BOTH: success", r.success);
        check("select edges direction=BOTH: 2 rows", r.edges.size() == 2);

        // FROM + LABEL narrows to the labelled outgoing edge.
        r = q.run("SELECT * FROM EDGES WHERE FROM = 1 AND LABEL = 'KNOWS'", graph);
        check("select edges from + label: success", r.success);
        check("select edges from + label: 1 row", r.edges.size() == 1);

        // FROM + TO keeps only edges touching the TO endpoint.
        r = q.run("SELECT * FROM EDGES WHERE FROM = 1 AND TO = 2", graph);
        check("select edges from + matching to: 1 row", r.success && r.edges.size() == 1);

        r = q.run("SELECT * FROM EDGES WHERE FROM = 1 AND TO = 3", graph);
        check("select edges from + non-matching to: success", r.success);
        check("select edges from + non-matching to: 0 rows", r.edges.size() == 0);
    }

    // -----------------------------------------------------------------
    // 2c. EXECUTION -- OR in WHERE
    // -----------------------------------------------------------------
    //
    // OR routes to a full scan with boolean evaluation (SQL precedence: AND binds
    // tighter than OR). Fixture: Alice (Person, age 30, id 1), Bob (Person, age
    // 25, id 2), Sydney (City, id 3); edges KNOWS (id 1), LIVES_IN (id 2).
    std::cout << "\n== Execute: OR in WHERE ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        // OR between two labels: Person (x2) OR City (x1) = all 3 nodes.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' OR LABEL = 'City'", graph);
        check("OR of two labels: success", r.success);
        check("OR of two labels: 3 rows", r.nodes.size() == 3);

        // OR between two ids: id 1 OR id 3 = Alice and Sydney.
        r = q.run("SELECT * FROM NODES WHERE ID = 1 OR ID = 3", graph);
        check("OR of two ids: 2 rows", r.success && r.nodes.size() == 2);

        // OR reaching an un-anchored property: LABEL = 'City' (Sydney) OR age =
        // '25' (Bob) = 2 rows. This is the case the old design forbade; it now
        // works via the full-scan fallback.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'City' OR age = '25'", graph);
        check("OR label-or-property: success (full scan)", r.success);
        check("OR label-or-property: 2 rows (Sydney, Bob)", r.nodes.size() == 2);

        // SQL precedence: LABEL='Person' AND age='30' OR LABEL='City'
        //   == (Person AND age30) OR City == Alice OR Sydney == 2 rows.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '30' OR LABEL = 'City'", graph);
        check("precedence (A AND B) OR C: success", r.success);
        check("precedence (A AND B) OR C: 2 rows (Alice, Sydney)", r.nodes.size() == 2);

        // Contrast the same conditions with AND-only: Person AND age30 = just Alice.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '30'", graph);
        check("AND-only sanity: 1 row (Alice)", r.success && r.nodes.size() == 1);

        // OR on edges: two edge labels = both edges.
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'KNOWS' OR LABEL = 'LIVES_IN'", graph);
        check("OR of two edge labels: 2 rows", r.success && r.edges.size() == 2);
    }

    // -----------------------------------------------------------------
    // 2d. EXECUTION -- COUNT
    // -----------------------------------------------------------------
    //
    // SELECT COUNT * reports the number of matching rows in the message
    // ("Count: <n>") and returns NO rows in nodes/edges. Same fixture as above.
    std::cout << "\n== Execute: SELECT COUNT ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        // COUNT with a label filter: 2 Person nodes.
        r = q.run("SELECT COUNT * FROM NODES WHERE LABEL = 'Person'", graph);
        check("count by label: success", r.success);
        check("count by label: message 'Count: 2'", r.message == "Count: 2");
        check("count by label: returns no rows", r.nodes.empty());

        // COUNT with no WHERE: counts the whole graph (3 nodes).
        r = q.run("SELECT COUNT * FROM NODES", graph);
        check("count all nodes: 'Count: 3'", r.success && r.message == "Count: 3");

        // COUNT with an OR filter: Person (2) OR City (1) = 3.
        r = q.run("SELECT COUNT * FROM NODES WHERE LABEL = 'Person' OR LABEL = 'City'", graph);
        check("count with OR: 'Count: 3'", r.success && r.message == "Count: 3");

        // COUNT by single id: 1 match.
        r = q.run("SELECT COUNT * FROM NODES WHERE ID = 1", graph);
        check("count by id (hit): 'Count: 1'", r.success && r.message == "Count: 1");

        // COUNT by a missing id: 0 matches, still a success.
        r = q.run("SELECT COUNT * FROM NODES WHERE ID = 999", graph);
        check("count by id (miss): 'Count: 0'", r.success && r.message == "Count: 0");

        // COUNT edges by label: 1 KNOWS edge.
        r = q.run("SELECT COUNT * FROM EDGES WHERE LABEL = 'KNOWS'", graph);
        check("count edges by label: 'Count: 1'", r.success && r.message == "Count: 1");

        // COUNT all edges: 2.
        r = q.run("SELECT COUNT * FROM EDGES", graph);
        check("count all edges: 'Count: 2'", r.success && r.message == "Count: 2");
    }

    // -----------------------------------------------------------------
    // 2b-ii. EXECUTION -- SELECT matching on LABELS and PROPERTIES
    // -----------------------------------------------------------------
    //
    // The fixtures above lean on ID lookups. These cases exercise the query
    // paths users actually reach for: matching NODES by label and by property
    // comparisons, and matching EDGES by label. A richer fixture is used so the
    // counts are meaningful (several nodes per label, several edges per label,
    // a node deliberately missing a property, mixed ages for comparisons).
    //
    // Fixture (ids are sequential from 1):
    //   1 Person { name=Alice, age=30, city=Sydney }
    //   2 Person { name=Bob,   age=25, city=Sydney }
    //   3 Person { name=Carol, age=40 }            <- no `city` property
    //   4 City   { name=Sydney,    population=5000000 }
    //   5 City   { name=Melbourne, population=5000000 }
    //   6 Robot  { name=R2 }                        <- unique, single-member label
    // Edges:
    //   1: 1 -KNOWS->    2      4: 2 -LIVES_IN-> 4
    //   2: 2 -KNOWS->    3      5: 3 -LIVES_IN-> 5
    //   3: 1 -LIVES_IN-> 4      6: 1 -OWNS->     6
    std::cout << "\n== Execute: SELECT NODES by label / property ==\n";
    {
        Graph graph;
        NodeId alice = graph.CreateNode("Person", { {"name","Alice"}, {"age","30"}, {"city","Sydney"} });
        NodeId bob = graph.CreateNode("Person", { {"name","Bob"},   {"age","25"}, {"city","Sydney"} });
        NodeId carol = graph.CreateNode("Person", { {"name","Carol"}, {"age","40"} });
        NodeId syd = graph.CreateNode("City", { {"name","Sydney"},    {"population","5000000"} });
        NodeId melb = graph.CreateNode("City", { {"name","Melbourne"}, {"population","5000000"} });
        NodeId r2 = graph.CreateNode("Robot", { {"name","R2"} });

        int w = operationSuccessful;
        graph.CreateEdge(alice, bob, "KNOWS", w);
        graph.CreateEdge(bob, carol, "KNOWS", w);
        graph.CreateEdge(alice, syd, "LIVES_IN", w);
        graph.CreateEdge(bob, syd, "LIVES_IN", w);
        graph.CreateEdge(carol, melb, "LIVES_IN", w);
        graph.CreateEdge(alice, r2, "OWNS", w);

        Query q;
        QueryResult r;

        // --- Node label matching (multiplicity) ---
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person'", graph);
        check("nodes label Person: success", r.success);
        check("nodes label Person: 3 rows", r.nodes.size() == 3);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'City'", graph);
        check("nodes label City: 2 rows", r.success && r.nodes.size() == 2);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Robot'", graph);
        check("nodes label Robot (single-member): 1 row", r.success && r.nodes.size() == 1);

        // A label present on a node returns whole rows with properties intact.
        check("nodes label Robot: row carries its property",
            r.nodes.size() == 1 && r.nodes[0].label == "Robot" &&
            r.nodes[0].properties.at("name") == "R2");

        // Unknown label -> reported as a failure (not an empty success).
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Ghost'", graph);
        check("nodes unknown label: reports failure", !r.success);

        // Labels are case-sensitive: 'person' is not 'Person'.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'person'", graph);
        check("nodes label is case-sensitive: 'person' fails", !r.success);

        // --- Property filtering on top of a label ---
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND name = 'Alice'", graph);
        check("nodes label + name=Alice: 1 row",
            r.success && r.nodes.size() == 1 && r.nodes[0].properties.at("name") == "Alice");

        // A property shared by several nodes narrows the label set.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND city = 'Sydney'", graph);
        check("nodes label + city=Sydney: 2 rows (Carol has no city)",
            r.success && r.nodes.size() == 2);

        // A node missing the property is treated as having the empty string,
        // so `city = ''` matches exactly the node without a city (Carol).
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND city = ''", graph);
        check("nodes label + city='' matches the property-less node: 1 row",
            r.success && r.nodes.size() == 1 && r.nodes[0].properties.at("name") == "Carol");

        // Property NAMES are case-sensitive: 'AGE' is not the stored key 'age',
        // so it reads as empty and matches nothing (but still succeeds).
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND AGE = '30'", graph);
        check("nodes property name is case-sensitive: 'AGE' matches 0 rows",
            r.success && r.nodes.size() == 0);

        // A property value that no node has -> success, zero rows.
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '99'", graph);
        check("nodes label + non-matching property: 0 rows (success)",
            r.success && r.nodes.size() == 0);

        // --- Comparison operators (values compare lexicographically) ---
        // ages: Alice 30, Bob 25, Carol 40
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age > '30'", graph);
        check("nodes age > '30': 1 row (Carol)", r.success && r.nodes.size() == 1);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age >= '30'", graph);
        check("nodes age >= '30': 2 rows (Alice, Carol)", r.success && r.nodes.size() == 2);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age < '30'", graph);
        check("nodes age < '30': 1 row (Bob)", r.success && r.nodes.size() == 1);

        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age != '30'", graph);
        check("nodes age != '30': 2 rows (Bob, Carol)", r.success && r.nodes.size() == 2);

        // --- Multiple ANDed property filters ---
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '25' AND name = 'Bob'", graph);
        check("nodes label + two matching filters: 1 row (Bob)",
            r.success && r.nodes.size() == 1);

        // Second filter fails -> zero rows. This is also a regression guard for
        // the executeCondition AND-semantics (a wrong condition must exclude).
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '25' AND name = 'Alice'", graph);
        check("nodes label + conflicting filters: 0 rows (AND excludes)",
            r.success && r.nodes.size() == 0);

        std::cout << "\n== Execute: SELECT EDGES by label ==\n";

        // --- Edge label matching (multiplicity) ---
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'KNOWS'", graph);
        check("edges label KNOWS: 2 rows", r.success && r.edges.size() == 2);

        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'LIVES_IN'", graph);
        check("edges label LIVES_IN: 3 rows", r.success && r.edges.size() == 3);

        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'OWNS'", graph);
        check("edges label OWNS (single): 1 row",
            r.success && r.edges.size() == 1 && r.edges[0].label == "OWNS");

        // Unknown edge label -> failure.
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'NOPE'", graph);
        check("edges unknown label: reports failure", !r.success);

        // Edge labels are case-sensitive too.
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'knows'", graph);
        check("edges label is case-sensitive: 'knows' fails", !r.success);

        // --- Edge label combined with a node endpoint ---
        r = q.run("SELECT * FROM EDGES WHERE FROM = 1 AND LABEL = 'LIVES_IN'", graph);
        check("edges from node 1 + label LIVES_IN: 1 row", r.success && r.edges.size() == 1);

        r = q.run("SELECT * FROM EDGES WHERE FROM = 1 AND LABEL = 'KNOWS'", graph);
        check("edges from node 1 + label KNOWS: 1 row", r.success && r.edges.size() == 1);

        // Node 2 sits on two KNOWS edges (incoming from 1, outgoing to 3);
        // BOTH direction with the label returns both.
        r = q.run("SELECT * FROM EDGES WHERE FROM = 2 AND LABEL = 'KNOWS' AND DIRECTION = 'BOTH'", graph);
        check("edges from node 2 + label KNOWS + BOTH: 2 rows", r.success && r.edges.size() == 2);
    }

    // -----------------------------------------------------------------
    // 2c. EXECUTION -- MATCH (BFS-backed traversals)
    // -----------------------------------------------------------------
    std::cout << "\n== Execute: MATCH traversals ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        // BFS runs undirected (BOTH), so all three nodes are reachable from 1.
        r = q.run("MATCH REACHABLE FROM 1", graph);
        check("match reachable: success", r.success);
        check("match reachable: 3 nodes", r.traversalResult.size() == 3);

        // Shortest path 1 -> 2 -> 3 spans three nodes.
        r = q.run("MATCH SHORTEST_PATH FROM 1 TO 3", graph);
        check("match shortest_path: success", r.success);
        check("match shortest_path: 3 nodes", r.traversalResult.size() == 3);
        check("match shortest_path: starts at source",
            r.traversalResult.size() == 3 && r.traversalResult.front() == NodeId(1));
        check("match shortest_path: ends at target",
            r.traversalResult.size() == 3 && r.traversalResult.back() == NodeId(3));

        // No path to an unreachable / nonexistent target: success, empty result.
        r = q.run("MATCH SHORTEST_PATH FROM 1 TO 999", graph);
        check("match shortest_path no path: success", r.success);
        check("match shortest_path no path: empty", r.traversalResult.empty());

        // KHOP with 1 step reaches node 1 (0 hops) and node 2 (1 hop) only.
        r = q.run("MATCH KHOP FROM 1 STEPS 1", graph);
        check("match khop steps=1: success", r.success);
        check("match khop steps=1: 2 nodes", r.traversalResult.size() == 2);

        // KHOP with 2 steps reaches all three nodes.
        r = q.run("MATCH KHOP FROM 1 STEPS 2", graph);
        check("match khop steps=2: 3 nodes", r.success && r.traversalResult.size() == 3);

        // Missing source node is an execution error.
        r = q.run("MATCH REACHABLE FROM 999", graph);
        check("match missing source: reports failure", !r.success);
    }

    // -----------------------------------------------------------------
    // 2b-iii. PROJECTION (selective columns) + TOP (row limit)
    // -----------------------------------------------------------------
    //
    // Projection: `SELECT col1, col2 FROM NODES` returns Node rows whose property
    // map is trimmed to the named columns; `SELECT *` keeps everything. The
    // reserved columns `id` and `label` are structural and always present, so
    // they never appear in (or are removed from) the property map. Projection is
    // NODES-only. TOP <n> caps the number of returned rows for SELECT and MATCH.
    std::cout << "\n== Parsing: projection + TOP grammar ==\n";
    {
        Query q;
        // Accepted forms.
        check("parse SELECT single column", q.parse("SELECT name FROM NODES WHERE LABEL = 'Person'"));
        check("parse SELECT column list", q.parse("SELECT name, age FROM NODES WHERE LABEL = 'Person'"));
        check("parse SELECT id/label columns", q.parse("SELECT id, label FROM NODES WHERE ID = 1"));
        check("parse SELECT TOP n + '*'", q.parse("SELECT TOP 10 * FROM NODES WHERE LABEL = 'Person'"));
        check("parse SELECT TOP n + columns", q.parse("SELECT TOP 5 name, age FROM NODES WHERE LABEL = 'Person'"));
        check("parse SELECT TOP on EDGES", q.parse("SELECT TOP 3 * FROM EDGES WHERE LABEL = 'KNOWS'"));
        check("parse MATCH TOP n", q.parse("MATCH TOP 2 REACHABLE FROM 1"));
        check("parse MATCH TOP n + SNAPSHOT", q.parse("MATCH TOP 2 REACHABLE FROM 1 SNAPSHOT"));

        // The parsed limit is exposed for inspection.
        check("parse records the TOP limit",
            q.parse("SELECT TOP 7 * FROM NODES WHERE LABEL = 'Person'") &&
            q.hasLimit() && q.limit() == 7);
        check("no TOP means no limit",
            q.parse("SELECT * FROM NODES WHERE LABEL = 'Person'") && !q.hasLimit());

        // Rejected forms.
        check("reject SELECT TOP without a count", !q.parse("SELECT TOP * FROM NODES WHERE ID = 1"));
        check("reject SELECT TOP negative count", !q.parse("SELECT TOP -1 * FROM NODES WHERE ID = 1"));
        check("reject trailing comma in column list", !q.parse("SELECT name, FROM NODES WHERE ID = 1"));
        check("reject missing comma in column list", !q.parse("SELECT name age FROM NODES WHERE ID = 1"));
        check("reject '*' mixed with columns", !q.parse("SELECT name, * FROM NODES WHERE ID = 1"));
        check("reject projection on EDGES", !q.parse("SELECT name FROM EDGES WHERE ID = 1"));
        check("reject MATCH TOP without a count", !q.parse("MATCH TOP REACHABLE FROM 1"));
    }

    std::cout << "\n== Execute: projection trims the property map ==\n";
    {
        // Alice/Bob carry name, age, city; Carol has no city (tests absent keys).
        Graph graph;
        graph.CreateNode("Person", { {"name","Alice"}, {"age","30"}, {"city","Sydney"} });
        graph.CreateNode("Person", { {"name","Bob"},   {"age","25"}, {"city","Sydney"} });
        graph.CreateNode("Person", { {"name","Carol"}, {"age","40"} });

        Query q; QueryResult r;

        // Single column -> each row's property map holds only that key.
        r = q.run("SELECT name FROM NODES WHERE LABEL = 'Person'", graph);
        bool onlyName = r.success && r.nodes.size() == 3;
        for (const Node& n : r.nodes)
            onlyName = onlyName && n.properties.size() == 1 && n.properties.count("name") == 1;
        check("project name: 3 rows, each property map == {name}", onlyName);

        // Column list -> exactly those keys, others dropped.
        r = q.run("SELECT name, age FROM NODES WHERE LABEL = 'Person'", graph);
        bool nameAge = r.success && r.nodes.size() == 3;
        for (const Node& n : r.nodes)
            nameAge = nameAge && n.properties.size() == 2 &&
            n.properties.count("name") && n.properties.count("age") && !n.properties.count("city");
        check("project name,age: property map == {name,age}, city dropped", nameAge);

        // id/label are structural: selecting them keeps the node's id and label
        // while leaving the property map trimmed to the *property* columns only.
        r = q.run("SELECT id, name FROM NODES WHERE LABEL = 'Person'", graph);
        check("project id,name: id+label still populated, props == {name}",
            r.success && r.nodes.size() == 3 &&
            r.nodes[0].id != NodeIdInvalid && r.nodes[0].label == "Person" &&
            r.nodes[0].properties.size() == 1 && r.nodes[0].properties.count("name"));

        // Selecting only label yields an empty property map (label lives on the
        // Node struct, not in properties).
        r = q.run("SELECT label FROM NODES WHERE LABEL = 'Person'", graph);
        check("project label: empty property map, label present",
            r.success && r.nodes[0].properties.empty() && r.nodes[0].label == "Person");

        // '*' keeps everything (Carol keeps her two properties, no city).
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person'", graph);
        bool starOk = r.success && r.nodes.size() == 3;
        int haveCity = 0;
        for (const Node& n : r.nodes) if (n.properties.count("city")) ++haveCity;
        check("project *: full property maps retained (2 of 3 have city)",
            starOk && haveCity == 2);

        // Projecting a key that some rows lack: those rows get an empty map for
        // it; the query still succeeds and returns every label-matched row.
        r = q.run("SELECT city FROM NODES WHERE LABEL = 'Person'", graph);
        int withCity = 0, withoutCity = 0;
        for (const Node& n : r.nodes) { if (n.properties.count("city")) ++withCity; else ++withoutCity; }
        check("project city: 2 rows carry city, 1 (Carol) has empty map",
            r.success && r.nodes.size() == 3 && withCity == 2 && withoutCity == 1);
    }

    std::cout << "\n== Execute: TOP <n> row limit (SELECT + MATCH) ==\n";
    {
        // Chain of five Person nodes so limits below/at/above the count are clear.
        Graph graph;
        NodeId n1 = graph.CreateNode("Person", { {"name","N1"} });
        NodeId n2 = graph.CreateNode("Person", { {"name","N2"} });
        NodeId n3 = graph.CreateNode("Person", { {"name","N3"} });
        NodeId n4 = graph.CreateNode("Person", { {"name","N4"} });
        NodeId n5 = graph.CreateNode("Person", { {"name","N5"} });
        int w = operationSuccessful;
        graph.CreateEdge(n1, n2, "KNOWS", w);
        graph.CreateEdge(n2, n3, "KNOWS", w);
        graph.CreateEdge(n3, n4, "KNOWS", w);
        graph.CreateEdge(n4, n5, "KNOWS", w);

        CSR_Representation snapshot(graph);
        snapshot.Load_CSR();

        Query q; QueryResult r;
        const std::string src = std::to_string(n1.value());

        // SELECT limits.
        r = q.run("SELECT TOP 3 * FROM NODES WHERE LABEL = 'Person'", graph);
        check("SELECT TOP 3 of 5: 3 rows", r.success && r.nodes.size() == 3);

        r = q.run("SELECT TOP 100 * FROM NODES WHERE LABEL = 'Person'", graph);
        check("SELECT TOP 100 (> available): all 5 rows", r.success && r.nodes.size() == 5);

        r = q.run("SELECT TOP 0 * FROM NODES WHERE LABEL = 'Person'", graph);
        check("SELECT TOP 0: 0 rows, success, 'Found 0 row(s).'",
            r.success && r.nodes.size() == 0 && r.message == "Found 0 row(s).");

        // TOP composes with projection.
        r = q.run("SELECT TOP 2 name FROM NODES WHERE LABEL = 'Person'", graph);
        bool ok = r.success && r.nodes.size() == 2;
        for (const Node& n : r.nodes) ok = ok && n.properties.size() == 1 && n.properties.count("name");
        check("SELECT TOP 2 + projection: 2 trimmed rows", ok);

        // TOP on an edge SELECT.
        r = q.run("SELECT TOP 2 * FROM EDGES WHERE LABEL = 'KNOWS'", graph);
        check("SELECT TOP 2 edges of 4 KNOWS: 2 rows", r.success && r.edges.size() == 2);

        // TOP caps a MATCH result (live BFS): 5 reachable, capped to 2.
        r = q.run("MATCH TOP 2 REACHABLE FROM " + src, graph);
        check("MATCH TOP 2 REACHABLE (live): 2 nodes", r.success && r.traversalResult.size() == 2);

        // TOP caps a MATCH result over the CSR snapshot too.
        q.parse("MATCH TOP 2 REACHABLE FROM " + src + " SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("MATCH TOP 2 REACHABLE (snapshot): 2 nodes", r.success && r.traversalResult.size() == 2);

        // TOP caps KHOP as well.
        r = q.run("MATCH TOP 1 KHOP FROM " + src + " STEPS 4", graph);
        check("MATCH TOP 1 KHOP: 1 node", r.success && r.traversalResult.size() == 1);
    }

    // -----------------------------------------------------------------
    // 2c-ii. ID-INDEPENDENT robustness: DELETE, SHORTEST_PATH, KHOP
    // -----------------------------------------------------------------
    //
    // Invariant under test: any statement that CAN be issued with a node or
    // edge id must also be issuable WITHOUT hard-coding one. These cases never
    // write a literal id into a query or an assertion. Instead the id is taken
    // from the value the graph assigned at creation time (NodeId::value() /
    // EdgeId::value()) and spliced into the SQL-like text, and every result is
    // checked by its CONTENT (labels / name properties resolved back through the
    // graph) rather than by comparing against a specific id.
    std::cout << "\n== Execute: id-independent DELETE (node, edge, cascade) ==\n";
    {
        // Fixture: Alice -KNOWS-> Bob, Bob -LIVES_IN-> Sydney, Alice -KNOWS-> Carol
        Graph graph;
        NodeId alice = graph.CreateNode("Person", { {"name","Alice"} });
        NodeId bob = graph.CreateNode("Person", { {"name","Bob"} });
        NodeId carol = graph.CreateNode("Person", { {"name","Carol"} });
        NodeId syd = graph.CreateNode("City", { {"name","Sydney"} });
        int w = operationSuccessful;
        graph.CreateEdge(alice, bob, "KNOWS", w);
        graph.CreateEdge(bob, syd, "LIVES_IN", w);
        graph.CreateEdge(alice, carol, "KNOWS", w);

        Query q; QueryResult r;
        // true if any returned node carries name == target
        auto nodesHaveName = [&](const std::vector<Node>& ns, const std::string& t) {
            for (const Node& n : ns) {
                auto it = n.properties.find("name");
                if (it != n.properties.end() && it->second == t) return true;
            }
            return false; };

        // DELETE a node addressed by its runtime-assigned id, verified by content.
        r = q.run("DELETE FROM NODES WHERE ID = " + std::to_string(bob.value()), graph);
        check("delete node by derived id: success", r.success);
        r = q.run("SELECT * FROM NODES WHERE LABEL = 'Person'", graph);
        check("after delete: 2 Person rows and Bob is gone",
            r.success && r.nodes.size() == 2 && !nodesHaveName(r.nodes, "Bob"));

        // Deleting the node cascaded to its incident edges.
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'LIVES_IN'", graph);
        check("cascade: Bob's only LIVES_IN edge is gone (0 rows)",
            r.success && r.edges.size() == 0);
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'KNOWS'", graph);
        check("cascade: Alice-Bob KNOWS gone, Alice-Carol KNOWS remains (1 row)",
            r.success && r.edges.size() == 1);
    }
    {
        // DELETE an edge addressed by its runtime-assigned EdgeId.
        Graph graph;
        NodeId a = graph.CreateNode("Person", { {"name","Alice"} });
        NodeId b = graph.CreateNode("Person", { {"name","Bob"} });
        int w = operationSuccessful;
        EdgeId e = graph.CreateEdge(a, b, "KNOWS", w);

        Query q; QueryResult r;
        r = q.run("DELETE FROM EDGES WHERE ID = " + std::to_string(e.value()), graph);
        check("delete edge by derived edge id: success", r.success);
        r = q.run("SELECT * FROM EDGES WHERE LABEL = 'KNOWS'", graph);
        check("after edge delete: 0 KNOWS edges remain", r.success && r.edges.size() == 0);
    }
    {
        // Deleting a missing target is a failure -- shown without fabricating an
        // id: delete once (succeeds), then delete the same target again.
        Graph graph;
        NodeId a = graph.CreateNode("Person", { {"name","Alice"} });
        NodeId b = graph.CreateNode("Person", { {"name","Bob"} });
        int w = operationSuccessful;
        EdgeId e = graph.CreateEdge(a, b, "KNOWS", w);   // cascades when 'a' is deleted

        Query q; QueryResult r;
        q.run("DELETE FROM NODES WHERE ID = " + std::to_string(a.value()), graph);
        r = q.run("DELETE FROM NODES WHERE ID = " + std::to_string(a.value()), graph);
        check("double-delete node: second attempt reports failure", !r.success);
        r = q.run("DELETE FROM EDGES WHERE ID = " + std::to_string(e.value()), graph);
        check("delete edge already removed by cascade: reports failure", !r.success);
    }

    return fail == 0 ? 0 : 1;
}

// Second half of the query-layer suite (see the note on test_query_layer_part1).
static int test_query_layer_part2() {
    std::cout << "\n== Execute: id-independent SHORTEST_PATH (live + snapshot) ==\n";
    {
        // Directed chain A -> B -> C -> D (traversal is undirected).
        Graph graph;
        NodeId a = graph.CreateNode("Stop", { {"name","A"} });
        NodeId b = graph.CreateNode("Stop", { {"name","B"} });
        NodeId c = graph.CreateNode("Stop", { {"name","C"} });
        NodeId d = graph.CreateNode("Stop", { {"name","D"} });
        int w = operationSuccessful;
        graph.CreateEdge(a, b, "NEXT", w);
        graph.CreateEdge(b, c, "NEXT", w);
        graph.CreateEdge(c, d, "NEXT", w);

        CSR_Representation snapshot(graph);
        snapshot.Load_CSR();

        Query q; QueryResult r;
        auto nameOf = [&](NodeId id) { Node n; if (!graph.GetNode(id, n)) return std::string("");
        auto it = n.properties.find("name"); return it != n.properties.end() ? it->second : std::string(""); };
        auto pathHas = [&](const std::vector<NodeId>& v, const std::string& t) {
            for (NodeId id : v) if (nameOf(id) == t) return true;
            return false; };
        const std::string src = std::to_string(a.value());
        const std::string dst = std::to_string(d.value());

        // Live (default) shortest path, asserted entirely by name.
        r = q.run("MATCH SHORTEST_PATH FROM " + src + " TO " + dst, graph);
        check("live shortest_path A..D: 4 nodes, A->D endpoints, contains B and C",
            r.success && r.traversalResult.size() == 4 &&
            nameOf(r.traversalResult.front()) == "A" &&
            nameOf(r.traversalResult.back()) == "D" &&
            pathHas(r.traversalResult, "B") && pathHas(r.traversalResult, "C"));

        // Snapshot shortest path over the frozen CSR, same content.
        q.parse("MATCH SHORTEST_PATH FROM " + src + " TO " + dst + " SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot shortest_path A..D: 4 nodes, A->D endpoints by name",
            r.success && r.traversalResult.size() == 4 &&
            nameOf(r.traversalResult.front()) == "A" &&
            nameOf(r.traversalResult.back()) == "D");
    }
    {
        // Two disconnected components: no path exists (success, empty) for both
        // engines. Ids are derived; assertions are on emptiness, not ids.
        Graph graph;
        NodeId p = graph.CreateNode("Stop", { {"name","P"} });
        NodeId qn = graph.CreateNode("Stop", { {"name","Q"} });
        NodeId x = graph.CreateNode("Stop", { {"name","X"} });
        NodeId y = graph.CreateNode("Stop", { {"name","Y"} });
        int w = operationSuccessful;
        graph.CreateEdge(p, qn, "NEXT", w);   // component 1
        graph.CreateEdge(x, y, "NEXT", w);   // component 2

        CSR_Representation snapshot(graph);
        snapshot.Load_CSR();

        Query q; QueryResult r;
        const std::string src = std::to_string(p.value());
        const std::string dst = std::to_string(x.value());

        r = q.run("MATCH SHORTEST_PATH FROM " + src + " TO " + dst, graph);
        check("live shortest_path across components: success, empty path",
            r.success && r.traversalResult.empty());

        q.parse("MATCH SHORTEST_PATH FROM " + src + " TO " + dst + " SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot shortest_path across components: success, empty path",
            r.success && r.traversalResult.empty());
    }

    std::cout << "\n== Execute: id-independent KHOP (star graph, steps 0/1) ==\n";
    {
        // Star: Hub linked to L1, L2, L3.
        Graph graph;
        NodeId hub = graph.CreateNode("Hub", { {"name","Hub"} });
        NodeId l1 = graph.CreateNode("Leaf", { {"name","L1"} });
        NodeId l2 = graph.CreateNode("Leaf", { {"name","L2"} });
        NodeId l3 = graph.CreateNode("Leaf", { {"name","L3"} });
        int w = operationSuccessful;
        graph.CreateEdge(hub, l1, "LINK", w);
        graph.CreateEdge(hub, l2, "LINK", w);
        graph.CreateEdge(hub, l3, "LINK", w);

        CSR_Representation snapshot(graph);
        snapshot.Load_CSR();

        Query q; QueryResult r;
        auto nameOf = [&](NodeId id) { Node n; if (!graph.GetNode(id, n)) return std::string("");
        auto it = n.properties.find("name"); return it != n.properties.end() ? it->second : std::string(""); };
        auto has = [&](const std::vector<NodeId>& v, const std::string& t) {
            for (NodeId id : v) if (nameOf(id) == t) return true;
            return false; };
        const std::string hubId = std::to_string(hub.value());
        const std::string leafId = std::to_string(l1.value());

        // 1 hop from the hub reaches the hub itself plus every leaf.
        r = q.run("MATCH KHOP FROM " + hubId + " STEPS 1", graph);
        check("live khop hub steps=1: hub + 3 leaves (4), all leaves present",
            r.success && r.traversalResult.size() == 4 &&
            has(r.traversalResult, "L1") && has(r.traversalResult, "L2") && has(r.traversalResult, "L3"));

        // 1 hop from a leaf reaches only that leaf and the hub, no sibling leaf.
        r = q.run("MATCH KHOP FROM " + leafId + " STEPS 1", graph);
        check("live khop leaf steps=1: leaf + hub (2), no sibling leaf",
            r.success && r.traversalResult.size() == 2 &&
            has(r.traversalResult, "Hub") && !has(r.traversalResult, "L2"));

        // 0 hops is just the source node, whichever id it was assigned.
        r = q.run("MATCH KHOP FROM " + hubId + " STEPS 0", graph);
        check("live khop steps=0: source only (1), by name",
            r.success && r.traversalResult.size() == 1 && nameOf(r.traversalResult.front()) == "Hub");

        // Same two behaviours over the CSR snapshot.
        q.parse("MATCH KHOP FROM " + hubId + " STEPS 1 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot khop hub steps=1: hub + 3 leaves (4)",
            r.success && r.traversalResult.size() == 4 && has(r.traversalResult, "L3"));

        q.parse("MATCH KHOP FROM " + hubId + " STEPS 0 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot khop steps=0: source only (1), by name",
            r.success && r.traversalResult.size() == 1 && nameOf(r.traversalResult.front()) == "Hub");
    }

    // -----------------------------------------------------------------
    // 2d. LIVE / SNAPSHOT execution-mode toggling
    // -----------------------------------------------------------------
    //
    // A statement may end with the reserved keyword LIVE (the default) or
    // SNAPSHOT. LIVE MATCH traversals run on the live graph via BFS_Searcher;
    // SNAPSHOT MATCH traversals run on a frozen CSR_Representation via
    // CSR_Searcher. Because the CSR snapshot only indexes adjacency, SNAPSHOT is
    // rejected on SELECT / INSERT / DELETE at parse time.
    std::cout << "\n== Parsing: LIVE / SNAPSHOT keyword ==\n";
    {
        Query q;

        // The trailing keyword is accepted and recorded on the parsed query.
        check("parse MATCH ... SNAPSHOT accepted",
            q.parse("MATCH REACHABLE FROM 1 SNAPSHOT") &&
            q.executionMode() == ExecutionMode::Snapshot);
        check("parse MATCH ... LIVE accepted",
            q.parse("MATCH REACHABLE FROM 1 LIVE") &&
            q.executionMode() == ExecutionMode::Live);
        // No trailing keyword defaults to LIVE.
        check("parse MATCH (no keyword) defaults to LIVE",
            q.parse("MATCH REACHABLE FROM 1") &&
            q.executionMode() == ExecutionMode::Live);
        // The keyword is case-insensitive, like every other keyword.
        check("parse SNAPSHOT is case-insensitive",
            q.parse("MATCH KHOP FROM 1 STEPS 2 snapshot") &&
            q.executionMode() == ExecutionMode::Snapshot);
        // A quoted value that happens to spell SNAPSHOT is NOT a mode keyword.
        check("quoted 'SNAPSHOT' value is not a mode keyword",
            q.parse("SELECT * FROM NODES WHERE LABEL = 'SNAPSHOT'") &&
            q.executionMode() == ExecutionMode::Live);

        // SNAPSHOT is valid on reads (SELECT, MATCH); mutations reject it.
        check("accept SELECT ... SNAPSHOT (node id filter)",
            q.parse("SELECT * FROM NODES WHERE ID = 1 SNAPSHOT") &&
            q.executionMode() == ExecutionMode::Snapshot);
        check("accept SELECT ... SNAPSHOT (unfiltered full scan)",
            q.parse("SELECT * FROM NODES SNAPSHOT") &&
            q.executionMode() == ExecutionMode::Snapshot);
        check("accept SELECT ... SNAPSHOT (edges)",
            q.parse("SELECT * FROM EDGES SNAPSHOT") &&
            q.executionMode() == ExecutionMode::Snapshot);
        check("reject INSERT ... SNAPSHOT", !q.parse("INSERT INTO NODES (label) VALUES ('X') SNAPSHOT"));
        check("reject DELETE ... SNAPSHOT", !q.parse("DELETE FROM NODES WHERE ID = 1 SNAPSHOT"));
    }

    std::cout << "\n== Execute: MATCH over a CSR SNAPSHOT ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);

        // Freeze a point-in-time CSR snapshot of the graph.
        CSR_Representation snapshot(graph);
        snapshot.Load_CSR();

        Query q;
        QueryResult r;

        // REACHABLE over the snapshot: undirected BFS reaches all three nodes.
        q.parse("MATCH REACHABLE FROM 1 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot reachable: success", r.success);
        check("snapshot reachable: 3 nodes", r.traversalResult.size() == 3);

        // SHORTEST_PATH over the snapshot: 1 -> 2 -> 3.
        q.parse("MATCH SHORTEST_PATH FROM 1 TO 3 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot shortest_path: success", r.success);
        check("snapshot shortest_path: 3 nodes", r.traversalResult.size() == 3);
        check("snapshot shortest_path: starts at source",
            r.traversalResult.size() == 3 && r.traversalResult.front() == NodeId(1));
        check("snapshot shortest_path: ends at target",
            r.traversalResult.size() == 3 && r.traversalResult.back() == NodeId(3));

        // SHORTEST_PATH to a node absent from the snapshot: success, empty path.
        q.parse("MATCH SHORTEST_PATH FROM 1 TO 999 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot shortest_path no path: success", r.success);
        check("snapshot shortest_path no path: empty", r.traversalResult.empty());

        // KHOP over the snapshot: 1 step reaches node 1 (0 hops) and node 2.
        q.parse("MATCH KHOP FROM 1 STEPS 1 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot khop steps=1: success", r.success);
        check("snapshot khop steps=1: 2 nodes", r.traversalResult.size() == 2);

        // A source node absent from the snapshot is an execution failure.
        q.parse("MATCH REACHABLE FROM 999 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot missing source: reports failure", !r.success);
    }

    std::cout << "\n== Execute: point-in-time divergence (LIVE vs SNAPSHOT) ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);           // nodes 1,2,3 ; edges 1->2, 2->3

        // Freeze the snapshot while the graph has exactly three nodes.
        CSR_Representation snapshot(graph);
        snapshot.Load_CSR();

        // Now mutate the LIVE graph: add node 4 and connect it to node 3.
        int warning = operationSuccessful;
        graph.CreateNode("Robot", { {"name", "R2"} });      // id 4
        graph.CreateEdge(NodeId(3), NodeId(4), "BUILT", warning);

        Query q;
        QueryResult r;

        // LIVE reflects the mutation: node 4 is now reachable -> 4 nodes.
        q.parse("MATCH REACHABLE FROM 1 LIVE");
        r = q.execute(graph, snapshot);
        check("live reachable sees new node: 4 nodes",
            r.success && r.traversalResult.size() == 4);

        // SNAPSHOT is frozen at 3 nodes: it does not see the later mutation.
        q.parse("MATCH REACHABLE FROM 1 SNAPSHOT");
        r = q.execute(graph, snapshot);
        check("snapshot reachable ignores new node: 3 nodes",
            r.success && r.traversalResult.size() == 3);

        // With no snapshot supplied, a SNAPSHOT query gracefully falls back to
        // the live BFS traversal (single-argument execute) -> sees 4 nodes.
        q.parse("MATCH REACHABLE FROM 1 SNAPSHOT");
        r = q.execute(graph);
        check("snapshot query, no snapshot supplied: falls back to live (4 nodes)",
            r.success && r.traversalResult.size() == 4);
    }

    // -----------------------------------------------------------------
    // 3. EXECUTION -- INSERT (mutating)
    // -----------------------------------------------------------------
    std::cout << "\n== Execute: INSERT ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        // Insert a node, then read it back through the query layer (new id = 4).
        r = q.run("INSERT INTO NODES (label, name) VALUES ('Robot', 'R2')", graph);
        check("insert node: success", r.success);
        check("insert node: 1 row returned", r.nodes.size() == 1);
        check("insert node: label preserved",
            r.nodes.size() == 1 && r.nodes[0].label == "Robot");

        r = q.run("SELECT * FROM NODES WHERE ID = 4", graph);
        check("inserted node is retrievable", r.success && r.nodes.size() == 1);

        // Insert a valid edge between existing nodes.
        r = q.run("INSERT INTO EDGES (from, to, label) VALUES (1, 3, 'VISITS')", graph);
        check("insert edge: success", r.success);
        check("insert edge: 1 row returned", r.edges.size() == 1);

        // Insert an edge with a nonexistent endpoint -> execution failure.
        r = q.run("INSERT INTO EDGES (from, to, label) VALUES (1, 999, 'BAD')", graph);
        check("insert edge bad endpoint: reports failure", !r.success);
    }

    // -----------------------------------------------------------------
    // 3b. EXECUTION -- DELETE (mutating)
    // -----------------------------------------------------------------
    std::cout << "\n== Execute: DELETE ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);
        Query q;
        QueryResult r;

        // Delete an existing edge, then confirm it is gone.
        r = q.run("DELETE FROM EDGES WHERE ID = 2", graph);
        check("delete edge: success", r.success);
        r = q.run("SELECT * FROM EDGES WHERE ID = 2", graph);
        check("deleted edge no longer found", r.success && r.edges.size() == 0);

        // Delete an existing node, then confirm it is gone.
        r = q.run("DELETE FROM NODES WHERE ID = 3", graph);
        check("delete node: success", r.success);
        r = q.run("SELECT * FROM NODES WHERE ID = 3", graph);
        check("deleted node no longer found", r.success && r.nodes.size() == 0);

        // Deleting something that does not exist is an execution failure.
        r = q.run("DELETE FROM NODES WHERE ID = 999", graph);
        check("delete missing node: reports failure", !r.success);

        r = q.run("DELETE FROM EDGES WHERE ID = 999", graph);
        check("delete missing edge: reports failure", !r.success);
    }

    std::cout << "\n" << pass << " passed, " << fail << " failed (cumulative).\n";
    return fail == 0 ? 0 : 1;
}

// =====================================================================
// SELECT ... SNAPSHOT test suite (MVCC point-in-time reads)
// =====================================================================
//
// Kept in its own function (rather than folded into test_query_layer_part2)
// so its per-block Graph / CSR_Representation locals do not add to that
// function's stack frame -- summing them there re-tripped the compiler
// stack-size warning. The pass/fail counters are file-static, so splitting
// this out is transparent to the reported totals.
//
// These verify that a SELECT resolved against a CSR snapshot observes the
// graph exactly as it was when the snapshot was captured, independent of any
// later inserts or deletes on the live graph. Tests avoid hardcoded NodeIds
// where the assertion is about counts/content; where an id is needed it is
// captured from CreateNode's return value.
static int test_select_snapshot() {
    std::cout << "\n============= SELECT ... SNAPSHOT (MVCC) =============\n";

    std::cout << "\n== Execute: SELECT ... SNAPSHOT (point-in-time isolation) ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);           // 3 nodes: 2 Person, 1 City

        // Freeze a snapshot at three nodes.
        CSR_Representation snap(graph);
        snap.Load_CSR();

        Query q;
        QueryResult r;

        // Unfiltered full scan over the snapshot sees exactly the frozen 3 nodes.
        q.parse("SELECT * FROM NODES SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot select all: success", r.success);
        check("snapshot select all: 3 rows", r.nodes.size() == 3);

        // Label filter over the snapshot: two Person nodes.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot select label=Person: 2 rows",
            r.success && r.nodes.size() == 2);

        // Now mutate the LIVE graph AFTER the snapshot was captured.
        graph.CreateNode("Person", { {"name", "Dave"}, {"age", "40"} });
        graph.CreateNode("Person", { {"name", "Eve"},  {"age", "50"} });

        // LIVE label select sees the two new Person nodes (4 total).
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person'");
        r = q.execute(graph, snap);
        check("live select label=Person after inserts: 4 rows",
            r.success && r.nodes.size() == 4);

        // SNAPSHOT is unchanged: still the 2 Person nodes frozen at capture time.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot select label=Person after inserts: still 2 rows",
            r.success && r.nodes.size() == 2);

        // Full snapshot scan still 3, ignoring the two later inserts.
        q.parse("SELECT * FROM NODES SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot select all after inserts: still 3 rows",
            r.success && r.nodes.size() == 3);
    }

    std::cout << "\n== Execute: SELECT ... SNAPSHOT ignores later DELETEs ==\n";
    {
        Graph graph;
        NodeId keep = graph.CreateNode("Person", { {"name", "Keep"} });
        NodeId drop = graph.CreateNode("Person", { {"name", "Drop"} });
        (void)keep;

        // Snapshot with both nodes present.
        CSR_Representation snap(graph);
        snap.Load_CSR();

        Query q;
        QueryResult r;

        // Sanity: live and snapshot both see 2 Person nodes to start.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        r = q.execute(graph, snap);
        check("pre-delete snapshot: 2 Person rows", r.success && r.nodes.size() == 2);

        // Delete one node on the LIVE graph.
        int warning = operationSuccessful;
        graph.DeleteNode(drop, warning);
        check("delete on live: success", warning == operationSuccessful);

        // LIVE now sees only 1 Person node.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person'");
        r = q.execute(graph, snap);
        check("live after delete: 1 Person row", r.success && r.nodes.size() == 1);

        // SNAPSHOT still sees the deleted node -- point-in-time is preserved.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot after delete: still 2 Person rows (tombstone visible)",
            r.success && r.nodes.size() == 2);

        // The deleted node is fetchable by id through the snapshot but not live.
        q.parse("SELECT * FROM NODES WHERE ID = " + std::to_string(drop.value()) + " SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot fetch deleted node by id: 1 row",
            r.success && r.nodes.size() == 1);

        q.parse("SELECT * FROM NODES WHERE ID = " + std::to_string(drop.value()));
        r = q.execute(graph, snap);
        check("live fetch deleted node by id: 0 rows",
            r.success && r.nodes.size() == 0);
    }

    std::cout << "\n== Execute: SELECT ... SNAPSHOT with projection and TOP ==\n";
    {
        Graph graph;
        graph.CreateNode("Widget", { {"color", "red"},   {"size", "L"} });
        graph.CreateNode("Widget", { {"color", "blue"},  {"size", "M"} });
        graph.CreateNode("Widget", { {"color", "green"}, {"size", "S"} });

        CSR_Representation snap(graph);
        snap.Load_CSR();

        // Mutate live afterwards; snapshot queries must ignore this.
        graph.CreateNode("Widget", { {"color", "black"}, {"size", "XL"} });

        Query q;
        QueryResult r;

        // Projection over the snapshot: only 'color' retained, id/label structural.
        q.parse("SELECT color FROM NODES WHERE LABEL = 'Widget' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot projection: 3 rows (ignores later insert)",
            r.success && r.nodes.size() == 3);
        bool onlyColor = !r.nodes.empty();
        for (const Node& n : r.nodes) {
            if (n.properties.size() != 1 || n.properties.find("color") == n.properties.end())
                onlyColor = false;
        }
        check("snapshot projection: each row has only 'color'", onlyColor);

        // TOP over the snapshot caps rows.
        q.parse("SELECT TOP 2 * FROM NODES WHERE LABEL = 'Widget' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot TOP 2: 2 rows", r.success && r.nodes.size() == 2);

        // TOP composes with projection on the snapshot.
        q.parse("SELECT TOP 1 color FROM NODES WHERE LABEL = 'Widget' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot TOP 1 + projection: 1 row",
            r.success && r.nodes.size() == 1);
        check("snapshot TOP 1 + projection: only 'color'",
            r.nodes.size() == 1 && r.nodes[0].properties.size() == 1 &&
            r.nodes[0].properties.count("color") == 1);

        // TOP 0 yields no rows even on the snapshot.
        q.parse("SELECT TOP 0 * FROM NODES WHERE LABEL = 'Widget' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot TOP 0: 0 rows", r.success && r.nodes.empty());
    }

    std::cout << "\n== Execute: SELECT ... SNAPSHOT over EDGES ==\n";
    {
        Graph graph;
        NodeId a = graph.CreateNode("Person", { {"name", "A"} });
        NodeId b = graph.CreateNode("Person", { {"name", "B"} });
        NodeId c = graph.CreateNode("Person", { {"name", "C"} });
        int warning = operationSuccessful;
        graph.CreateEdge(a, b, "KNOWS", warning);
        graph.CreateEdge(b, c, "KNOWS", warning);

        CSR_Representation snap(graph);
        snap.Load_CSR();

        // Add another edge on the live graph after capture.
        graph.CreateEdge(a, c, "KNOWS", warning);

        Query q;
        QueryResult r;

        // Snapshot edge scan by label: the 2 frozen edges, not the 3rd.
        q.parse("SELECT * FROM EDGES WHERE LABEL = 'KNOWS' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot edges by label: 2 rows (ignores later edge)",
            r.success && r.edges.size() == 2);

        // Live sees all 3 edges.
        q.parse("SELECT * FROM EDGES WHERE LABEL = 'KNOWS'");
        r = q.execute(graph, snap);
        check("live edges by label: 3 rows", r.success && r.edges.size() == 3);

        // Snapshot outgoing edges from 'a': only the frozen a->b edge.
        q.parse("SELECT * FROM EDGES WHERE FROM = " + std::to_string(a.value()) +
            " AND DIRECTION = 'OUTGOING' SNAPSHOT");
        r = q.execute(graph, snap);
        check("snapshot edges FROM a OUTGOING: 1 row (a->b only)",
            r.success && r.edges.size() == 1);
    }

    std::cout << "\n== Execute: snapshot history reclaimed after release (GC) ==\n";
    {
        Graph graph;
        NodeId n1 = graph.CreateNode("Temp", { {"k", "v1"} });
        NodeId n2 = graph.CreateNode("Temp", { {"k", "v2"} });
        (void)n1;

        int warning = operationSuccessful;
        {
            // Hold a snapshot; delete a node. History must be retained while the
            // snapshot is alive so the snapshot can still observe the deleted node.
            CSR_Representation snap(graph);
            snap.Load_CSR();
            check("snapshot active while alive", graph.ActiveSnapshotCount() == 1);

            graph.DeleteNode(n2, warning);

            Query q;
            q.parse("SELECT * FROM NODES WHERE LABEL = 'Temp' SNAPSHOT");
            QueryResult r = q.execute(graph, snap);
            check("held snapshot still sees deleted node: 2 rows",
                r.success && r.nodes.size() == 2);
            // History retains the tombstoned node while the snapshot is held.
            check("history retained while snapshot held (>= 2 records)",
                graph.MvccNodeHistorySize() >= 2);
        } // snap destructor releases the snapshot and triggers garbage collection

        check("no active snapshots after release", graph.ActiveSnapshotCount() == 0);
        // With no snapshot to preserve it, the tombstoned node's history is
        // reclaimed, leaving only the one surviving live node.
        check("tombstoned history reclaimed after release (1 record)",
            graph.MvccNodeHistorySize() == 1);
    }

    std::cout << "\n== Execute: SELECT ... SNAPSHOT falls back to live without a snapshot ==\n";
    {
        Graph graph;
        buildQueryFixture(graph);           // 3 nodes

        Query q;
        // No CSR snapshot supplied: single-argument execute. SELECT ... SNAPSHOT
        // has no captured version, so it resolves against the live graph. An id
        // lookup still works because the live path handles WHERE ID.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        QueryResult r = q.execute(graph);
        check("snapshot select, no snapshot supplied: falls back to live (2 Person)",
            r.success && r.nodes.size() == 2);
    }

    // -----------------------------------------------------------------------
    // CSR-driven membership: node SELECT ... SNAPSHOT takes *which* nodes are
    // visible from the CSR snapshot's captured id set, and reads the payload
    // (label/properties) from the graph's MVCC history at the snapshot version.
    // These checks distinguish the CSR-membership design from a plain
    // version-only scan: two snapshots captured at different times must each
    // report their own frozen node set from the same live graph.
    // -----------------------------------------------------------------------
    std::cout << "\n== Execute: SELECT ... SNAPSHOT membership driven by the CSR snapshot ==\n";
    {
        Graph graph;
        graph.CreateNode("Person", { {"name", "A1"} });
        graph.CreateNode("Person", { {"name", "A2"} });

        // First snapshot: sees 2 nodes.
        CSR_Representation snapEarly(graph);
        snapEarly.Load_CSR();
        check("early snapshot CSR size == 2", snapEarly.Size() == 2);

        // Grow the live graph, then take a second snapshot: sees 4 nodes.
        graph.CreateNode("Person", { {"name", "A3"} });
        graph.CreateNode("Person", { {"name", "A4"} });
        CSR_Representation snapLate(graph);
        snapLate.Load_CSR();
        check("late snapshot CSR size == 4", snapLate.Size() == 4);

        Query q;
        QueryResult r;

        // The SAME query against two different snapshots must yield each
        // snapshot's own frozen membership -- 2 for early, 4 for late.
        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        r = q.execute(graph, snapEarly);
        check("early snapshot select: 2 rows (its captured set)",
            r.success && r.nodes.size() == 2);

        q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
        r = q.execute(graph, snapLate);
        check("late snapshot select: 4 rows (its captured set)",
            r.success && r.nodes.size() == 4);

        // Payloads are still resolved: the property read from MVCC history is
        // present for a node in the early snapshot's membership.
        q.parse("SELECT * FROM NODES SNAPSHOT");
        r = q.execute(graph, snapEarly);
        bool payloadOk = (r.nodes.size() == 2);
        for (const Node& n : r.nodes) {
            if (n.label != "Person" || n.properties.find("name") == n.properties.end())
                payloadOk = false;
        }
        check("early snapshot payloads resolved (label + name present)", payloadOk);
    }

    std::cout << "\n" << pass << " passed, " << fail << " failed (cumulative).\n";
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// UPDATE / LOAD / SAVE query commands
// ---------------------------------------------------------------------------
static int test_update_load_save() {
    std::cout << "\n============= UPDATE / LOAD / SAVE =============\n";

    // -- UPDATE NODES by property value ---------------------------------------
    {
        Graph graph;
        graph.CreateNode("Person", { {"name", "Alice"}, {"age", "25"} });
        graph.CreateNode("Person", { {"name", "Bob"},   {"age", "25"} });
        graph.CreateNode("City", { {"name", "Sydney"} });

        Query q;
        QueryResult r = q.run("UPDATE NODES WHERE age = 25 SET age = 26", graph);
        check("UPDATE NODES by property: success", r.success);
        check("UPDATE NODES by property: 2 updated", r.message == "Updated 2 node(s).");

        QueryResult v = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = 26", graph);
        check("UPDATE NODES by property: 2 rows now match new value",
            v.success && v.nodes.size() == 2);
        QueryResult old = q.run("SELECT * FROM NODES WHERE LABEL = 'Person' AND age = 25", graph);
        check("UPDATE NODES by property: 0 rows at old value",
            old.success && old.nodes.empty());

        QueryResult city = q.run("SELECT * FROM NODES WHERE LABEL = 'City'", graph);
        check("UPDATE NODES by property: non-matching row untouched",
            city.success && city.nodes.size() == 1 &&
            city.nodes[0].properties.find("age") == city.nodes[0].properties.end());
    }

    // -- UPDATE NODES by id ----------------------------------------------------
    {
        Graph graph;
        NodeId alice = graph.CreateNode("Person", { {"name", "Alice"}, {"status", "active"} });
        NodeId bob = graph.CreateNode("Person", { {"name", "Bob"},   {"status", "active"} });

        Query q;
        QueryResult r = q.run("UPDATE NODES WHERE ID = " + std::to_string(alice.value()) +
            " SET status = inactive", graph);
        check("UPDATE NODES by id: success", r.success);
        check("UPDATE NODES by id: 1 updated", r.message == "Updated 1 node(s).");

        Node a; graph.GetNode(alice, a);
        Node b; graph.GetNode(bob, b);
        check("UPDATE NODES by id: target changed", a.properties.at("status") == "inactive");
        check("UPDATE NODES by id: other unchanged", b.properties.at("status") == "active");
    }

    // -- UPDATE NODES adds a new property key ----------------------------------
    {
        Graph graph;
        NodeId n = graph.CreateNode("Person", { {"name", "Alice"} });
        Query q;
        QueryResult r = q.run("UPDATE NODES WHERE ID = " + std::to_string(n.value()) +
            " SET nickname = Al", graph);
        check("UPDATE NODES new key: success", r.success);
        Node got; graph.GetNode(n, got);
        check("UPDATE NODES new key: key added",
            got.properties.count("nickname") == 1 && got.properties.at("nickname") == "Al");
        check("UPDATE NODES new key: existing key preserved", got.properties.at("name") == "Alice");
    }

    // -- UPDATE EDGES label ----------------------------------------------------
    {
        Graph graph;
        NodeId n1 = graph.CreateNode("Person", {});
        NodeId n2 = graph.CreateNode("Person", {});
        NodeId n3 = graph.CreateNode("City", {});
        int w = operationSuccessful;
        graph.CreateEdge(n1, n2, "KNOWS", w);
        graph.CreateEdge(n2, n3, "KNOWS", w);

        Query q;
        QueryResult r = q.run("UPDATE EDGES WHERE LABEL = 'KNOWS' SET label = BEFRIENDS", graph);
        check("UPDATE EDGES label: success", r.success);
        check("UPDATE EDGES label: 2 updated", r.message == "Updated 2 edge(s).");

        int w2 = operationSuccessful;
        std::vector<Edge> befriends;
        graph.FindEdgesByLabel(befriends, "BEFRIENDS", w2);
        check("UPDATE EDGES label: new label indexed",
            w2 == operationSuccessful && befriends.size() == 2);

        int w3 = operationSuccessful;
        std::vector<Edge> knows;
        graph.FindEdgesByLabel(knows, "KNOWS", w3);
        check("UPDATE EDGES label: old label no longer present", knows.empty());
    }

    // -- UPDATE EDGES rejects a non-label column -------------------------------
    {
        Graph graph;
        NodeId n1 = graph.CreateNode("Person", {});
        NodeId n2 = graph.CreateNode("Person", {});
        int w = operationSuccessful;
        graph.CreateEdge(n1, n2, "KNOWS", w);

        Query q;
        QueryResult r = q.run("UPDATE EDGES WHERE LABEL = 'KNOWS' SET weight = 5", graph);
        check("UPDATE EDGES bad column: rejected", !r.success);
        check("UPDATE EDGES bad column: message mentions label",
            r.message.find("label") != std::string::npos);
    }

    // -- UPDATE parse errors ---------------------------------------------------
    {
        Query q;
        check("UPDATE no WHERE: parse fails", !q.parse("UPDATE NODES SET name = Bob"));
        check("UPDATE no WHERE: WHERE mentioned", q.lastError().find("WHERE") != std::string::npos);

        Query q2;
        check("UPDATE no SET: parse fails", !q2.parse("UPDATE NODES WHERE LABEL = 'Person'"));
        check("UPDATE no SET: SET mentioned", q2.lastError().find("SET") != std::string::npos);

        Query q3;
        check("UPDATE bad target: parse fails", !q3.parse("UPDATE THINGS WHERE ID = 1 SET x = y"));

        Query q4;
        check("UPDATE SNAPSHOT: parse fails",
            !q4.parse("UPDATE NODES WHERE ID = 1 SET age = 30 SNAPSHOT"));
        check("UPDATE SNAPSHOT: SNAPSHOT mentioned", q4.lastError().find("SNAPSHOT") != std::string::npos);
    }

    // -- LOAD / SAVE parse + path extraction -----------------------------------
    {
        Query q;
        check("SAVE FILE: parse succeeds", q.parse("SAVE FILE 'graph.db'"));
        check("SAVE FILE: operation is Save", q.operation() == QueryOperation::Save);
        check("SAVE FILE: path extracted (quotes stripped)", q.filePath() == "graph.db");

        Query q2;
        check("LOAD FILE: parse succeeds", q2.parse("LOAD FILE 'graph.db'"));
        check("LOAD FILE: operation is Load", q2.operation() == QueryOperation::Load);
        check("LOAD FILE: path extracted", q2.filePath() == "graph.db");

        Query q3;
        check("SAVE no path: parse fails", !q3.parse("SAVE FILE"));

        Query q4;
        check("LOAD path matches SAVE path", q4.parse("LOAD FILE 'graph.db'") &&
            q4.filePath() == "graph.db");
    }

    // -- SAVE then LOAD round-trip through StorageEngine using parsed paths -----
    {
        const std::string path = "test_update_roundtrip.db";
        Graph original;
        original.CreateNode("Person", { {"name", "Alice"} });
        original.CreateNode("City", { {"name", "Sydney"} });

        Query saveQ;
        check("round-trip: SAVE parses",
            saveQ.parse("SAVE FILE '" + path + "'") && saveQ.operation() == QueryOperation::Save);
        StorageEngine saveEngine(saveQ.filePath());
        check("round-trip: StorageEngine.Save succeeds", saveEngine.Save(original));

        Query loadQ;
        check("round-trip: LOAD parses",
            loadQ.parse("LOAD FILE '" + path + "'") && loadQ.operation() == QueryOperation::Load);
        Graph restored;
        StorageEngine loadEngine(loadQ.filePath());
        check("round-trip: StorageEngine.Load succeeds", loadEngine.Load(restored));

        std::vector<NodeId> ids;
        restored.GetNodeIdOrder(ids);
        check("round-trip: node count preserved", ids.size() == 2);
        std::remove(path.c_str());
    }

    // -- IMPORT CSV / EXPORT CSV parse + path extraction -----------------------
    {
        Query q;
        check("EXPORT CSV: parse succeeds", q.parse("EXPORT CSV 'graph.csv'"));
        check("EXPORT CSV: operation is Export", q.operation() == QueryOperation::Export);
        check("EXPORT CSV: path extracted (quotes stripped)", q.filePath() == "graph.csv");

        Query q2;
        check("IMPORT CSV: parse succeeds", q2.parse("IMPORT CSV 'graph.csv'"));
        check("IMPORT CSV: operation is Import", q2.operation() == QueryOperation::Import);
        check("IMPORT CSV: path extracted", q2.filePath() == "graph.csv");

        // keyword is case-insensitive like every other keyword
        Query q3;
        check("import csv (lowercase) parses",
            q3.parse("import csv 'g.csv'") && q3.operation() == QueryOperation::Import);

        // malformed forms are rejected
        Query q4;
        check("EXPORT no CSV keyword: parse fails", !q4.parse("EXPORT 'graph.csv'"));
        Query q5;
        check("IMPORT CSV no path: parse fails", !q5.parse("IMPORT CSV"));
        Query q6;
        check("EXPORT CSV empty path: parse fails", !q6.parse("EXPORT CSV ''"));
        Query q7;
        check("IMPORT CSV trailing tokens: parse fails",
            !q7.parse("IMPORT CSV 'g.csv' EXTRA"));

        // Executed against a bare Graph (no StorageEngine), CSV ops report that a
        // coordinator is required -- they never touch disk from the Query layer.
        Graph g;
        Query q8;
        QueryResult r = q8.run("EXPORT CSV 'graph.csv'", g);
        check("EXPORT CSV bare-graph execute: reports failure", !r.success);
        check("EXPORT CSV bare-graph execute: mentions coordinator",
            r.message.find("coordinator") != std::string::npos);
    }

    // -- EXPORT CSV then IMPORT CSV round-trip through StorageEngine -----------
    {
        const std::string path = "test_csv_roundtrip.csv";
        Graph original;
        NodeId a = original.CreateNode("Person", { {"name", "Alice"} });
        NodeId b = original.CreateNode("City", { {"name", "Sydney"} });
        int warning = operationSuccessful;
        original.CreateEdge(a, b, "LIVES_IN", warning);

        // parse EXPORT and drive the engine with the parsed path
        Query exportQ;
        check("csv round-trip: EXPORT parses",
            exportQ.parse("EXPORT CSV '" + path + "'") &&
            exportQ.operation() == QueryOperation::Export);
        StorageEngine exportEngine(exportQ.filePath());
        check("csv round-trip: StorageEngine.ExportCSV succeeds",
            exportEngine.ExportCSV(original, exportQ.filePath()));

        // parse IMPORT and rebuild a fresh graph
        Query importQ;
        check("csv round-trip: IMPORT parses",
            importQ.parse("IMPORT CSV '" + path + "'") &&
            importQ.operation() == QueryOperation::Import);
        Graph restored;
        StorageEngine importEngine(importQ.filePath());
        check("csv round-trip: StorageEngine.ImportCSV succeeds",
            importEngine.ImportCSV(restored, importQ.filePath()));

        std::vector<NodeId> ids;
        restored.GetNodeIdOrder(ids);
        check("csv round-trip: node count preserved", ids.size() == 2);
        std::vector<EdgeId> eids;
        restored.GetAllEdgeIds(eids);
        check("csv round-trip: edge count preserved", eids.size() == 1);
        std::remove(path.c_str());
    }

    std::cout << "\n" << pass << " passed, " << fail << " failed (cumulative).\n";
    return fail == 0 ? 0 : 1;
}

// Public entry point: prints the banner and runs both halves of the query-layer
// suite plus the SELECT ... SNAPSHOT and UPDATE / LOAD / SAVE suites. Returns
// non-zero if any recorded a failure.
int test_query_layer() {
    std::cout << "\n==================== QUERY LAYER ====================\n";
    int rc1 = test_query_layer_part1();
    int rc2 = test_query_layer_part2();
    int rc3 = test_select_snapshot();
    int rc4 = test_update_load_save();
    return (rc1 == 0 && rc2 == 0 && rc3 == 0 && rc4 == 0) ? 0 : 1;
}

// =====================================================================
// StorageEngine test suite
// =====================================================================
//
// Exercises the full public surface of StorageEngine:
//   Save / Load   -- binary persistence round-trip + content fidelity
//   ValidateGraph -- node/edge count integrity check (post-Load)
//   ImportCSV     -- build a graph from a CSV description
//   ExportCSV     -- serialise a graph to CSV
//   Export->Import round-trip -- the two CSV halves should compose
//
// All artefacts are written to temporary files in the current working
// directory and removed at the end of the test.

// Helper: build the small, deterministic graph shared by the storage tests.
//   node 0: Person { name=Alice, age=30 }
//   node 1: Person { name=Bob,   age=25 }
//   node 2: City   { name=Sydney }
//   edge  : 0 --KNOWS-->    1
//   edge  : 1 --LIVES_IN--> 2
// (Node/edge ids are ephemeral; the storage format references nodes by their
// 0-based insertion index, which is what the index comments above refer to.)
static void buildStorageFixture(Graph& graph) {
    NodeId alice = graph.CreateNode("Person", { {"name", "Alice"}, {"age", "30"} });
    NodeId bob = graph.CreateNode("Person", { {"name", "Bob"},   {"age", "25"} });
    NodeId city = graph.CreateNode("City", { {"name", "Sydney"} });

    int warning = operationSuccessful;
    graph.CreateEdge(alice, bob, "KNOWS", warning);
    graph.CreateEdge(bob, city, "LIVES_IN", warning);
}

int test_storage_engine() {
    std::cout << "\n=================== STORAGE ENGINE ===================\n";

    const std::string binPath = "se_test_graph.bin";
    const std::string csvInPath = "se_test_import.csv";
    const std::string csvOutPath = "se_test_export.csv";

    // -----------------------------------------------------------------
    // 1. Binary Save + Load round-trip and content fidelity
    // -----------------------------------------------------------------
    std::cout << "\n== Save / Load (binary) round-trip ==\n";
    {
        Graph original;
        buildStorageFixture(original);

        StorageEngine engine(binPath);
        check("Save: returns true", engine.Save(original));

        Graph loaded;
        check("Load: returns true", engine.Load(loaded));

        std::vector<NodeId> nodeIds;
        loaded.GetNodeIdOrder(nodeIds);
        std::vector<EdgeId> edgeIds;
        loaded.GetAllEdgeIds(edgeIds);

        check("Load: 3 nodes restored", nodeIds.size() == 3);
        check("Load: 2 edges restored", edgeIds.size() == 2);

        // Node content is preserved, and so is insertion order, so index 0 is
        // Alice and index 2 is the City node.
        Node n0, n2;
        bool got0 = nodeIds.size() == 3 && loaded.GetNode(nodeIds[0], n0);
        bool got2 = nodeIds.size() == 3 && loaded.GetNode(nodeIds[2], n2);
        check("Load: node 0 label preserved", got0 && n0.label == "Person");
        check("Load: node 0 name preserved", got0 && n0.properties.at("name") == "Alice");
        check("Load: node 0 age preserved", got0 && n0.properties.at("age") == "30");
        check("Load: node 2 label preserved", got2 && n2.label == "City");
        check("Load: node 2 name preserved", got2 && n2.properties.at("name") == "Sydney");

        // Edge content is preserved, including endpoints (by relative position).
        int warning = operationSuccessful;
        std::vector<Edge> knows;
        loaded.FindEdgesByLabel(knows, "KNOWS", warning);
        check("Load: KNOWS edge restored",
            warning == operationSuccessful && knows.size() == 1);
        check("Load: KNOWS endpoints preserved",
            knows.size() == 1 && nodeIds.size() == 3 &&
            knows[0].from == nodeIds[0] && knows[0].to == nodeIds[1]);

        // -------------------------------------------------------------
        // 2. ValidateGraph (uses the counts captured during Load)
        // -------------------------------------------------------------
        std::cout << "\n== ValidateGraph ==\n";
        check("ValidateGraph: matches loaded graph", engine.ValidateGraph(loaded));

        // Mutating the graph so its counts no longer match the file's header
        // must be detected.
        loaded.CreateNode("Extra", {});
        check("ValidateGraph: detects count mismatch", !engine.ValidateGraph(loaded));
    }

    // -----------------------------------------------------------------
    // 3. Load failure path (missing / invalid file)
    // -----------------------------------------------------------------
    std::cout << "\n== Load failure handling ==\n";
    {
        StorageEngine missing("se_nonexistent_file_zzz.bin");
        Graph g;
        check("Load: fails cleanly on missing file", !missing.Load(g));
    }

    // -----------------------------------------------------------------
    // 4. ImportCSV from a well-formed CSV description
    // -----------------------------------------------------------------
    // Documented CSV format: node lines "Label,key=value,...", then a blank
    // line, then edge lines "fromIndex,toIndex,label".
    std::cout << "\n== ImportCSV ==\n";
    {
        {
            std::ofstream csv(csvInPath);
            csv << "Person,name=Alice,age=30\n";
            csv << "Person,name=Bob,age=25\n";
            csv << "City,name=Sydney\n";
            csv << "\n";                    // blank line separates nodes from edges
            csv << "0,1,KNOWS\n";
            csv << "1,2,LIVES_IN\n";
        }

        Graph imported;
        StorageEngine engine(csvInPath);
        check("ImportCSV: returns true", engine.ImportCSV(imported, csvInPath));

        std::vector<NodeId> nodeIds;
        imported.GetNodeIdOrder(nodeIds);
        std::vector<EdgeId> edgeIds;
        imported.GetAllEdgeIds(edgeIds);
        check("ImportCSV: 3 nodes created", nodeIds.size() == 3);
        check("ImportCSV: 2 edges created", edgeIds.size() == 2);

        Node n0;
        bool got0 = nodeIds.size() == 3 && imported.GetNode(nodeIds[0], n0);
        check("ImportCSV: node label parsed", got0 && n0.label == "Person");
        check("ImportCSV: node property parsed", got0 && n0.properties.at("name") == "Alice");

        int warning = operationSuccessful;
        std::vector<Edge> lives;
        imported.FindEdgesByLabel(lives, "LIVES_IN", warning);
        check("ImportCSV: edge label parsed",
            warning == operationSuccessful && lives.size() == 1);
        check("ImportCSV: edge endpoints parsed",
            lives.size() == 1 && nodeIds.size() == 3 &&
            lives[0].from == nodeIds[1] && lives[0].to == nodeIds[2]);
    }

    // -----------------------------------------------------------------
    // 5. ExportCSV serialises graph content
    // -----------------------------------------------------------------
    std::cout << "\n== ExportCSV ==\n";
    {
        Graph original;
        buildStorageFixture(original);

        StorageEngine engine(csvOutPath);
        check("ExportCSV: returns true", engine.ExportCSV(original, csvOutPath));

        std::ifstream in(csvOutPath);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string content = buffer.str();

        check("ExportCSV: node label written", content.find("Person") != std::string::npos);
        check("ExportCSV: node property written", content.find("name=Alice") != std::string::npos);
        check("ExportCSV: KNOWS edge written", content.find("0,1,KNOWS") != std::string::npos);
        check("ExportCSV: LIVES_IN edge written", content.find("1,2,LIVES_IN") != std::string::npos);
    }

    // -----------------------------------------------------------------
    // 6. ExportCSV -> ImportCSV round-trip (intended behaviour)
    // -----------------------------------------------------------------
    // A graph exported to CSV should import back to an equivalent graph.
    std::cout << "\n== ExportCSV -> ImportCSV round-trip ==\n";
    {
        Graph reimported;
        StorageEngine engine(csvOutPath);
        bool ok = engine.ImportCSV(reimported, csvOutPath);

        std::vector<NodeId> nodeIds;
        reimported.GetNodeIdOrder(nodeIds);
        std::vector<EdgeId> edgeIds;
        reimported.GetAllEdgeIds(edgeIds);
        check("Round-trip: 3 nodes", ok && nodeIds.size() == 3);
        check("Round-trip: 2 edges", ok && edgeIds.size() == 2);
    }

    // Clean up temporary artefacts.
    std::remove(binPath.c_str());
    std::remove(csvInPath.c_str());
    std::remove(csvOutPath.c_str());

    std::cout << "\n" << pass << " passed, " << fail << " failed (cumulative).\n";
    return fail == 0 ? 0 : 1;
}

// ============================================================================
// test_graph_handler_concurrent
// ============================================================================
// Tests GraphHandler's thread-safety guarantees with concurrent readers and
// writers. Verifies:
//   1. Multiple threads can execute SELECT queries concurrently (shared_lock)
//   2. INSERT operations serialize correctly under the exclusive lock
//   3. Snapshots are created/released safely without data races
//   4. Snapshot isolation: a snapshot's frozen membership is unaffected by
//      later live mutations (SELECT ... SNAPSHOT resolves against MVCC history)
//
// Query grammar used here matches the parser: a live SELECT requires a WHERE
// clause (WHERE ID/LABEL), a snapshot SELECT may be an unfiltered full scan via
// the trailing SNAPSHOT keyword, and INSERT NODES must supply a label column.
// ============================================================================

int test_graph_handler_concurrent() {
    std::cout << "\n== GraphHandler Concurrent Access ==\n";

    int pass_count = 0, fail_count = 0;

    auto check = [&](const std::string& name, bool condition) {
        std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << name << "\n";
        if (condition) ++pass_count; else ++fail_count;
        };

    // Setup: create a graph with initial data (3 Person nodes, 2 KNOWS edges)
    auto graph = std::make_unique<Graph>();
    NodeId alice = graph->CreateNode("Person", { {"name", "Alice"}, {"age", "30"} });
    NodeId bob = graph->CreateNode("Person", { {"name", "Bob"},   {"age", "25"} });
    NodeId carol = graph->CreateNode("Person", { {"name", "Carol"}, {"age", "28"} });

    int warning = operationSuccessful;
    graph->CreateEdge(alice, bob, "KNOWS", warning);
    graph->CreateEdge(bob, carol, "KNOWS", warning);

    // Create GraphHandler that owns the graph
    auto handler = std::make_unique<GraphHandler>(std::move(graph));

    // ------------------------------------------------------------------------
    // Test 1: many concurrent readers (shared_lock lets them all proceed)
    // ------------------------------------------------------------------------
    std::cout << "\n--- Test 1: Concurrent SELECT queries ---\n";
    {
        const int kReaders = 8;
        std::vector<std::thread> threads;
        std::atomic<int> select_success_count(0);

        for (int i = 0; i < kReaders; ++i) {
            threads.emplace_back([&]() {
                // Live SELECT requires a WHERE clause; label filter returns all 3 Persons.
                QueryResult result =
                    handler->executeQueryLive("SELECT * FROM NODES WHERE LABEL = 'Person'");
                if (result.success && result.nodes.size() == 3) {
                    select_success_count++;
                }
                });
        }

        for (auto& t : threads) t.join();

        check("8 concurrent SELECT queries all succeeded",
            select_success_count == kReaders);
    }

    // ------------------------------------------------------------------------
    // Test 2: a writer under the exclusive lock mutates the graph correctly
    // ------------------------------------------------------------------------
    std::cout << "\n--- Test 2: Exclusive-lock write ---\n";
    {
        QueryResult ins = handler->executeQueryLive(
            "INSERT INTO NODES (label, name) VALUES ('Person', 'Dave')");
        check("INSERT succeeded", ins.success);
        check("Node count is 4 after INSERT", handler->getNodeCount() == 4);
    }

    // ------------------------------------------------------------------------
    // Test 3: snapshot isolation -- a snapshot's membership is frozen
    // ------------------------------------------------------------------------
    std::cout << "\n--- Test 3: Snapshot isolation ---\n";
    {
        // Snapshot the current state (4 Person nodes).
        uint64_t snapshot_id = handler->createSnapshot();
        check("Snapshot created (id > 0)", snapshot_id > 0);
        check("Active snapshot count = 1", handler->activeSnapshotCount() == 1);

        // Mutate the live graph after the snapshot was taken.
        QueryResult ins = handler->executeQueryLive(
            "INSERT INTO NODES (label, name) VALUES ('Person', 'Eve')");
        check("INSERT into live view succeeded", ins.success);
        check("Live view now has 5 nodes", handler->getNodeCount() == 5);

        // Snapshot SELECT is an unfiltered full scan via the SNAPSHOT keyword.
        // It must still see the frozen 4-node membership, not the live 5.
        QueryResult snap =
            handler->executeQuerySnapshot(snapshot_id, "SELECT * FROM NODES SNAPSHOT");
        check("Snapshot SELECT succeeded", snap.success);
        check("Snapshot still sees 4 nodes (frozen)", snap.nodes.size() == 4);

        handler->releaseSnapshot(snapshot_id);
        check("Snapshot released, active count = 0",
            handler->activeSnapshotCount() == 0);
    }

    // ------------------------------------------------------------------------
    // Test 4: concurrent snapshot creation is race-free
    // ------------------------------------------------------------------------
    std::cout << "\n--- Test 4: Concurrent snapshot creation ---\n";
    {
        const int kSnaps = 4;
        std::vector<std::thread> threads;
        std::vector<uint64_t> snapshot_ids;
        std::mutex snap_mutex;

        for (int i = 0; i < kSnaps; ++i) {
            threads.emplace_back([&]() {
                uint64_t id = handler->createSnapshot();
                std::lock_guard<std::mutex> lock(snap_mutex);
                snapshot_ids.push_back(id);
                });
        }

        for (auto& t : threads) t.join();

        check("4 snapshots created concurrently", snapshot_ids.size() == kSnaps);
        check("Active snapshot count = 4", handler->activeSnapshotCount() == kSnaps);

        // Every id must be unique (no two threads got the same snapshot id).
        std::sort(snapshot_ids.begin(), snapshot_ids.end());
        bool unique = std::adjacent_find(snapshot_ids.begin(),
            snapshot_ids.end()) == snapshot_ids.end();
        check("All snapshot ids are unique", unique);

        for (uint64_t id : snapshot_ids) handler->releaseSnapshot(id);
        check("All snapshots released, count = 0",
            handler->activeSnapshotCount() == 0);
    }

    // ------------------------------------------------------------------------
    // Test 5: concurrent read-only getters (all take the shared lock)
    // ------------------------------------------------------------------------
    std::cout << "\n--- Test 5: Concurrent property getters ---\n";
    {
        const int kThreads = 8;
        std::vector<std::thread> threads;
        std::atomic<int> getter_success(0);

        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&]() {
                size_t nodes = handler->getNodeCount();
                size_t edges = handler->getEdgeCount();
                uint64_t version = handler->getGraphVersion();
                if (nodes == 5 && edges == 2 && version > 0) {
                    getter_success++;
                }
                });
        }

        for (auto& t : threads) t.join();

        check("8 concurrent getters saw a consistent snapshot",
            getter_success == kThreads);
    }

    // ------------------------------------------------------------------------
    // Test 6: mixed readers and writers under contention
    // ------------------------------------------------------------------------
    std::cout << "\n--- Test 6: Mixed concurrent operations ---\n";
    {
        const int kReaders = 10, kWriters = 3;
        std::vector<std::thread> threads;
        std::atomic<int> reader_count(0), writer_count(0);

        for (int i = 0; i < kReaders; ++i) {
            threads.emplace_back([&]() {
                QueryResult r =
                    handler->executeQueryLive("SELECT * FROM NODES WHERE LABEL = 'Person'");
                if (r.success) reader_count++;
                });
        }

        for (int i = 0; i < kWriters; ++i) {
            threads.emplace_back([&, i]() {
                QueryResult r = handler->executeQueryLive(
                    std::string("INSERT INTO NODES (label, name) VALUES ('Person', 'W")
                    + std::to_string(i) + "')");
                if (r.success) writer_count++;
                });
        }

        for (auto& t : threads) t.join();

        check("All 10 readers succeeded", reader_count == kReaders);
        check("All 3 writers succeeded", writer_count == kWriters);
        // Started this test with 5 nodes, added 3 writers => 8.
        check("Node count is 8 after 3 concurrent inserts",
            handler->getNodeCount() == 8);
    }

    std::cout << "\n== GraphHandler Concurrent Results ==\n";
    std::cout << "Pass: " << pass_count << ", Fail: " << fail_count << "\n";

    return fail_count;
}

// ============================================================================
// test_graph_handler_robust_concurrency
// ============================================================================
// Turns the claim "GraphHandler is safe for multi-user concurrent access" into
// evidence, rather than an assertion. It runs several stress scenarios against a
// SINGLE shared handler -- each thread is a simulated user hammering the same
// handler, which exercises the exact shared_mutex path a network server would.
//
// The scenarios, and what each proves:
//   1. Disjoint-quota writers (data-corruption check). N threads each insert a
//      fixed quota of nodes under a label unique to that thread. Afterwards the
//      EXACT expected counts must hold -- any lost or torn write breaks the
//      arithmetic. This is the corruption test; heavy oversubscription of threads
//      maximises the odd interleavings where a locking bug would show.
//   2. Concurrent readers during writes (torn-read check). While writers insert,
//      readers query continuously and must never crash, never fail, and never see
//      a malformed result.
//   3. Snapshot lifecycle under contention (the most race-prone handler path).
//      Threads create and release snapshots while writers mutate; every create
//      must be released and the active count must return to zero.
//   4. Multi-user session simulation (plausibility). A realistic number of
//      threads each run a mixed read/write "session"; all must get valid results.
//
// Repetition matters: races are probabilistic, so the corruption scenario runs
// many rounds. A single green run proves little; many green runs under heavy
// oversubscription is real evidence. Build under a thread sanitizer to also catch
// races that did not happen to manifest as a visible failure this run.
// ============================================================================
int test_graph_handler_robust_concurrency() {
    std::cout << "\n== GraphHandler Robust Concurrency (stress) ==\n";

    int pass_count = 0, fail_count = 0;
    auto check = [&](const std::string& name, bool condition) {
        std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << name << "\n";
        if (condition) ++pass_count; else ++fail_count;
        };

    // Scale thread counts to the machine. hardware_concurrency() may report 0 on
    // some platforms, so fall back to a sane default.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    // ------------------------------------------------------------------------
    // Scenario 1: disjoint-quota writers -> exact-count corruption check.
    // Heavy oversubscription (many more threads than cores) to force contention.
    // ------------------------------------------------------------------------
    std::cout << "\n--- Scenario 1: Disjoint-quota concurrent writers (corruption) ---\n";
    {
        // Heavy oversubscription (writers >> cores) to force odd interleavings,
        // repeated many rounds because races are probabilistic -- but bounded so
        // total work stays reasonable on any machine. Tune up for a longer soak.
        const int kRounds = 10;                 // repeat: races are probabilistic
        const int kWriters = (int)hw * 8;       // heavy oversubscription
        const int kQuota = 15;                  // nodes each writer inserts

        bool allRoundsOk = true;
        for (int round = 0; round < kRounds && allRoundsOk; ++round) {
            // Fresh handler each round so the expected counts are exact.
            auto handler = std::make_unique<GraphHandler>(std::make_unique<Graph>());

            std::vector<std::thread> threads;
            threads.reserve(kWriters);
            for (int w = 0; w < kWriters; ++w) {
                threads.emplace_back([&handler, w, kQuota]() {
                    // Each writer uses a label only it uses: "W<w>". So the final
                    // per-label counts are deterministic and independently checkable.
                    const std::string label = "W" + std::to_string(w);
                    for (int q = 0; q < kQuota; ++q) {
                        handler->executeQueryLive(
                            "INSERT INTO NODES (label, name) VALUES ('" + label +
                            "', 'n" + std::to_string(q) + "')");
                    }
                    });
            }
            for (auto& t : threads) t.join();

            // Total must be exactly writers * quota -- no lost/torn writes.
            const size_t expectedTotal = (size_t)kWriters * kQuota;
            if (handler->getNodeCount() != expectedTotal) {
                allRoundsOk = false;
                break;
            }
            // Every thread's label must have exactly its quota (spot-check a few,
            // since checking all kWriters*rounds would dominate runtime). COUNT is
            // the natural tool here.
            for (int w = 0; w < kWriters; w += (kWriters / 8 + 1)) {
                QueryResult r = handler->executeQueryLive(
                    "SELECT COUNT * FROM NODES WHERE LABEL = 'W" + std::to_string(w) + "'");
                if (!r.success || r.message != "Count: " + std::to_string(kQuota)) {
                    allRoundsOk = false;
                    break;
                }
            }
        }
        check("disjoint-quota writers: exact node totals over all rounds (no lost writes)",
            allRoundsOk);
    }

    // ------------------------------------------------------------------------
    // Scenario 2: concurrent readers during writes -> torn-read check.
    // ------------------------------------------------------------------------
    std::cout << "\n--- Scenario 2: Concurrent readers during writes (torn reads) ---\n";
    {
        auto handler = std::make_unique<GraphHandler>(std::make_unique<Graph>());
        // Seed some data so readers have something to read.
        for (int i = 0; i < 50; ++i) {
            handler->executeQueryLive(
                "INSERT INTO NODES (label, name) VALUES ('Seed', 'n" + std::to_string(i) + "')");
        }

        std::atomic<bool> stop(false);
        std::atomic<bool> readerFault(false);   // set if a reader ever sees garbage
        std::atomic<long> readCount(0);

        const int kReaders = (int)hw * 4;
        const int kWriters = (int)hw * 2;

        std::vector<std::thread> threads;
        // Readers: query continuously until told to stop; assert well-formedness.
        // IMPORTANT: yield after each read. std::shared_mutex gives NO writer
        // priority, so readers spinning with zero pause can starve the writers
        // indefinitely -- especially when readers outnumber cores. A yield lets a
        // waiting writer acquire the exclusive lock, so the finite writer work
        // actually completes. (Writer starvation under unthrottled readers is a
        // real shared_mutex property, not a handler bug; we simply don't let the
        // test provoke a livelock.)
        for (int i = 0; i < kReaders; ++i) {
            threads.emplace_back([&]() {
                while (!stop.load()) {
                    QueryResult r =
                        handler->executeQueryLive("SELECT * FROM NODES WHERE LABEL = 'Seed'");
                    // A read must succeed and never return more Seed rows than exist
                    // (50), nor a nonsense negative -- a torn read would violate this.
                    if (!r.success || r.nodes.size() > 50) readerFault.store(true);
                    readCount.fetch_add(1);
                    std::this_thread::yield();   // give waiting writers a turn
                }
                });
        }
        // Writers: add non-Seed nodes (so the Seed count the readers check stays 50).
        for (int i = 0; i < kWriters; ++i) {
            threads.emplace_back([&, i]() {
                for (int q = 0; q < 200; ++q) {
                    handler->executeQueryLive(
                        "INSERT INTO NODES (label, name) VALUES ('Other', 'w" +
                        std::to_string(i) + "_" + std::to_string(q) + "')");
                }
                });
        }
        // Let writers finish, then stop readers.
        // (Writers are the finite work; readers spin until stop.)
        for (int i = kReaders; i < kReaders + kWriters; ++i) threads[i].join();
        stop.store(true);
        for (int i = 0; i < kReaders; ++i) threads[i].join();

        check("readers never saw a torn/failed result during concurrent writes",
            !readerFault.load());
        check("readers actually ran (sanity: read count > 0)", readCount.load() > 0);
    }

    // ------------------------------------------------------------------------
    // Scenario 3: snapshot lifecycle under contention.
    // ------------------------------------------------------------------------
    std::cout << "\n--- Scenario 3: Snapshot create/release under contention ---\n";
    {
        auto handler = std::make_unique<GraphHandler>(std::make_unique<Graph>());
        for (int i = 0; i < 30; ++i) {
            handler->executeQueryLive(
                "INSERT INTO NODES (label, name) VALUES ('S', 'n" + std::to_string(i) + "')");
        }

        const int kSnapThreads = (int)hw * 4;
        std::vector<std::thread> threads;
        std::atomic<bool> fault(false);

        // Each thread repeatedly creates then releases a snapshot. Every create is
        // paired with its own release, so the active count must return to 0.
        for (int i = 0; i < kSnapThreads; ++i) {
            threads.emplace_back([&]() {
                for (int r = 0; r < 20; ++r) {
                    uint64_t id = handler->createSnapshot();
                    if (id == 0) { fault.store(true); return; }
                    // A snapshot read must not crash or fail.
                    QueryResult q =
                        handler->executeQuerySnapshot(id, "SELECT COUNT * FROM NODES SNAPSHOT");
                    if (!q.success) fault.store(true);
                    handler->releaseSnapshot(id);
                }
                });
        }
        // Meanwhile, a couple of writers mutate the live graph.
        for (int i = 0; i < (int)hw; ++i) {
            threads.emplace_back([&, i]() {
                for (int q = 0; q < 50; ++q) {
                    handler->executeQueryLive(
                        "INSERT INTO NODES (label, name) VALUES ('S', 'x" +
                        std::to_string(i) + "_" + std::to_string(q) + "')");
                }
                });
        }
        for (auto& t : threads) t.join();

        check("snapshot create/release under contention: no fault", !fault.load());
        check("all snapshots released, active count returned to 0",
            handler->activeSnapshotCount() == 0);
    }

    // ------------------------------------------------------------------------
    // Scenario 4: multi-user session plausibility.
    // Moderate oversubscription; each thread is a "user" doing a mixed session.
    // ------------------------------------------------------------------------
    std::cout << "\n--- Scenario 4: Multi-user mixed sessions ---\n";
    {
        auto handler = std::make_unique<GraphHandler>(std::make_unique<Graph>());
        const int kUsers = (int)hw * 3;         // realistic busy-team overlap
        std::atomic<bool> sessionFault(false);

        std::vector<std::thread> threads;
        for (int u = 0; u < kUsers; ++u) {
            threads.emplace_back([&, u]() {
                const std::string label = "U" + std::to_string(u);
                // A mixed session: a few inserts, a few reads, a count.
                for (int i = 0; i < 10; ++i) {
                    QueryResult ins = handler->executeQueryLive(
                        "INSERT INTO NODES (label, name) VALUES ('" + label +
                        "', 'n" + std::to_string(i) + "')");
                    if (!ins.success) sessionFault.store(true);
                }
                QueryResult sel = handler->executeQueryLive(
                    "SELECT * FROM NODES WHERE LABEL = '" + label + "'");
                // Each user must see exactly its own 10 inserts (its label is unique).
                if (!sel.success || sel.nodes.size() != 10) sessionFault.store(true);
                QueryResult cnt = handler->executeQueryLive(
                    "SELECT COUNT * FROM NODES WHERE LABEL = '" + label + "'");
                if (!cnt.success || cnt.message != "Count: 10") sessionFault.store(true);
                });
        }
        for (auto& t : threads) t.join();

        check("every user session got correct, isolated results", !sessionFault.load());
        check("total nodes == users * 10 (all sessions committed)",
            handler->getNodeCount() == (size_t)kUsers * 10);
    }

    std::cout << "\n== Robust Concurrency Results ==\n";
    std::cout << "Pass: " << pass_count << ", Fail: " << fail_count << "\n";
    return fail_count;
}

// ============================================================================
// test_graph_handler_performance
// ============================================================================
// Measures how GraphHandler performance SCALES WITH CPU CORES, separately for
// reads, writes, and updates. Design choices that make the numbers trustworthy
// on any machine (including a modest laptop):
//
//   * CORE-BOUNDED THREADS. Thread counts are swept 1..core-count and never past.
//     Beyond core-count you measure the OS scheduler time-slicing threads onto
//     cores that don't exist -- noise, not scaling. Every point at/below cores is
//     a real parallelism measurement.
//   * FIXED-TIME BUDGET (count ops done), not fixed op-count. Can never hang -- a
//     slow config just completes fewer ops in the same window -- and it adapts to
//     the hardware automatically.
//   * SEPARATE PURE SWEEPS for READ / WRITE / UPDATE. Each op type is measured in
//     isolation so its scaling curve is attributable to one lock path; a mixed
//     workload would blur them.
//   * SPEEDUP vs 1 thread is the headline metric (throughput_N / throughput_1).
//     Dimensionless, so a slow laptop and a fast server compare directly: reads
//     should scale toward the core count (shared locks run in parallel), writes
//     and updates should stay ~1.0x (the exclusive lock serialises them -- flat
//     is CORRECT and confirms the lock works). The 1-thread p50 is the
//     uncontended per-op latency (raw cost of one operation).
//   * STEADY STATE. Reads look up by id; write churn inserts+deletes; update edits
//     in place -- graph size stays fixed, so work per op is constant no matter how
//     long the run lasts. (An earlier version grew the graph via INSERT, which
//     inflated later ops.)
//
// Parsing is INCLUDED in every measured op by design: executeQueryLive is the
// real entry point, so this is END-TO-END COMMAND scaling. Parsing is lock-free
// CPU that scales perfectly, so it lifts every curve somewhat toward linear -- a
// flat-ish write curve reflects the exclusive lock under the parallel parse cost.
//
// Prints tables (a measurement, not pass/fail); gated behind test_performance,
// returns 0.
// ============================================================================
int test_graph_handler_performance() {
    std::cout << "\n== GraphHandler Performance (core scaling) ==\n";
    // ------------------------------------------------------------------------
    // Design (see also the block comment above):
    //   * Threads swept 1..cores ONLY (past cores = scheduler noise, not scaling).
    //   * Fixed-TIME budget per point (count ops done) -> never hangs, adapts to hw.
    //   * SEPARATE pure sweeps for READ / WRITE / UPDATE so each lock path's curve
    //     is attributable.
    //   * SPEEDUP vs 1 thread is the headline: reads should climb toward core count
    //     (shared lock, parallel), writes/updates should stay ~1.0x (exclusive lock
    //     serialises -- flat is CORRECT). The 1-thread p50 is the uncontended cost.
    //   * STEADY STATE: reads look up by id; the write churn inserts+deletes; update
    //     edits in place -- graph size stays fixed so per-op work is constant.
    //   * Parsing is included (executeQueryLive is the real entry point); it is
    //     lock-free CPU, so it lifts every curve somewhat toward linear.
    // ------------------------------------------------------------------------

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    // Thread counts: 1, 2, 4, ... doubling, ALWAYS ending exactly at the core
    // count. Never exceed cores.
    std::vector<int> threadCounts;
    for (int t = 1; t < (int)hw; t *= 2) threadCounts.push_back(t);
    threadCounts.push_back((int)hw);

    const double budgetSec = 1.5;              // timed window per point
    const double warmupSec = 0.3;              // untimed warm-up per point
    const int kSeed = 2000;                    // fixed benchmark graph size (ids 1..kSeed)

    // Seed a fresh handler with kSeed labelled nodes and one pre-made snapshot.
    auto makeSeededHandler = [&](uint64_t& snapIdOut) {
        auto handler = std::make_unique<GraphHandler>(std::make_unique<Graph>());
        for (int i = 0; i < kSeed; ++i) {
            handler->executeQueryLive(
                "INSERT INTO NODES (label, name, age) VALUES ('Bench', 'n" +
                std::to_string(i) + "', '30')");
        }
        snapIdOut = handler->createSnapshot();  // snapshot reads target this id
        return handler;
        };

    // Percentile helper (expects a sorted vector).
    auto pct = [](std::vector<long>& sorted, double p) -> long {
        if (sorted.empty()) return 0;
        size_t idx = (size_t)(p * (sorted.size() - 1));
        return sorted[idx];
        };

    // Run one op-type sweep. `doOp` performs exactly one operation of the type
    // under test (parsing included). Prints a row per thread count with throughput,
    // speedup-vs-1-thread, and latency percentiles.
    auto sweepOpType = [&](const char* opName,
        const std::function<void(GraphHandler&, uint64_t, int, long)>& doOp) {
            std::cout << "\n--- " << opName << " ---\n";
            std::cout << "  threads |    ops |  seconds |    ops/sec | speedup |  p50us |  p95us |  p99us\n";
            std::cout << "  --------+--------+----------+------------+---------+--------+--------+-------\n";

            double baseOpsPerSec = 0.0;         // 1-thread throughput, for speedup

            for (int threads : threadCounts) {
                uint64_t snapId = 0;
                auto handler = makeSeededHandler(snapId);

                std::atomic<bool> stop(false);
                std::atomic<long> counter(0);   // unique op index across threads
                std::atomic<long> completed(0);
                std::vector<std::vector<long>> perThread(threads);

                auto worker = [&](int tid) {
                    std::vector<long>& lat = perThread[tid];
                    long localDone = 0;
                    while (!stop.load(std::memory_order_relaxed)) {
                        long op = counter.fetch_add(1, std::memory_order_relaxed);
                        auto t0 = std::chrono::steady_clock::now();
                        doOp(*handler, snapId, tid, op);
                        auto t1 = std::chrono::steady_clock::now();
                        lat.push_back((long)std::chrono::duration_cast<
                            std::chrono::microseconds>(t1 - t0).count());
                        ++localDone;
                    }
                    completed.fetch_add(localDone, std::memory_order_relaxed);
                    };

                // Warm-up (untimed): let caches / allocations settle.
                {
                    std::vector<std::thread> wts;
                    for (int t = 0; t < threads; ++t)
                        wts.emplace_back([&]() {
                        auto until = std::chrono::steady_clock::now() +
                            std::chrono::duration<double>(warmupSec);
                        long o = 0;
                        while (std::chrono::steady_clock::now() < until)
                            doOp(*handler, snapId, 0, o++);
                            });
                    for (auto& t : wts) t.join();
                }

                // Timed run: launch, sleep the budget, stop, join.
                auto start = std::chrono::steady_clock::now();
                std::vector<std::thread> ts;
                for (int t = 0; t < threads; ++t) ts.emplace_back(worker, t);
                std::this_thread::sleep_for(std::chrono::duration<double>(budgetSec));
                stop.store(true, std::memory_order_relaxed);
                for (auto& t : ts) t.join();
                auto end = std::chrono::steady_clock::now();

                double secs = std::chrono::duration<double>(end - start).count();
                long ops = completed.load();
                double opsPerSec = secs > 0 ? (double)ops / secs : 0.0;
                if (threads == 1) baseOpsPerSec = opsPerSec;
                double speedup = baseOpsPerSec > 0 ? opsPerSec / baseOpsPerSec : 0.0;

                std::vector<long> lat;
                for (auto& v : perThread) lat.insert(lat.end(), v.begin(), v.end());
                std::sort(lat.begin(), lat.end());

                std::printf("  %7d | %6ld | %8.3f | %10.0f | %6.2fx | %6ld | %6ld | %6ld\n",
                    threads, ops, secs, opsPerSec, speedup,
                    pct(lat, 0.50), pct(lat, 0.95), pct(lat, 0.99));
            }
        };

    // READS -- shared lock; a deterministic variety by op index. Should scale.
    sweepOpType("READ variety (shared lock -- should scale toward core count)",
        [&](GraphHandler& h, uint64_t snapId, int, long op) {
            int id = 1 + (int)(op % kSeed);
            switch (op % 5) {
            case 0:
                h.executeQueryLive("SELECT * FROM NODES WHERE ID = " + std::to_string(id));
                break;
            case 1:
                h.executeQueryLive("SELECT * FROM NODES WHERE LABEL = 'Bench'");
                break;
            case 2:
                h.executeQueryLive("SELECT COUNT * FROM NODES WHERE LABEL = 'Bench'");
                break;
            case 3:
                h.executeQueryLive("MATCH KHOP FROM " + std::to_string(id) + " STEPS 1");
                break;
            case 4:
                h.executeQuerySnapshot(snapId,
                    "SELECT * FROM NODES WHERE ID = " + std::to_string(id) + " SNAPSHOT");
                break;
            }
        });

    // WRITES -- INSERT then DELETE the same node by id, keeping the graph size
    // constant (steady state) while exercising the exclusive-lock write path.
    // Ids are sequential from 1, and the seed used ids 1..kSeed, so a fresh INSERT
    // gets a new id; DELETE removes it right back. We recover that id from the
    // handler's node count (which is kSeed while balanced, so the new node's id is
    // the current max). To stay robust we delete by the name we just inserted via
    // an id lookup is unavailable through the string API, so instead each op does
    // an INSERT immediately followed by a DELETE of the highest id.
    sweepOpType("WRITE: INSERT+DELETE churn (exclusive lock -- should stay ~1.0x)",
        [&](GraphHandler& h, uint64_t, int tid, long op) {
            std::string tag = "t" + std::to_string(tid) + "_" + std::to_string(op);
            h.executeQueryLive(
                "INSERT INTO NODES (label, name) VALUES ('Churn', '" + tag + "')");
            // Delete by the current node count as id: ids are sequential from 1, so
            // the just-inserted node has the largest id == current node count.
            size_t topId = h.getNodeCount();
            h.executeQueryLive("DELETE FROM NODES WHERE ID = " + std::to_string(topId));
        });

    // UPDATES -- edit an existing node in place by id. Exclusive lock, NO growth.
    sweepOpType("UPDATE by id (exclusive lock, no growth -- should stay ~1.0x)",
        [&](GraphHandler& h, uint64_t, int, long op) {
            int id = 1 + (int)(op % kSeed);
            h.executeQueryLive(
                "UPDATE NODES WHERE ID = " + std::to_string(id) +
                " SET age = '" + std::to_string(op % 100) + "'");
        });

    std::cout << "\nReading the results:\n"
        << "  * READ speedup should climb toward the core count -- shared locks let\n"
        << "    reads run in parallel. Flat reads would mean reads are serialised.\n"
        << "  * WRITE and UPDATE speedup should stay near 1.0x -- the exclusive lock\n"
        << "    serialises them, so more cores don't help. Flat here is CORRECT and\n"
        << "    confirms the write lock works. (Lock-free parsing lifts these a little\n"
        << "    above 1.0x -- that is the parallel parse cost, not the graph.)\n"
        << "  * The 1-thread p50 is the uncontended per-op latency (raw op cost).\n";

    return 0;   // a measurement, not a pass/fail
}

int run_tests(bool test_robustness = false, bool test_performance = false) {
    // test graph suite
    int fail = test_graph();
    if (fail != 0)
        return fail;

    // test query layer
    fail = test_query_layer();
    if (fail != 0)
        return fail;

    // test storage
    fail = test_storage_engine();
    if (fail != 0)
        return fail;

    // test graph handler concurrent access
    fail = test_graph_handler_concurrent();
    if (fail != 0)
        return fail;

    // test robustness (opt-in: heavy oversubscribed stress, several seconds)
    if (test_robustness) {
        fail = test_graph_handler_robust_concurrency();
        if (fail != 0)
            return fail;
    }

    // test performance (opt-in: throughput sweep; prints a table, returns 0)
    if (test_performance) {
        fail = test_graph_handler_performance();
        if (fail != 0)
            return fail;
    }

    return fail;
}