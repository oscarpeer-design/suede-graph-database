#include "test_graph_suite.h"

#include "Graph.h"
#include "Query.h"
#include "StorageEngine.h"
#include "CSR_Representation.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <sstream>
#include <cstdio>

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

int test_query_layer() {
    std::cout << "\n==================== QUERY LAYER ====================\n";

    // -----------------------------------------------------------------
    // 1. PARSING -- statements that must parse successfully
    // -----------------------------------------------------------------
    std::cout << "\n== Parsing: well-formed statements accepted ==\n";
    {
        Query q;
        check("parse SELECT by id", q.parse("SELECT * FROM NODES WHERE ID = 1"));
        check("parse SELECT by label", q.parse("SELECT * FROM NODES WHERE LABEL = 'Person'"));
        check("parse SELECT with AND", q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' AND age > 20"));
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
        check("reject SELECT without '*'", !q.parse("SELECT name FROM NODES WHERE ID = 1"));
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
        check("reject WHERE OR (only AND)", !q.parse("SELECT * FROM NODES WHERE age > 20 OR age < 10"));
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
            for (NodeId id : v) if (nameOf(id) == t) return true; return false; };
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
            for (NodeId id : v) if (nameOf(id) == t) return true; return false; };
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

        // SNAPSHOT is only valid on MATCH; the other statements reject it.
        check("reject SELECT ... SNAPSHOT", !q.parse("SELECT * FROM NODES WHERE ID = 1 SNAPSHOT"));
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

int run_tests() {
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

    return fail;
}