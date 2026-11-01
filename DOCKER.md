# Running Suede in Docker

Runs the Suede server in a Linux container on port **4001**, reachable from your
machine only. For normal local development, use the Visual Studio build in
[SETUP.md](SETUP.md).

- The server is compiled inside the image with `g++` (C++17). Visual Studio is not
  needed and your project settings are unaffected.
- A volume (`suede-data`) holds the revocation counter and any graph saved with `FLUSH`.
- **The graph itself is held in memory.** Stopping or rebuilding the container empties it
  (see section 6).

---

## 1. Install the Docker daemon (Windows)

1. Enable virtualisation in the BIOS if needed. Task Manager > Performance > CPU should
   show "Virtualization: Enabled".
2. In an administrator PowerShell, run `wsl --install` and restart.
3. Install Docker Desktop from docker.com with "Use WSL 2" ticked, then start it.
   (Free for personal use and small businesses; larger companies need a paid plan.)
4. Use Linux containers. If the tray whale menu offers "Switch to Linux containers",
   click it.
5. Check it works:
   ```powershell
   docker version
   docker run hello-world
   ```
   `docker version` must show a Server section. Docker Desktop must be open
   whenever you run `docker` commands.

---

## 2. Files (repo root)

Put these next to `Suede Graph Database.sln`. The build needs the whole repo as its
context, so they must be in the root.

| File | In git? | Purpose |
|---|---|---|
| `Dockerfile` | yes | Builds the image |
| `docker-compose.yml` | yes | Port, key and volume settings |
| `.dockerignore` | yes | Keeps junk and secrets out of the build |
| `.env` | no | Holds the secret key |

Windows editors can save these as `Dockerfile.txt` or `.dockerignore.txt`. Use Visual
Studio or VS Code, or in Notepad set "Save as type" to All Files and quote the name.
If Explorer hides extensions, turn on View > Show > File name extensions.

### Dockerfile

```dockerfile
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN g++ -std=c++17 -O2 -pthread -I SuedeServer -o /suede \
      "Graph and Searchers/BFS_Searcher.cpp" "Graph and Searchers/CSR_Representation.cpp" \
      "Graph and Searchers/CSR_Searcher.cpp" "Graph and Searchers/Graph.cpp" \
      "Queries and Graph Handlers/GraphHandler.cpp" "Queries and Graph Handlers/Query.cpp" \
      Storage/StorageEngine.cpp SuedeServer/Server_Core/UseServer.cpp SuedeServer/SuedeServer.cpp

FROM ubuntu:24.04
RUN useradd -r -u 10001 suede && mkdir /data && chown suede /data
COPY --from=build /suede /usr/local/bin/suede
COPY SuedeServer/SuedeVisualiser.htm /data/
WORKDIR /data
USER suede
EXPOSE 4001
ENTRYPOINT ["suede", "4001", "--public"]
```

- `--public` makes the server listen on all interfaces inside the container. Without it
  the published port cannot reach the server. Running the server natively stays
  localhost-only.
- The `g++` line has its own source list. If you add a `.cpp` to the Visual Studio
  project, add it here too.

### docker-compose.yml

```yaml
services:
  suede:
    build: .
    image: suede:dev
    container_name: suede
    ports:
      - "127.0.0.1:4001:4001"
    environment:
      SUEDE_SECRET_KEY: ${SUEDE_SECRET_KEY:?set SUEDE_SECRET_KEY in a .env file}
    volumes:
      - suede-data:/data

volumes:
  suede-data:
```

Use spaces, not tabs. Validate with `docker compose config`. The `127.0.0.1:` prefix
limits access to your own machine; remove it only after reading section 7.

### .dockerignore

```
.git
.vs
**/.vs
x64
**/x64
**/*.obj
**/*.pdb
**/__pycache__
**/*.csv
.env
```

`.env` is listed so the key is never copied into the image.

### .env

```
SUEDE_SECRET_KEY=<128 hex characters>
```

Generate a key with `openssl rand -hex 64`, or in PowerShell:

```powershell
-join ((1..64) | ForEach-Object { '{0:x2}' -f (Get-Random -Max 256) })
```

The key must be exactly 128 hex characters. It signs every token, so keep it secret and
git-ignored. Changing it invalidates all existing tokens.

---

