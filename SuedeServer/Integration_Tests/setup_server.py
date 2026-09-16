"""
setup_server.py -- a tiny mock of SuedeServer, for UI testing ONLY.

It stands in for the real C++ server so the Selenium tests in test_visualiser.py
can drive the visualiser without a built server, a real token, or a real graph.
It does two things:

  * GET  /        -> serves the real SuedeVisualiser.htm from disk (so the tests
                     exercise the exact file that ships).
  * POST /query   -> returns a canned QueryResult JSON, chosen by looking at the
                     command string. The shapes match the real server's toJson
                     (see Json.h): success, message, nodes, edges, traversal,
                     truncated, totalMatched.

It does NOT verify tokens or run any real query logic -- it only replays fixed
responses so the UI's *behaviour* can be tested deterministically. (The seam:
this proves the visualiser handles a given JSON contract correctly; it does not
prove the real server produces that JSON. That is a separate, server-level test.)

Dependencies: standard library only (http.server, json, threading). No pip.

Usage from a test:

    from setup_server import MockSuedeServer
    server = MockSuedeServer(html_path=r"C:\\...\\SuedeVisualiser.htm")
    server.start()            # runs on a background thread; returns immediately
    ...  # drive http://localhost:server.port/ with Selenium
    server.stop()
"""

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# ---------------------------------------------------------------------------
# Fixtures: canned QueryResult responses, matching the real toJson shape.
# Each has success/message/nodes/edges/traversal/truncated/totalMatched.
# ---------------------------------------------------------------------------

def _node(node_id, label, **props):
    return {"id": node_id, "label": label, "properties": props}


def _edge(edge_id, frm, to, label):
    return {"id": edge_id, "from": frm, "to": to, "label": label}


# A small whole-graph result: 3 nodes, 2 edges (Alice -KNOWS-> Bob -LIVES_IN-> Sydney).
GRAPH = {
    "success": True,
    "message": "Graph: 3 node(s), 2 edge(s).",
    "nodes": [
        _node(1, "Person", name="Alice"),
        _node(2, "Person", name="Bob"),
        _node(3, "City", name="Sydney"),
    ],
    "edges": [_edge(1, 1, 2, "KNOWS"), _edge(2, 2, 3, "LIVES_IN")],
    "traversal": [],
    "truncated": False,
    "totalMatched": 3,
}

# The same graph AFTER an insert -- a 4th node and a 3rd edge. Used to prove that
# Refresh re-runs the last read and picks up a change.
GRAPH_AFTER_INSERT = {
    "success": True,
    "message": "Graph: 4 node(s), 3 edge(s).",
    "nodes": GRAPH["nodes"] + [_node(4, "Robot", name="R2")],
    "edges": GRAPH["edges"] + [_edge(3, 3, 4, "BUILT")],
    "traversal": [],
    "truncated": False,
    "totalMatched": 4,
}

# EDGES-only result: edge rows that reference nodes BY ID with NO node rows.
# The visualiser must synthesise stub endpoints and still draw the edges.
EDGES_ONLY = {
    "success": True,
    "message": "Found 2 row(s).",
    "nodes": [],
    "edges": [_edge(1, 1, 2, "KNOWS"), _edge(2, 2, 3, "LIVES_IN")],
    "traversal": [],
    "truncated": False,
    "totalMatched": 2,
}

# A COUNT result: a number, no rows. Must NOT blank the current drawing.
COUNT = {
    "success": True,
    "message": "Count: 2",
    "nodes": [],
    "edges": [],
    "traversal": [],
    "truncated": False,
    "totalMatched": 0,
}

# A genuine read that matched nothing. Must also NOT blank the drawing.
EMPTY = {
    "success": True,
    "message": "Found 0 row(s).",
    "nodes": [],
    "edges": [],
    "traversal": [],
    "truncated": False,
    "totalMatched": 0,
}

# An INSERT response: only the one new node (no full graph). The visualiser
# should leave the drawing alone and let Refresh pull the change in.
INSERT = {
    "success": True,
    "message": "Inserted node.",
    "nodes": [_node(4, "Robot", name="R2")],
    "edges": [],
    "traversal": [],
    "truncated": False,
    "totalMatched": 1,
}

# A capped full scan: 1000 rows returned, 5000 matched. Drives the truncation
# banner. (Only 3 nodes are actually sent so the test stays light; the flags are
# what the banner reads.)
CAPPED = {
    "success": True,
    "message": "Found 1000 row(s). (capped at 1000; 5000 total -- use TOP <n> for more)",
    "nodes": [_node(1, "Person", name="Alice"),
              _node(2, "Person", name="Bob"),
              _node(3, "Person", name="Carol")],
    "edges": [],
    "traversal": [],
    "truncated": True,
    "totalMatched": 5000,
}

