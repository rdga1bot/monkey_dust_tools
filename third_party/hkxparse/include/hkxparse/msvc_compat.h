#pragma once
// hkxparse is Windows/MSVC-oriented: relies on transitive includes MSVC's
// STL happens to provide (<vector>, <cstddef>) that libstdc++ does not, and
// uses _byteswap_* intrinsics (only exercised on the big-endian branch,
// which our little-endian PC .hkt files never take at runtime, but the code
// still needs to compile under GCC).
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#if !defined(_MSC_VER)
inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t _byteswap_ulong(uint32_t v)  { return __builtin_bswap32(v); }
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }
#endif
