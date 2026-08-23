#!/usr/bin/env python3
"""
local_server_smoke_test.py - self-driving integration / smoke / load test for
SuedeServer.

This is NOT a unit test. It exercises a REAL SuedeServer process over HTTP,
exactly the way a real client would. By default it is fully self-driving:

    python local_server_smoke_test.py

...and it will, by itself:
    1. generate a fresh random 128-hex secret key,
    2. launch SuedeServer with that key in its environment,
    3. wait until the server is actually listening,
    4. mint a bearer token via `SuedeServer --mint 127.0.0.1` (capturing stdout),
    5. run every test phase (auth, reads, writes, errors, concurrency),
    6. ALWAYS shut the server down again, even if a test fails.

Where to find the executable: set SUEDE_SERVER_EXE to its path, or let the
harness try a few common build locations. On Windows that's typically
    x64\\Release\\SuedeServer.exe

External mode (test an already-running server, the old behaviour):
    set SUEDE_TEST_TOKEN=<a token minted for 127.0.0.1>
    python local_server_smoke_test.py --external
In --external mode the harness does NOT launch or mint anything; it just uses
SUEDE_TEST_TOKEN against whatever server is already at HOST.

Uses only the Python standard library - no `pip install` needed.
"""

import json
import os
import secrets
import socket
import subprocess
import sys
import time
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
HOST_NAME = "127.0.0.1"
PORT = 8080
HOST = f"http://{HOST_NAME}:{PORT}"
CONCURRENCY = 50        # how many simultaneous requests in the load phase
LOAD_REQUESTS = 500     # total requests fired in the load phase
CLIENT_IP = "127.0.0.1"  # the IP this test connects FROM; the token is bound to it

# How long to wait for the server to come up before giving up (seconds).
SERVER_START_TIMEOUT = 10.0

# The token is set during startup (self-driving) or read from the environment
# (--external mode). Populated by main() before the phases run.
TEST_TOKEN = ""

# simple pass/fail bookkeeping
_passed = 0
_failed = 0


def _check(name, condition, detail=""):
    """Record and print a single assertion result."""
    global _passed, _failed
    if condition:
        _passed += 1
        print(f"  [PASS] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name}   {detail}")


# ===========================================================================
# Server lifecycle (self-driving mode)
# ===========================================================================

def find_server_exe():
    """Locate the SuedeServer executable, trying hard so you don't have to set
    anything. Order:
      1. SUEDE_SERVER_EXE env var, if set (an explicit override always wins).
      2. common build-output locations, resolved relative to BOTH this script's
         folder and the current working directory (so it works no matter where
         you launch python from).
      3. last resort: walk up from the script looking for any SuedeServer.exe /
         SuedeServer under the repo (finds the VS build output wherever it lands).
    Returns the path or None.
    """
    # 1) explicit override
    env_path = os.environ.get("SUEDE_SERVER_EXE")
    if env_path and os.path.isfile(env_path):
        return env_path

    # the executable's possible names (Windows vs POSIX build)
    exe_names = ["SuedeServer.exe", "SuedeServer", "suede"]

    # relative sub-paths where a build typically lands, from a base directory
    rel_layouts = [
        os.path.join("x64", "Release"),
        os.path.join("x64", "Debug"),
        os.path.join("SuedeServer", "x64", "Release"),
        os.path.join("SuedeServer", "x64", "Debug"),
        "",   # the base dir itself (e.g. a local g++ build next to the script)
    ]

    # 2) search relative to the SCRIPT's folder and the CURRENT WORKING DIR.
    #    Using the script's folder is what makes "just run it" work regardless of
    #    which directory you happen to be in when you launch python.
    script_dir = os.path.dirname(os.path.abspath(__file__))
    bases = [script_dir, os.getcwd()]
    for base in bases:
        for layout in rel_layouts:
            for name in exe_names:
                candidate = os.path.join(base, layout, name)
                if os.path.isfile(candidate):
                    return candidate

    # 3) last resort: walk the tree from the script folder AND a couple of parent
    #    levels (the script may live under SuedeServer/, the .exe under the repo
    #    root's x64/). Return the first SuedeServer executable found.
    roots = [script_dir,
             os.path.dirname(script_dir),
             os.path.dirname(os.path.dirname(script_dir))]
    seen = set()
    for root in roots:
        root = os.path.abspath(root)
        if root in seen or not os.path.isdir(root):
            continue
        seen.add(root)
        for dirpath, _dirnames, filenames in os.walk(root):
            for name in ("SuedeServer.exe", "SuedeServer"):
                if name in filenames:
                    return os.path.join(dirpath, name)

    return None


