#pragma once
//
// Crc32.h -- a small, dependency-free CRC32 checksum used to detect corruption
// in the binary graph file. CRC32 answers ONE question cheaply: "are these bytes
// identical to what was written?" A single linear pass over the payload produces
// a 32-bit fingerprint; on load we recompute it and compare to the stored value.
// A mismatch means the file was truncated, half-written, or rotted on disk.
//
// What CRC does NOT catch: bytes that were written *wrong but consistently* (a
// serialiser bug) -- the CRC over bad bytes still matches on reload. That class
// is caught by the structural checks (length/index bounds) and ValidateGraph.
// CRC is the cheap front-line for on-disk corruption, not a correctness proof.
//
// This is the standard IEEE 802.3 / zlib CRC32 (polynomial 0xEDB88320, reflected).
// Standard test vector: crc32 of the ASCII string "123456789" == 0xCBF43926.

#include <cstdint>
#include <cstddef>

// crc32Update: fold `len` bytes into a running CRC. Start a fresh checksum with
// crc == 0xFFFFFFFF, feed every payload chunk through here, then finalise by
// XOR-ing with 0xFFFFFFFF (crc32Finalize). Incremental so we can checksum while
// streaming writes without buffering the whole payload.
inline uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
    // Initialize a static lookup table for CRC calculation (Sarwate's table-driven method).
    // This table is computed once on first use and reused for all subsequent calls.
    // Each entry represents the CRC polynomial applied to all bit patterns (0-255).
    static uint32_t table[256];
    
    // Flag to track whether the lookup table has been built. Starts as false, set to true
    // after the table is initialized on the first call to this function.
    static bool tableBuilt = false;
    
    // Build the CRC lookup table on first invocation only. This precomputation eliminates
    // the need for bit-by-bit CRC operations during data processing, trading memory for speed.
    if (!tableBuilt) {
        // Iterate through all 256 possible byte values (0x00 to 0xFF).
        for (uint32_t i = 0; i < 256; ++i) {
            // Initialize the CRC value for this byte pattern as the byte itself.
            uint32_t c = i;
            
            // Process each of the 8 bits in the current byte pattern.
            for (int bit = 0; bit < 8; ++bit) {
                // If the least significant bit (LSB) of c is 1, XOR c right-shifted by 1 
                // with the CRC polynomial (0xEDB88320). Otherwise, just right-shift c by 1.
                // This implements the Galois field division used in CRC computation.
                uint32_t lsb = c & 1;
                c = (c >> 1) ^ (0xEDB88320u & -static_cast<int32_t>(lsb));
            }
            
            // Store the computed CRC remainder for this byte value in the lookup table.
            table[i] = c;
        }
        
        // Mark the table as built so it won't be recomputed on subsequent calls.
        tableBuilt = true;
    }

    // Cast the input data pointer to unsigned char* for byte-by-byte access.
    // This ensures we process the data as a sequence of individual bytes.
    const unsigned char* p = static_cast<const unsigned char*>(data);
    
    // Iterate through each byte in the input data buffer.
    for (size_t i = 0; i < len; ++i) {
        // Extract the low byte of the CRC and XOR it with the current data byte to form
        // an index (0-255) into the lookup table. This represents the feedback position
        // in the CRC polynomial shift register.
        // Use the table lookup to replace the lowest 8 bits of the CRC register and
        // shift the remaining CRC bits right by 8 positions. This implements one step
        // of the byte-wide CRC computation.
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    
    // Return the updated CRC value. This can be fed into another crc32Update call for
    // incremental computation, or passed to crc32Finalize for the final result.
    return crc;
}

// Convenience: compute a complete CRC32 over one buffer in a single call.
// (init 0xFFFFFFFF, update, finalise) -- used by the load path over the whole
// payload block.
inline uint32_t crc32(const void* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32Update(crc, data, len);
    return crc ^ 0xFFFFFFFFu;
}