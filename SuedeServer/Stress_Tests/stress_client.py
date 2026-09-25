"""
stress_client.py -- the load engine for the Suede stress test.

A pool of worker threads fires a MIXED workload (SELECT / INSERT / UPDATE / MATCH)
at a running SuedeServer as fast as it can, records per-request metrics, and hands
each result to a callback. The dashboard consumes those results live; the logger
persists them to CSV. This module is pure standard library -- no pip dependencies
-- so the load engine itself stays dependency-free; only the dashboard's charts
use matplotlib.

WHAT WE MEASURE, per request:
  * operation        -- the operation type ("SELECT" / "INSERT" / "UPDATE" / "MATCH")
  * clientMillis     -- client-side round-trip time in milliseconds. This includes
                        HTTP transport and Python overhead -- i.e. what a real API
                        caller actually experiences.
  * serverMicros     -- the SERVER'S own service time in microseconds, read from the
                        `serverMicros` field the server returns. This is the true
                        engine time, isolated from all network and client cost.
  * httpStatus       -- HTTP status code (200 ok, 422 rejected, 0 = never completed)
  * succeeded        -- True only when the HTTP status was 2xx AND the engine itself
                        reported success in the response body.

The gap between clientMillis and serverMicros is the HTTP+Python tax. Reporting
both is how we honour the "discount Python clock time" requirement: serverMicros is
the number to trust for engine performance, clientMillis for end-to-end experience.

WHY THREADS (not processes, not a single loop):
  Firing requests one at a time from a single thread would measure "how fast can one
  Python thread dribble HTTP requests", not the server's ceiling. A pool of worker
  threads keeps many requests in flight at once, so the SERVER becomes the
  bottleneck rather than the client. Threads (not processes) are correct here
  because the work is I/O-bound -- each worker spends almost all its time blocked on
  the socket waiting for a reply, which is exactly where Python's GIL steps aside
  and lets other threads run.
"""

import json
import time
import threading
import random
import http.client
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse


# ---------------------------------------------------------------------------
# RequestResult -- the record produced for a single completed request.
#
# __slots__ keeps these small and fast to allocate: we may create hundreds of
# thousands of them during a run, so avoiding a per-instance __dict__ matters.
# ---------------------------------------------------------------------------
class RequestResult:
    __slots__ = ("operation", "clientMillis", "serverMicros",
                 "httpStatus", "succeeded", "errorText", "finishedAtEpoch")

    def __init__(self, operation, clientMillis, serverMicros,
                 httpStatus, succeeded, errorText, finishedAtEpoch):
        self.operation = operation             # "SELECT" / "INSERT" / "UPDATE" / "MATCH"
        self.clientMillis = clientMillis       # round-trip time, milliseconds (client side)
        self.serverMicros = serverMicros       # server engine time, microseconds (0 if absent)
        self.httpStatus = httpStatus           # HTTP status; 0 means the request never completed
        self.succeeded = succeeded             # True on a genuine success (2xx + engine success)
        self.errorText = errorText             # short error description, or "" on success
        self.finishedAtEpoch = finishedAtEpoch # epoch seconds when the request finished


