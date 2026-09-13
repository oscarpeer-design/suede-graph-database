# Suede Graph Database

A small, in-memory graph database written in C++ (C++17, Visual Studio, Windows/x64),
with a lightweight SQL-like query language, an HTTP REST server, hand-rolled HMAC
bearer-token auth, binary + CSV persistence, and a browser-based graph visualiser.

Suede is a directed, labelled multigraph: **nodes** carry a label and string
properties, **edges** are labelled directed connections. You talk to it in a
compact query language (`SELECT`, `INSERT`, `MATCH`, `UPDATE`, …) over HTTP, and
you can see the result drawn live in the visualiser.

---

## Quickstart (Windows)

If you just want a graph on screen:

1. Build the solution in Visual Studio (**Release / x64**).
2. Set your secret key once, then start the server (**[SETUP.md](SETUP.md)** has
   the exact commands).
3. Mint a token: `SuedeServer --mint 127.0.0.1`.
4. Open **`http://localhost:8080/`** in your browser, paste the token, press **Run**.

That's it. The default query `SELECT * FROM GRAPH` draws your whole graph.

> The one thing that trips everyone up: open the server's **URL**
> (`http://localhost:8080/`), **not** the `.htm` file. See **[SETUP.md](SETUP.md)**.

---

## Documentation

| If you want to… | Read |
|-----------------|------|
| **Set up and run** the server (prerequisites, keys, token, troubleshooting) | **[SETUP.md](SETUP.md)** |
| **Call the HTTP API** from your own client (routes, auth header, JSON shapes, status codes) | **[API.md](API.md)** |
| **Write queries** — the full query language and every message it can return | **[QUERY_REFERENCE.md](QUERY_REFERENCE.md)** |
| See runnable example commands | **[example_commands.txt](example_commands.txt)** |

---

## The 30-second mental model

- The **server** (`SuedeServer.exe`) holds one graph in memory and exposes it over HTTP.
- It signs auth tokens with a **secret key** read from the `SUEDE_SECRET_KEY`
  environment variable, and **refuses to start without one** (fail-closed).
- A **token** is minted with `SuedeServer --mint <ip>`; it is bound to that client
  IP, expires after a TTL, and is sent on every request as
  `Authorization: Bearer <token>`.
- The **visualiser** page is served *by* the server at `GET /`, so it calls the
  API on the same origin — which is why you must open
  **`http://localhost:8080/`**, never the `.htm` file from disk.

---

## Ports & routes

Default port **8080** (override with `SuedeServer <port>`).

| Route | Method | Auth | Purpose |
|-------|--------|------|---------|
| `/` | GET | public | The visualiser page |
| `/query` | POST | Bearer token | Run a query-language command |
| `/stats` | GET | Bearer token | Node / edge counts + graph version |

See `SuedeServer --help` for full CLI usage.
