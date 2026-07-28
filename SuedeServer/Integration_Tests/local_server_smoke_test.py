#!/usr/bin/env python3
"""
server_smoke_test.py - integration / smoke / load test for SuedeServer.

This is NOT a unit test. It does not link or compile any Suede C++ code. It is an
EXTERNAL CLIENT that talks to a *running* SuedeServer.exe over HTTP, exactly the
way a real user (curl, another service, a web front-end) would. It therefore
requires the server to already be running.

    Terminal 1:  SuedeServer.exe                 (starts listening on :8080)
    Terminal 2:  python server_smoke_test.py     (this script hammers it)

What it checks, in order:
    1. Reachability      - GET /stats returns 200 and valid JSON.
    2. Statefulness      - inserts persist ACROSS separate requests (the whole
                           point of the in-process shared graph: request N sees
                           what request N-1 wrote).
    3. Reads             - SELECT / MATCH come back correctly.
    4. Error handling    - bad JSON -> 400, bad query -> 422, bad route -> 404.
    5. Concurrency       - many simultaneous requests exercise the shared_mutex
                           / MVCC through the real socket path, then a final
                           /stats confirms nothing was lost or corrupted.

Uses only the Python standard library (urllib) - no `pip install` needed.
"""

import json
import sys
import time
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
HOST = "http://localhost:8080"
CONCURRENCY = 50        # how many simultaneous requests in the load phase
LOAD_REQUESTS = 500     # total requests fired in the load phase

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


# ---------------------------------------------------------------------------
# HTTP helpers - the client side of the protocol.
# Each returns (status_code, parsed_json_or_None, raw_text).
# ---------------------------------------------------------------------------
def http_get(path):
    url = HOST + path
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read().decode("utf-8", "replace")
            return resp.status, _try_json(raw), raw
    except urllib.error.HTTPError as e:
        # 4xx/5xx come back as HTTPError; we still want the status + body
        raw = e.read().decode("utf-8", "replace")
        return e.code, _try_json(raw), raw
    except urllib.error.URLError as e:
        return None, None, f"CONNECTION FAILED: {e}"


def http_post(path, body_obj=None, raw_body=None):
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
            headers={"Content-Type": "application/json"},
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


def query(command):
    """Convenience: POST a Query-SQL command to /query."""
    return http_post("/query", {"command": command})


# ---------------------------------------------------------------------------
# Phase 1 - reachability
# ---------------------------------------------------------------------------
def phase_reachability():
    print("\n== Phase 1: reachability ==")
    status, body, raw = http_get("/stats")
    if status is None:
        print(f"  [FATAL] Could not reach the server at {HOST}.")
        print(f"          Is SuedeServer.exe running? ({raw})")
        sys.exit(2)
    _check("GET /stats returns 200", status == 200, f"got {status}")
    _check("GET /stats returns JSON with 'nodes'",
           isinstance(body, dict) and "nodes" in body,
           f"body={raw!r}")
    return body


# ---------------------------------------------------------------------------
# Phase 2 - statefulness (inserts persist across separate requests)
# ---------------------------------------------------------------------------
def phase_statefulness():
    print("\n== Phase 2: statefulness (state persists across requests) ==")
    # baseline count
    _, before, _ = http_get("/stats")
    start_nodes = before.get("nodes", 0) if isinstance(before, dict) else 0

    # three inserts, each its OWN request. A subprocess-per-request server would
    # lose these between calls; the in-process shared graph must retain them.
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

    # count again - must have grown by exactly len(inserts)
    _, after, _ = http_get("/stats")
    end_nodes = after.get("nodes", 0) if isinstance(after, dict) else 0
    _check("node count persisted and grew by 3 across separate requests",
           end_nodes == start_nodes + len(inserts),
           f"before={start_nodes} after={end_nodes}")

    # an edge between the first two inserted nodes (ids are 1-based in insertion order
    # for a fresh graph; if the server was pre-populated this may differ, so we only
    # assert the request itself is well-formed, not a specific id)
    status, body, raw = query("INSERT INTO EDGES (from, to, label) VALUES (1, 2, 'KNOWS')")
    _check("edge insert returns a well-formed response",
           status in (200, 422),  # 200 if ids exist, 422 if engine rejects - both are valid HTTP
           f"status={status} body={raw!r}")


# ---------------------------------------------------------------------------
# Phase 3 - reads
# ---------------------------------------------------------------------------
def phase_reads():
    print("\n== Phase 3: reads ==")
    status, body, raw = query("SELECT * FROM NODES WHERE LABEL = 'Person'")
    _check("SELECT returns 200", status == 200, f"status={status}")
    _check("SELECT result has a 'nodes' array",
           isinstance(body, dict) and isinstance(body.get("nodes"), list),
           f"body={raw!r}")

    status, body, raw = query("MATCH REACHABLE FROM 1")
    _check("MATCH returns 200 with a 'traversal' array",
           status == 200 and isinstance(body, dict) and "traversal" in body,
           f"status={status} body={raw!r}")


# ---------------------------------------------------------------------------
# Phase 4 - error handling (the HTTP status contract)
# ---------------------------------------------------------------------------
def phase_errors():
    print("\n== Phase 4: error handling ==")
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


# ---------------------------------------------------------------------------
# Phase 5 - concurrency / load
# ---------------------------------------------------------------------------
def _one_load_request(i):
    """A single load request: mostly reads, some writes, to exercise read/write
    contention on the shared graph. Returns (ok, status, elapsed_seconds).
    Never raises - a dropped/failed connection is reported as ok=False so one
    bad request cannot abort the whole load run."""
    t0 = time.perf_counter()
    try:
        if i % 10 == 0:
            # ~10% writes
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
    print(f"\n== Phase 5: concurrency ({LOAD_REQUESTS} requests, "
          f"{CONCURRENCY} at a time) ==")

    # count before, so we can verify the concurrent writes all landed
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

    # Tolerate a tiny fraction of dropped connections under heavy load (a real
    # server may occasionally reset one); the strict correctness signal is the
    # write-count check below, not a perfect 500/500.
    success_rate = ok_count / LOAD_REQUESTS if LOAD_REQUESTS else 0
    _check("concurrent success rate >= 99%",
           success_rate >= 0.99,
           f"{ok_count}/{LOAD_REQUESTS} OK ({success_rate*100:.1f}%)")

    # every concurrent write must have landed exactly once - proves the
    # shared_mutex serialised writes without losing or double-counting any.
    _, after, _ = http_get("/stats")
    end_nodes = after.get("nodes", 0) if isinstance(after, dict) else 0
    _check("all concurrent writes persisted exactly once",
           end_nodes == start_nodes + expected_writes,
           f"before={start_nodes} after={end_nodes} "
           f"expected +{expected_writes} (got +{end_nodes - start_nodes})")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    print(f"Suede server integration test -> {HOST}")
    phase_reachability()
    phase_statefulness()
    phase_reads()
    phase_errors()
    phase_load()

    print("\n" + "=" * 48)
    print(f"  RESULT: {_passed} passed, {_failed} failed")
    print("=" * 48)
    sys.exit(0 if _failed == 0 else 1)


if __name__ == "__main__":
    main()