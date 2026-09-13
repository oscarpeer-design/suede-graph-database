# Setting up & running Suede

This guide takes you from a freshly-built binary to a graph on screen, and
explains the two things that most often go wrong. If you only read one section,
read **[Troubleshooting](#troubleshooting)** — every error you're likely to hit
is listed there with its fix.

---

## 1. Prerequisites

- **Windows** (x64).
- **Visual Studio** with the C++ toolset. Suede is **C++17** (it uses
  `std::shared_mutex`); the default MSVC standard is fine.
- No external libraries to install — the crypto (SHA-256 / HMAC) is hand-rolled,
  and the only vendored dependency is a single-header JSON library already in the
  tree.

Build the solution in **Release / x64**. The server binary lands in
`x64\Release\SuedeServer.exe`.

> **`SuedeVisualiser.htm` must sit next to `SuedeServer.exe`.** The server reads
> the visualiser page from its own working directory. If you build to
> `x64\Release`, make sure `SuedeVisualiser.htm` is copied there too (a
> post-build copy step, or copy it by hand).

---

## 2. Running the server

### 2a. One-time: set the secret key

The server signs auth tokens with a secret key from the `SUEDE_SECRET_KEY`
environment variable and **refuses to start without it**. This is deliberate: a
missing or default key would make every token forgeable, so Suede fails closed.

**Windows (PowerShell)** — generate a 64-byte key and save it permanently for
your user:

```powershell
$key = -join ((1..64) | ForEach-Object { '{0:x2}' -f (Get-Random -Max 256) })
[Environment]::SetEnvironmentVariable("SUEDE_SECRET_KEY", $key, "User")
```

**Linux / macOS (bash)** — add to `~/.bashrc` or `~/.profile`:

```bash
export SUEDE_SECRET_KEY=$(openssl rand -hex 64)
```

> ⚠️ **Environment variables are only seen by terminals opened *after* you set
> them.** After setting the key, **open a new terminal** before starting the
> server or minting a token. This is the #1 setup gotcha — a stale terminal will
> report "server has no auth key configured" even though you just set the key.

### 2b. Start the server

In a **new** terminal, from the folder containing `SuedeServer.exe`:

```
SuedeServer
```

You should see `Starting Suede Server on port 8080`. Leave this terminal running —
it *is* the server. To use a different port: `SuedeServer 9090`.

### 2c. Mint a token

In **another** terminal (also able to see `SUEDE_SECRET_KEY`):

```
SuedeServer --mint 127.0.0.1
```

This prints one token, bound to `127.0.0.1` (i.e. this machine), valid for 1 hour.
For a longer-lived token, pass a TTL in seconds:

```
SuedeServer --mint 127.0.0.1 86400      # 24 hours
```

> ⚠️ Type the IP exactly: `127.0.0.1`. A typo like `127.01.01` mints a token bound
> to a different address, and every request then fails with a generic rejection.

### 2d. Open the visualiser — at the URL, not the file

Open your browser to:

```
http://localhost:8080/
```

**Do not double-click `SuedeVisualiser.htm`.** See the box below — this is the
single most common mistake.

Paste your token into the token box, and press **Run**. The default query
`SELECT * FROM GRAPH` loads the whole graph.

> ### ❗ Open the URL, never the file
>
> The visualiser is served **by** the server so its API calls are *same-origin*.
> If you open the `.htm` file directly, the browser loads it from `file:///C:/…`
> and its `fetch("/query")` resolves to `file:///C:/query`, which the browser
> blocks — you'll see **"Could not reach the server: Failed to fetch"** and a CORS
> error in the console. The fix is always: go to **`http://localhost:8080/`**.
> The address bar must show `localhost:8080`, not `file:///`.

---

## 3. Loading existing data

Suede persists to two formats: **binary** (`FLUSH`/`LOAD`, compact `.bin`) and
**CSV** (`EXPORT CSV`/`IMPORT CSV`, human-readable). Full semantics are in
[QUERY_REFERENCE.md](QUERY_REFERENCE.md) §2.7–2.10.

**Load into a *fresh* graph.** `LOAD` and `IMPORT CSV` build into the *current*
graph rather than replacing it, so loading on top of existing data duplicates
rows and fails the load's integrity check. Always load into a freshly-started,
empty server — before inserting anything this session.

> ### ⚠️ Binary uses `LOAD <path>`, not `LOAD FILE '<path>'`
>
> This is the syntax trap that produces the "coordinator / StorageEngine" 422.
> Through the server (the visualiser's query box or the HTTP API), binary
> persistence uses the **bare-word** commands:
>
> ```
> LOAD graph.bin       ← load a binary graph  (NOT: LOAD FILE 'graph.bin')
> FLUSH graph.bin      ← save a binary graph  (NOT: SAVE FILE 'graph.bin')
> ```
>
> If you type `LOAD FILE 'graph.bin'`, the command layer treats `FILE 'graph.bin'`
> as the path and the load fails with a 422. Use `LOAD graph.bin`.
>
> **CSV is different** — it *does* use the quoted form: `IMPORT CSV 'graph.csv'`
> and `EXPORT CSV 'graph.csv'`.

Paths are **relative to the server's working directory** (where `SuedeServer.exe`
runs from — `x64\Release` for a Release build), so put the file there or give an
absolute path.

---

## 4. Stopping the server

Press **Ctrl+C** in the server's terminal. Suede installs a clean-shutdown handler
that stops the accept loop and flushes its auth state, rather than being hard-
killed. (A hard kill still won't corrupt anything — the revocation counter is
persisted at the moment it changes, not only on shutdown.)

---

## Troubleshooting

| What you see | Why | Fix |
|--------------|-----|-----|
| **"Could not reach the server: Failed to fetch"** + a CORS error mentioning `file:///` | You opened the `.htm` file from disk, not the server's URL. | Go to **`http://localhost:8080/`**. The address bar must show `localhost:8080`. |
| **"server has no auth key configured"** (server won't start, or 500 on a request) | The terminal can't see `SUEDE_SECRET_KEY`. | Set the key (§2a), then **open a new terminal** and try again. Env vars only reach terminals opened *after* they're set. |
| **The server exits immediately on start** | No key set at all, or a corrupt auth-state file. | Set `SUEDE_SECRET_KEY` (§2a). Suede fails closed by design. |
| **Every request returns 422 "invalid or unauthorised token"** | Token expired, bound to a different IP, or minted under a different key than the server started with. | Mint a fresh token (§2c) with the correct IP. If you changed the key after starting the server, restart the server too. |
| **`http://localhost:8080/` won't load at all** | The server isn't running, or it's on another port. | Check the server terminal for errors; confirm the port. Start it in a terminal that can see the key. |
| **500 "could not open SuedeVisualiser.htm"** | The `.htm` isn't in the server's working directory. | Copy `SuedeVisualiser.htm` next to `SuedeServer.exe` (e.g. into `x64\Release`). |
| **422 on `LOAD` mentioning a "coordinator / StorageEngine"** | You used `LOAD FILE '…'`; the server's binary persistence uses the bare-word form. | Use **`LOAD graph.bin`** / **`FLUSH graph.bin`** (no `FILE`, no quotes). CSV keeps the quoted form: `IMPORT CSV '…'`. See §3. |
| **`SuedeServer --mint` fails with a key error** | The minting terminal can't see the key. | Same as the auth-key row: new terminal after setting the key. |

For anything about the *query language itself* (why a `SELECT` behaves as it does,
what `MATCH` returns, the row cap, error messages), see
**[QUERY_REFERENCE.md](QUERY_REFERENCE.md)**.