## 3. Build and start

From the repo root:

```powershell
docker compose up -d --build
docker compose logs -f
```

The log should show `Starting Suede Server on port 4001 (all network interfaces)`.
`--build` recompiles your latest code; the first build takes a few minutes.

---

## 4. Tokens and use

Tokens are bound to a client IP. Inside Docker the server sees the Docker network
gateway, not `127.0.0.1`. Find it and mint a token for it:

```powershell
docker network inspect bridge --format '{{(index .IPAM.Config 0).Gateway}}'
docker compose exec suede suede --mint <gateway-ip>
```

Add a number of seconds to change the 1-hour default, e.g. `... --mint <ip> 86400`.

Open <http://localhost:4001/>, paste the token into the visualiser, and run queries, or
call the API with `Authorization: Bearer <token>`.

For the stress test, run `python stress_dashboard.py`, set Server URL to
`http://127.0.0.1:4001`, and paste a token. Results include Docker network overhead, so
do not compare them directly with native Windows runs.

---

## 5. Stop, update, reset

| Task | Command |
|---|---|
| Stop and remove the container (volume kept) | `docker compose down` |
| Rebuild after code changes | `docker compose up -d --build` |
| Restart | `docker compose restart` |
| Show status / logs | `docker compose ps` / `docker compose logs -f` |
| Delete everything, including the volume | `docker compose down -v` |

Warning: `down -v` deletes `server_counter.txt`, which resets revocation. Tokens you
revoked become valid again.

---

## 6. Persistence

| What | Survives `down` | Survives `down -v` |
|---|---|---|
| Graph (memory) | no | no |
| `/data/graph.bin` (from `FLUSH`) | yes | no |
| `/data/server_counter.txt` | yes | no |

Save and reload with:

```
FLUSH /data/graph.bin
LOAD /data/graph.bin
```

Use the bare-word form: no `FILE` and no quotes. The server does not load a graph at
start-up, so run `LOAD` after each restart, into a fresh graph.

The visualiser page is copied into the volume when the volume is first created. If you
edit `SuedeVisualiser.htm` later, copy the new file in or recreate the volume.

The graph and every snapshot are held in RAM. Give Docker Desktop enough memory
(Settings > Resources); an out-of-memory kill loses everything not flushed.

---

## 7. Security

- The server uses plain HTTP with no TLS. Keep the `127.0.0.1` port mapping, or put a
  TLS reverse proxy in front.
- Behind a proxy, every client appears to come from the proxy's IP, so IP-bound tokens
  protect much less.
- Never put the key in the Dockerfile or commit `.env`. Anyone with the key can mint
  tokens.
- The server has no connection limit. Do not expose it directly to the internet.

---

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `failed to connect to the docker API` | Docker Desktop not running | Start it and wait for the icon to settle |
| `set SUEDE_SECRET_KEY in a .env file` | `.env` missing or in the wrong folder | Create it in the repo root; run commands from there |
| `docker compose config` YAML error | Tabs or bad indentation | Use spaces; match the layout above |
| Build cannot find a file | Not run from the repo root, or `.dockerignore` excludes it | Run from the root; check `.dockerignore` |
| Build fails after you add a source file | The `g++` line has its own file list | Add the new `.cpp` to it |
| `port is already allocated` | Port 4001 in use | Free it, or change it in both the Dockerfile and compose file |
| Page does not load | Container stopped or crashed | `docker compose ps`, then `docker compose logs` |
| Page hangs or resets | Server bound to localhost inside the container | Log must say "all network interfaces"; entrypoint needs `--public` |
| 422 invalid or unauthorised token | Wrong IP, expired, or different key | Mint for the IP the server sees; remint after any key change |
| Graph empty after restart | Graph is memory-only | `LOAD /data/graph.bin` |
| `LOAD` returns 422 | Used the `FILE '...'` form | Use `LOAD /data/graph.bin` |
| Revoked tokens work again | Volume was deleted | Avoid `down -v`; re-revoke |

If the bridge gateway IP does not work for tokens, try:

```powershell
docker inspect suede --format '{{range .NetworkSettings.Networks}}{{.Gateway}}{{end}}'
```

If tokens are still rejected, the server's view of your address is something else. The
fix is a server log line that prints the client IP it checks tokens against.
