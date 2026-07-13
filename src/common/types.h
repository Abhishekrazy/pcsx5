#pragma once

#include <cstdint>
#include <cstddef>

// Fixed-width unsigned integers
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Fixed-width signed integers
using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// Guest-to-Host pointer helper structures
using guest_addr_t = u64;

// Utilities
#define ALIGN_UP(val, align)   (((val) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(val, align) ((val) & ~((align) - 1))
#define PAGE_SIZE              0x4000 // PS5 page size is typically 16KB (0x4000) or 4KB (0x1000). Let's support 16KB.
