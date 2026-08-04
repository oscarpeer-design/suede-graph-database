#pragma once
/*HMAC encryption algorithm built using custom SHA - 256 algorithm
sources: https://datatracker.ietf.org/doc/html/rfc2104
         nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
*/

#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstddef>

// initial hash values
const uint32_t INITIAL_HASH_VALUES[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

// all 64 SHA-256 round constants
const uint32_t round[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// byte constants
// the mandatory padding byte: a 1 bit followed by seven 0 bits (10000000b)
const uint8_t PAD_START_BYTE = 0x80;
// SHA-256 processes the message in blocks of this many BYTES (the "block size").
// NOTE: this is 64 *bytes*, not "bits in a byte" -- renamed for clarity.
const size_t BLOCK_SIZE_BYTES = 64;
// after appending PAD_START_BYTE we zero-pad until the block has this many bytes
// used, leaving exactly 8 bytes for the 64-bit length field (56 + 8 == 64).
const size_t LENGTH_FIELD_OFFSET = 56;

// rotates a given set of bits right by n bits
// take the lost bits (if there are any) and put them back on the left
static uint32_t rotate_right(uint32_t bits, uint32_t n) {
    uint32_t movement_right = bits >> n;
    uint32_t ending = 32 - n;
    uint32_t rotation_left_bits = bits << ending;
    return (movement_right | rotation_left_bits);
}

// reads 4 bytes, big-endian
// 'big-endian' means the most significant byte comes first  and is the lowest address
static uint32_t read_big_endian(const uint8_t* pointer_byte_array) {
    // In this case the most significant bit, p[0] is read first
    return ((uint32_t)pointer_byte_array[0] << 24)
        | ((uint32_t)pointer_byte_array[1] << 16)
        | ((uint32_t)pointer_byte_array[2] << 8)
        | ((uint32_t)pointer_byte_array[3]);
}

// writes 4 bytes, big-endian, into a pointer_byte_array
static void write_be32(uint32_t bits, uint8_t* pointer_byte_array) {
    pointer_byte_array[0] = (uint8_t)(bits >> 24);
    pointer_byte_array[1] = (uint8_t)(bits >> 16);
    pointer_byte_array[2] = (uint8_t)(bits >> 8);
    pointer_byte_array[3] = (uint8_t)(bits);
}

// ---------------------------------------------------------------------------
// SHA-256 mixing functions (FIPS 180-4 sec 4.1.2).
//
// There are FOUR of them and they are easy to mix up:
//   * UPPERCASE-sigma (big_sigma0/1) are used inside the 64-round compression.
//   * lowercase-sigma (small_sigma0/1) are used to build the message schedule.
// They differ in their rotate amounts, and small_sigma uses a plain SHIFT
// (>>) on the last term where big_sigma does not. Getting these swapped is
// the #1 SHA-256 bug, so they are named explicitly here.
// ---------------------------------------------------------------------------
static uint32_t big_sigma0(uint32_t word) {
    return rotate_right(word, 2) ^ rotate_right(word, 13) ^ rotate_right(word, 22);
}
static uint32_t big_sigma1(uint32_t word) {
    return rotate_right(word, 6) ^ rotate_right(word, 11) ^ rotate_right(word, 25);
}
static uint32_t small_sigma0(uint32_t word) {
    // two rotates and one PLAIN right shift
    return rotate_right(word, 7) ^ rotate_right(word, 18) ^ (word >> 3);
}
static uint32_t small_sigma1(uint32_t word) {
    return rotate_right(word, 17) ^ rotate_right(word, 19) ^ (word >> 10);
}

// choose: for each bit, the `selector` bit decides which source to take --
// where selector is 1 take on_one's bit, where it is 0 take on_zero's bit.
static uint32_t choose(uint32_t selector, uint32_t on_one, uint32_t on_zero) {
    return (selector & on_one) ^ (~selector & on_zero);
}
// majority: for each bit position, the value that at least two of the three inputs agree on.
static uint32_t majority(uint32_t first, uint32_t second, uint32_t third) {
    return (first & second) ^ (first & third) ^ (second & third);
}

// pads data into a multiple of 64 bytes, IN PLACE.
// note that before calling this function, the original message bytes must be put in.
//
// FIXED from the earlier draft, which (1) took the vector by value so the padding
// was thrown away, and (2) looped the length field instead of doing the three
// distinct padding moves. The correct recipe is:
//   move 1: append a single 0x80 byte,
//   move 2: append 0x00 bytes until size % 64 == 56,
//   move 3: append the ORIGINAL bit-length as a 64-bit big-endian value.
// The bit-length MUST be captured from the original size, before any appending.
static void pad_64_bytes(std::vector<uint8_t>& data) {
    // capture the original length in BITS *before* we append anything.
    uint64_t bitLen = (uint64_t)data.size() * 8;

    // move 1: the mandatory 1-bit (as a whole byte 0x80).
    data.push_back(PAD_START_BYTE);

    // move 2: zero-pad until we are 56 bytes into the final block, leaving room
    // for the 8-byte length. The while-loop transparently handles the case where
    // padding must spill into a whole extra block.
    while (data.size() % BLOCK_SIZE_BYTES != LENGTH_FIELD_OFFSET)
        data.push_back(0x00);

    // move 3: append the 64-bit big-endian bit length (most-significant byte first).
    for (int i = 7; i >= 0; --i)
        data.push_back((uint8_t)(bitLen >> (i * 8)));
}

// produces 32 hashed bytes off of a set of unencrypted data
static void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    // ---- build the padded message ---------------------------------------
    // move 1 of padding (copy the raw message in) happens here in the caller;
    // pad_64_bytes appends the rest so `message` becomes a multiple of 64 bytes.
    std::vector<uint8_t> message(data, data + len);
    pad_64_bytes(message);

    // ---- Step A: working state, seeded from the initial hash values ------
    // `state` is the running 8-word hash. It carries across every block and,
    // after the last block, IS the final digest. The spec calls these H0..H7.
    uint32_t state[8];
    std::copy(std::begin(INITIAL_HASH_VALUES), std::end(INITIAL_HASH_VALUES), state);

    // ---- Step B: process every 64-byte block ----------------------------
    for (size_t offset = 0; offset < message.size(); offset += BLOCK_SIZE_BYTES) {
        const uint8_t* block = &message[offset];

        // ---- Step C1: build the 64-word message schedule --------------------
        // `schedule` expands the 16 words of this block into 64 mixed words,
        // one consumed per round below. (The spec calls this W.)
        uint32_t schedule[64];
        // first 16 words are simply the block read as big-endian 32-bit words
        for (int index = 0; index < 16; ++index)
            schedule[index] = read_big_endian(block + index * 4);
        // remaining 48 words are each derived by mixing four earlier words
        for (int index = 16; index < 64; ++index)
            schedule[index] = small_sigma1(schedule[index - 2]) + schedule[index - 7]
            + small_sigma0(schedule[index - 15]) + schedule[index - 16];

        // ---- Step C2: 64-round compression ---------------------------------
        // Eight working variables, initialised from the running state. The spec
        // names them a..h; here they carry their ROLE. Each round they shift
        // "down" the chain (v0 -> v1 -> ... -> v7) with two freshly-computed
        // words fed in at the top -- an 8-stage shift register being stirred.
        uint32_t v0 = state[0];   // spec: a  (receives the new mixed word each round)
        uint32_t v1 = state[1];   // spec: b
        uint32_t v2 = state[2];   // spec: c
        uint32_t v3 = state[3];   // spec: d
        uint32_t v4 = state[4];   // spec: e  (the other "hot" word, drives `choose`)
        uint32_t v5 = state[5];   // spec: f
        uint32_t v6 = state[6];   // spec: g
        uint32_t v7 = state[7];   // spec: h  (falls off the bottom each round)

        for (int r = 0; r < 64; ++r) {
            // the word entering at the top: bottom var + mixing of v4 + this round's constant/schedule
            uint32_t mixed_in = v7 + big_sigma1(v4) + choose(v4, v5, v6) + round[r] + schedule[r];
            // the extra stirring folded into the very top variable
            uint32_t top_stir = big_sigma0(v0) + majority(v0, v1, v2);

            // shift every variable down one position...
            v7 = v6;
            v6 = v5;
            v5 = v4;
            v4 = v3 + mixed_in;          // v4 also absorbs the incoming word
            v3 = v2;
            v2 = v1;
            v1 = v0;
            v0 = mixed_in + top_stir;    // v0 gets the fully-stirred new word
        }

        // ---- Step C3: fold the working variables back into the state --------
        state[0] += v0;
        state[1] += v1;
        state[2] += v2;
        state[3] += v3;
        state[4] += v4;
        state[5] += v5;
        state[6] += v6;
        state[7] += v7;
    }

    // ---- Step D: write the 8 state words out as 32 big-endian bytes -----
    for (int index = 0; index < 8; ++index)
        write_be32(state[index], out + index * 4);
}

// ---------------------------------------------------------------------------
// hmac_sha256: the real HMAC core (RFC 2104), keyed with a raw BYTE STRING.
//
//   HMAC(K, m) = H( (K' XOR opad) || H( (K' XOR ipad) || m ) )
//
// where || is concatenation, H is our sha256, and:
//   * the block size B is 64 bytes (SHA-256's block, NOT its 32-byte output),
//   * K' is the key normalised to exactly B bytes:
//       - if the key is longer than B, hash it first (H(K) -> 32 bytes),
//       - then zero-pad (or, if already <= B, just zero-pad) out to B bytes,
//   * ipad is the byte 0x36 repeated B times, opad is 0x5c repeated B times.
//
// The key is taken as (key, keyLen) BYTES -- never an integer -- because an
// HMAC key is a byte string. (A uint64_t would hold only 8 of e.g. a 64-byte
// secret and silently truncate it.) Output is 32 raw bytes in `out`.
// ---------------------------------------------------------------------------
static void hmac_sha256(const uint8_t* key, size_t keyLen,
    const uint8_t* message, size_t messageLen,
    uint8_t out[32]) {
    const uint8_t IPAD = 0x36;
    const uint8_t OPAD = 0x5c;

    // --- Piece 1: normalise the key to exactly BLOCK_SIZE_BYTES (K') --------
    // Start all-zero so the tail is zero-padded automatically; then copy the
    // key (or its hash, if the key is over-long) into the front.
    std::vector<uint8_t> keyPrime(BLOCK_SIZE_BYTES, 0x00);
    if (keyLen > BLOCK_SIZE_BYTES) {
        // over-long key: replace it with its 32-byte hash, zero-padded to 64.
        uint8_t hashedKey[32];
        sha256(key, keyLen, hashedKey);
        std::copy(hashedKey, hashedKey + 32, keyPrime.begin());
    }
    else {
        std::copy(key, key + keyLen, keyPrime.begin());
    }

    // --- Piece 2: build the ipad / opad blocks (XOR the WHOLE 64-byte K') ---
    std::vector<uint8_t> innerPad(BLOCK_SIZE_BYTES);
    std::vector<uint8_t> outerPad(BLOCK_SIZE_BYTES);
    for (size_t i = 0; i < BLOCK_SIZE_BYTES; ++i) {
        innerPad[i] = keyPrime[i] ^ IPAD;
        outerPad[i] = keyPrime[i] ^ OPAD;
    }

    // --- Piece 3: inner hash = H( innerPad || message ) --------------------
    std::vector<uint8_t> innerInput;
    innerInput.reserve(BLOCK_SIZE_BYTES + messageLen);
    innerInput.insert(innerInput.end(), innerPad.begin(), innerPad.end());
    innerInput.insert(innerInput.end(), message, message + messageLen);
    uint8_t innerResult[32];
    sha256(innerInput.data(), innerInput.size(), innerResult);

    // --- Piece 4: outer hash = H( outerPad || innerResult ) -> final HMAC --
    std::vector<uint8_t> outerInput;
    outerInput.reserve(BLOCK_SIZE_BYTES + 32);
    outerInput.insert(outerInput.end(), outerPad.begin(), outerPad.end());
    outerInput.insert(outerInput.end(), innerResult, innerResult + 32);
    sha256(outerInput.data(), outerInput.size(), out);
}

// ---------------------------------------------------------------------------
// to_hex: render 32 raw bytes as a 64-char lowercase hex string. Handy for
// putting an HMAC into a token or comparing against RFC test vectors.
// ---------------------------------------------------------------------------
static std::string to_hex(const uint8_t* bytes, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s += digits[bytes[i] >> 4];
        s += digits[bytes[i] & 0x0F];
    }
    return s;
}

// ---------------------------------------------------------------------------
// constant_time_equal: compare two 32-byte tags WITHOUT early-exit.
//
// A normal memcmp/== returns as soon as it finds a differing byte, and the tiny
// timing difference leaks how many leading bytes matched -- letting an attacker
// recover a valid tag byte-by-byte. This ORs every byte difference together and
// only checks the result at the very end, so it always inspects all 32 bytes
// and runs in the same time regardless of where (or whether) they differ.
// USE THIS to compare a received token's tag against the recomputed one.
// ---------------------------------------------------------------------------
static bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Convenience wrapper: HMAC over std::string inputs, returning the tag as a
// 64-char hex string. `key` and `message` are both treated as raw bytes. This
// just forwards to hmac_sha256 -- keep the byte-pointer core for anything that
// isn't already a std::string (e.g. a raw 64-byte secret array).
// ---------------------------------------------------------------------------
static std::string HMAC(const std::string& message, const std::string& key) {
    uint8_t tag[32];
    hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
        reinterpret_cast<const uint8_t*>(message.data()), message.size(),
        tag);
    return to_hex(tag, 32);
}