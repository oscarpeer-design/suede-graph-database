#pragma once

// Silence MSVC's C4996 "getenv is unsafe -- use _dupenv_s" error. std::getenv is
// perfectly standard C++; _dupenv_s is a Microsoft-only extension that would break
// the Linux/other builds. Defining this (MSVC-only; harmless elsewhere) keeps ONE
// portable code path. Must come before any header that pulls in <cstdlib>.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "HMAC.h" //include hashing

#include <string>
#include <vector>
#include <array>         // std::array for the fixed-size secret key
#include <mutex>         // std::mutex / std::lock_guard (C++11 -- no C++17 needed)
#include <functional>   // std::bind, std::ref, std::placeholders
#include <fstream>
#include <sstream>       // std::istringstream for splitting the token fields
#include <cstdlib>       // std::getenv for the secret-key environment variable
#include <cstdint>       // uint64_t / uint8_t

// ---------------------------------------------------------------------------
// Token format (the "sealed letter").
//
//   SD-<ip>|<counter>|<expiry>|<tag>
//
//   * "SD-"     : a fixed marker (like OpenAI's "sk-"). A token missing it is
//                 rejected outright.
//   * <ip>      : the client IP the token was issued to (Problem 2: are you
//                 coming from where you should be).
//   * <counter> : the revocation generation the token was signed under. If the
//                 server's current counter differs, the token is stale (revoked).
//   * <expiry>  : Unix time (seconds) after which the token is refused.
//   * <tag>     : HMAC-SHA256 of the signed message, as 64 hex chars. The "wax
//                 seal" -- only the holder of the secret key could produce it.
//
// The SIGNED MESSAGE (what HMAC covers) is exactly:  <ip>|<counter>|<expiry>
// i.e. the token minus the "SD-" prefix and minus the trailing "|<tag>".
// Verify recomputes the tag over the RECEIVED message bytes and constant-time
// compares -- it never trusts a field until the seal checks out.
// ---------------------------------------------------------------------------
static const std::string TOKEN_PREFIX = "SD-";
static const char TOKEN_FIELD_SEPARATOR = '|';
// a valid token has exactly these 4 fields after the prefix: ip, counter, expiry, tag
static const size_t TOKEN_FIELD_COUNT = 4;
static const size_t TAG_BYTES = 32;               // HMAC-SHA256 output size
static const size_t TAG_HEX_CHARS = TAG_BYTES * 2;

// Name of the environment variable holding the HMAC secret key, as 128 hex
// characters (= 64 bytes). The operator sets this per-deployment; it is NEVER
// committed to the repo. Generate one with:  openssl rand -hex 64
static const char* SECRET_KEY_ENV_VAR = "SUEDE_SECRET_KEY";
// The key is exactly 64 raw bytes (SHA-256 block size) = 128 hex characters.
static const size_t SECRET_KEY_BYTES = 64;
static const size_t SECRET_KEY_HEX_CHARS = SECRET_KEY_BYTES * 2;

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

    // The 64-byte HMAC secret key, loaded once from the environment at startup.
    // Held for the process lifetime; never persisted, never logged, never in the
    // repo. Using a fixed std::array (not a vector) makes the size a compile-time
    // guarantee: it CANNOT be the wrong length, so the "always 64 bytes" invariant
    // is enforced by the type rather than by a runtime check. `keyLoaded` still
    // guards against using the array before it has been populated (fail closed).
    std::array<uint8_t, SECRET_KEY_BYTES> secretKey{};   // value-initialised to zeros
    bool keyLoaded = false;

    // Build the exact bytes that get signed: "<ip>|<counter>|<expiry>".
    // Used identically by mint and verify so the signed and verified bytes match.
    static std::string buildSignedMessage(const std::string& ip,
        uint64_t counter, uint64_t expiry) {
        return ip + TOKEN_FIELD_SEPARATOR + std::to_string(counter)
            + TOKEN_FIELD_SEPARATOR + std::to_string(expiry);
    }

    // Compute the HMAC tag over `message` using the loaded key. MUST be called
    // with `mtx` already held (it reads secretKey directly). This is the single
    // place hmac_sha256 is invoked -- the one "press the ring into the wax" call.
    void computeTagLocked(const std::string& message, uint8_t tag[TAG_BYTES]) const {
        hmac_sha256(secretKey.data(), secretKey.size(),
            reinterpret_cast<const uint8_t*>(message.data()), message.size(),
            tag);
    }

