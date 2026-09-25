"""
stress_dashboard.py -- the Tkinter dashboard for the Suede stress test.

A live control panel and monitor for hammering a running SuedeServer with a mixed
workload and watching it in real time. This is the entry point -- run this file:

    python stress_dashboard.py

WHAT IT DOES
  * Config panel: server URL, bearer token, worker-thread count, total requests,
    the workload mix (SELECT/INSERT/UPDATE/MATCH weights), LIVE vs SNAPSHOT reads,
    and how many nodes to seed first.
  * Seed: optionally builds a graph of N nodes before the load starts, so that
    reads and traversals hit real rows. In SNAPSHOT mode it also captures a snapshot
    after seeding.
  * Start / Stop: runs the load on a background thread so the GUI stays responsive.
  * Live tiles: current req/s, issued-so-far, success and failure counts, elapsed.
  * Live charts (matplotlib, embedded): rolling req/s over time, and per-operation
    median latency bars (client-ms AND server-ms side by side) -- the view where you
    SEE writes bottleneck while reads fly, which is the whole point of the exercise.
  * CSV logging: every run writes <run>_requests.csv and <run>_summary.csv via
    CsvLogger, so results persist and can be charted later.

DEPENDENCIES: tkinter (standard library) + matplotlib (pip: `pip install
matplotlib`). The load engine (stress_client) and logger (stress_logger) are pure
standard library; only this view layer needs matplotlib.

THREADING MODEL (important):
  Tkinter is not thread-safe: widgets may be touched ONLY from the GUI thread. But
  the load engine calls our result callback from many worker threads at once. So the
  callback does the minimum -- it appends each result to a thread-safe holding area
  -- and a periodic timer callback running ON the GUI thread drains that area and
  updates the widgets and charts. That "workers enqueue, GUI-thread drains" split is
  the standard safe pattern for a live Tkinter dashboard.
"""

import os
import threading
import time
import collections

import tkinter as tk
from tkinter import ttk, messagebox

import matplotlib
matplotlib.use("TkAgg")   # render matplotlib figures inside Tkinter
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

from stress_client import StressClient, Workload, sendOneCommand, sendManyCommands
from stress_logger import CsvLogger


# The default operation mix, and the fixed order operations appear in the charts.
DEFAULT_OPERATION_MIX = {"SELECT": 70, "INSERT": 15, "UPDATE": 10, "MATCH": 5}
OPERATION_ORDER = ["SELECT", "INSERT", "UPDATE", "MATCH"]

# How often the GUI drains results and refreshes (milliseconds).
GUI_REFRESH_MS = 200

# The seed graph uses these labels; kept in sync with Workload.NODE_LABELS.
SEED_LABELS = ["Person", "City", "Product", "Company", "Event"]


