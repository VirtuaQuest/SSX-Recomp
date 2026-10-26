#pragma once

#include "function.h"
#include "heap.h"
#include "memory.h"
#include "objects.h"

// Kernel and XAM imports are implemented as host functions in the "imports" namespace (to avoid clashing with
// Windows API names) and bound to the import thunks the recompiled code calls.
#define AR_IMPORT(name) \
    PPC_FUNC(__imp__##name) \
    { \
        if (logger::g_minimumLevel == logger::Level::Trace) \
            logger::Print(logger::Level::Trace, #name, "r3 0x{:08X} r4 0x{:08X} r5 0x{:08X} lr 0x{:08X}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr)); \
        HostToGuestFunction<imports::name>(ctx, base); \
    }

// Guest pointer helpers.
template<typename T>
inline T* GuestPtr(uint32_t address)
{
    return g_memory.Translate<T>(address);
}

inline uint32_t GuestAddress(const void* host)
{
    return host != nullptr ? g_memory.MapVirtual(host) : 0;
}

// Reads a guest ANSI_STRING.
inline std::string ReadAnsiString(const XANSI_STRING* string)
{
    if (string == nullptr || string->Buffer.get() == nullptr)
        return {};

    return std::string(string->Buffer.get(), string->Length.get());
}

// Win32 error codes used by XAM.
constexpr uint32_t X_ERROR_SUCCESS = 0;
constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t X_ERROR_ACCESS_DENIED = 5;
constexpr uint32_t X_ERROR_INVALID_HANDLE = 6;
constexpr uint32_t X_ERROR_NO_MORE_FILES = 18;
constexpr uint32_t X_ERROR_NOT_SUPPORTED = 50;
constexpr uint32_t X_ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t X_ERROR_ALREADY_EXISTS = 183;
constexpr uint32_t X_ERROR_IO_PENDING = 997;
constexpr uint32_t X_ERROR_FUNCTION_FAILED = 1627;
constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t X_ERROR_NOT_FOUND = 1168;
constexpr uint32_t X_ERROR_CANCELLED = 1223;
constexpr uint32_t X_ERROR_NOT_LOGGED_ON = 1245;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 1317;
constexpr uint32_t X_ERROR_EMPTY = 4306;

// Completes an XAM overlapped request: stores the result and signals its event.
void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0, uint32_t extendedError = 0);
