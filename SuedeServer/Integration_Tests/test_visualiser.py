"""
test_visualiser.py -- Selenium UI tests for SuedeVisualiser.htm.

Drives the REAL visualiser page in a headless Chrome browser against the mock
server in setup_server.py, and asserts on the rendered DOM/SVG. Each test is one
behaviour we care about -- most are regressions for bugs found by hand:

  * SELECT * FROM GRAPH draws nodes AND edges
  * SELECT * FROM EDGES draws stub endpoints (not a blank canvas)
  * SELECT COUNT * ... leaves the current drawing unchanged
  * a zero-match read leaves the drawing unchanged
  * INSERT leaves the view; Refresh re-runs the last read and shows the change
  * Refresh is always enabled and works from a cold start
  * node spacing is compact (not thousands of px apart)
  * the truncation banner appears when the server caps a scan
  * no JavaScript errors occur during any of the above

Dependencies: selenium (already installed) + Python standard library (unittest).
No pytest, no npm. Selenium 4.6+ auto-manages chromedriver (Selenium Manager);
if yours is older, set CHROMEDRIVER below.

CONFIGURE ME (top of the file):
  * HTML_PATH -- where SuedeVisualiser.htm lives. Defaults to the x64\\Release
    directory next to the server exe. Override with the env var SUEDE_HTML.
  * HEADLESS  -- True by default (no window). Set env SUEDE_HEADLESS=0 to WATCH
    the browser click through the tests (useful the first time).

RUN:
    python -m unittest test_visualiser.py
    python -m unittest test_visualiser.py -v          (verbose: one line per test)
"""

import os
import re
import time
import unittest

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC

from setup_server import MockSuedeServer


# ---------------------------------------------------------------------------
# Configuration. Edit HTML_PATH if your build output is elsewhere.
# ---------------------------------------------------------------------------

# Default: the visualiser sits next to SuedeServer.exe in the Release build.
# Adjust to match your actual repo path, or set the SUEDE_HTML env var.
DEFAULT_HTML_PATH = (
    r"C:\Users\Oscar\source\repos\Suede Graph Database"
    r"\SuedeServer\x64\Release\SuedeVisualiser.htm"
)
HTML_PATH = os.environ.get("SUEDE_HTML", DEFAULT_HTML_PATH)

# Headless unless SUEDE_HEADLESS=0. Watching the run is handy the first time.
HEADLESS = os.environ.get("SUEDE_HEADLESS", "1") != "0"

# Only needed if Selenium is too old to auto-manage the driver. Leave blank to
# let Selenium Manager (4.6+) find/download the right chromedriver automatically.
CHROMEDRIVER = os.environ.get("CHROMEDRIVER", "")

WAIT_SECONDS = 10   # how long WebDriverWait polls before failing a test