def generate_key_hex():
    """A fresh random 64-byte key as 128 hex characters (matches the server's
    SUEDE_SECRET_KEY format)."""
    return secrets.token_hex(64)


def wait_until_listening(host, port, timeout):
    """Poll the TCP port until something accepts a connection, or timeout.
    Returns True if the server became reachable, False otherwise. This is far
    more reliable than a fixed sleep -- it waits for ACTUAL readiness."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def mint_token(exe, env, ip):
    """Run `<exe> --mint <ip>` and capture the token it prints to stdout. Returns
    the token string, or raises RuntimeError with the server's stderr on failure.
    The minting process uses the SAME env (same key) as the launched server, so
    the token it produces will verify against that server."""
    proc = subprocess.run(
        [exe, "--mint", ip],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=10,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"mint failed (exit {proc.returncode}): {proc.stderr.strip()}")
    token = proc.stdout.strip()
    if not token:
        raise RuntimeError("mint produced no token on stdout")
    return token


def start_server(exe, env):
    """Launch the server as a subprocess with the given environment. Returns the
    Popen handle. The caller MUST ensure stop_server() is called (use try/finally)."""
    # inherit stdout/stderr so the operator sees the server's own logs inline
    return subprocess.Popen([exe, str(PORT)], env=env)


def stop_server(proc):
    """Shut the server down cleanly, escalating to kill if it doesn't exit.
    Safe to call with proc=None. Never raises."""
    if proc is None:
        return
    try:
        if proc.poll() is not None:
            return  # already exited
        proc.terminate()          # SIGTERM -> our graceful shutdown handler
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()           # last resort
            proc.wait(timeout=5)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# HTTP helpers - the client side of the protocol.
# Each returns (status_code, parsed_json_or_None, raw_text).
#
# `token` controls the Authorization header:
#   * a string  -> sent as "Authorization: Bearer <string>"
#   * None      -> no Authorization header at all (to test the unauthed path)
#   * "" / default sentinel -> use the global TEST_TOKEN
# ---------------------------------------------------------------------------
_USE_DEFAULT = object()   # sentinel: "use the global TEST_TOKEN"


def _auth_headers(token, extra=None):
    headers = dict(extra or {})
    if token is _USE_DEFAULT:
        token = TEST_TOKEN
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    return headers


def http_get(path, token=_USE_DEFAULT):
    url = HOST + path
    try:
        req = urllib.request.Request(url, method="GET", headers=_auth_headers(token))
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read().decode("utf-8", "replace")
            return resp.status, _try_json(raw), raw
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        return e.code, _try_json(raw), raw
    except urllib.error.URLError as e:
        return None, None, f"CONNECTION FAILED: {e}"


def http_post(path, body_obj=None, raw_body=None, token=_USE_DEFAULT):
    """POST JSON. Pass body_obj to send a dict as JSON, or raw_body to send an
    exact string (used to test malformed JSON)."""
    url = HOST + path
    if raw_body is not None:
        data = raw_body.encode("utf-8")
    else:
        data = json.dumps(body_obj).encode("utf-8")
    try:
        req = urllib.request.Request(
            url, data=data, method="POST",
            headers=_auth_headers(token, {"Content-Type": "application/json"}),
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read().decode("utf-8", "replace")
            return resp.status, _try_json(raw), raw
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        return e.code, _try_json(raw), raw
    except urllib.error.URLError as e:
        return None, None, f"CONNECTION FAILED: {e}"


def _try_json(raw):
    try:
        return json.loads(raw)
    except Exception:
        return None


def query(command, token=_USE_DEFAULT):
    """Convenience: POST a Query-SQL command to /query (authed by default)."""
    return http_post("/query", {"command": command}, token=token)


# ===========================================================================
# Test phases
# ===========================================================================

def phase_auth():
    print("\n== Phase 1: authentication ==")

    # (a) no Authorization header at all -> rejected
    status, _, raw = http_get("/stats", token=None)
    if status is None:
        print(f"  [FATAL] Could not reach the server at {HOST}.")
        print(f"          ({raw})")
        sys.exit(2)
    _check("no token -> rejected (not 200)", status != 200, f"got {status}: {raw!r}")

    # (b) a syntactically-wrong token -> rejected
    status, _, raw = http_get("/stats", token="SD-not-a-real-token")
    _check("garbage token -> rejected (not 200)", status != 200, f"got {status}: {raw!r}")

    # (c) the valid token -> accepted
    status, body, raw = http_get("/stats", token=TEST_TOKEN)
    _check("valid token -> 200", status == 200, f"got {status}: {raw!r}")
    _check("valid token -> JSON with 'nodes'",
           isinstance(body, dict) and "nodes" in body, f"body={raw!r}")


def phase_reachability():
    print("\n== Phase 2: reachability (authed) ==")
    status, body, raw = http_get("/stats")
    _check("GET /stats returns 200", status == 200, f"got {status}")
    _check("GET /stats returns JSON with 'nodes'",
           isinstance(body, dict) and "nodes" in body, f"body={raw!r}")
    return body


def phase_statefulness():
    print("\n== Phase 3: statefulness (state persists across requests) ==")
    _, before, _ = http_get("/stats")
    start_nodes = before.get("nodes", 0) if isinstance(before, dict) else 0

    inserts = [
        "INSERT INTO NODES (label, name, age) VALUES ('Person', 'Alice', '30')",
        "INSERT INTO NODES (label, name, age) VALUES ('Person', 'Bob', '25')",
        "INSERT INTO NODES (label, name) VALUES ('City', 'London')",
    ]
    for cmd in inserts:
        status, body, raw = query(cmd)
        _check(f"insert accepted: {cmd[:40]}...",
               status == 200 and isinstance(body, dict) and body.get("success"),
               f"status={status} body={raw!r}")

    _, after, _ = http_get("/stats")
    end_nodes = after.get("nodes", 0) if isinstance(after, dict) else 0
    _check("node count persisted and grew by 3 across separate requests",
           end_nodes == start_nodes + len(inserts),
           f"before={start_nodes} after={end_nodes}")

    status, body, raw = query("INSERT INTO EDGES (from, to, label) VALUES (1, 2, 'KNOWS')")
    _check("edge insert returns a well-formed response",
           status in (200, 422),
           f"status={status} body={raw!r}")


def phase_reads():
    print("\n== Phase 4: reads ==")
    status, body, raw = query("SELECT * FROM NODES WHERE LABEL = 'Person'")
    _check("SELECT returns 200", status == 200, f"status={status}")
    _check("SELECT result has a 'nodes' array",
           isinstance(body, dict) and isinstance(body.get("nodes"), list),
           f"body={raw!r}")

    status, body, raw = query("MATCH REACHABLE FROM 1")
    _check("MATCH returns 200 with a 'traversal' array",
           status == 200 and isinstance(body, dict) and "traversal" in body,
           f"status={status} body={raw!r}")


def phase_errors():
    print("\n== Phase 5: error handling (authed) ==")
    # malformed JSON -> 400
    status, _, raw = http_post("/query", raw_body='{ this is not json ')
    _check("malformed JSON -> 400", status == 400, f"got {status}: {raw!r}")

    # valid JSON, missing 'command' field -> 400
    status, _, raw = http_post("/query", {"nope": 1})
    _check("missing 'command' field -> 400", status == 400, f"got {status}: {raw!r}")

    # valid JSON + command, but a query the engine rejects -> 422
    status, _, raw = query("THIS IS NOT A VALID QUERY")
    _check("engine-rejected query -> 422", status == 422, f"got {status}: {raw!r}")

    # unknown route -> 404
    status, _, raw = http_get("/does-not-exist")
    _check("unknown route -> 404", status == 404, f"got {status}: {raw!r}")


def _one_load_request(i):
    """A single authed load request: mostly reads, some writes. Returns
    (ok, status, elapsed_seconds). Never raises."""
    t0 = time.perf_counter()
    try:
        if i % 10 == 0:
            status, body, _ = query(
                f"INSERT INTO NODES (label, name) VALUES ('Load', 'n{i}')")
        elif i % 3 == 0:
            status, body, _ = query("MATCH REACHABLE FROM 1")
        else:
            status, body, _ = query("SELECT * FROM NODES WHERE LABEL = 'Person'")
        ok = (status == 200 and isinstance(body, dict) and body.get("success", True))
    except Exception:
        status, ok = None, False
    elapsed = time.perf_counter() - t0
    return ok, status, elapsed


def phase_load():
    print(f"\n== Phase 6: concurrency ({LOAD_REQUESTS} requests, "
          f"{CONCURRENCY} at a time) ==")

    _, before, _ = http_get("/stats")
    start_nodes = before.get("nodes", 0) if isinstance(before, dict) else 0
    expected_writes = sum(1 for i in range(LOAD_REQUESTS) if i % 10 == 0)

    results = []
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=CONCURRENCY) as pool:
        futures = [pool.submit(_one_load_request, i) for i in range(LOAD_REQUESTS)]
        for f in as_completed(futures):
            results.append(f.result())
    wall = time.perf_counter() - t0

    ok_count = sum(1 for ok, _, _ in results if ok)
    latencies = sorted(e for _, _, e in results)
    p50 = latencies[len(latencies) // 2]
    p99 = latencies[int(len(latencies) * 0.99)]
    rps = LOAD_REQUESTS / wall if wall > 0 else 0

    print(f"     completed {ok_count}/{LOAD_REQUESTS} OK in {wall:.2f}s "
          f"({rps:.0f} req/s)")
    print(f"     latency  p50={p50*1000:.1f}ms  p99={p99*1000:.1f}ms")

    success_rate = ok_count / LOAD_REQUESTS if LOAD_REQUESTS else 0
    _check("concurrent success rate >= 99%",
           success_rate >= 0.99,
           f"{ok_count}/{LOAD_REQUESTS} OK ({success_rate*100:.1f}%)")

    _, after, _ = http_get("/stats")
    end_nodes = after.get("nodes", 0) if isinstance(after, dict) else 0
    _check("all concurrent writes persisted exactly once",
           end_nodes == start_nodes + expected_writes,
           f"before={start_nodes} after={end_nodes} "
           f"expected +{expected_writes} (got +{end_nodes - start_nodes})")


def run_all_phases():
    """Run every test phase in order."""
    phase_auth()
    phase_reachability()
    phase_statefulness()
    phase_reads()
    phase_errors()
    phase_load()


# ===========================================================================
# main
# ===========================================================================

def run_external():
    """--external: test an already-running server using SUEDE_TEST_TOKEN."""
    global TEST_TOKEN
    print(f"Suede integration test (external) -> {HOST}")
    TEST_TOKEN = os.environ.get("SUEDE_TEST_TOKEN", "")
    if not TEST_TOKEN:
        print("  [FATAL] --external mode needs SUEDE_TEST_TOKEN set to a token")
        print("          minted for 127.0.0.1. Set it and re-run.")
        sys.exit(2)
    run_all_phases()


def run_self_driving():
    """Default: generate a key, launch the server, mint a token, run, tear down."""
    global TEST_TOKEN
    print(f"Suede integration test (self-driving) -> {HOST}")

    exe = find_server_exe()
    if exe is None:
        print("  [FATAL] Could not find the SuedeServer executable.")
        print("          Set SUEDE_SERVER_EXE to its path, e.g.:")
        print("            set SUEDE_SERVER_EXE=x64\\Release\\SuedeServer.exe")
        sys.exit(2)
    print(f"  server exe: {exe}")

    # a fresh key for THIS run; both the server and the mint call use it.
    key = generate_key_hex()
    child_env = dict(os.environ)
    child_env["SUEDE_SECRET_KEY"] = key

    server = None
    try:
        # launch the server, then wait until it is actually accepting connections
        print(f"  launching server on port {PORT} ...")
        server = start_server(exe, child_env)
        if not wait_until_listening(HOST_NAME, PORT, SERVER_START_TIMEOUT):
            # if the server died on startup, surface why
            if server.poll() is not None:
                print(f"  [FATAL] server exited during startup (code {server.returncode}).")
            else:
                print(f"  [FATAL] server did not start listening within "
                      f"{SERVER_START_TIMEOUT}s.")
            sys.exit(2)
        print("  server is listening.")

        # mint a token bound to the client IP, using the same key
        print(f"  minting a token for {CLIENT_IP} ...")
        TEST_TOKEN = mint_token(exe, child_env, CLIENT_IP)
        print(f"  token minted ({len(TEST_TOKEN)} chars).")

        run_all_phases()
    finally:
        # ALWAYS shut the server down, even if a phase raised or sys.exit fired.
        print("\n  shutting server down ...")
        stop_server(server)


def main():
    external = "--external" in sys.argv

    if external:
        run_external()
    else:
        run_self_driving()

    print("\n" + "=" * 48)
    print(f"  RESULT: {_passed} passed, {_failed} failed")
    print("=" * 48)
    sys.exit(0 if _failed == 0 else 1)


if __name__ == "__main__":
    main()