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
2. **Parse** — the first keyword (`SELECT`, `INSERT`, `DELETE`, `MATCH`,
   `UPDATE`, `LOAD`, `SAVE`, `IMPORT`, `EXPORT`) selects a clause parser, which
   validates the grammar and records the operation, target, conditions, and
   values. Keywords are case-insensitive.
3. **Execute** — the parsed statement runs against a `Graph`, returning a
   `QueryResult`.

The convenience method `run(text, graph)` does parse-then-execute in one call
and is what most callers use.

A `QueryResult` contains: `success` (bool), a human-readable `message`, and —
depending on the statement — `nodes`, `edges` (both populated for a `GRAPH`
read), or `traversalResult` (a list of `NodeId`s for `MATCH`). A full scan also
sets `truncated` (true when the row cap dropped rows) and `totalMatched` (the
match count before the cap), so a caller can render "showing 1000 of 5000".

> **Note on `success`:** a query that runs correctly but matches nothing is a
> **success** with zero rows (e.g. looking up an id that doesn't exist). A query
> that references something structurally absent (an unknown label) is reported
> as a **failure**. See the per-statement sections for exactly which is which.

### 1.4 Traversals (BFS live, CSR snapshot)

`MATCH` queries walk the graph, and there are **two interchangeable traversal
engines** behind them:

- **`BFS_Searcher` (LIVE)** — a breadth-first search over the live `Graph`. It
  always reflects the current state of the graph, including every insert and
  delete applied so far. This is the default.
- **`CSR_Searcher` (SNAPSHOT)** — a breadth-first search over a
  `CSR_Representation`: an immutable, point-in-time snapshot of the graph stored
  as Compressed Sparse Row arrays for fast, cache-friendly traversal. A snapshot
  is built once (via `Load_CSR()`) and thereafter observes the graph exactly as
  it was at build time, regardless of later live mutations.

Both engines run **undirected** (they follow edges in both directions) from a
source node, and both return the same shape of result, so a query behaves
identically whichever engine serves it — only *which point in time* it observes,
and *how fast* it runs, differ. A query chooses its engine with the trailing
`LIVE` / `SNAPSHOT` keyword (see §2.5).

The CSR snapshot indexes only adjacency (not labels or properties), so the CSR
*traversal* engine serves `MATCH` only. Point-in-time `SELECT ... SNAPSHOT`
reads, however, are served from the graph's MVCC history at the snapshot's
captured version (see §2.5), so a snapshot can return full node/edge rows too.
`INSERT` and `DELETE` always run against the live graph and reject `SNAPSHOT`.

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
- **`WHERE` supports both `AND` and `OR`** as connectives, with **SQL precedence:
  `AND` binds tighter than `OR`**, so `A AND B OR C` means `(A AND B) OR C`. There
  is no parenthesised grouping. See §2.1 "AND / OR in WHERE".
- Valid comparison operators are `=`, `!=`, `<`, `>`, `<=`, `>=`. Comparisons are
  **lexicographic** (string) comparisons, since all values are stored as strings.
- A statement may end with an optional **execution-mode keyword**, `LIVE` (the
  default) or `SNAPSHOT`. `LIVE` reads the current graph; `SNAPSHOT` reads a
  point-in-time view and applies to reads (`SELECT`, `MATCH`) only. See §2.5.
- `SELECT` and `MATCH` accept an optional leading **`TOP <n>`** that caps the
  number of rows returned. `SELECT` also accepts a **column projection** in place
  of `*`. See §2.1 and §2.4.
- A `SELECT` may be run **with or without** a `WHERE`. With a `WHERE ID`/`LABEL`
  predicate it uses the fast index path; **without** a `WHERE` it is a full scan
  that returns every matching row, **capped at `DEFAULT_SCAN_CAP` (1000) rows** by
  default (raise or lower the cap with `TOP <n>`). The `GRAPH` target
  (`SELECT * FROM GRAPH`) reads the whole graph — all nodes **and** all edges — in
  one result. See §2.1.

The statements are `SELECT`, `INSERT`, `DELETE`, `MATCH`, `UPDATE`, `LOAD`,
`SAVE`, `IMPORT`, and `EXPORT`. `LOAD`/`SAVE` move the graph to and from the
compact **binary** format; `IMPORT`/`EXPORT` do the same with the human-readable
**CSV** format (see §2.9, §2.10, and §4).

---

### 2.1 SELECT

Retrieve nodes, edges, or the whole graph. A `WHERE` clause is **optional**: with
one, the read uses the fast index path (`ID`/`LABEL`); without one, it is a full
scan capped at 1000 rows (see "Full scans and the row cap" below). The projection
may be `*` (whole rows) or a comma-separated list of columns, and an optional
leading `TOP <n>` caps the number of rows returned.

```
SELECT [TOP <n>] <* | col1, col2, ...> FROM NODES [WHERE <conditions>]
SELECT [TOP <n>] *                     FROM EDGES [WHERE <conditions>]
SELECT           *                     FROM GRAPH

SELECT [TOP <n>] <* | col1, col2, ...> FROM NODES [WHERE <conditions>] SNAPSHOT
SELECT [TOP <n>] *                     FROM EDGES [WHERE <conditions>] SNAPSHOT
SELECT           *                     FROM GRAPH                        SNAPSHOT
```

#### Full scans and the row cap

A `SELECT` with **no `WHERE`** is a full scan that returns every matching row:

```sql
SELECT * FROM NODES      -- every node
SELECT * FROM EDGES      -- every edge
SELECT * FROM GRAPH      -- every node AND every edge, in one result
```

Full scans are the natural "show me everything" gesture — for example, loading a
whole graph into the visualiser. To keep a stray full scan from flooding a client
or the wire, every WHERE-less scan is **capped at `DEFAULT_SCAN_CAP` (1000) rows**
by default. When the cap drops rows, the result still succeeds but says so, e.g.:

```
Found 1000 row(s). (capped at 1000; 5000 total -- use TOP <n> for more)
```

`TOP <n>` overrides the default cap for a given query — below it (`TOP 50` returns
50) or above it (`TOP 5000` returns up to 5000). The cap is a fixed constant, not
a function of free memory, so the same query returns the same rows on every run;
raising it is always an explicit `TOP`. (An operator-configurable hard ceiling is
a planned future addition.)

Projection and property filters still compose with a full scan:
`SELECT name FROM NODES` returns every node trimmed to `name`, and
`SELECT * FROM NODES WHERE age > '20'` scans and filters by property with no
`ID`/`LABEL` anchor.

#### SELECT FROM GRAPH (whole-graph read)

`SELECT * FROM GRAPH` returns **all nodes and all edges** in a single
`QueryResult` (`nodes` and `edges` both populated) — the one query that reads the
entire graph in one round trip. It is what the visualiser issues to draw the graph.

`GRAPH` is a whole-graph read only, so it accepts **only** the bare
`SELECT * FROM GRAPH` form. It rejects:

- **`WHERE`** — a predicate on `GRAPH` is ambiguous (filter nodes? edges? both?);
  use `FROM NODES` / `FROM EDGES` to filter.
- **`COUNT`** — "count *what*, nodes or edges?"; use `COUNT * FROM NODES` /
  `FROM EDGES`.
- **a column list** — it returns edges too, which have no columns; use `*`.

Both the node and edge scans are capped as above. Crucially, `GRAPH` keeps only
edges whose **both** endpoints are among the returned nodes, so the result is
always internally consistent — no edge dangles off a node the cap dropped. When
that trimming (or either scan's cap) drops rows, the result message reports it.

```sql
SELECT * FROM GRAPH               -- the whole graph (nodes + edges), capped at 1000 each
SELECT * FROM GRAPH SNAPSHOT      -- the whole graph as of a point-in-time snapshot (§2.5)
```

#### OR and COUNT also run as full scans

Two more forms are served by the same full-scan machinery:

- **`OR` queries** (§2.1 "AND / OR in WHERE"). When a WHERE contains `OR`, the
  query cannot be served from a single index anchor, so it runs as a full scan
  with per-row boolean evaluation. Pure-`AND` queries still take the fast index
  path.
- **`COUNT`** (below). `SELECT COUNT * FROM NODES` with no WHERE counts the whole
  graph, a natural aggregate over a full scan. (A `COUNT` reports a number, not
  rows, so it is **not** subject to the row cap.)

#### COUNT

`SELECT COUNT * FROM <NODES|EDGES> [WHERE ...]` returns **how many rows match**
rather than the rows themselves. `COUNT` must be written as `COUNT *` (there is
nothing to project when only counting). It composes with `WHERE` (including `OR`)
exactly like a normal `SELECT`, and may omit `WHERE` to count the whole graph. A
`COUNT` reports a single number, so unlike a row-returning scan it is **not**
subject to the 1000-row cap. `COUNT` is not supported on the `GRAPH` target
(count nodes or edges specifically instead).

```sql
SELECT COUNT * FROM NODES WHERE LABEL = 'Person'   -- Count: 15
SELECT COUNT * FROM NODES                           -- Count: <all nodes>
SELECT COUNT * FROM EDGES WHERE LABEL = 'KNOWS'     -- Count: 10
SELECT COUNT * FROM NODES WHERE LABEL = 'Person' OR LABEL = 'City'
```

The result message is `Count: <n>`; no rows are returned in `nodes` / `edges`.
`TOP` and `COUNT` are independent — use one or the other.

#### AND / OR in WHERE

`WHERE` combines conditions with `AND` and `OR`. Precedence follows SQL: **`AND`
binds tighter than `OR`**, and there is no parenthesised grouping. So a WHERE is
a sum-of-products — runs of `AND`-joined conditions form groups, and those groups
are `OR`-ed together:

```sql
-- (LABEL = 'Person' AND age > '20') OR LABEL = 'City'
SELECT * FROM NODES WHERE LABEL = 'Person' AND age > '20' OR LABEL = 'City'

-- union of two label sets
SELECT * FROM NODES WHERE LABEL = 'Person' OR LABEL = 'City'

-- union of two ids
SELECT * FROM NODES WHERE ID = 1 OR ID = 5
```

Performance note: a pure-`AND` query anchored on `ID`/`LABEL` uses the fast index
path. A query containing `OR` falls back to a full scan, because its branches
cannot all be resolved from a single index anchor. The query still returns the
correct rows; it just does more work. This keeps `OR` a complete, always-valid
part of the language rather than a form that is sometimes rejected. Edges support
`OR` over `ID`/`LABEL` (edges have no properties).

#### Projection (selective columns)

`SELECT *` returns every column. A column list instead returns only the named
columns:

```sql
SELECT name, age FROM NODES WHERE LABEL = 'Person'
```

Column names are node **property keys** plus the two reserved names `id` and
`label`. Projection trims each returned node's **property map** to the selected
property keys — so after `SELECT name FROM NODES ...`, each result node's
`properties` map contains only `name`. The `id` and `label` are structural
fields on the node (not entries in the property map): they are always present on
the returned row, and naming them as columns is allowed but does not add
anything to the property map (`SELECT label FROM NODES ...` yields rows with an
empty property map and the label still set).

Projecting a key that a particular node does not have simply leaves that key
absent from that node's (already trimmed) map — it is not an error.

**Projection is NODES-only.** Edges have no property map to trim, so an explicit
column list against `EDGES` is rejected; use `*` for edges.

#### TOP (row limit)

`TOP <n>` (T-SQL style, placed right after `SELECT`) returns **at most** `n`
rows. `n` is a non-negative integer; `TOP 0` returns zero rows (a success), and
a `TOP` larger than the number of matches simply returns them all. It applies
after filtering and projection, to both `NODES` and `EDGES`.

```sql
SELECT TOP 100 * FROM NODES WHERE LABEL = 'Person'
SELECT TOP 10 name, age FROM NODES WHERE LABEL = 'Person'
```

#### SELECT FROM NODES

Supported `WHERE` forms:

| Form | Meaning |
|------|---------|
| *(no `WHERE`)* | Full scan: every node, capped at 1000 (or `TOP <n>`). |
| `WHERE ID = <n>` | Look up a single node by id. |
| `WHERE LABEL = '<label>'` | All nodes with the given label. |
| `WHERE LABEL = '<label>' AND <prop> <op> <value> ...` | Label match, then filter by property comparisons. |
| `WHERE <prop> <op> <value>` | Property filter with no `ID`/`LABEL` anchor: full scan, then filter. |

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
| *(no `WHERE`)* | Full scan: every edge, capped at 1000 (or `TOP <n>`). |
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
MATCH [TOP <n>] REACHABLE FROM <src>
MATCH [TOP <n>] SHORTEST_PATH FROM <src> TO <dst>
MATCH [TOP <n>] KHOP FROM <src> STEPS <k>
```

An optional leading `TOP <n>` caps the number of nodes returned in
`traversalResult`. For `REACHABLE` and `KHOP` it keeps the first `n` of the
result set; for `SHORTEST_PATH` it caps the returned path to its first `n` nodes
(from the source).

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

### 2.5 LIVE / SNAPSHOT execution modes

Any statement may end with an optional execution-mode keyword. It selects which
view of the graph the query resolves against:

```
<statement> LIVE       -- (default) read the current, live graph
<statement> SNAPSHOT   -- read a frozen, point-in-time CSR snapshot
```

`LIVE` is the default, so omitting the keyword is the same as writing `LIVE`.
Writing it explicitly is a harmless no-op that documents intent.

`SNAPSHOT` selects a **point-in-time** view of the graph: the state as it was at
the moment the snapshot was captured, unaffected by any insert or delete applied
afterwards. It is valid on both kinds of read — `MATCH` traversals and `SELECT`
queries — but not on the mutations `INSERT` / `DELETE`, which always apply to the
live graph.

- A **`MATCH ... SNAPSHOT`** traversal runs on the CSR snapshot engine
  (`CSR_Searcher` over a `CSR_Representation`) rather than the live
  `BFS_Searcher`. The CSR layout makes these traversals fast.
- A **`SELECT ... SNAPSHOT`** query resolves against the graph's MVCC history at
  the snapshot's captured version (see "How snapshots work: MVCC" below).

```sql
-- Live traversal (default): reflects every insert/delete so far
MATCH REACHABLE FROM 1
MATCH REACHABLE FROM 1 LIVE          -- identical, explicit

-- Snapshot traversal: reads the frozen CSR snapshot
MATCH SHORTEST_PATH FROM 1 TO 3 SNAPSHOT
MATCH KHOP FROM 1 STEPS 2 SNAPSHOT

-- Snapshot SELECT: point-in-time reads over nodes / edges / whole graph
SELECT * FROM NODES SNAPSHOT                              -- full point-in-time scan
SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT
SELECT name, age FROM NODES WHERE LABEL = 'Person' SNAPSHOT
SELECT TOP 10 * FROM NODES SNAPSHOT
SELECT * FROM EDGES WHERE LABEL = 'KNOWS' SNAPSHOT
SELECT * FROM GRAPH SNAPSHOT                              -- whole graph, point-in-time
```

Rules and behaviour:

- **`SNAPSHOT` applies to reads only.** `SELECT` and `MATCH` accept it; `INSERT`
  and `DELETE` **reject it at parse time**
  (`SNAPSHOT applies only to reads (SELECT, MATCH); INSERT and DELETE always run
  against the live graph.`).
- **`SELECT ... SNAPSHOT` supports the same forms as a live `SELECT`,** including
  a WHERE-less full scan — which live reads now also allow (§2.1). The difference
  is only *which version of the graph* the read observes: a `SNAPSHOT` read sees
  the frozen point-in-time set, a live read sees the current graph. All the same
  shapes apply: `ID`/`LABEL` and property filters for nodes;
  `ID`/`LABEL`/`FROM`(+`DIRECTION`)/`TO` for edges; the whole-graph `GRAPH` target;
  projection; and `TOP <n>`. Full scans are capped at 1000 rows in either mode.

  A `SELECT ... SNAPSHOT` behaves consistently **regardless of whether a CSR
  snapshot object is supplied**. When executed with a snapshot (`execute(Graph&,
  CSR_Representation&)`) the read resolves against MVCC history at the captured
  version; when executed with no snapshot available (the single-argument
  `execute(Graph&)`, e.g. the batch command route), it gracefully **falls back to
  the live read**. Either way the query succeeds — only the observed version
  differs.
- **A snapshot must actually exist.** The snapshot is supplied by the caller
  (`Query::execute(Graph& live, CSR_Representation& snapshot)`). If a `SNAPSHOT`
  query is executed with no snapshot available (the single-argument
  `Query::execute(Graph&)`), it **gracefully falls back** to the live path
  (live BFS for `MATCH`, the live graph for `SELECT`) rather than failing.
- **The keyword is a reserved trailing token.** Only a bare, final `LIVE` /
  `SNAPSHOT` is treated as a mode keyword; a quoted value such as `'SNAPSHOT'`
  keeps its quotes and is left untouched, so
  `SELECT * FROM NODES WHERE LABEL = 'SNAPSHOT'` is an ordinary live query.
- Results and messages are identical to the live equivalents — a `SNAPSHOT`
  `SELECT` returns `Found <n> row(s).`, a `SNAPSHOT` `REACHABLE` returns the
  reachable set, and so on.

#### How snapshots work: MVCC (Multi-Version Concurrency Control)

Snapshots do **not** copy the graph. Instead the graph maintains a monotonic
version counter that is bumped on every mutation, and every node/edge carries a
`createdAtVersion` and (once deleted) a `deletedAtVersion` in a retained history
store. Deletions are recorded as **tombstones** rather than discarding data.

A row is visible to a snapshot captured at version `V` iff it was created at or
before `V` **and** either has not been deleted or was deleted strictly after `V`:

```
visible(row, V)  ==  createdAtVersion <= V
                     && (deletedAtVersion == 0 || deletedAtVersion > V)
```

Building a `CSR_Representation` captures the current version as the snapshot id
(`CaptureSnapshot()`); destroying it releases that registration
(`ReleaseSnapshot()`). This is RAII — the snapshot is a resource whose lifetime
bounds how long the graph must retain the history it can observe.

**Garbage collection.** History that predates every active snapshot can no longer
be observed by any snapshot, so it is reclaimed. When a snapshot is released (and
opportunistically after each delete), the graph drops any tombstoned record whose
`deletedAtVersion` is at or before the oldest active snapshot's version — or all
tombstones when no snapshot is active. Live (non-deleted) records are never
reclaimed. This bounds memory: tombstones do not accumulate indefinitely, they
persist only as long as some snapshot still needs them. Copying is avoided
entirely — a snapshot costs a version number plus whatever history its lifetime
pins in place, not a duplicate of the graph.

From code, the two execution forms are:

```cpp
CSR_Representation snapshot(graph);  // captures the current version (RAII)
snapshot.Load_CSR();                 // freeze adjacency for MATCH traversals

Query q;

// MATCH: CSR traversal vs live BFS fallback
q.parse("MATCH REACHABLE FROM 1 SNAPSHOT");
QueryResult live = q.execute(graph);            // no snapshot -> live BFS fallback
QueryResult snap = q.execute(graph, snapshot);  // CSR snapshot traversal

// SELECT: point-in-time read at the snapshot's captured version
q.parse("SELECT * FROM NODES WHERE LABEL = 'Person' SNAPSHOT");
QueryResult rows = q.execute(graph, snapshot);  // resolved against MVCC history

// Later mutations to `graph` are visible to LIVE queries but NOT to `snapshot`.
// When `snapshot` goes out of scope, its registration is released and any
// history only it was pinning becomes eligible for garbage collection.
```

---

### 2.6 UPDATE

Modify existing rows in place. An `UPDATE` requires both a `WHERE` clause (which
rows to change) and a `SET` clause (what to change them to).

```
UPDATE NODES WHERE <conditions> SET key = value [, key = value ...]
UPDATE EDGES WHERE <conditions> SET label = <value>
```

#### UPDATE NODES

Merge the `SET` assignments into the property map of every node matching `WHERE`.
Existing keys are overwritten, new keys are added, and keys not named in `SET`
are left untouched. The structural fields `id` and `label` are not changed.

```sql
UPDATE NODES WHERE ID = 5 SET name = Alice, age = 30
UPDATE NODES WHERE LABEL = 'Person' SET status = active
UPDATE NODES WHERE age = 25 SET age = 26
```

`WHERE` supports the usual forms: `ID` and `LABEL` (`=` / `!=`), and property
comparisons (`=`, `!=`, `<`, `>`, `<=`, `>=`), combined with `AND`.

Result message: `Updated <n> node(s).` (matching zero rows is still a success).

Because the MVCC history retains a single record per node, a
`SELECT ... SNAPSHOT` captured *before* an update observes the node as created at
the update's version; property revisions are not versioned independently.

#### UPDATE EDGES

Change the label of every edge matching `WHERE`. Edges carry only a label, so
**`label` is the only assignable column** — any other `SET` target is rejected.
Edge structure (`from` / `to`) is immutable. The label index is kept consistent.

```sql
UPDATE EDGES WHERE ID = 10 SET label = KNOWS
UPDATE EDGES WHERE LABEL = 'WORKS_AT' SET label = EMPLOYED_BY
```

`WHERE` for edges supports `ID` and `LABEL` (`=` / `!=`).

Result message: `Updated <n> edge(s).`

`UPDATE` always runs against the live graph; `UPDATE ... SNAPSHOT` is rejected at
parse time.

---

### 2.7 LOAD

Load a graph from a persisted binary file.

```
LOAD FILE '<path>'
```

```sql
LOAD FILE 'graph.db'
LOAD FILE 'backups/graph_2026_07_09.db'
```

The quoted path is parsed and exposed via `operation()` / `filePath()`. The actual
file read is performed by a coordinator that owns a `StorageEngine`; executing a
bare `LOAD` against a `Graph` alone reports that a coordinator/StorageEngine is
required, because the `Query` object has no storage of its own.

---

### 2.8 SAVE

Save the current graph to a binary file.

```
SAVE FILE '<path>'
```

```sql
SAVE FILE 'graph.db'
SAVE FILE 'backups/graph_2026_07_09.db'
```

As with `LOAD`, the quoted path is parsed and exposed via `filePath()`, and the
actual write is performed by a coordinator that owns a `StorageEngine`. This keeps
the `Query` layer free of file-system concerns while letting file operations be
expressed as ordinary statements.

---

### 2.9 IMPORT

Build the current graph from a human-readable **CSV** file.

```
IMPORT CSV '<path>'
```

```sql
IMPORT CSV 'social.csv'
IMPORT CSV 'data/people_2026.csv'
```

`IMPORT`/`EXPORT` are the CSV counterparts of the binary `LOAD`/`SAVE`. The
distinct keyword pair keeps the two formats unambiguous at a glance: `LOAD FILE`
/ `SAVE FILE` are binary `.bin`; `IMPORT CSV` / `EXPORT CSV` are CSV. Exactly like
`LOAD`/`SAVE`, the `Query` layer only **parses** the statement and records the
quoted path via `filePath()`; a coordinator that owns a `StorageEngine` performs
the actual read (`StorageEngine::ImportCSV`). Executed against a bare `Graph`
alone it reports that a coordinator is required.

`IMPORT CSV` builds nodes and edges **into the current graph** (it does not clear
first), so importing into an already-populated graph adds to it. To reconstruct a
saved graph faithfully, import into a fresh, empty session. The CSV layout is
given in §4.2.

---

### 2.10 EXPORT

Write the current graph to a human-readable **CSV** file.

```
EXPORT CSV '<path>'
```

```sql
EXPORT CSV 'social.csv'
EXPORT CSV 'backups/graph_2026_07_09.csv'
```

The mirror of `IMPORT`. The quoted path is parsed and exposed via `filePath()`,
and a coordinator that owns a `StorageEngine` performs the write
(`StorageEngine::ExportCSV`). The output is two blocks — nodes, a blank-line
separator, then edges — exactly the format `IMPORT CSV` reads back (§4.2), so an
`EXPORT CSV` followed by an `IMPORT CSV` into a fresh session round-trips the
graph.

---

## 3. Error and status messages

Two kinds of messages can come back: **parse errors** (the statement is
malformed and never runs) and **execution messages** (the statement parsed but
the executor reports the outcome). Below is the authoritative list.

### 3.1 Parse errors

| Message | Triggered by |
|---------|--------------|
| `Empty query.` | Empty or whitespace-only input. |
| `Unrecognized query keyword: <tok>` | First word isn't SELECT/INSERT/DELETE/MATCH/UPDATE/LOAD/SAVE/IMPORT/EXPORT. |
| `UPDATE: expected NODES or EDGES.` | `UPDATE` with a bad or missing target. |
| `UPDATE: a WHERE clause is required.` | `UPDATE` with no `WHERE`. |
| `UPDATE: a SET clause is required.` | `UPDATE` with no `SET`. |
| `SET: expected a column name.` / `SET: expected '=' after column name.` / `SET: expected a value after '='.` | Malformed `SET` assignment. |
| `SET: expected ',' between assignments.` | Two `SET` pairs with no comma. |
| `LOAD: expected FILE ...` / `LOAD FILE: expected a file path.` | Malformed `LOAD`. |
| `SAVE: expected FILE ...` / `SAVE FILE: expected a file path.` | Malformed `SAVE`. |
| `IMPORT: expected CSV ...` / `IMPORT CSV: expected a file path.` / `IMPORT CSV: file path cannot be empty.` / `IMPORT CSV: unexpected trailing tokens.` | Malformed `IMPORT CSV`. |
| `EXPORT: expected CSV ...` / `EXPORT CSV: expected a file path.` / `EXPORT CSV: file path cannot be empty.` / `EXPORT CSV: unexpected trailing tokens.` | Malformed `EXPORT CSV`. |
| `SELECT: expected '*'.` | Missing projection after `SELECT`. |
| `SELECT: expected '*' or a column list.` | No projection after `SELECT` (or after `TOP <n>`). |
| `SELECT: expected a column name.` | Column list is empty or ends without a name. |
| `SELECT: unexpected ',' in column list.` | A leading or doubled comma in the column list. |
| `SELECT: expected ',' between columns.` | Two column names with no comma between them. |
| `SELECT: cannot mix '*' with named columns.` | `*` used alongside named columns. |
| `SELECT: selective columns are only supported for NODES; use '*' for EDGES.` | A column list against `EDGES`. |
| `SELECT TOP: expected a non-negative integer row count after TOP.` | `TOP` without a valid count. |
| `MATCH TOP: expected a non-negative integer row count after TOP.` | `MATCH TOP` without a valid count. |
| `SELECT: expected FROM.` | Missing `FROM`. |
| `SELECT: expected NODES, EDGES, or GRAPH after FROM.` | Bad or missing target. |
| `SELECT: unexpected token '<tok>'.` | Extra token where `WHERE` was expected. |
| `SELECT: FROM GRAPH returns nodes and edges; it does not take a column list (use '*').` | A column list against `GRAPH`. |
| `SELECT: COUNT is not supported on GRAPH; use SELECT COUNT * FROM NODES or FROM EDGES.` | `COUNT` against `GRAPH`. |
| `SELECT: FROM GRAPH reads the whole graph and takes no WHERE clause (use FROM NODES or FROM EDGES to filter).` | A `WHERE` against `GRAPH`. |
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
| `SNAPSHOT applies only to reads (SELECT, MATCH); INSERT, DELETE, and UPDATE always run against the live graph.` | `SNAPSHOT` keyword used on an `INSERT`, `DELETE`, or `UPDATE`. |
| `WHERE: expected operator.` | Property with no operator. |
| `WHERE: invalid operator '<op>'.` | Operator not in the valid set. |
| `WHERE: expected value.` | Operator with no right-hand value. |
| `WHERE: unexpected token '<tok>' (expected AND or OR).` | A connective other than `AND` / `OR` (e.g. `XOR`). |
| `SELECT COUNT: expected '*' after COUNT (e.g. SELECT COUNT * FROM NODES ...).` | `COUNT` not followed by `*`. |
| `WHERE: no conditions found.` | `WHERE` with nothing after it. |

### 3.2 Execution messages

| Message | Meaning | `success` |
|---------|---------|-----------|
| `Found 1 row.` | Single-id lookup hit. | ✅ |
| `Found 0 row(s).` | Single-id lookup missed. | ✅ |
| `Found <n> row(s).` | Label / edge query result count. | ✅ |
| `Found <n> row(s). (capped at <n>; <total> total -- use TOP <n> for more)` | A full scan whose result hit the row cap (`DEFAULT_SCAN_CAP` or `TOP`); `<total>` is the true match count before capping. | ✅ |
| `Graph: <n> node(s), <m> edge(s).` | `SELECT * FROM GRAPH` — whole-graph read (with trailing notes when nodes/edges were capped or edges hidden for an out-of-cap endpoint). | ✅ |
| `Count: <n>` | `SELECT COUNT * FROM ...` — number of matching rows (no rows returned). | ✅ |
| `Inserted node.` / `Inserted edge.` | Insert succeeded. | ✅ |
| `Deleted node.` / `Deleted edge.` | Delete succeeded. | ✅ |
| `Updated <n> node(s).` | `UPDATE NODES` result count. | ✅ |
| `Updated <n> edge(s).` | `UPDATE EDGES` result count. | ✅ |
| `UPDATE EDGES: only the 'label' column can be updated.` | `SET` on an edge column other than `label`. | ❌ |
| `LOAD must be executed by a coordinator that owns a StorageEngine.` | Bare `LOAD` executed against a `Graph` only. | ❌ |
| `SAVE must be executed by a coordinator that owns a StorageEngine.` | Bare `SAVE` executed against a `Graph` only. | ❌ |
| `IMPORT CSV must be executed by a coordinator that owns a StorageEngine.` | Bare `IMPORT CSV` executed against a `Graph` only. | ❌ |
| `EXPORT CSV must be executed by a coordinator that owns a StorageEngine.` | Bare `EXPORT CSV` executed against a `Graph` only. | ❌ |
| `Found <n> reachable node(s).` | `MATCH REACHABLE` result. | ✅ |
| `Path found with <n> node(s).` | `MATCH SHORTEST_PATH` result. | ✅ |
| `No path found between the given nodes.` | `SHORTEST_PATH` target unreachable. | ✅ |
| `Found <n> node(s) within <k> hop(s).` | `MATCH KHOP` result. | ✅ |
| `Invalid node id.` / `Invalid edge id.` | Id value wasn't a valid integer. | ❌ |
| `No nodes found with that label.` | `SELECT NODES` label unknown. | ❌ |
| `No edges found with that label.` | `SELECT EDGES` label unknown. | ❌ |
| `No edges found for that node.` | Edge lookup for a node failed. | ❌ |
| `SELECT FROM NODES requires WHERE ID = ... or WHERE LABEL = ....` | A live node `SELECT` whose `WHERE` is present but uses **no** supported anchor and is not a form the full-scan path handles. A WHERE-*less* `SELECT` is now a valid full scan (§2.1), so this fires only for a malformed / unsupported predicate, not an absent one. | ❌ |
| `SELECT FROM EDGES requires WHERE ID = ..., WHERE LABEL = ..., or WHERE FROM = ....` | A live edge `SELECT` whose `WHERE` uses no supported anchor and is not a full-scan form. Same as nodes — an absent `WHERE` is a valid full scan. | ❌ |
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

-- Just the names of the people, at most 10 of them
SELECT TOP 10 name FROM NODES WHERE LABEL = 'Person'                   -- rows carry only `name`

-- The whole graph in one result (nodes + edges) -- what the visualiser loads
SELECT * FROM GRAPH                                                     -- Graph: 3 node(s), 2 edge(s).

-- Everyone/everything reachable from Alice
MATCH REACHABLE FROM 1                                                  -- Found 3 reachable node(s).

-- The two closest reachable nodes only
MATCH TOP 2 REACHABLE FROM 1                                            -- Found 2 reachable node(s).

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