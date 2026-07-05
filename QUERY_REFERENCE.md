# Suede Graph Database — System & Query Reference

Suede is a small, in-memory graph database with a lightweight SQL-like query
language, binary persistence, and CSV import/export. This document explains how
the pieces fit together and gives the complete, authoritative reference for the
query syntax — including every error message the parser and executor can
produce, with worked examples.

---

## 1. How the system works

The database is organised into a few cooperating layers. Data lives in memory in
the `Graph`; the `Query` layer parses and executes statements against it; the
`StorageEngine` moves graphs to and from disk; and the traversal helpers power
graph-walking queries.

### 1.1 The data model

The graph is a directed, labelled multigraph.

A **node** has a unique `NodeId` (a sequential integer assigned on creation
starting at 1), a single string **label** (e.g. `Person`, `City`), and a map of
string **properties** (e.g. `name=Alice`, `age=30`). Both keys and values are
stored as strings.

An **edge** has a unique `EdgeId` (also sequential from 1), a **from** node, a
**to** node, and a single string **label** (e.g. `KNOWS`, `LIVES_IN`). Edges do
**not** carry properties — an edge is just a labelled, directed connection
between two nodes.

Ids are opaque and strongly typed (`NodeId` and `EdgeId` are distinct types, so
they cannot be mixed up). The value `0` is reserved to mean "invalid".

### 1.2 The `Graph` (storage core)

`Graph` holds everything in `unordered_map`-based indexes so that the common
lookups are all near-constant time:

- `nodes` / `edges` — id → object.
- `labelToNodes` / `labelToEdges` — label → list of ids, for label lookups.
- `adjacencyOut` / `adjacencyIn` — node → outgoing / incoming edge ids.
- `neighbourOut` / `neighbourIn` — node → neighbouring node ids, used for fast
  traversal.
- Insertion-order lists for nodes and edges, so persistence and export are
  deterministic.

A subtle but important design point: an adjacency map only gains an entry for a
node once that node actually gets an edge. A node with zero edges in a given
direction simply has no key. The `Graph` treats a missing key as a valid **empty
result**, not an error.

### 1.3 The `Query` layer

A statement flows through three stages:

1. **Tokenize** — the raw string is split into tokens: identifiers/keywords,
   numbers, quoted strings (`'...'` or `"..."`), the punctuation `(`, `)`, `,`,
   and the comparison operators `=`, `!=`, `<`, `>`, `<=`, `>=`.
2. **Parse** — the first keyword (`SELECT`, `INSERT`, `DELETE`, `MATCH`)
   selects a clause parser, which validates the grammar and records the
   operation, target, conditions, and values. Keywords are case-insensitive.
3. **Execute** — the parsed statement runs against a `Graph`, returning a
   `QueryResult`.

The convenience method `run(text, graph)` does parse-then-execute in one call
and is what most callers use.

A `QueryResult` contains: `success` (bool), a human-readable `message`, and —
depending on the statement — `nodes`, `edges`, or `traversalResult` (a list of
`NodeId`s for `MATCH`).

