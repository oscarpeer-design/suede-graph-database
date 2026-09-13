# Suede HTTP API

The Suede server exposes the graph over an HTTP/1.1 REST API. This document is the
wire contract: every route, the authentication scheme, the exact request and
response JSON, and every status code — enough to write a client in any language.

For the **query language** these calls carry, see
[QUERY_REFERENCE.md](QUERY_REFERENCE.md). For **setting up and running** the
server, see [SETUP.md](SETUP.md).

---

## 1. Basics

- **Base URL:** `http://localhost:<port>/` (default port 8080; override with
  `SuedeServer <port>`).
- **Binding:** by default the server listens on loopback only (`127.0.0.1`), so it
  is not reachable from other machines. An operator build flag can bind it
  publicly; that is not the default.
- **Content type:** all request and response bodies are JSON
  (`Content-Type: application/json`). The one exception is `GET /`, which returns
  `text/html` (the visualiser page).
- **Connections:** the server sends `Connection: close` — one request per
  connection. Open a fresh connection per call; most HTTP clients do this
  automatically.
- **Body limit:** requests larger than 4 MB are rejected.

---

## 2. Authentication

Every API route except `GET /` requires a bearer token in the standard header:

```
Authorization: Bearer <token>
```

A token is minted out of band with the server binary. Minting needs the secret
key, so it can only be produced by whoever runs the server, never over the API:

```
SuedeServer --mint 127.0.0.1
```

The token is bound to the client IP given at mint time, has a built-in expiry, and
is signed with the server's secret key. See [SETUP.md](SETUP.md) section 2c for
minting and token lifetimes.

**Why token failures are not itemised.** The server does not report why a token
was rejected — forged, expired, wrong-IP, and revoked all collapse to one generic
`422` with `{"error":"invalid or unauthorised token"}`. This is deliberate:
distinguishing the causes would help an attacker probe. The one exception is a
missing server key, which is the operator's misconfiguration rather than the
client's, and returns `500`.

---

## 3. Routes

### GET / — the visualiser page

Public (no token). Returns the `SuedeVisualiser.htm` page as `text/html`. A browser
loads this page, which then calls `/query` on the same origin.

| Status | When |
|--------|------|
| `200` | Page served. |
| `500` | The server could not read `SuedeVisualiser.htm` from its working directory. This is a deployment problem: the file must sit next to the executable. |

Aliases: `GET /index.htm` and `GET /index.html` serve the same page.

---

### POST /query — run a command

The primary route. Runs one query-language command, or a persistence or snapshot
command (see section 4), and returns the result.

Auth: required.

**Request body:**

```json
{ "command": "SELECT * FROM GRAPH" }
```

The single field `command` (string, required) is the statement to run, exactly as
it would be typed in the visualiser. Anything the query language accepts is valid.

**Response body** (`200` or `422`; see the status table):

```json
{
  "success": true,
  "message": "Graph: 3 node(s), 2 edge(s).",
  "nodes": [
    { "id": 1, "label": "Person", "properties": { "name": "Alice", "age": "30" } },
    { "id": 2, "label": "Person", "properties": { "name": "Bob",   "age": "25" } },
    { "id": 3, "label": "City",   "properties": { "name": "Sydney" } }
  ],
  "edges": [
    { "id": 1, "from": 1, "to": 2, "label": "KNOWS" },
    { "id": 2, "from": 2, "to": 3, "label": "LIVES_IN" }
  ],
  "traversal": [],
  "truncated": false,
  "totalMatched": 3
}
```

Every field is always present; arrays may be empty.

| Field | Type | Meaning |
|-------|------|---------|
| `success` | bool | `true` if the engine accepted and ran the command; `false` if it rejected it. Drives the HTTP status (200 vs 422). |
| `message` | string | Human-readable result or error text (for example `Found 5 row(s).`, `Inserted node.`, or a parse-error explanation). |
| `nodes` | array | Node rows (for `SELECT ... FROM NODES`, `FROM GRAPH`, and `INSERT`). Each element has `id` (number), `label` (string), and `properties` (object of string to string). |
| `edges` | array | Edge rows (for `SELECT ... FROM EDGES` and `FROM GRAPH`). Each element has `id`, `from`, `to` (numbers) and `label` (string). |
| `traversal` | array | Node ids (numbers) from a `MATCH` traversal, in visit or path order. |
| `truncated` | bool | `true` when a WHERE-less full scan hit the row cap and dropped rows. |
| `totalMatched` | number | For a full scan, how many rows matched before the cap, so a client can report "showing 1000 of 5000". Equals the returned count when not truncated. |

Which fields are populated depends on the command: a node `SELECT` fills `nodes`;
an edge `SELECT` fills `edges`; `SELECT * FROM GRAPH` fills both; a `MATCH` fills
`traversal`; a mutation (`INSERT`, `UPDATE`, `DELETE`) reports through `message`,
and an `INSERT` also echoes the new row. See
[QUERY_REFERENCE.md](QUERY_REFERENCE.md) for the per-statement shapes and the full
list of `message` strings.

**Status codes:**