# ---------------------------------------------------------------------------
# Workload -- turns a desired operation "mix" into concrete command strings.
#
# The mix is a dict of operation -> weight, e.g. {"SELECT":70, "INSERT":15,
# "UPDATE":10, "MATCH":5}. pickOperation() draws an operation at random by those
# weights; commandFor() builds a concrete query string for it. Queries are
# randomised over a bounded id space so they exercise real rows without the client
# needing to know the graph's exact contents.
# ---------------------------------------------------------------------------
class Workload:
    # The labels the seeded graph uses; commands are generated against these so a
    # label scan actually finds rows.
    NODE_LABELS = ["Person", "City", "Product", "Company", "Event"]

    def __init__(self, operationMix, idSpace=10000, snapshotId=None):
        # Split the mix dict into two parallel lists for random.choices().
        self.operations = list(operationMix.keys())
        self.operationWeights = [operationMix[op] for op in self.operations]
        # Ids are drawn from 1..idSpace; clamp to at least 1 so randint never fails.
        self.idSpace = max(1, idSpace)
        # When snapshotId is set, READ operations (SELECT / MATCH) are issued as
        # point-in-time SNAPSHOT reads. WRITES always stay LIVE, because the engine
        # rejects SNAPSHOT on a mutation by design -- so a write drawn while in
        # snapshot mode simply runs against the live graph (see commandFor).
        self.snapshotId = snapshotId

    def pickOperation(self):
        """Return one operation name, chosen at random by the configured weights."""
        return random.choices(self.operations, weights=self.operationWeights, k=1)[0]

    def commandFor(self, operation):
        """Build a (operation, commandString) pair for the given operation.

        The returned operation is echoed back (unchanged) so a caller that mapped an
        unknown op to a fallback still records the operation actually issued.
        """
        randomId = random.randint(1, self.idSpace)
        isSnapshotRead = self.snapshotId is not None

        if operation == "SELECT":
            # Half label-scans, half id-lookups -- these stress different code paths
            # (label index vs. the id fast-path). SNAPSHOT-tag it in snapshot mode.
            if random.random() < 0.5:
                label = random.choice(self.NODE_LABELS)
                command = "SELECT * FROM NODES WHERE LABEL = '%s'" % label
            else:
                command = "SELECT * FROM NODES WHERE ID = %d" % randomId
            if isSnapshotRead:
                command += " SNAPSHOT"
            return ("SELECT", command)

        if operation == "MATCH":
            # A spread of traversal shapes so we exercise REACHABLE / KHOP /
            # SHORTEST_PATH rather than only one.
            traversal = random.choice([
                "REACHABLE FROM %d" % randomId,
                "KHOP FROM %d STEPS 2" % randomId,
                "SHORTEST_PATH FROM %d TO %d" % (randomId, random.randint(1, self.idSpace)),
            ])
            command = "MATCH " + traversal
            if isSnapshotRead:
                command += " SNAPSHOT"
            return ("MATCH", command)

        if operation == "INSERT":
            label = random.choice(self.NODE_LABELS)
            command = ("INSERT INTO NODES (label, name, n) VALUES ('%s', 'node_%d', '%d')"
                       % (label, randomId, randomId))
            return ("INSERT", command)

        if operation == "UPDATE":
            command = ("UPDATE NODES WHERE ID = %d SET n = %d"
                       % (randomId, random.randint(0, 1_000_000)))
            return ("UPDATE", command)

        # Unknown operation -- fall back to the cheapest possible read.
        return ("SELECT", "SELECT * FROM NODES WHERE ID = 1")


