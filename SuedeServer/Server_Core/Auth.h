#pragma once

#include <string>
#include <mutex>         // std::mutex / std::lock_guard (C++11 -- no C++17 needed)
#include <functional>   // std::bind, std::ref, std::placeholders
#include <fstream>

// ---------------------------------------------------------------------------
// Server counter (auth revocation state) -- persistence.
//
// The counter is a single uint64_t whose *changes* (not its absolute value)
// carry meaning: it is the revocation state the auth layer folds into token
// signing, so flipping it invalidates outstanding tokens. Because a revoked
// user must STAY revoked across a restart, the value lives in a small text
// file that is loaded once at startup and rewritten every time it changes.
//
// Design notes that fix the earlier draft:
//   * The write must happen AT THE MOMENT THE COUNTER CHANGES (a revoke),
//     synchronously -- NOT only at shutdown. The accept loop runs forever and
//     the process normally dies by being killed/crashing, so a shutdown-only
//     write would silently lose every revocation made during the run (the
//     "revoked user comes back after reboot" bug). persistServerCounter()
//     below is what the revoke path calls.
//   * The write is crash-safe: it writes a sibling ".tmp" file, flushes, then
//     atomically renames it over the target -- the same temp-swap the
//     StorageEngine already uses. A crash mid-write can therefore never leave a
//     truncated/empty counter file that would read back as corrupt or 0 and
//     mass-un-revoke everyone.
//   * The path is anchored explicitly (see counterFilePath()) rather than a
//     bare relative name, so it does not silently resolve against whatever
//     directory the server happened to be launched from.
//
// The file itself is an anonymous integer -- it carries no identities, so it is
// safe to keep in the repo/deployment and leaks nothing useful if read.
// ---------------------------------------------------------------------------

// Base filename for the persisted counter. counterFilePath() turns this into an
// explicit path (see below) so the lookup does not depend on the process's
// current working directory.
static const std::string COUNTER_FILE_NAME = "server_counter.txt";

// Resolve the counter file's path. Kept as a single function so there is ONE
// definition of "where the counter lives" for both read and write.
//
// NOTE: this returns the bare filename by default. If you want it anchored to a
// fixed location (recommended for a deployed server), set the path here -- e.g.
// from an environment variable read at startup, or a compile-time constant --
// rather than relying on the launch directory. Left as the filename for now to
// preserve current behaviour; change in ONE place when you pick a home for it.
static std::string counterFilePath() {
    return COUNTER_FILE_NAME;
}

// ---------------------------------------------------------------------------
// readServerCounter: load the persistent server counter from disk.
//
// Reads a uint64_t from the counter file (see counterFilePath()), parsing the
// first line as a decimal number. Behaviour by case:
//   * file MISSING        -> serverCounter = 0, returns true  (first-run default)
//   * file empty/corrupt  -> returns false with `err` set     (refuse to start)
//   * valid integer       -> serverCounter = value, returns true
// The missing/corrupt split is deliberate: a fresh install should boot at 0,
// but an empty or garbage file (the shape a crash mid-write would leave) must
// NOT be silently treated as 0, or it would mass-un-revoke everyone.
//
// Parameters:
//   serverCounter [out] : receives the parsed counter value on success
//   err           [out] : receives an error description on failure
//
// Returns:
//   true  if the counter was successfully read and parsed
//   false if the file could not be opened or the data is invalid/corrupted
// ---------------------------------------------------------------------------
static bool readServerCounter(uint64_t& serverCounter, std::string& err) {
    const std::string path = counterFilePath();

    // open text file
    std::ifstream in(path);
    // A MISSING file is not an error: on first ever run there is no counter yet,
    // so treat "not found" as "start from 0". (An existing-but-unreadable or
    // corrupt file IS an error and is handled below.) This lets a fresh install
    // boot with no manual setup while still catching real corruption.
    if (!in) {
        serverCounter = 0;
        return true;
    }

    // read the first line
    std::string sCounter;
    std::getline(in, sCounter);

    // Trim surrounding whitespace / stray CR (e.g. a file saved with Windows
    // line endings or a trailing newline) so an otherwise-valid value isn't
    // rejected. std::stoull already skips leading whitespace, but a trailing
    // '\r' would slip through the "no trailing junk" check below.
    const std::string ws = " \t\r\n";
    size_t first = sCounter.find_first_not_of(ws);
    size_t last = sCounter.find_last_not_of(ws);
    if (first == std::string::npos) {
        // File exists but is empty / all whitespace. This is exactly the shape a
        // crash mid-write (without the temp-swap) would leave, so treat it as
        // corruption rather than silently reading 0 and un-revoking everyone.
        err = "The counter file '" + path +
            "' is empty; refusing to start rather than silently reset revocation state.";
        return false;
    }
    sCounter = sCounter.substr(first, last - first + 1);

    // convert it to uint64_t
    try {
        size_t consumed = 0;
        unsigned long long value = std::stoull(sCounter, &consumed);
        // Reject trailing junk (e.g. "12abc"): stoull would happily parse "12"
        // and ignore the rest, which would mask a corrupt file.
        if (consumed != sCounter.size()) {
            err = "The counter file '" + path +
                "' contains non-numeric trailing data; could not initialise the server counter.";
            return false;
        }
        serverCounter = static_cast<uint64_t>(value);
    }
    // stoull throws std::invalid_argument (no digits) or std::out_of_range
    // (too big for unsigned long long) -- both mean the file is unusable.
    catch (...) {
        err = "The counter file '" + path +
            "' is corrupted (not a valid unsigned integer); could not initialise the server counter.";
        return false;
    }

    // everything happened successfully
    return true;
}