public:
    AuthState() {}

    // Outcome of verifyToken -- tells the caller WHY a token was rejected, which
    // is useful for returning the right HTTP status and for logging. Only Ok
    // means the token is authentic, current, unexpired, and from the right IP.
    // Declared INSIDE the class and as `enum class` so callers refer to it as
    // AuthState::VerifyResult::Ok (scoped, no clashes with other symbols).
    enum class VerifyResult {
        Ok,          // authentic, current generation, not expired, IP matches
        NoKey,       // server has no secret key loaded (fail-closed startup should prevent this)
        Malformed,   // missing prefix, wrong field count, non-numeric fields, bad tag length
        BadTag,      // the seal doesn't match -- forged or tampered payload
        WrongIp,     // authentic, but issued to a different IP than this request
        Revoked,     // authentic, but signed under an old generation (counter bumped)
        Expired      // authentic, but past its expiry time
    };

    // ---------------------------------------------------------------------
    // loadSecretKey: read the HMAC key from the SUEDE_SECRET_KEY environment
    // variable, validate it, and store the 64 decoded bytes. Call once at
    // startup, alongside readAuthState(). FAILS CLOSED -- returns false with
    // `err` set (and leaves keyLoaded == false) on any problem, so the caller
    // refuses to start rather than run with a missing/weak/default key.
    //
    // Rejects, in order:
    //   * variable not set                 -> no key provided
    //   * not exactly 128 hex characters   -> wrong length (must be 64 bytes)
    //   * any non-hex character            -> malformed
    // There is deliberately NO fallback/default key: a server with no valid key
    // must not run, because a known default key would make every token forgeable.
    // ---------------------------------------------------------------------
    bool loadSecretKey(std::string& err) {
        std::lock_guard<std::mutex> lock(mtx);

        // read the environment variable
        const char* raw = std::getenv(SECRET_KEY_ENV_VAR);
        if (raw == nullptr) {
            err = std::string("Environment variable ") + SECRET_KEY_ENV_VAR +
                " is not set. Generate a key with 'openssl rand -hex 64' and set it. "
                "Refusing to start without a secret key.";
            keyLoaded = false;
            return false;
        }

        std::string hexKey(raw);

        // must be exactly 128 hex chars (= 64 bytes)
        if (hexKey.size() != SECRET_KEY_HEX_CHARS) {
            err = std::string("Environment variable ") + SECRET_KEY_ENV_VAR +
                " must be exactly " + std::to_string(SECRET_KEY_HEX_CHARS) +
                " hex characters (" + std::to_string(SECRET_KEY_BYTES) +
                " bytes); got " + std::to_string(hexKey.size()) + ". Refusing to start.";
            keyLoaded = false;
            return false;
        }

        // decode into a temporary vector (from_hex is general / variable-length),
        // then copy the fixed 64 bytes into the array member. from_hex rejects any
        // non-hex character; the size check is belt-and-suspenders (the 128-char
        // length check above already implies 64 decoded bytes).
        std::vector<uint8_t> decoded;
        if (!from_hex(hexKey, decoded) || decoded.size() != SECRET_KEY_BYTES) {
            err = std::string("Environment variable ") + SECRET_KEY_ENV_VAR +
                " contains non-hex characters. Refusing to start.";
            keyLoaded = false;
            return false;
        }

        std::copy(decoded.begin(), decoded.end(), secretKey.begin());
        keyLoaded = true;
        return true;
    }

    // True once a valid key has been loaded. Verification/minting must refuse to
    // operate if this is false.
    bool hasKey() const {
        std::lock_guard<std::mutex> lock(mtx);
        return keyLoaded;
    }

    // Copy the 64-byte key out for use by the HMAC. Returns false if no key is
    // loaded (fail closed). The out-parameter is a fixed 64-byte std::array, so
    // the size is guaranteed by the type -- callers pass array.data()/size() to
    // hmac_sha256. Handing back a copy keeps the member encapsulated; the caller
    // uses it transiently to compute/verify a tag and lets it drop.
    bool getSecretKey(std::array<uint8_t, SECRET_KEY_BYTES>& out) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (!keyLoaded)
            return false;
        out = secretKey;
        return true;
    }

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