class Dashboard:
    def __init__(self, root):
        self.root = root
        root.title("Suede Graph Database -- Stress Test")
        root.geometry("1100x760")

        # --- data shared between the worker callback and the GUI thread ---
        # Workers append RequestResults here; the GUI thread drains it each tick.
        # A deque with a lock is our simple thread-safe hand-off.
        self.pendingResults = collections.deque()
        self.pendingResultsLock = threading.Lock()

        # Rolling aggregates that the charts read. Touched only on the GUI thread.
        self.throughputHistory = collections.deque(maxlen=120)  # last ~2 min of req/s
        self.requestsThisSecond = 0
        self.lastThroughputTick = time.time()
        self.clientMillisByOperation = {
            op: collections.deque(maxlen=500) for op in OPERATION_ORDER}
        self.serverMicrosByOperation = {
            op: collections.deque(maxlen=500) for op in OPERATION_ORDER}

        # --- run state ---
        self.loadClient = None       # the StressClient currently running (or None)
        self.csvLogger = None        # the CsvLogger for the current run (or None)
        self.runThread = None        # background thread the run executes on
        self.isRunning = False
        # "phase" tracks WHERE we are so the tiles read sensibly: "idle" before a
        # run, "seeding" while building the graph (no load yet), "load" once real
        # requests are flying. runStartTime is set when the LOAD starts, not at
        # button press -- so the elapsed timer measures the load, not the seed.
        self.phase = "idle"
        self.seededSoFar = 0
        self.seedTarget = 0
        self.runStartTime = None
        self.totalIssued = 0
        self.totalSucceeded = 0
        self.totalFailed = 0
        self.totalTarget = 0

        self._buildUi()
        # Kick off the periodic GUI updater (drains results, refreshes tiles+charts).
        self.root.after(GUI_REFRESH_MS, self._onGuiTick)

    # ----------------------------------------------------------------- UI ---

    def _buildUi(self):
        configFrame = ttk.LabelFrame(self.root, text="Configuration", padding=8)
        configFrame.pack(fill="x", padx=8, pady=6)

        def labelledEntry(parent, labelText, defaultValue, entryWidth=22):
            """Build a small [label / entry] stack and return its StringVar."""
            holder = ttk.Frame(parent)
            holder.pack(side="left", padx=6)
            ttk.Label(holder, text=labelText).pack(anchor="w")
            variable = tk.StringVar(value=str(defaultValue))
            ttk.Entry(holder, textvariable=variable, width=entryWidth).pack()
            return variable

        # The URL and token default to the SUEDE_URL / SUEDE_TOKEN environment
        # variables when present, so a launcher (Run-Stress-Test.bat) can start the
        # server, mint a token, and pass both in -- the boxes arrive pre-filled and
        # you just press Start. When the vars are absent the boxes fall back to their
        # old defaults (empty token), so running the dashboard by hand still works.
        defaultUrl = os.environ.get("SUEDE_URL", "http://127.0.0.1:8080")
        defaultToken = os.environ.get("SUEDE_TOKEN", "")
        self.urlVar = labelledEntry(configFrame, "Server URL", defaultUrl)
        self.tokenVar = labelledEntry(configFrame, "Bearer token", defaultToken, entryWidth=30)
        self.threadsVar = labelledEntry(configFrame, "Threads", "8", entryWidth=6)
        self.totalRequestsVar = labelledEntry(configFrame, "Total requests", "100000", entryWidth=10)
        self.seedNodesVar = labelledEntry(configFrame, "Seed nodes", "10000", entryWidth=8)

        mixFrame = ttk.LabelFrame(self.root, text="Workload mix (weights) & mode", padding=8)
        mixFrame.pack(fill="x", padx=8, pady=6)
        self.selectWeightVar = labelledEntry(mixFrame, "SELECT", DEFAULT_OPERATION_MIX["SELECT"], entryWidth=6)
        self.insertWeightVar = labelledEntry(mixFrame, "INSERT", DEFAULT_OPERATION_MIX["INSERT"], entryWidth=6)
        self.updateWeightVar = labelledEntry(mixFrame, "UPDATE", DEFAULT_OPERATION_MIX["UPDATE"], entryWidth=6)
        self.matchWeightVar = labelledEntry(mixFrame, "MATCH", DEFAULT_OPERATION_MIX["MATCH"], entryWidth=6)

        modeHolder = ttk.Frame(mixFrame)
        modeHolder.pack(side="left", padx=12)
        ttk.Label(modeHolder, text="Read mode").pack(anchor="w")
        self.readModeVar = tk.StringVar(value="LIVE")
        ttk.Combobox(modeHolder, textvariable=self.readModeVar,
                     values=["LIVE", "SNAPSHOT"], width=10, state="readonly").pack()

        buttonHolder = ttk.Frame(mixFrame)
        buttonHolder.pack(side="left", padx=12)
        ttk.Label(buttonHolder, text=" ").pack()   # spacer to align buttons with entries
        self.startButton = ttk.Button(buttonHolder, text="Start", command=self.onStart)
        self.startButton.pack(side="left", padx=3)
        self.stopButton = ttk.Button(buttonHolder, text="Stop",
                                     command=self.onStop, state="disabled")
        self.stopButton.pack(side="left", padx=3)

        # --- live tiles ---
        tilesFrame = ttk.Frame(self.root)
        tilesFrame.pack(fill="x", padx=8, pady=4)
        self.tileVars = {}
        tileDefinitions = [
            ("reqPerSec", "req/s"),
            ("issued", "issued / total"),
            ("succeeded", "ok"),
            ("failed", "failed"),
            ("elapsed", "elapsed s"),
        ]
        for tileKey, tileLabel in tileDefinitions:
            tile = ttk.LabelFrame(tilesFrame, text=tileLabel, padding=6)
            tile.pack(side="left", expand=True, fill="x", padx=4)
            valueVar = tk.StringVar(value="-")
            ttk.Label(tile, textvariable=valueVar, font=("Segoe UI", 16, "bold")).pack()
            self.tileVars[tileKey] = valueVar

        # --- status line ---
        self.statusVar = tk.StringVar(value="Idle. Configure and press Start.")
        ttk.Label(self.root, textvariable=self.statusVar).pack(anchor="w", padx=10)

        # --- charts ---
        chartsFrame = ttk.Frame(self.root)
        chartsFrame.pack(fill="both", expand=True, padx=8, pady=6)
        self.figure = Figure(figsize=(10, 4.2), dpi=100)
        self.throughputAxes = self.figure.add_subplot(1, 2, 1)
        self.latencyAxes = self.figure.add_subplot(1, 2, 2)
        self.throughputAxes.set_title("Throughput (req/s)")
        self.throughputAxes.set_xlabel("seconds")
        self.throughputAxes.set_ylabel("req/s")
        self.latencyAxes.set_title("Median latency by op")
        self.latencyAxes.set_ylabel("ms")
        self.figure.tight_layout()
        self.figureCanvas = FigureCanvasTkAgg(self.figure, master=chartsFrame)
        self.figureCanvas.get_tk_widget().pack(fill="both", expand=True)

    # ------------------------------------------------------------- actions ---

    def _readOperationMix(self):
        """Read the four weight boxes into a mix dict, dropping any zero weights."""
        def readInt(variable, fallback):
            try:
                return max(0, int(variable.get()))
            except ValueError:
                return fallback

        mix = {
            "SELECT": readInt(self.selectWeightVar, 70),
            "INSERT": readInt(self.insertWeightVar, 15),
            "UPDATE": readInt(self.updateWeightVar, 10),
            "MATCH": readInt(self.matchWeightVar, 5),
        }
        # Drop zero-weight operations so random.choices() never gets an all-zero
        # weight vector (which would raise). Guarantee at least one operation.
        mix = {operation: weight for operation, weight in mix.items() if weight > 0}
        if not mix:
            mix = {"SELECT": 1}
        return mix

    def onStart(self):
        if self.isRunning:
            return

        serverUrl = self.urlVar.get().strip()
        token = self.tokenVar.get().strip()
        if not token:
            messagebox.showerror(
                "Missing token",
                "Enter a bearer token (get one with: SuedeServer --mint 127.0.0.1).")
            return
        try:
            workerCount = max(1, int(self.threadsVar.get()))
            totalRequests = max(1, int(self.totalRequestsVar.get()))
            seedNodeCount = max(0, int(self.seedNodesVar.get()))
        except ValueError:
            messagebox.showerror("Bad number",
                                 "Threads / total requests / seed nodes must be integers.")
            return
        operationMix = self._readOperationMix()
        readMode = self.readModeVar.get()

        # Reset all counters and chart history for a fresh run.
        self.totalIssued = self.totalSucceeded = self.totalFailed = 0
        self.totalTarget = totalRequests
        self.throughputHistory.clear()
        for series in self.clientMillisByOperation.values():
            series.clear()
        for series in self.serverMicrosByOperation.values():
            series.clear()
        self.requestsThisSecond = 0
        self.lastThroughputTick = time.time()

        # Everything network-touching runs on a background thread so the GUI never
        # freezes (seeding a large graph can take a while).
        self.isRunning = True
        self.startButton.config(state="disabled")
        self.stopButton.config(state="normal")
        # We start in the seeding phase (or go straight to load if seedNodeCount is
        # 0). runStartTime is deliberately left None here and set when the load
        # actually begins, so "elapsed" doesn't tick through the seed.
        self.phase = "seeding" if seedNodeCount > 0 else "load"
        self.seedTarget = seedNodeCount
        self.seededSoFar = 0
        self.runStartTime = None
        self.runThread = threading.Thread(
            target=self._runEverything,
            args=(serverUrl, token, workerCount, totalRequests,
                  seedNodeCount, operationMix, readMode),
            daemon=True)
        self.runThread.start()

    def _runEverything(self, serverUrl, token, workerCount, totalRequests,
                       seedNodeCount, operationMix, readMode):
        """The whole run, on a background thread: seed -> (snapshot) -> load."""
        try:
            # 1. Optionally seed a graph so reads/matches hit real rows.
            if seedNodeCount > 0:
                self._setStatus("Seeding %d nodes..." % seedNodeCount)
                self._seedGraph(serverUrl, token, seedNodeCount)

            # 2. For SNAPSHOT mode, capture a snapshot after seeding and learn its id.
            snapshotId = None
            if readMode == "SNAPSHOT":
                self._setStatus("Creating snapshot...")
                status, response = sendOneCommand(serverUrl, token, "SNAPSHOT CREATE")
                # The message looks like "Snapshot created with ID: 1" -- pull the id.
                message = str(response.get("message", ""))
                snapshotId = 1
                for token_ in message.replace(":", " ").split():
                    if token_.isdigit():
                        snapshotId = int(token_)
                self._setStatus("Snapshot %d created; starting load..." % snapshotId)

            # 3. Wire up the logger and the load client, then run the load.
            self.csvLogger = CsvLogger(outputDir=".", summaryWindowSeconds=1.0)
            self.csvLogger.start()
            workload = Workload(operationMix,
                                idSpace=max(1, seedNodeCount),
                                snapshotId=snapshotId)
            self.loadClient = StressClient(
                serverUrl, token, workload,
                workerCount=workerCount, totalRequests=totalRequests,
                onResult=self._onRequestResult)
            # The load proper begins NOW -- start the elapsed clock here (not at
            # button press) and flip into the load phase so the tiles switch from
            # the seeding view to live throughput.
            self.phase = "load"
            self.runStartTime = time.time()
            self.lastThroughputTick = self.runStartTime
            self.requestsThisSecond = 0
            self._setStatus("Load running: %d requests, %d threads, %s reads."
                            % (totalRequests, workerCount, readMode))
            self.loadClient.run()
            self._setStatus("Load complete.")
        except Exception as error:
            self._setStatus("ERROR: %s" % error)
        finally:
            if self.csvLogger is not None:
                self.csvLogger.stop()
            self.isRunning = False
            # Re-enable Start / disable Stop back on the GUI thread.
            self.root.after(0, self._onRunFinished)

    def _seedGraph(self, serverUrl, token, seedNodeCount):
        """Insert `seedNodeCount` nodes to give the load something real to read.
        NOT part of the measured load.

        Uses sendManyCommands so all the inserts go over ONE reused connection --
        the previous version opened a fresh connection per node, which made seeding
        10k nodes take minutes purely in TCP setup/teardown. The commands are
        produced lazily by a generator so we never build a 10k-element list."""
        def commandStream():
            for i in range(1, seedNodeCount + 1):
                label = SEED_LABELS[i % len(SEED_LABELS)]
                yield ("INSERT INTO NODES (label, name, n) VALUES ('%s', 'seed_%d', '%d')"
                       % (label, i, i))

        def onProgress(done):
            self.seededSoFar = done
            self._setStatus("Seeding... %d / %d" % (done, seedNodeCount))

        sendManyCommands(
            serverUrl, token, commandStream(),
            onProgress=onProgress, progressEvery=500,
            shouldStop=lambda: not self.isRunning)
        self.seededSoFar = seedNodeCount

    def _onRequestResult(self, result):
        """CALLED FROM WORKER THREADS. Must not touch Tkinter here -- just log the
        result to CSV and hand it to the GUI thread via the thread-safe deque."""
        self.csvLogger.log(result)
        with self.pendingResultsLock:
            self.pendingResults.append(result)

    def _onRunFinished(self):
        self.phase = "idle"
        self.startButton.config(state="normal")
        self.stopButton.config(state="disabled")

    def onStop(self):
        if self.loadClient is not None:
            self.loadClient.stop()
        self._setStatus("Stopping...")

    def _setStatus(self, text):
        """Update the status line. Safe from any thread -- marshals onto the GUI
        thread via root.after (Tk widgets must be touched on the GUI thread only)."""
        self.root.after(0, lambda: self.statusVar.set(text))

    # --------------------------------------------------------- GUI updater ---

    def _onGuiTick(self):
        """Runs on the GUI thread every GUI_REFRESH_MS: drain worker results, fold
        them into the rolling aggregates, and refresh the tiles (and charts once a
        second)."""
        # 1. Drain everything the workers have produced since the last tick.
        drainedResults = []
        with self.pendingResultsLock:
            if self.pendingResults:
                drainedResults = list(self.pendingResults)
                self.pendingResults.clear()

        for result in drainedResults:
            self.totalIssued += 1
            if result.succeeded:
                self.totalSucceeded += 1
            else:
                self.totalFailed += 1
            self.requestsThisSecond += 1
            if result.operation in self.clientMillisByOperation:
                self.clientMillisByOperation[result.operation].append(result.clientMillis)
                self.serverMicrosByOperation[result.operation].append(result.serverMicros)

        # 2. Once a second has passed, record the throughput and refresh the charts.
        now = time.time()
        if now - self.lastThroughputTick >= 1.0:
            self.throughputHistory.append(self.requestsThisSecond)
            self.requestsThisSecond = 0
            self.lastThroughputTick = now
            self._refreshCharts()

        # 3. Update the numeric tiles every tick. The tiles read differently by
        # phase so a long seed doesn't look like a hung run showing zeros.
        if self.phase == "seeding":
            # No load yet -- show seed progress in place of throughput/issued, and
            # make the elapsed tile say "seeding" rather than counting.
            self.tileVars["reqPerSec"].set("seeding")
            self.tileVars["issued"].set("%d / %d" % (self.seededSoFar, self.seedTarget))
            self.tileVars["succeeded"].set("-")
            self.tileVars["failed"].set("-")
            self.tileVars["elapsed"].set("seeding")
        else:
            # Load phase. Show a LIVE req/s that includes the current (partial)
            # second's progress, so the number moves immediately instead of waiting
            # a full second for the first history bucket. We divide this second's
            # count so far by how much of the second has elapsed.
            secondFraction = now - self.lastThroughputTick
            if secondFraction >= 0.05:
                liveReqPerSec = self.requestsThisSecond / secondFraction
            elif self.throughputHistory:
                liveReqPerSec = self.throughputHistory[-1]
            else:
                liveReqPerSec = 0
            self.tileVars["reqPerSec"].set("%.0f" % liveReqPerSec)
            self.tileVars["issued"].set("%d / %d" % (self.totalIssued, self.totalTarget))
            self.tileVars["succeeded"].set(str(self.totalSucceeded))
            self.tileVars["failed"].set(str(self.totalFailed))
            self.tileVars["elapsed"].set(
                "%.1f" % (now - self.runStartTime) if self.runStartTime else "0.0")

        self.root.after(GUI_REFRESH_MS, self._onGuiTick)

    @staticmethod
    def _median(values):
        """Median of an iterable of numbers; 0.0 for empty."""
        orderedValues = sorted(values)
        count = len(orderedValues)
        if count == 0:
            return 0.0
        middle = count // 2
        if count % 2:
            return orderedValues[middle]
        return (orderedValues[middle - 1] + orderedValues[middle]) / 2.0

    def _refreshCharts(self):
        # --- throughput line: req/s over the last ~2 minutes ---
        self.throughputAxes.clear()
        self.throughputAxes.set_title("Throughput (req/s)")
        self.throughputAxes.set_xlabel("seconds")
        self.throughputAxes.set_ylabel("req/s")
        if self.throughputHistory:
            self.throughputAxes.plot(range(len(self.throughputHistory)),
                                     list(self.throughputHistory))

        # --- per-operation median latency bars: client-ms vs server-ms ---
        # This is the money shot: writes (INSERT/UPDATE) serialise on the exclusive
        # lock, so their bars should tower over the read (SELECT/MATCH) bars.
        self.latencyAxes.clear()
        self.latencyAxes.set_title("Median latency by op")
        clientMedians = [self._median(self.clientMillisByOperation[op])
                         for op in OPERATION_ORDER]
        # server times are microseconds; divide by 1000 to plot on the same ms axis.
        serverMediansMs = [self._median(self.serverMicrosByOperation[op]) / 1000.0
                           for op in OPERATION_ORDER]
        barPositions = range(len(OPERATION_ORDER))
        barWidth = 0.38
        self.latencyAxes.bar([p - barWidth / 2 for p in barPositions],
                             clientMedians, barWidth, label="client ms")
        self.latencyAxes.bar([p + barWidth / 2 for p in barPositions],
                             serverMediansMs, barWidth, label="server ms")
        self.latencyAxes.set_xticks(list(barPositions))
        self.latencyAxes.set_xticklabels(OPERATION_ORDER)
        self.latencyAxes.set_ylabel("ms")
        self.latencyAxes.legend(fontsize=8)

        self.figure.tight_layout()
        self.figureCanvas.draw_idle()


def main():
    root = tk.Tk()
    Dashboard(root)
    root.mainloop()


if __name__ == "__main__":
    main()