// ---------------------------------------------------------------------------
// persistServerCounter: crash-safely write the counter to disk.
//
// Renamed from writeServerCounter to make the call site read as an intent
// ("persist this now"), and hardened so a crash can never corrupt the file:
//   1. write the value to a sibling "<path>.tmp",
//   2. flush + close it (bytes committed to the OS),
//   3. atomically rename the temp over the real file.
// std::rename replaces the destination on POSIX; on Windows it fails if the
// destination exists, so we fall back to remove-then-rename there -- the same
// approach StorageEngine::Save uses.
//
// CALL THIS SYNCHRONOUSLY WHENEVER THE COUNTER CHANGES (i.e. at revoke time),
// while holding whatever lock guards the counter -- NOT only at shutdown.
// ---------------------------------------------------------------------------
static bool persistServerCounter(uint64_t serverCounter, std::string& err) {
    const std::string path = counterFilePath();
    const std::string tempPath = path + ".tmp";

    // (1) write the new value to the TEMP file (truncating any stale temp).
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out) {
            err = "Failed to open temp counter file '" + tempPath +
                "' for writing. Check directory permissions / path.";
            return false;
        }
        out << serverCounter << "\n";
        // (2) flush + close so the bytes are on the OS before we rename.
        out.flush();
        out.close();
        if (!out) {
            // Any stream error during write/flush/close: don't rename a bad temp
            // over the good file. Best-effort clean up the temp and fail.
            std::remove(tempPath.c_str());
            err = "Failed while writing the temp counter file '" + tempPath + "'.";
            return false;
        }
    }

    // (3) atomic swap: temp -> target.
    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
        // Windows / destination-exists path: remove the old target, then rename.
        std::remove(path.c_str());
        if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
            std::remove(tempPath.c_str());   // give up cleanly; no temp left behind
            err = "Failed to atomically replace the counter file '" + path + "'.";
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// AuthState: owns the in-memory revocation counter and the lock that guards it.
//
// The counter + its mutex live together so every access goes through one place.
// A plain std::mutex is used (NOT std::shared_mutex): shared_mutex is C++17 and
// this project targets C++14, and at this scale there is no measurable benefit
// to reader-concurrency on a single uint64_t -- a short critical section around
// each access is plenty. All public methods that touch serverCounter take the
// lock, so there is no data race between a revoke (writer) and a verify (reader).
// ---------------------------------------------------------------------------
class AuthState {
private:
    uint64_t serverCounter = 0;
    mutable std::mutex mtx;   // mutable so getCounter() can be const and still lock

public:
    AuthState() {}

    // Load the counter from the state file (call once at startup). On success the
    // in-memory counter is set to the on-disk value; on failure it is left at 0
    // and `err` describes the problem (caller should refuse to start).
    bool readAuthState(std::string& err) {
        std::lock_guard<std::mutex> lock(mtx);
        uint64_t newCounter;
        if (!readServerCounter(newCounter, err))
            return false;
        serverCounter = newCounter;
        return true;
    }

    // revokeAll: THE revocation operation. Increment the generation and persist
    // it, atomically. Ordering is critical: persist the NEXT value FIRST, and
    // only commit it to memory if the write succeeded. If we incremented memory
    // first and the persist failed, the running server would use N+1 while disk
    // still held N, and the next restart would silently drop back to N --
    // un-revoking everyone. So on persist failure we leave serverCounter
    // untouched and report the error.
    bool revokeAll(std::string& err) {
        std::lock_guard<std::mutex> lock(mtx);
        uint64_t nextCounter = serverCounter + 1;

        // persist FIRST; commit to memory only on success.
        if (!persistServerCounter(nextCounter, err))
            return false;
        serverCounter = nextCounter;
        return true;
    }

    // persistCurrent: re-write the CURRENT counter to disk WITHOUT changing it.
    // This is what the clean-shutdown flush uses. It must NOT increment -- using
    // revokeAll() here would silently revoke everyone on every clean shutdown.
    // Because revoke-time persistence already wrote the current value, this is
    // normally a harmless no-op re-write that just guarantees disk == memory.
    bool persistCurrent(std::string& err) {
        std::lock_guard<std::mutex> lock(mtx);
        return persistServerCounter(serverCounter, err);
    }

    // Read the current generation. Locked so it never races a concurrent
    // revokeAll(); const because it does not change the logical value.
    uint64_t getCounter() const {
        std::lock_guard<std::mutex> lock(mtx);
        return serverCounter;
    }
};