> **Note on `success`:** a query that runs correctly but matches nothing is a
> **success** with zero rows (e.g. looking up an id that doesn't exist). A query
> that references something structurally absent (an unknown label) is reported
> as a **failure**. See the per-statement sections for exactly which is which.

### 1.4 Traversals (BFS)

`MATCH` queries are backed by a breadth-first search (`BFS_Searcher`). The search
runs **undirected** (it follows edges in both directions) from a source node and
records the visit order and a shortest-path tree. From that single traversal the
executor derives reachable sets, shortest paths, and k-hop neighbourhoods.

### 1.5 Persistence (`StorageEngine`)

The `StorageEngine` provides two independent on-disk formats:

- **Binary** (`Save` / `Load`) — a compact `.bin` format with an 8-byte magic
  (`GRAPHDB`), a version number, and node/edge counts, followed by the nodes
  (label + properties) and edges (endpoints stored as node **indices** into
  insertion order, plus the label). `Load` reconstructs the graph and validates
  that the number of nodes and edges matches the file header.
- **CSV** (`ImportCSV` / `ExportCSV`) — a human-readable format: one node per
  line, then a blank line, then one edge per line. See section 4.

Because edges are stored by node **index** (not by raw id), a saved graph
reloads consistently even though the reconstructed nodes receive fresh ids.

---

## 2. Query language reference

General rules that apply everywhere:

- **Keywords are case-insensitive.** `SELECT`, `select`, and `SeLeCt` are
  equivalent. Property names and string values are **case-sensitive**.
- **String values** may be single- or double-quoted; the quotes are stripped.
  Unquoted bare words are also accepted as values.
- **Numeric ids** must be non-negative integers.
- **`WHERE` supports only `AND`** as a connective. `OR` is not supported.
- Valid comparison operators are `=`, `!=`, `<`, `>`, `<=`, `>=`. Comparisons are
  **lexicographic** (string) comparisons, since all values are stored as strings.

The four statements are `SELECT`, `INSERT`, `DELETE`, and `MATCH`.

---

### 2.1 SELECT

Retrieve nodes or edges. Only the `*` projection is supported (you always get
whole rows), and a `WHERE` clause is **required**.

```
SELECT * FROM NODES WHERE <conditions>
SELECT * FROM EDGES WHERE <conditions>
```

#### SELECT FROM NODES

Supported `WHERE` forms:

| Form | Meaning |
|------|---------|
| `WHERE ID = <n>` | Look up a single node by id. |
| `WHERE LABEL = '<label>'` | All nodes with the given label. |
| `WHERE LABEL = '<label>' AND <prop> <op> <value> ...` | Label match, then filter by property comparisons. |

Examples:

```sql
-- Fetch node 1
SELECT * FROM NODES WHERE ID = 1

-- All Person nodes
SELECT * FROM NODES WHERE LABEL = 'Person'

-- All Person nodes whose age is greater than "20" (lexicographic)
SELECT * FROM NODES WHERE LABEL = 'Person' AND age > '20'

-- Combine multiple property filters
SELECT * FROM NODES WHERE LABEL = 'Person' AND age = '30' AND name = 'Alice'
```

Behaviour notes:

- `WHERE ID = <n>` for a non-existent id → **success**, `Found 0 row(s).`
- `WHERE LABEL = '<label>'` for a label no node has → **failure**,
  `No nodes found with that label.`
- Property filters compare the stored string value; a node missing the property
  is treated as having the empty string `""`.

#### SELECT FROM EDGES

Supported `WHERE` forms (only equality conditions are used to build the edge
lookup; other operators in an edge `WHERE` are ignored):

| Form | Meaning |
|------|---------|
| `WHERE ID = <n>` | Look up a single edge by id. |
| `WHERE LABEL = '<label>'` | All edges with the given label. |
| `WHERE FROM = <n>` | Edges touching node `<n>` (default direction `OUTGOING`). |
| `... AND DIRECTION = 'OUTGOING'\|'INCOMING'\|'BOTH'` | Choose direction for a `FROM` query. |
| `... AND LABEL = '<label>'` | Restrict a `FROM` query to one edge label. |
| `... AND TO = <n>` | Keep only edges whose other endpoint is `<n>`. |

Examples:

```sql
-- Edge 1
SELECT * FROM EDGES WHERE ID = 1

-- All KNOWS edges
SELECT * FROM EDGES WHERE LABEL = 'KNOWS'

-- Outgoing edges from node 1 (default direction)
SELECT * FROM EDGES WHERE FROM = 1

-- Every edge touching node 2, in either direction
SELECT * FROM EDGES WHERE FROM = 2 AND DIRECTION = 'BOTH'

-- Outgoing KNOWS edges from node 1
SELECT * FROM EDGES WHERE FROM = 1 AND LABEL = 'KNOWS'

-- Does an edge from 1 reach node 2?
SELECT * FROM EDGES WHERE FROM = 1 AND TO = 2
```

Behaviour notes:

- `WHERE ID = <n>` for a non-existent id → **success**, `Found 0 row(s).`
- `WHERE LABEL = '<label>'` for a label no edge has → **failure**,
  `No edges found with that label.`
- The `DIRECTION` value is interpreted from the node's perspective: `OUTGOING`
  edges start at the node, `INCOMING` edges end at it, `BOTH` returns either.

---

### 2.2 INSERT

Create a node or an edge. Columns and values are matched **by position**.

```
INSERT INTO NODES (col1, col2, ...) VALUES (val1, val2, ...)
INSERT INTO EDGES (from, to, label) VALUES (<from>, <to>, '<label>')
```

Rules:

- **Nodes** must include a `label` column. Any other columns become properties.
- **Edges** must include exactly `from`, `to`, and `label` — no more, no fewer.
  Edges have no properties. `from` and `to` must be numeric node ids that already
  exist.

Examples:

```sql
-- A node with a label and two properties
INSERT INTO NODES (label, name, age) VALUES ('Person', 'Alice', '30')

-- A node with only a label
INSERT INTO NODES (label) VALUES ('City')

-- An edge between existing nodes 1 and 2
INSERT INTO EDGES (from, to, label) VALUES (1, 2, 'KNOWS')
```

On success the result echoes the created row (`Inserted node.` /
`Inserted edge.`). Inserting an edge whose endpoints don't exist is a **failure**
(`INSERT INTO EDGES failed: one or both endpoint nodes do not exist.`).

---

### 2.3 DELETE

Delete a single node or edge by id. Only `WHERE ID = <n>` is supported.

```
DELETE FROM NODES WHERE ID = <n>
DELETE FROM EDGES WHERE ID = <n>
```

Examples:

```sql
DELETE FROM NODES WHERE ID = 3
DELETE FROM EDGES WHERE ID = 2
```

Deleting a node also removes all of its incident edges (both directions), keeping
the graph consistent. Deleting something that doesn't exist is a **failure**
(`DELETE failed: node does not exist.` / `... edge does not exist.`).

---

### 2.4 MATCH (traversals)

Walk the graph from a source node. All traversals are undirected (edges are
followed both ways).

```
MATCH REACHABLE FROM <src>
MATCH SHORTEST_PATH FROM <src> TO <dst>
MATCH KHOP FROM <src> STEPS <k>
```

| Mode | Returns |
|------|---------|
| `REACHABLE` | Every node reachable from `<src>`, in BFS visit order. |
| `SHORTEST_PATH` | The node sequence of a shortest path from `<src>` to `<dst>`. |
| `KHOP` | Every node within `<k>` hops of `<src>` (inclusive). |

Examples:

```sql
-- Everything reachable from node 1
MATCH REACHABLE FROM 1

-- A shortest path from node 1 to node 3
MATCH SHORTEST_PATH FROM 1 TO 3

-- All nodes within 2 hops of node 1
MATCH KHOP FROM 1 STEPS 2
```

The result is returned in `traversalResult` (a list of `NodeId`s). Behaviour
notes:

- `SHORTEST_PATH` to an unreachable or non-existent target is a **success** with
  an empty path (`No path found between the given nodes.`).
- A `MATCH` whose source node does not exist is a **failure**
  (`MATCH: source node does not exist.`).
- `REACHABLE` and `KHOP` always include the source node itself (0 hops).

---

## 3. Error and status messages

Two kinds of messages can come back: **parse errors** (the statement is
malformed and never runs) and **execution messages** (the statement parsed but
the executor reports the outcome). Below is the authoritative list.

### 3.1 Parse errors

| Message | Triggered by |
|---------|--------------|
| `Empty query.` | Empty or whitespace-only input. |
| `Unrecognized query keyword: <tok>` | First word isn't SELECT/INSERT/DELETE/MATCH. |
| `SELECT: expected '*'.` | Missing projection after `SELECT`. |
| `SELECT: only '*' projections are supported.` | A projection other than `*` (e.g. `SELECT name ...`). |
| `SELECT: expected FROM.` | Missing `FROM`. |
| `SELECT: expected NODES or EDGES after FROM.` | Bad or missing target. |
| `SELECT: unexpected token '<tok>'.` | Extra token where `WHERE` was expected. |
| `SELECT: a WHERE clause is required (WHERE ID = ... or WHERE LABEL = ...).` | `SELECT` with no `WHERE`. |
| `INSERT: expected INTO.` | Missing `INTO`. |
| `INSERT: expected NODES or EDGES after INTO.` | Bad or missing target. |
| `INSERT: expected '(' to begin column list.` | Missing `(` before columns. |
| `INSERT: unterminated column list.` | Missing `)` after columns. |
| `INSERT: expected VALUES.` | Missing `VALUES`. |
| `INSERT: expected '(' to begin value list.` | Missing `(` before values. |
| `INSERT: unterminated value list.` | Missing `)` after values. |
| `INSERT: column count does not match value count.` | Unequal columns and values. |
| `INSERT: unexpected trailing tokens.` | Extra tokens after the value list. |
| `INSERT INTO NODES requires a 'label' column.` | Node insert with no `label`. |
| `INSERT INTO EDGES requires 'from', 'to', and 'label' columns.` | Edge insert missing a required column. |
| `INSERT INTO EDGES only supports 'from', 'to', and 'label' columns (edges have no properties).` | Edge insert with extra columns. |
| `DELETE: expected FROM.` | Missing `FROM`. |
| `DELETE: expected NODES or EDGES after FROM.` | Bad or missing target. |
| `DELETE: a WHERE ID = <id> clause is required.` | `DELETE` with no `WHERE`. |
| `DELETE: only WHERE ID = <id> is supported.` | A `WHERE` that isn't a single `ID =`. |
| `MATCH: expected SHORTEST_PATH, REACHABLE, or KHOP.` | Missing mode. |
| `MATCH: unknown mode '<tok>'.` | Unrecognised mode. |
| `MATCH: expected FROM.` | Missing `FROM`. |
| `MATCH: expected a numeric node id after FROM.` | Non-numeric source. |
| `MATCH SHORTEST_PATH: expected TO.` | `SHORTEST_PATH` without `TO`. |
| `MATCH SHORTEST_PATH: expected a numeric node id after TO.` | Non-numeric target. |
| `MATCH KHOP: expected STEPS.` | `KHOP` without `STEPS`. |
| `MATCH KHOP: expected a numeric step count after STEPS.` | Non-numeric step count. |
| `MATCH: unexpected trailing tokens.` | Extra tokens after a complete `MATCH`. |
| `WHERE: expected operator.` | Property with no operator. |
| `WHERE: invalid operator '<op>'.` | Operator not in the valid set. |
| `WHERE: expected value.` | Operator with no right-hand value. |
| `WHERE: unexpected token '<tok>' (only AND is supported).` | A connective other than `AND` (e.g. `OR`). |
| `WHERE: no conditions found.` | `WHERE` with nothing after it. |

### 3.2 Execution messages

| Message | Meaning | `success` |
|---------|---------|-----------|
| `Found 1 row.` | Single-id lookup hit. | ✅ |
| `Found 0 row(s).` | Single-id lookup missed. | ✅ |
| `Found <n> row(s).` | Label / edge query result count. | ✅ |
| `Inserted node.` / `Inserted edge.` | Insert succeeded. | ✅ |
| `Deleted node.` / `Deleted edge.` | Delete succeeded. | ✅ |
| `Found <n> reachable node(s).` | `MATCH REACHABLE` result. | ✅ |
| `Path found with <n> node(s).` | `MATCH SHORTEST_PATH` result. | ✅ |
| `No path found between the given nodes.` | `SHORTEST_PATH` target unreachable. | ✅ |
| `Found <n> node(s) within <k> hop(s).` | `MATCH KHOP` result. | ✅ |
| `Invalid node id.` / `Invalid edge id.` | Id value wasn't a valid integer. | ❌ |
| `No nodes found with that label.` | `SELECT NODES` label unknown. | ❌ |
| `No edges found with that label.` | `SELECT EDGES` label unknown. | ❌ |
| `No edges found for that node.` | Edge lookup for a node failed. | ❌ |
| `SELECT FROM NODES requires WHERE ID = ... or WHERE LABEL = ....` | Unsupported node `WHERE`. | ❌ |
| `SELECT FROM EDGES requires WHERE ID = ..., WHERE LABEL = ..., or WHERE FROM = ....` | Unsupported edge `WHERE`. | ❌ |
| `Invalid DIRECTION (expected OUTGOING, INCOMING, or BOTH).` | Bad `DIRECTION` value. | ❌ |
| `Invalid FROM node id.` / `Invalid TO node id.` | Non-numeric `FROM` / `TO`. | ❌ |
| `INSERT INTO EDGES: 'from' and 'to' must be numeric node ids.` | Non-numeric endpoints. | ❌ |
| `INSERT INTO EDGES failed: one or both endpoint nodes do not exist.` | Endpoint node missing. | ❌ |
| `DELETE failed: node does not exist.` / `... edge does not exist.` | Delete target missing. | ❌ |
| `MATCH: source node does not exist.` | `MATCH` source missing. | ❌ |
| `Query has not been successfully parsed.` | `execute()` called after a failed/absent parse. | ❌ |

---

## 4. Persistence formats

### 4.1 Binary (`.bin`)

Written by `Save`, read by `Load`. Layout:

```
[ magic: "GRAPHDB" (8 bytes) ][ version: uint32 ][ nodeCount: uint64 ][ edgeCount: uint64 ]
then, per node:  [ labelLen ][ label ][ propCount ] then per property [ keyLen ][ key ][ valLen ][ val ]
then, per edge:  [ fromIndex ][ toIndex ][ labelLen ][ label ]
```

Edge endpoints are stored as **indices** into node insertion order, so the file
is independent of the specific `NodeId` values. On `Load`, the node/edge counts
are checked against the header (`ValidateGraph`); a mismatch fails the load.

### 4.2 CSV

Written by `ExportCSV`, read by `ImportCSV`. One graph, one file, in two blocks
separated by a blank line:

```
Person,name=Alice,age=30
Person,name=Bob,age=25
City,name=Sydney

0,1,KNOWS
1,2,LIVES_IN
```

- **Node lines:** `Label,key=value,key=value,...`. The first field is the label;
  each remaining `key=value` becomes a property. Tokens without `=` are ignored.
- **Blank line:** separates the node block from the edge block. This separator is
  required — it is how the importer knows the nodes have ended and the edges
  begin.
- **Edge lines:** `fromIndex,toIndex,label`, where the indices are 0-based into
  the order the nodes appear above.

---

## 5. End-to-end example

Build a tiny social graph, query it, and persist it.

```sql
-- Create three nodes
INSERT INTO NODES (label, name, age) VALUES ('Person', 'Alice', '30')   -- id 1
INSERT INTO NODES (label, name, age) VALUES ('Person', 'Bob', '25')     -- id 2
INSERT INTO NODES (label, name)      VALUES ('City', 'Sydney')          -- id 3

-- Connect them
INSERT INTO EDGES (from, to, label) VALUES (1, 2, 'KNOWS')              -- edge 1
INSERT INTO EDGES (from, to, label) VALUES (2, 3, 'LIVES_IN')           -- edge 2

-- Who does Bob know or live with? (both directions)
SELECT * FROM EDGES WHERE FROM = 2 AND DIRECTION = 'BOTH'               -- Found 2 row(s).

-- Everyone/everything reachable from Alice
MATCH REACHABLE FROM 1                                                  -- Found 3 reachable node(s).

-- Shortest hop-path from Alice to Sydney
MATCH SHORTEST_PATH FROM 1 TO 3                                         -- Path found with 3 node(s).

-- Remove the city; its LIVES_IN edge is cleaned up automatically
DELETE FROM NODES WHERE ID = 3                                          -- Deleted node.
```

Persisting from code:

```cpp
StorageEngine engine("social.bin");
engine.Save(graph);          // write binary snapshot

Graph restored;
engine.Load(restored);       // read it back and validate counts

engine.ExportCSV(graph, "social.csv");   // human-readable export
```