# A MATCH traversal: a path of node ids with no node rows. The visualiser
# synthesises the path nodes and highlights them. Path 1 -> 2 -> 3.
MATCH = {
    "success": True,
    "message": "Path found with 3 node(s).",
    "nodes": [],
    "edges": [],
    "traversal": [1, 2, 3],
    "truncated": False,
    "totalMatched": 3,
}

# An engine-rejected query: the real server returns HTTP 422 with success:false
# and the reason in `message`. Used to test that the visualiser surfaces the
# error and does NOT blank the current drawing. Served with status 422 (see the
# handler, which reads the '#ERROR422' marker).
ERROR_422 = {
    "success": False,
    "message": "Parse error: unexpected token 'FROBNICATE'.",
    "nodes": [],
    "edges": [],
    "traversal": [],
    "truncated": False,
    "totalMatched": 0,
}


def response_for(command):
    """Pick the canned response for a command string (case-insensitive).

    The matching is deliberately simple -- it looks for keywords in the command,
    in an order that resolves overlaps (e.g. COUNT before FROM GRAPH). A test can
    request any specific fixture by using an obvious command, e.g.:
        'SELECT * FROM GRAPH'          -> GRAPH
        'SELECT * FROM GRAPH #INSERTED' -> GRAPH_AFTER_INSERT   (marker for the refresh test)
        'SELECT * FROM EDGES'          -> EDGES_ONLY
        'SELECT COUNT * FROM EDGES'    -> COUNT
        'SELECT * FROM NODES WHERE ... #EMPTY' -> EMPTY
        'INSERT ...'                   -> INSERT
        'SELECT * FROM NODES #CAPPED'  -> CAPPED
    """
    c = command.upper()
    if "#ERROR422" in c:
        return ERROR_422
    if c.startswith("MATCH"):
        return MATCH
    if c.startswith("INSERT") or c.startswith("UPDATE") or c.startswith("DELETE"):
        return INSERT
    if "#CAPPED" in c:
        return CAPPED
    if "#EMPTY" in c:
        return EMPTY
    if "COUNT" in c:
        return COUNT
    if "FROM EDGES" in c:
        return EDGES_ONLY
    if "#INSERTED" in c:
        return GRAPH_AFTER_INSERT
    if "FROM GRAPH" in c or "FROM NODES" in c:
        return GRAPH
    # default: an empty, harmless success
    return EMPTY


class MockSuedeServer:
    """A background HTTP server that serves the visualiser and mocks /query.

    Start it, read `.port` (or pass one), drive the browser against
    http://localhost:<port>/, then stop it. Runs on its own thread so the test
    process stays free to drive Selenium.
    """

    def __init__(self, html_path, host="127.0.0.1", port=0):
        # port=0 lets the OS pick a free port -- avoids clashes with a real
        # server on 8080 and lets tests run even if 8080 is busy.
        self.html_path = html_path
        self.host = host
        self._requested_port = port
        self._httpd = None
        self._thread = None
        self.port = None

    def start(self):
        html_path = self.html_path

        class Handler(BaseHTTPRequestHandler):
            # silence the default per-request logging to keep test output clean
            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path in ("/", "/index.htm", "/index.html"):
                    try:
                        with open(html_path, "rb") as f:
                            body = f.read()
                    except OSError as exc:
                        self.send_error(500, "could not read visualiser: %s" % exc)
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    self.send_error(404, "no such route")

            def do_POST(self):
                if self.path != "/query":
                    self.send_error(404, "no such route")
                    return
                length = int(self.headers.get("Content-Length", 0))
                raw = self.rfile.read(length) if length else b""
                try:
                    command = json.loads(raw or b"{}").get("command", "")
                except (ValueError, AttributeError):
                    command = ""

                # TEST MARKERS in the command (additive; real commands don't carry them):
                #   #SLOW     -> wait before replying, so a test can observe the UI's
                #                "busy" state (disabled buttons) DURING a query.
                #   #ERROR422 -> reply with HTTP 422 (an engine-rejected query), to
                #                test that the visualiser surfaces the error and does
                #                not blank the drawing.
                upper = command.upper()
                if "#SLOW" in upper:
                    time.sleep(0.4)

                status = 422 if "#ERROR422" in upper else 200
                body = json.dumps(response_for(command)).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self._httpd = ThreadingHTTPServer((self.host, self._requested_port), Handler)
        self.port = self._httpd.server_address[1]   # the actual (possibly OS-chosen) port
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()
        return self

    def base_url(self):
        return "http://%s:%d/" % (self.host, self.port)

    def stop(self):
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None