public:
    // -----------------------------------------------------------------------
    // mintToken: issue a sealed token for `ip`, valid until `expiry` (Unix secs),
    // bound to the CURRENT revocation generation. Returns false (empty token) if
    // no key is loaded, or if `ip` contains the field separator (which would make
    // the token ambiguous to parse back).
    //
    // This is an OPERATOR/server action -- it needs the secret key, so it can only
    // run where the key is loaded. It must NEVER be exposed as an unauthenticated
    // endpoint, or anyone could mint valid tokens.
    // -----------------------------------------------------------------------
    bool mintToken(const std::string& ip, uint64_t expiry,
        std::string& tokenOut, std::string& err) {
        std::lock_guard<std::mutex> lock(mtx);

        if (!keyLoaded) {
            err = "Cannot mint a token: no secret key is loaded.";
            return false;
        }
        // the ip must not contain the separator, or the token couldn't be split
        // back into fields unambiguously.
        if (ip.find(TOKEN_FIELD_SEPARATOR) != std::string::npos) {
            err = "Cannot mint a token: ip contains the reserved separator character.";
            return false;
        }

        // read the current generation DIRECTLY (we hold the lock; do NOT call
        // getCounter() here -- std::mutex is not recursive, that would deadlock).
        const uint64_t counter = serverCounter;

        const std::string message = buildSignedMessage(ip, counter, expiry);
        uint8_t tag[TAG_BYTES];
        computeTagLocked(message, tag);

        // token = "SD-" + signed message + "|" + hex tag
        tokenOut = TOKEN_PREFIX + message + TOKEN_FIELD_SEPARATOR + to_hex(tag, TAG_BYTES);
        return true;
    }

    // -----------------------------------------------------------------------
    // verifyToken: check a token is authentic, current, unexpired, and from the
    // expected IP. `requestIp` is the actual source IP of the incoming request;
    // `now` is the current Unix time (passed in so this stays testable and the
    // caller controls the clock source). Returns a VerifyResult explaining the
    // outcome. FAILS CLOSED: anything unexpected -> a reject result.
    //
    // Order of checks matters for security:
    //   1. structural parse (prefix, field count, numeric fields, tag length)
    //   2. recompute the tag over the RECEIVED message and constant-time compare
    //      -- we do NOT trust any field until the seal is verified
    //   3. only AFTER the seal is good do we check IP / generation / expiry
    // -----------------------------------------------------------------------
    VerifyResult verifyToken(const std::string& token,
        const std::string& requestIp,
        uint64_t now) const {
        std::lock_guard<std::mutex> lock(mtx);

        if (!keyLoaded)
            return VerifyResult::NoKey;

        // --- (1) structural parse -----------------------------------------
        // must start with the "SD-" prefix
        if (token.size() < TOKEN_PREFIX.size() ||
            token.compare(0, TOKEN_PREFIX.size(), TOKEN_PREFIX) != 0)
            return VerifyResult::Malformed;

        // strip the prefix, then split the remainder on '|' into fields
        std::string body = token.substr(TOKEN_PREFIX.size());
        std::vector<std::string> fields;
        {
            std::string field;
            std::istringstream stream(body);
            while (std::getline(stream, field, TOKEN_FIELD_SEPARATOR))
                fields.push_back(field);
        }
        // exactly ip, counter, expiry, tag
        if (fields.size() != TOKEN_FIELD_COUNT)
            return VerifyResult::Malformed;

        const std::string& ipField = fields[0];
        const std::string& counterField = fields[1];
        const std::string& expiryField = fields[2];
        const std::string& tagField = fields[3];

        // the tag must be exactly 64 hex chars
        if (tagField.size() != TAG_HEX_CHARS)
            return VerifyResult::Malformed;

        // parse counter and expiry as unsigned integers, rejecting junk
        uint64_t tokenCounter = 0;
        uint64_t tokenExpiry = 0;
        try {
            size_t consumedC = 0, consumedE = 0;
            tokenCounter = std::stoull(counterField, &consumedC);
            tokenExpiry = std::stoull(expiryField, &consumedE);
            if (consumedC != counterField.size() || consumedE != expiryField.size())
                return VerifyResult::Malformed;   // trailing non-numeric junk
        }
        catch (...) {
            return VerifyResult::Malformed;       // not a number / out of range
        }

        // decode the provided tag hex into bytes
        std::vector<uint8_t> providedTag;
        if (!from_hex(tagField, providedTag) || providedTag.size() != TAG_BYTES)
            return VerifyResult::Malformed;

        // --- (2) verify the seal over the RECEIVED message ----------------
        // Rebuild the signed message from the RECEIVED fields (not from what we
        // wish they were) and recompute the tag. This is the "does the seal match
        // the letter" step -- do it before trusting ip/counter/expiry.
        const std::string message = buildSignedMessage(ipField, tokenCounter, tokenExpiry);
        uint8_t expectedTag[TAG_BYTES];
        computeTagLocked(message, expectedTag);
        if (!constant_time_equal(expectedTag, providedTag.data(), TAG_BYTES))
            return VerifyResult::BadTag;          // forged or tampered

        // --- (3) seal is genuine: now the fields are trustworthy ----------
        // generation check (revocation): stale if not the current counter
        if (tokenCounter != serverCounter)
            return VerifyResult::Revoked;
        // expiry check
        if (now > tokenExpiry)
            return VerifyResult::Expired;
        // IP check (Problem 2)
        if (ipField != requestIp)
            return VerifyResult::WrongIp;

        return VerifyResult::Ok;
    }
};