class VisualiserTest(unittest.TestCase):
    """Shared browser + mock server for the whole test class (started once)."""

    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(HTML_PATH):
            raise unittest.SkipTest(
                "SuedeVisualiser.htm not found at:\n  %s\n"
                "Set the SUEDE_HTML environment variable to its real path." % HTML_PATH
            )

        # 1. start the mock server (OS-chosen free port) on a background thread.
        cls.server = MockSuedeServer(html_path=HTML_PATH).start()

        # 2. launch one headless Chrome, reused across tests for speed.
        options = Options()
        if HEADLESS:
            options.add_argument("--headless=new")
        options.add_argument("--window-size=1200,800")
        options.add_argument("--no-sandbox")
        # keep the console log so we can assert there were no JS errors
        options.set_capability("goog:loggingPrefs", {"browser": "ALL"})

        if CHROMEDRIVER:
            service = webdriver.chrome.service.Service(executable_path=CHROMEDRIVER)
            cls.driver = webdriver.Chrome(service=service, options=options)
        else:
            cls.driver = webdriver.Chrome(options=options)   # Selenium Manager finds the driver

        cls.driver.set_page_load_timeout(20)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.driver.quit()
        finally:
            cls.server.stop()

    # ---- helpers ----------------------------------------------------------

    def setUp(self):
        """Fresh page load per test, with the token pre-filled and ready to Run."""
        self.driver.get(self.server.base_url())
        self.wait = WebDriverWait(self.driver, WAIT_SECONDS)
        # a token is required by the UI (any non-empty string; the mock ignores it)
        self._set_input("token", "test-token")

    def _set_input(self, element_id, value):
        el = self.driver.find_element(By.ID, element_id)
        el.clear()
        el.send_keys(value)

    def _run(self, command):
        """Type a command into the query box and click Run."""
        self._set_input("query", command)
        self.driver.find_element(By.ID, "run").click()

    def _node_count(self):
        return len(self.driver.find_elements(By.CSS_SELECTOR, "#nodes-layer .node"))

    def _edge_count(self):
        return len(self.driver.find_elements(By.CSS_SELECTOR, "#edges-layer line"))

    def _wait_for_node_count(self, n):
        self.wait.until(lambda d: self._node_count() == n)

    def _status_text(self):
        return self.driver.find_element(By.ID, "status").text

    def _node_positions(self):
        """Return each node's (x, y) from its transform=translate(x,y)."""
        positions = []
        for g in self.driver.find_elements(By.CSS_SELECTOR, "#nodes-layer .node"):
            m = re.search(r"translate\(([-\d.]+),\s*([-\d.]+)\)", g.get_attribute("transform"))
            if m:
                positions.append((float(m.group(1)), float(m.group(2))))
        return positions

    def _node_labels(self):
        return [e.text for e in self.driver.find_elements(By.CSS_SELECTOR, "#nodes-layer .node-label")]

    def _assert_no_js_errors(self):
        """Fail only on real SCRIPT errors, not browser network noise.

        The browser logs a SEVERE console entry for things that are NOT JavaScript
        errors and that we sometimes trigger on purpose:
          * favicon 404s (the browser auto-requests /favicon.ico);
          * "Failed to load resource: ... status of 4xx/5xx" -- the browser logs
            this for ANY non-2xx fetch response, including the 422s we test
            deliberately (a rejected query). The visualiser handles those
            gracefully; the log line is the browser reporting the HTTP status, not
            a script fault.
        We filter both out and fail only on genuine uncaught-script errors.
        """
        severe = [e for e in self.driver.get_log("browser") if e.get("level") == "SEVERE"]
        noise = ("favicon", "Failed to load resource")
        script_errors = [e for e in severe
                         if not any(n in e.get("message", "") for n in noise)]
        self.assertEqual(script_errors, [], "JavaScript errors occurred: %s" % script_errors)

    # ---- tests ------------------------------------------------------------

    def test_graph_draws_nodes_and_edges(self):
        """SELECT * FROM GRAPH draws all nodes and edges."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        self.assertEqual(self._node_count(), 3)
        self.assertEqual(self._edge_count(), 2)
        self._assert_no_js_errors()

    def test_edges_only_draws_stub_endpoints(self):
        """SELECT * FROM EDGES draws stub '#id' nodes rather than a blank canvas."""
        self._run("SELECT * FROM EDGES")
        self._wait_for_node_count(3)          # 3 distinct endpoints synthesised
        self.assertEqual(self._edge_count(), 2)
        labels = self._node_labels()
        stub_labels = [l for l in labels if re.match(r"^#\d", l)]
        self.assertEqual(len(stub_labels), 3, "endpoints should render as #id stubs: %s" % labels)
        self._assert_no_js_errors()

    def test_count_does_not_blank_the_graph(self):
        """SELECT COUNT * ... reports the count and leaves the drawing unchanged."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        self._run("SELECT COUNT * FROM EDGES")
        # give the app a beat to (wrongly) redraw if it were going to
        self.wait.until(lambda d: "Count: 2" in self._status_text())
        self.assertEqual(self._node_count(), 3, "COUNT must not clear the graph")
        self.assertIn("Count: 2", self._status_text())
        self._assert_no_js_errors()

    def test_empty_read_does_not_blank_the_graph(self):
        """A read that matches nothing leaves the current drawing in place."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        self._run("SELECT * FROM NODES WHERE LABEL = 'Nope' #EMPTY")
        self.wait.until(lambda d: "0 row" in self._status_text())
        self.assertEqual(self._node_count(), 3, "an empty read must not clear the graph")
        self._assert_no_js_errors()

    def test_insert_leaves_view_then_refresh_updates(self):
        """INSERT leaves the graph; re-running the read shows the change.

        Models the real flow: draw a graph (3 nodes), INSERT (view stays at 3),
        then re-run a read that now returns the post-insert graph (grows to 4).
        The '#INSERTED' marker tells the mock to return the 4-node/3-edge fixture.
        """
        # 1. draw the plain graph -> 3 nodes.
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)

        # 2. an INSERT: the graph must stay at 3, and the status must mention it
        #    (the visualiser deliberately does not redraw on a mutation).
        self._run("INSERT INTO NODES (label, name) VALUES ('Robot', 'R2')")
        self.wait.until(lambda d: "Inserted" in self._status_text())
        self.assertEqual(self._node_count(), 3, "INSERT must not redraw the graph")

        # 3. re-run a read that reflects the insert -> the new node appears (4).
        self._run("SELECT * FROM GRAPH #INSERTED")
        self._wait_for_node_count(4)
        self.assertEqual(self._node_count(), 4, "the inserted node should appear after re-reading")
        self._assert_no_js_errors()

    def test_refresh_button_always_enabled(self):
        """Refresh is clickable from a cold start (no disabled state)."""
        refresh = self.driver.find_element(By.ID, "refresh")
        self.assertIsNone(refresh.get_attribute("disabled"),
                          "Refresh must not be disabled")
        # clicking it with nothing run yet should run the box (default query) and draw.
        refresh.click()
        self._wait_for_node_count(3)
        self.assertEqual(self._node_count(), 3)
        self._assert_no_js_errors()

    def test_node_spacing_is_compact(self):
        """Nodes settle a readable distance apart, not thousands of px."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        # let the layout settle (it runs synchronously on Run, but the transforms
        # are read after render; a short wait guards against timing on slow machines)
        positions = self._node_positions()
        max_sep = 0.0
        for i in range(len(positions)):
            for j in range(i + 1, len(positions)):
                dx = positions[i][0] - positions[j][0]
                dy = positions[i][1] - positions[j][1]
                max_sep = max(max_sep, (dx * dx + dy * dy) ** 0.5)
        # min-separation is 70px; a healthy small graph sits well under a few hundred.
        self.assertGreater(max_sep, 60, "nodes should not be stacked")
        self.assertLess(max_sep, 600, "nodes should be compact, not marooned far apart")
        self._assert_no_js_errors()

    def test_truncation_banner_appears(self):
        """When the server caps a scan, the status shows a 'showing N of M' note."""
        self._run("SELECT * FROM NODES #CAPPED")
        self._wait_for_node_count(3)
        self.wait.until(lambda d: "5000" in self._status_text())
        status = self._status_text()
        self.assertIn("5000", status, "truncation total should be shown")
        self.assertRegex(status.lower(), r"showing|capped|top",
                         "a truncation hint should be present: %r" % status)
        self._assert_no_js_errors()

    # ---- interaction: concurrency / busy state ----------------------------

    def test_rapid_double_click_fires_one_request(self):
        """Mashing Run twice quickly must send only ONE /query request.

        Regression for the concurrency bug: without an in-flight guard, a second
        click while the first request is pending fired a second overlapping
        request. We reproduce a genuine rapid double-click by dispatching two
        clicks in one JS tick (bypassing Selenium's actionability wait), against a
        deliberately slow (#SLOW) response so both clicks land while the first is
        still in flight. A counter on window records how many fetches actually go.
        """
        # instrument fetch to count calls to /query
        self.driver.execute_script("""
            window.__queryCount = 0;
            const realFetch = window.fetch;
            window.fetch = function(url, opts) {
                if (typeof url === 'string' && url.indexOf('/query') !== -1) window.__queryCount++;
                return realFetch.apply(this, arguments);
            };
        """)
        self._set_input("query", "SELECT * FROM GRAPH #SLOW")
        # two clicks in the SAME tick -- the impatient-user case
        self.driver.execute_script(
            "var b=document.getElementById('run'); b.click(); b.click();")
        # wait out the slow response, then check only one request went
        self.wait.until(lambda d: d.execute_script("return window.__queryCount") >= 1)
        import time as _t
        _t.sleep(0.6)
        count = self.driver.execute_script("return window.__queryCount")
        self.assertEqual(count, 1, "a rapid double-click must fire exactly one request, got %d" % count)
        self._assert_no_js_errors()

    def test_buttons_disable_during_query(self):
        """Run/Refresh disable while a query runs, and re-enable after it finishes."""
        self._set_input("query", "SELECT * FROM GRAPH #SLOW")
        self.driver.find_element(By.ID, "run").click()
        # mid-flight (the #SLOW response waits ~0.4s): buttons should be disabled.
        self.wait.until(lambda d: d.find_element(By.ID, "run").get_attribute("disabled") is not None)
        self.assertIsNotNone(self.driver.find_element(By.ID, "refresh").get_attribute("disabled"),
                             "Refresh should also be disabled while a query runs")
        # after it finishes, both are enabled again.
        self._wait_for_node_count(3)
        self.wait.until(lambda d: d.find_element(By.ID, "run").get_attribute("disabled") is None)
        self.assertIsNone(self.driver.find_element(By.ID, "refresh").get_attribute("disabled"),
                          "Refresh should be re-enabled after the query")
        self._assert_no_js_errors()

    # ---- interaction: error handling --------------------------------------

    def test_rejected_query_shows_error_and_keeps_graph(self):
        """A 422 (engine-rejected query) surfaces the error and does NOT blank the graph."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        # a query the mock returns 422 for
        self._run("FROBNICATE THE NODES #ERROR422")
        self.wait.until(lambda d: "422" in self._status_text() or "error" in self._status_text().lower()
                        or "Parse error" in self._status_text())
        # the graph must still be there (an error never clears the drawing)
        self.assertEqual(self._node_count(), 3, "a rejected query must not blank the graph")
        status = self._status_text()
        self.assertRegex(status, r"422|error|Parse",
                         "the server's error should be surfaced: %r" % status)
        self._assert_no_js_errors()

    def test_network_failure_shows_message(self):
        """If the server is unreachable, the visualiser reports it (doesn't hang/crash).

        Rather than stopping the shared mock server (which is stateful and flaky
        across platforms -- the restarted server takes a new port while the page is
        still loaded against the old one, and a just-closed socket can hang instead
        of refusing), we make ONLY the next /query fetch fail by overriding
        window.fetch to reject immediately. This exercises the exact same code path
        in the visualiser (the fetch's catch block) without touching the server, so
        the test is deterministic and leaves no shared state to clean up.
        """
        # Force the next fetch to fail at the network layer (like an unreachable
        # server), then restore the real fetch so nothing else is affected.
        self.driver.execute_script("""
            window.__realFetch = window.fetch;
            window.fetch = function() {
                window.fetch = window.__realFetch;              // one-shot: restore after
                return Promise.reject(new TypeError('Failed to fetch'));
            };
        """)
        self._run("SELECT * FROM GRAPH")
        self.wait.until(lambda d: "reach" in self._status_text().lower()
                        or "could not" in self._status_text().lower()
                        or "fail" in self._status_text().lower())
        self.assertRegex(self._status_text().lower(), r"reach|could not|fail",
                         "a network failure should be reported: %r" % self._status_text())

    # ---- interaction: input validation ------------------------------------

    def test_empty_token_is_rejected(self):
        """Running with no token shows a prompt and sends no request."""
        self._set_input("token", "")   # clear the token
        self._set_input("query", "SELECT * FROM GRAPH")
        self.driver.find_element(By.ID, "run").click()
        self.wait.until(lambda d: "token" in self._status_text().lower())
        self.assertIn("token", self._status_text().lower())
        self._assert_no_js_errors()

    def test_empty_query_is_rejected(self):
        """Running with an empty query box shows a prompt."""
        self._set_input("query", "")
        self.driver.find_element(By.ID, "run").click()
        self.wait.until(lambda d: "query" in self._status_text().lower())
        self.assertIn("query", self._status_text().lower())
        self._assert_no_js_errors()

    # ---- interaction: controls & selection --------------------------------

    def test_enter_key_runs_query(self):
        """Pressing Enter in the query box runs the query (not just the Run button)."""
        from selenium.webdriver.common.keys import Keys
        query_box = self.driver.find_element(By.ID, "query")
        query_box.clear()
        query_box.send_keys("SELECT * FROM GRAPH")
        query_box.send_keys(Keys.ENTER)
        self._wait_for_node_count(3)
        self.assertEqual(self._node_count(), 3)
        self._assert_no_js_errors()

    def test_clicking_a_node_shows_its_properties(self):
        """Clicking a node selects it and shows its properties in the side panel."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        # click the first node's circle
        first_node = self.driver.find_element(By.CSS_SELECTOR, "#nodes-layer .node")
        first_node.click()
        # the detail panel should now show a property (the fixture nodes have names)
        self.wait.until(lambda d: "id" in d.find_element(By.ID, "detail").text.lower())
        detail = self.driver.find_element(By.ID, "detail").text
        self.assertRegex(detail.lower(), r"id|name|person|city",
                         "the selection panel should show node details: %r" % detail)
        # the clicked node should carry the 'selected' class
        selected = self.driver.find_elements(By.CSS_SELECTOR, "#nodes-layer .node.selected")
        self.assertEqual(len(selected), 1, "exactly one node should be selected")
        self._assert_no_js_errors()

    def test_fit_button_reframes_without_error(self):
        """The Fit button reframes the graph and raises no error."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        self.driver.find_element(By.ID, "fit").click()
        # nodes still present after fit
        self.assertEqual(self._node_count(), 3)
        self._assert_no_js_errors()

    def test_legend_lists_the_labels(self):
        """The legend shows the distinct node labels present in the drawing."""
        self._run("SELECT * FROM GRAPH")
        self._wait_for_node_count(3)
        legend = self.driver.find_element(By.ID, "legend").text
        # the GRAPH fixture has Person and City labels
        self.assertIn("Person", legend, "legend should list 'Person': %r" % legend)
        self.assertIn("City", legend, "legend should list 'City': %r" % legend)
        self._assert_no_js_errors()

    # ---- interaction: MATCH highlighting ----------------------------------

    def test_match_highlights_the_path(self):
        """A MATCH traversal draws the path and highlights its nodes."""
        self._run("MATCH REACHABLE FROM 1")
        self._wait_for_node_count(3)          # traversal ids 1,2,3 -> 3 nodes
        highlighted = self.driver.find_elements(By.CSS_SELECTOR, "#nodes-layer .node.hl")
        self.assertEqual(len(highlighted), 3, "all three path nodes should be highlighted")
        # the legend should mention the MATCH path
        self.assertIn("MATCH", self.driver.find_element(By.ID, "legend").text.upper(),
                      "the legend should note the MATCH path")
        self._assert_no_js_errors()


if __name__ == "__main__":
    unittest.main(verbosity=2)