| Status | When | Body |
|--------|------|------|
| `200` | Command parsed and the engine succeeded (`success: true`). | The full result object above. |
| `400` | The request itself was malformed: missing or invalid `Authorization` header, non-JSON body, body over 4 MB, or a missing or non-string `command` field. | `{"error":"..."}` |
| `422` | Either the token was rejected, or the command parsed but the engine rejected it (`success: false` — for example a parse error, an unknown label, or a bad edge endpoint). | For an engine rejection, the full result object with `success: false`. For an auth failure, `{"error":"invalid or unauthorised token"}`. |
| `500` | The server has no auth key configured (operator misconfiguration), or an unexpected server error occurred. | `{"error":"..."}` |

Note: `422` carries two distinct meanings — "the token is bad" and "the command is
bad" — distinguished by the body. An auth rejection returns
`{"error":"invalid or unauthorised token"}` with no `success` field; an engine
rejection returns the normal result object with `"success": false` and the reason
in `message`. Check for the presence of a `success` field to tell them apart.

---

### GET /stats — node and edge counts

A lightweight introspection route: counts and the current graph version, without
running a query.

Auth: required.

**Response body** (`200`):

```json
{ "nodes": 3, "edges": 2, "version": 5 }
```

| Field | Type | Meaning |
|-------|------|---------|
| `nodes` | number | Total nodes in the live graph. |
| `edges` | number | Total edges in the live graph. |
| `version` | number | The graph's monotonic version counter, bumped on every mutation. Useful for change detection. |

Status codes: `200` on success; `400`, `422`, and `500` for the same
authentication reasons as `/query`.

---

### Unknown routes

Any other method or path returns `404` with
`{"error":"no such route: <METHOD> <path>"}`.

---

## 4. Commands beyond queries (via POST /query)

The `command` field accepts more than query-language statements. The server's
command layer recognises a few bare-word commands before handing the rest to the
query engine. Their syntax differs from the query language, so they are listed
here explicitly.

| Command (in `command`) | What it does |
|------------------------|--------------|
| `NODE COUNT` | Returns `message: "Node count: <n>"`. |
| `EDGE COUNT` | Returns `message: "Edge count: <n>"`. |
| `SNAPSHOT CREATE` | Captures a point-in-time snapshot; `message` carries the new snapshot id. |
| `SNAPSHOT RELEASE <id>` | Releases a snapshot by id. |
| `FLUSH [path]` | Saves the live graph to binary. Bare `FLUSH` uses the engine's existing path; `FLUSH graph.bin` saves to that path. |
| `LOAD [path]` | Loads the live graph from binary. `LOAD graph.bin` loads that file. |
| `IMPORT CSV '<path>'` | Imports a CSV file into the live graph (query-language statement). |
| `EXPORT CSV '<path>'` | Exports the live graph to CSV (query-language statement). |

**Persistence syntax.** Binary persistence through the server uses the bare-word
forms `FLUSH <path>` and `LOAD <path>` — not the `SAVE FILE '...'` and
`LOAD FILE '...'` forms the query language parses. If `LOAD FILE 'graph.bin'` is
sent to `/query`, the command layer treats `FILE 'graph.bin'` as the path and the
load fails with a `422`. Use `LOAD graph.bin`. CSV, by contrast, uses the
query-language statements `IMPORT CSV '<path>'` and `EXPORT CSV '<path>'` with a
quoted path, which the server routes to the storage engine correctly.

All paths are relative to the server's working directory (where `SuedeServer.exe`
runs). `LOAD` builds into the current graph, so load into a freshly started server
to avoid duplicate rows failing the integrity check.

---

## 5. Worked examples (curl)

Assume the server is running on `localhost:8080` and a token has been minted:

```bash
TOKEN=$(SuedeServer --mint 127.0.0.1)     # bind to 127.0.0.1, one-hour default lifetime
```

Run a query for the whole graph:

```bash
curl -s http://localhost:8080/query \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"command":"SELECT * FROM GRAPH"}'
```

Insert a node:

```bash
curl -s http://localhost:8080/query \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"command\":\"INSERT INTO NODES (label, name) VALUES ('Person', 'Alice')\"}"
```

Run a traversal:

```bash
curl -s http://localhost:8080/query \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"command":"MATCH REACHABLE FROM 1"}'
```

Read stats:

```bash
curl -s http://localhost:8080/stats -H "Authorization: Bearer $TOKEN"
```

Load a saved graph (note the bare-word form):

```bash
curl -s http://localhost:8080/query \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"command":"LOAD graph.bin"}'
```

On Windows PowerShell, quoting differs; use `Invoke-RestMethod`:

```powershell
$h = @{ Authorization = "Bearer $TOKEN" }
Invoke-RestMethod -Uri http://localhost:8080/query -Method Post -Headers $h `
  -ContentType 'application/json' -Body '{"command":"SELECT * FROM GRAPH"}'
```

---

## 6. Client checklist

The five points that most often cause problems when writing a client:

1. Send the token on every call except `GET /`, as `Authorization: Bearer <token>`.
2. Mint the token for the IP the client actually connects from. From the same
   machine that is `127.0.0.1`. A mismatch produces a silent generic `422`.
3. `POST /query` bodies are `{"command": "..."}` — a JSON object with one string
   field, not the raw query text.
4. Distinguish the two kinds of `422` by whether the body has a `success` field:
   no field means an authentication failure; `success: false` means the command
   was rejected, with the reason in `message`.
5. A capped result still returns `200` with `success: true`. Check `truncated` and
   `totalMatched` rather than assuming the full result was returned.
