// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// uDepot's record checksum, computed independently of the store, for tests
// that check what is on the device.

#pragma once

#include <cstddef>
#include <cstdint>

// zlib's crc32(crc, buf, len), bit by bit, independent of the store's table.
inline uint32_t crc32_bitwise(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// As uDepot's checksum16(timestamp, md): a CRC32 seeded with the timestamp
// of the record's segment, over the 6-byte record header, then over the
// device seed, truncated to 16 bits.
inline uint16_t record_checksum(const uint8_t* hdr, uint64_t seg_ts,
                                uint64_t seed) {
    uint32_t crc = crc32_bitwise(static_cast<uint32_t>(seg_ts), hdr, 6);
    crc = crc32_bitwise(crc, reinterpret_cast<const uint8_t*>(&seed),
                        sizeof(seed));
    return static_cast<uint16_t>(crc);
}
