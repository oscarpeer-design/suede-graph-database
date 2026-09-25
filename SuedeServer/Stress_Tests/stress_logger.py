"""
stress_logger.py -- thread-safe CSV logging for the stress test.

Many worker threads finish requests at the same time, so they must NOT all write to
the CSV directly -- interleaved writes from different threads would corrupt the
file. Instead, every result is dropped onto a thread-safe queue, and ONE dedicated
writer thread drains that queue and does all the actual writing. This serialises
file access cleanly, and keeps the hot path cheap: a worker just enqueues its result
and moves straight on to the next request.

Two CSV files are written per run, each name stamped with the start time so that
successive runs never overwrite one another:

  * <run>_requests.csv -- one row PER REQUEST:
        wall_time, operation, client_ms, server_us, http_status, succeeded, error
    This is the raw record. Chart it or post-process it however you like.

  * <run>_summary.csv  -- one row per aggregation WINDOW (default: every 1 second):
    the rolling throughput and latency picture over time -- requests/sec, success
    and failure counts, overall client/server latency percentiles, and a per-op
    breakdown. This is the file you would plot to tell the story of a run without
    wading through millions of raw rows.

Standard library only (csv, queue, threading, time).
"""

import csv
import queue
import threading
import time
import os
from datetime import datetime