# ---------------------------------------------------------------------------
# StressClient -- the load driver.
#
# Fires `totalRequests` requests at `baseUrl` using `workerCount` worker threads.
# onResult(RequestResult) is invoked for EVERY completed request, from the worker
# threads -- so the callback MUST be thread-safe (or, as the dashboard does, simply
# hand results off to a thread-safe queue and touch the GUI only from the GUI
# thread). stop() requests an early, graceful halt.
# ---------------------------------------------------------------------------
class StressClient:
    def __init__(self, baseUrl, token, workload, workerCount=8, totalRequests=10000,
                 onResult=None, requestTimeoutSeconds=15.0):
        parsedUrl = urlparse(baseUrl)
        self.host = parsedUrl.hostname or "127.0.0.1"
        self.port = parsedUrl.port or 8080
        self.token = token
        self.workload = workload
        self.workerCount = max(1, workerCount)
        self.totalRequests = max(0, totalRequests)
        self.onResult = onResult or (lambda result: None)
        self.requestTimeoutSeconds = requestTimeoutSeconds

        # Set when a caller asks us to stop early; every worker checks it each loop.
        self.stopRequested = threading.Event()

        # How many request "slots" have been claimed so far. Workers claim a slot
        # under a lock, then release the lock before doing the (slow) network call --
        # so the counter is the only thing serialised, not the requests themselves.
        self.requestsClaimed = 0
        self.claimLock = threading.Lock()

        # Each worker keeps its OWN persistent HTTP connection so we reuse sockets
        # instead of reconnecting per request. Thread-local storage gives each worker
        # thread a private connection with no sharing and no locking.
        self.threadLocal = threading.local()

    def stop(self):
        """Ask all workers to finish after their current request. Idempotent."""
        self.stopRequested.set()

    # ---- per-worker connection ------------------------------------------

    def _connectionForThisWorker(self):
        """Return this worker thread's persistent HTTP connection, opening it lazily
        on first use. Stored in thread-local storage, so each worker has exactly one."""
        connection = getattr(self.threadLocal, "connection", None)
        if connection is None:
            connection = http.client.HTTPConnection(
                self.host, self.port, timeout=self.requestTimeoutSeconds)
            self.threadLocal.connection = connection
        return connection

    # ---- issuing one request --------------------------------------------

    def _issueOneRequest(self):
        """Issue a single request and return a RequestResult. Never raises: any
        failure (timeout, connection reset, non-JSON body) becomes a failed result."""
        operation = self.workload.pickOperation()
        operation, command = self.workload.commandFor(operation)
        requestBody = json.dumps({"command": command})
        requestHeaders = {
            "Content-Type": "application/json",
            "Authorization": "Bearer " + self.token,
        }

        startTime = time.perf_counter()
        try:
            connection = self._connectionForThisWorker()
            connection.request("POST", "/query", body=requestBody, headers=requestHeaders)
            response = connection.getresponse()
            responseBytes = response.read()
            clientMillis = (time.perf_counter() - startTime) * 1000.0

            httpStatus = response.status
            serverMicros = 0
            succeeded = False
            errorText = ""
            try:
                parsed = json.loads(responseBytes)
                serverMicros = int(parsed.get("serverMicros", 0) or 0)
                # A genuine success requires BOTH a 2xx status AND the engine's own
                # success flag -- a 200 carrying {"success": false} is still a reject.
                succeeded = (200 <= httpStatus < 300) and bool(parsed.get("success", False))
                if not succeeded:
                    errorText = str(parsed.get("message")
                                    or parsed.get("error")
                                    or ("HTTP %d" % httpStatus))[:120]
            except (ValueError, AttributeError):
                errorText = "non-JSON response"

            return RequestResult(operation, clientMillis, serverMicros,
                                 httpStatus, succeeded, errorText, time.time())

        except Exception as networkError:
            # Timeout / connection reset / refused, etc. The connection may now be
            # broken, so drop it -- the worker reconnects on its next request. Report
            # a status-0 failure (0 = "never completed", distinct from any HTTP code).
            self.threadLocal.connection = None
            clientMillis = (time.perf_counter() - startTime) * 1000.0
            return RequestResult(operation, clientMillis, 0, 0, False,
                                 type(networkError).__name__, time.time())

    # ---- the worker loop ------------------------------------------------

    def _runWorkerLoop(self):
        """One worker: keep claiming and issuing requests until the batch is done or
        a stop was requested."""
        while not self.stopRequested.is_set():
            # Claim the next slot. If the batch is already fully claimed, this worker
            # is finished. Only the counter update is inside the lock; the network
            # call below runs unlocked so workers truly run in parallel.
            with self.claimLock:
                if self.requestsClaimed >= self.totalRequests:
                    return
                self.requestsClaimed += 1

            result = self._issueOneRequest()
            self.onResult(result)

    # ---- run the whole batch --------------------------------------------

    def run(self):
        """Run the entire batch, blocking until it finishes or stop() is called.

        Intended to be called on a BACKGROUND thread by the dashboard so the GUI
        stays responsive. Spawns `workerCount` workers, each pulling request slots
        until `totalRequests` is reached.
        """
        self.stopRequested.clear()
        self.requestsClaimed = 0
        with ThreadPoolExecutor(max_workers=self.workerCount) as pool:
            workerFutures = [pool.submit(self._runWorkerLoop)
                             for _ in range(self.workerCount)]
            for future in workerFutures:
                future.result()   # re-raise any unexpected worker exception here
        # Each worker's thread-local connection is closed automatically as its thread
        # ends, so there is nothing to clean up explicitly.


