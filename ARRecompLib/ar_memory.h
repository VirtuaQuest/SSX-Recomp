#pragma once

#include <cstdint>

// Host location of guest memory. On the Xbox 360, the physical memory view at 0xE0000000 maps physical memory
// 4 KB higher than the views at 0xA0000000 and 0xC0000000: virtual 0xE0000000 + X is physical X + 0x1000. Host
// views must be 64 KB aligned, so instead every access at or above 0xE0000000 is moved up by 4 KB, which lines
// it up with the other views of the same physical memory. Force included into the recompiled code and runtime.
inline uint64_t PPCHostOffset(uint32_t address)
{
    return uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

#define PPC_MEMORY_ADDRESS(x) (base + PPCHostOffset(uint32_t(x)))