class CsvLogger:
    def __init__(self, outputDir=".", runName=None, summaryWindowSeconds=1.0):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        baseName = runName or ("suede_stress_" + timestamp)
        os.makedirs(outputDir, exist_ok=True)
        self.requestsPath = os.path.join(outputDir, baseName + "_requests.csv")
        self.summaryPath = os.path.join(outputDir, baseName + "_summary.csv")
        self.summaryWindowSeconds = summaryWindowSeconds

        # Worker threads put RequestResults here; the writer thread gets them.
        self.resultQueue = queue.Queue()
        self.stopRequested = threading.Event()
        self.writerThread = None

        # The set of results seen so far in the current summary window, plus when the
        # window started. Touched only by the single writer thread, so no lock needed.
        self.currentWindowResults = []
        self.currentWindowStart = None

    # ---- lifecycle -------------------------------------------------------

    def start(self):
        """Start the background writer thread. Call once, before any log()."""
        self.writerThread = threading.Thread(target=self._writerLoop, daemon=True)
        self.writerThread.start()

    def log(self, result):
        """Enqueue one RequestResult. Safe to call from any worker thread."""
        self.resultQueue.put(result)

    def stop(self):
        """Flush and close the files. Blocks until the writer has drained the queue."""
        self.stopRequested.set()
        self.resultQueue.put(None)   # sentinel: wakes the writer if it is idle-waiting
        if self.writerThread is not None:
            self.writerThread.join(timeout=10)

    # ---- the single writer thread ---------------------------------------

    def _writerLoop(self):
        requestsFile = open(self.requestsPath, "w", newline="", encoding="utf-8")
        summaryFile = open(self.summaryPath, "w", newline="", encoding="utf-8")
        requestsWriter = csv.writer(requestsFile)
        summaryWriter = csv.writer(summaryFile)

        requestsWriter.writerow([
            "wall_time", "operation", "client_ms", "server_us",
            "http_status", "succeeded", "error"])
        summaryWriter.writerow([
            "window_end", "window_s", "requests", "req_per_s",
            "ok", "failed",
            "client_ms_p50", "client_ms_p95", "client_ms_p99",
            "server_us_p50", "server_us_p95", "server_us_p99",
            "select_n", "insert_n", "update_n", "match_n",
            "select_client_ms_p50", "insert_client_ms_p50",
            "update_client_ms_p50", "match_client_ms_p50"])
        requestsFile.flush()
        summaryFile.flush()

        try:
            while True:
                # Wait up to one window for the next result. The timeout guarantees we
                # still close windows on schedule even during a lull in traffic.
                try:
                    nextResult = self.resultQueue.get(timeout=self.summaryWindowSeconds)
                except queue.Empty:
                    nextResult = None

                now = time.time()
                if self.currentWindowStart is None:
                    self.currentWindowStart = now

                # A real result (not the idle-timeout None, not the stop sentinel):
                # write its raw row and add it to the current window.
                if nextResult is not None:
                    result = nextResult
                    requestsWriter.writerow([
                        ("%.3f" % result.finishedAtEpoch),
                        result.operation,
                        ("%.3f" % result.clientMillis),
                        result.serverMicros,
                        result.httpStatus,
                        int(result.succeeded),
                        result.errorText])
                    self.currentWindowResults.append(result)

                # Close the summary window once its duration has elapsed.
                if now - self.currentWindowStart >= self.summaryWindowSeconds:
                    self._writeSummaryRow(summaryWriter, now)
                    requestsFile.flush()
                    summaryFile.flush()
                    self.currentWindowResults = []
                    self.currentWindowStart = now

                # Exit once we've been asked to stop AND the queue is fully drained.
                if self.stopRequested.is_set() and self.resultQueue.empty():
                    # Emit one final partial window so trailing results aren't lost.
                    if self.currentWindowResults:
                        self._writeSummaryRow(summaryWriter, time.time())
                    break
        finally:
            requestsFile.flush()
            requestsFile.close()
            summaryFile.flush()
            summaryFile.close()

    # ---- summary aggregation --------------------------------------------

    @staticmethod
    def _percentile(sortedValues, percentile):
        """Linear-interpolated percentile of an ALREADY-SORTED list of numbers.

        A percentile answers "what latency was this run at or below X% of the time?"
        -- p95 = 40ms means 95% of requests finished in 40ms or under. We report
        percentiles rather than the mean because the mean hides tail latency: one
        2-second stall among thousands of fast requests barely moves the average, but
        it is exactly the stall a user notices. p50 (the median), p95 and p99 make
        that tail visible.

        To find (say) p95 we locate the point 95% of the way along the sorted list.
        That point almost never lands exactly on a real data point -- it usually falls
        BETWEEN two of them -- so we interpolate: we take the two neighbouring values
        that straddle the target and blend them in proportion to how close the target
        sits to each. That blend is the "linear interpolation".

        Worked example: 11 sorted values occupy indices 0..10. For p95, the target
        index is 10 * 0.95 = 9.5 -- halfway between index 9 and index 10 -- so the
        result is the average of those two values. (This matches numpy's default
        'linear' percentile method.)

        Returns 0.0 for an empty list (e.g. a window with no traffic). `percentile`
        is on a 0..100 scale (95 for p95, 50 for the median).
        """
        if not sortedValues:
            return 0.0
        if len(sortedValues) == 1:
            # A single sample IS every percentile of itself.
            return sortedValues[0]

        # Target position as a fractional index into the sorted list. We scale by
        # (len - 1), not len, because valid indices run 0..len-1: this makes p0 land
        # exactly on the first element and p100 exactly on the last.
        position = (len(sortedValues) - 1) * (percentile / 100.0)

        # The two real data points that straddle the fractional position, and how far
        # (0..1) the target sits between them. e.g. position 9.5 -> lower=9, upper=10,
        # fraction=0.5 (dead centre); fraction 0.9 would weight the upper value 9:1.
        lowerIndex = int(position)                                # truncates 9.5 -> 9
        upperIndex = min(lowerIndex + 1, len(sortedValues) - 1)
        # The min() clamp matters only when `position` is a whole number (e.g. p50 on
        # an odd-length list, or p100): there lowerIndex+1 would point one past the
        # end of the list. Clamping keeps upperIndex in bounds. In that case fraction
        # is 0, so the (clamped) upper value gets zero weight and the clamp can never
        # distort the answer -- it purely prevents an out-of-range index.
        fraction = position - lowerIndex

        # Weighted blend of the two straddling values (linear interpolation): the
        # closer the target is to the upper index, the more the upper value counts.
        return (sortedValues[lowerIndex] * (1 - fraction)
                + sortedValues[upperIndex] * fraction)

    def _writeSummaryRow(self, summaryWriter, windowEnd):
        """Aggregate the current window's results into one summary row and write it.

        One row summarises everything that happened in this ~1-second window: how many
        requests, how many succeeded/failed, the throughput, the overall latency
        percentiles, and a per-operation breakdown.
        """
        windowResults = self.currentWindowResults
        requestCount = len(windowResults)
        windowSeconds = self.summaryWindowSeconds
        requestsPerSecond = (requestCount / windowSeconds) if windowSeconds > 0 else 0.0
        successCount = sum(1 for result in windowResults if result.succeeded)
        failureCount = requestCount - successCount

        # Overall latency distributions across every operation in the window. _percentile
        # requires sorted input, so we sort here once and reuse for p50/p95/p99. We keep
        # BOTH client-side (ms, incl. HTTP+Python overhead) and server-side (us, pure
        # engine time) so a reader can see the network/Python tax as the gap between them.
        clientMillisSorted = sorted(result.clientMillis for result in windowResults)
        serverMicrosSorted = sorted(result.serverMicros for result in windowResults)
        pctl = self._percentile   # local alias to keep the writerow() call below readable

        # The SAME client latencies, but split into one bucket per operation type. This
        # split is the point of the whole stress test: writes (INSERT/UPDATE) serialise
        # on the engine's exclusive lock while reads (SELECT/MATCH) run concurrently, so
        # the per-op medians below should show the write buckets towering over the reads.
        # Each bucket is sorted because _percentile needs sorted input.
        clientMillisByOperation = {"SELECT": [], "INSERT": [], "UPDATE": [], "MATCH": []}
        for result in windowResults:
            if result.operation in clientMillisByOperation:
                clientMillisByOperation[result.operation].append(result.clientMillis)
        for operationName in clientMillisByOperation:
            clientMillisByOperation[operationName].sort()

        # Column order MUST match the summary header written in _writerLoop. The "%.3f"
        # / "%.1f" formatting just keeps the CSV tidy (3 dp for ms, 1 dp for us & req/s)
        # rather than emitting long raw floats.
        summaryWriter.writerow([
            ("%.3f" % windowEnd), ("%.3f" % windowSeconds),
            requestCount, ("%.1f" % requestsPerSecond),
            successCount, failureCount,
            ("%.3f" % pctl(clientMillisSorted, 50)),
            ("%.3f" % pctl(clientMillisSorted, 95)),
            ("%.3f" % pctl(clientMillisSorted, 99)),
            ("%.1f" % pctl(serverMicrosSorted, 50)),
            ("%.1f" % pctl(serverMicrosSorted, 95)),
            ("%.1f" % pctl(serverMicrosSorted, 99)),
            len(clientMillisByOperation["SELECT"]),
            len(clientMillisByOperation["INSERT"]),
            len(clientMillisByOperation["UPDATE"]),
            len(clientMillisByOperation["MATCH"]),
            ("%.3f" % pctl(clientMillisByOperation["SELECT"], 50)),
            ("%.3f" % pctl(clientMillisByOperation["INSERT"], 50)),
            ("%.3f" % pctl(clientMillisByOperation["UPDATE"], 50)),
            ("%.3f" % pctl(clientMillisByOperation["MATCH"], 50)),
        ])
