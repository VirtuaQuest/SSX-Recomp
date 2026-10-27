// Shim: scalar versions of the byte swapping copies from Xenia's memory.cc.
#include "xenia/base/memory.h"

namespace xe
{
    template<typename T>
    static void CopySwap(void* dest, const void* src, size_t count)
    {
        auto* d = static_cast<T*>(dest);
        auto* s = static_cast<const T*>(src);
        for (size_t i = 0; i < count; i++)
        {
            T value;
            memcpy(&value, s + i, sizeof(T));
            value = byte_swap(value);
            memcpy(d + i, &value, sizeof(T));
        }
    }

    void copy_and_swap_16_aligned(void* dest, const void* src, size_t count) { CopySwap<uint16_t>(dest, src, count); }
    void copy_and_swap_16_unaligned(void* dest, const void* src, size_t count) { CopySwap<uint16_t>(dest, src, count); }
    void copy_and_swap_32_aligned(void* dest, const void* src, size_t count) { CopySwap<uint32_t>(dest, src, count); }
    void copy_and_swap_32_unaligned(void* dest, const void* src, size_t count) { CopySwap<uint32_t>(dest, src, count); }
    void copy_and_swap_64_aligned(void* dest, const void* src, size_t count) { CopySwap<uint64_t>(dest, src, count); }
    void copy_and_swap_64_unaligned(void* dest, const void* src, size_t count) { CopySwap<uint64_t>(dest, src, count); }

    void copy_and_swap_16_in_32_aligned(void* dest, const void* src, size_t count)
    {
        auto* d = static_cast<uint32_t*>(dest);
        auto* s = static_cast<const uint32_t*>(src);
        for (size_t i = 0; i < count; i++)
            d[i] = (s[i] >> 16) | (s[i] << 16);
    }

    void copy_and_swap_16_in_32_unaligned(void* dest, const void* src, size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            uint32_t value;
            memcpy(&value, static_cast<const uint32_t*>(src) + i, 4);
            value = (value >> 16) | (value << 16);
            memcpy(static_cast<uint32_t*>(dest) + i, &value, 4);
        }
    }
}