# ---------------------------------------------------------------------------
# sendOneCommand -- a simple one-off request, used by the dashboard for ADMIN
# work (seeding the graph, creating a snapshot) that is NOT part of the measured
# load. Opens a fresh connection, sends the command, returns (status, parsedJson).
# ---------------------------------------------------------------------------
def sendOneCommand(baseUrl, token, command, timeoutSeconds=30.0):
    parsedUrl = urlparse(baseUrl)
    connection = http.client.HTTPConnection(
        parsedUrl.hostname or "127.0.0.1",
        parsedUrl.port or 8080,
        timeout=timeoutSeconds)
    try:
        connection.request(
            "POST", "/query",
            body=json.dumps({"command": command}),
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + token})
        response = connection.getresponse()
        responseBytes = response.read()
        try:
            return response.status, json.loads(responseBytes)
        except ValueError:
            # Non-JSON body: return it as a failure message rather than raising.
            return response.status, {
                "success": False,
                "message": responseBytes[:200].decode("utf-8", "replace"),
            }
    finally:
        connection.close()


# ---------------------------------------------------------------------------
# sendManyCommands -- fire a sequence of ADMIN commands over a SINGLE reused
# connection. This is what seeding uses: opening a fresh connection per node (as
# sendOneCommand does) means thousands of TCP connect/close cycles, which is slow
# out of all proportion to the actual inserts. Reusing one keep-alive socket turns
# a ~2-minute seed of 10k nodes into a few seconds.
#
# `commands` is any iterable of command strings. `onProgress(doneCount)` -- if
# given -- is called every `progressEvery` commands so the caller can show a live
# count. `shouldStop()` -- if given -- is polled each command; returning True ends
# the run early (used by the dashboard's Stop button). Returns the number sent.
# ---------------------------------------------------------------------------
def sendManyCommands(baseUrl, token, commands, timeoutSeconds=30.0,
                     onProgress=None, progressEvery=500, shouldStop=None):
    parsedUrl = urlparse(baseUrl)
    host = parsedUrl.hostname or "127.0.0.1"
    port = parsedUrl.port or 8080
    headers = {"Content-Type": "application/json",
               "Authorization": "Bearer " + token}
    connection = http.client.HTTPConnection(host, port, timeout=timeoutSeconds)
    sentCount = 0
    try:
        for command in commands:
            if shouldStop is not None and shouldStop():
                break
            try:
                connection.request("POST", "/query",
                                   body=json.dumps({"command": command}),
                                   headers=headers)
                response = connection.getresponse()
                response.read()   # drain the body so the socket can be reused
            except Exception:
                # A dropped keep-alive socket (server closed it, timeout, etc.):
                # reconnect once and retry this one command, so a single hiccup
                # doesn't abort the whole seed.
                try:
                    connection.close()
                except Exception:
                    pass
                connection = http.client.HTTPConnection(host, port, timeout=timeoutSeconds)
                connection.request("POST", "/query",
                                   body=json.dumps({"command": command}),
                                   headers=headers)
                connection.getresponse().read()

            sentCount += 1
            if onProgress is not None and sentCount % progressEvery == 0:
                onProgress(sentCount)
    finally:
        connection.close()
    return sentCount
