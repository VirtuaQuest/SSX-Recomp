// Runtime library: strings, time conversion, formatting and exceptions.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/guest_format.h>
#include <kernel/xex_loader.h>
#include <cpu/guest_thread.h>

struct XUnicodeString
{
    be<uint16_t> length;        // Bytes, without terminator.
    be<uint16_t> maximumLength; // Bytes.
    be<uint32_t> buffer;
};

namespace imports
{
    void RtlInitAnsiString(XANSI_STRING* destination, uint32_t source)
    {
        uint16_t length = source != 0 ? uint16_t(strlen(GuestPtr<char>(source))) : 0;
        destination->Length = length;
        destination->MaximumLength = source != 0 ? length + 1 : 0;
        destination->Buffer.ptr = source;
    }

    void RtlInitUnicodeString(XUnicodeString* destination, uint32_t source)
    {
        uint16_t length = 0;
        if (source != 0)
        {
            for (auto* c = GuestPtr<be<uint16_t>>(source); c->get() != 0; c++)
                length += 2;
        }

        destination->length = length;
        destination->maximumLength = source != 0 ? length + 2 : 0;
        destination->buffer = source;
    }

    void RtlFreeAnsiString(XANSI_STRING* string)
    {
        if (string->Buffer.ptr != 0)
            g_runtimeHeap.FreeGuest(string->Buffer.ptr);

        string->Buffer.ptr = 0;
        string->Length = 0;
        string->MaximumLength = 0;
    }

    uint32_t RtlUnicodeStringToAnsiString(XANSI_STRING* destination, XUnicodeString* source, uint32_t allocate)
    {
        uint32_t characters = source->length / 2;
        if (allocate)
        {
            destination->Buffer.ptr = g_runtimeHeap.AllocGuest(characters + 1);
            destination->MaximumLength = uint16_t(characters + 1);
        }
        else if (destination->MaximumLength.get() <= characters)
        {
            return X_STATUS_BUFFER_TOO_SMALL;
        }

        auto* input = GuestPtr<be<uint16_t>>(source->buffer);
        char* output = destination->Buffer.get();
        for (uint32_t i = 0; i < characters; i++)
            output[i] = input[i] < 0x80 ? char(input[i]) : '?';

        output[characters] = '\0';
        destination->Length = uint16_t(characters);
        return X_STATUS_SUCCESS;
    }

    uint32_t RtlMultiByteToUnicodeN(be<uint16_t>* unicode, uint32_t maxBytes, be<uint32_t>* bytesWritten, const char* multiByte, uint32_t multiByteBytes)
    {
        uint32_t length = std::min(maxBytes / 2, multiByteBytes);
        for (uint32_t i = 0; i < length; i++)
            unicode[i] = uint16_t(uint8_t(multiByte[i]));

        if (bytesWritten != nullptr)
            *bytesWritten = length * 2;

        return X_STATUS_SUCCESS;
    }

    uint32_t RtlUnicodeToMultiByteN(char* multiByte, uint32_t maxBytes, be<uint32_t>* bytesWritten, be<uint16_t>* unicode, uint32_t unicodeBytes)
    {
        uint32_t length = std::min(maxBytes, unicodeBytes / 2);
        for (uint32_t i = 0; i < length; i++)
            multiByte[i] = unicode[i] < 0x100 ? char(unicode[i].get()) : '?';

        if (bytesWritten != nullptr)
            *bytesWritten = length;

        return X_STATUS_SUCCESS;
    }

    uint32_t RtlUpcaseUnicodeChar(uint32_t character)
    {
        return uint32_t(towupper(wchar_t(character & 0xFFFF)));
    }

    int32_t RtlCompareStringN(const char* string1, uint32_t length1, const char* string2, uint32_t length2, uint32_t caseInsensitive)
    {
        uint32_t length = std::min(length1, length2);
        for (uint32_t i = 0; i < length; i++)
        {
            int c1 = uint8_t(string1[i]);
            int c2 = uint8_t(string2[i]);
            if (caseInsensitive)
            {
                c1 = toupper(c1);
                c2 = toupper(c2);
            }

            if (c1 != c2)
                return c1 - c2;
        }

        return int32_t(length1) - int32_t(length2);
    }

    uint32_t RtlCompareMemoryUlong(be<uint32_t>* source, uint32_t length, uint32_t pattern)
    {
        uint32_t matched = 0;
        for (uint32_t i = 0; i < length / 4 && source[i] == pattern; i++)
            matched += 4;

        return matched;
    }

    void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
    {
        for (uint32_t i = 0; i < length / 4; i++)
            destination[i] = pattern;
    }

    void RtlTimeToTimeFields(be<int64_t>* time, XTIME_FIELDS* fields)
    {
        // 100ns intervals since 1601 to a calendar date, via Unix time.
        int64_t value = time->get();
        int64_t unixSeconds = (value - 116444736000000000ll) / 10000000;
        int64_t milliseconds = (value / 10000) % 1000;
        std::time_t seconds = std::time_t(unixSeconds);

        std::tm tm{};
        gmtime_s(&tm, &seconds);
        fields->Year = uint16_t(tm.tm_year + 1900);
        fields->Month = uint16_t(tm.tm_mon + 1);
        fields->Day = uint16_t(tm.tm_mday);
        fields->Hour = uint16_t(tm.tm_hour);
        fields->Minute = uint16_t(tm.tm_min);
        fields->Second = uint16_t(tm.tm_sec);
        fields->Milliseconds = uint16_t(milliseconds);
        fields->Weekday = uint16_t(tm.tm_wday);
    }

    uint32_t RtlTimeFieldsToTime(XTIME_FIELDS* fields, be<int64_t>* time)
    {
        std::tm tm{};
        tm.tm_year = fields->Year - 1900;
        tm.tm_mon = fields->Month - 1;
        tm.tm_mday = fields->Day;
        tm.tm_hour = fields->Hour;
        tm.tm_min = fields->Minute;
        tm.tm_sec = fields->Second;

        std::time_t seconds = _mkgmtime(&tm);
        if (seconds == -1)
            return false;

        *time = int64_t(seconds) * 10000000 + 116444736000000000ll + int64_t(fields->Milliseconds) * 10000;
        return true;
    }

    uint32_t RtlNtStatusToDosError(uint32_t status)
    {
        switch (status)
        {
        case X_STATUS_SUCCESS: return X_ERROR_SUCCESS;
        case X_STATUS_PENDING: return X_ERROR_IO_PENDING;
        case X_STATUS_NO_MORE_FILES: return X_ERROR_NO_MORE_FILES;
        case X_STATUS_INVALID_HANDLE: return X_ERROR_INVALID_HANDLE;
        case X_STATUS_INVALID_PARAMETER: return X_ERROR_INVALID_PARAMETER;
        case X_STATUS_NO_SUCH_FILE:
        case X_STATUS_OBJECT_NAME_NOT_FOUND: return X_ERROR_FILE_NOT_FOUND;
        case X_STATUS_OBJECT_PATH_NOT_FOUND: return X_ERROR_PATH_NOT_FOUND;
        case X_STATUS_OBJECT_NAME_COLLISION: return X_ERROR_ALREADY_EXISTS;
        case X_STATUS_ACCESS_DENIED: return X_ERROR_ACCESS_DENIED;
        case X_STATUS_END_OF_FILE: return 38; // ERROR_HANDLE_EOF
        case X_STATUS_NO_MEMORY: return 8;    // ERROR_NOT_ENOUGH_MEMORY
        case X_STATUS_BUFFER_TOO_SMALL: return X_ERROR_INSUFFICIENT_BUFFER;
        case X_STATUS_NOT_IMPLEMENTED:
        case X_STATUS_NOT_SUPPORTED: return X_ERROR_NOT_SUPPORTED;
        case X_STATUS_TIMEOUT: return 1460;   // ERROR_TIMEOUT
        default:
            if ((status & 0xFFFF0000) == 0x80070000) // HRESULT_FROM_WIN32
                return status & 0xFFFF;
            return 317; // ERROR_MR_MID_NOT_FOUND
        }
    }

    uint32_t RtlImageXexHeaderField(uint32_t xexHeader, uint32_t key)
    {
        uint32_t address = FindXexHeaderField(xexHeader, key);
        if (address == 0)
            return 0;

        // Inline values are returned directly, other fields as the address of their data.
        if ((key & 0xFF) == 0 || (key & 0xFF) == 1)
            return *GuestPtr<be<uint32_t>>(address);

        return address;
    }

    // EXCEPTION_RECORD: ExceptionCode, ExceptionFlags, ExceptionRecord, ExceptionAddress, NumberParameters,
    // ExceptionInformation[15].
    void RtlRaiseException(be<uint32_t>* record)
    {
        uint32_t code = record[0];

        // MSVC's SetThreadName raises this exception for debuggers to pick up the thread name.
        if (code == 0x406D1388)
        {
            uint32_t nameAddress = record[6];
            uint32_t threadId = record[7];
            const char* name = nameAddress != 0 ? GuestPtr<char>(nameAddress) : "";

            auto thread = threadId == 0xFFFFFFFF ? GetCurrentThreadObject() : FindThreadById(threadId);
            if (thread)
            {
                SetThreadDescription(thread->hostThread, std::filesystem::path(name).wstring().c_str());
                LOGF_INFO("thread {} is named \"{}\"", thread->threadId, name);

                // Experiment (AR_BOOST_THREADS=<name part>): threads whose name contains it run at highest priority.
                if (const char* boost = AR_ENV("AR_BOOST_THREADS"); boost && strstr(name, boost))
                {
                    SetThreadPriority(thread->hostThread, THREAD_PRIORITY_HIGHEST);
                    LOGF_INFO("thread {} boosted", thread->threadId);
                }
            }

            return;
        }

        LOGF_ERROR("guest exception 0x{:08X} at 0x{:08X}", code, record[3].get());
    }

    void RtlUnwind(uint32_t targetFrame, uint32_t targetIp, uint32_t exceptionRecord, uint32_t returnValue)
    {
        LOGF_ERROR("RtlUnwind to frame 0x{:08X} ip 0x{:08X} is not supported", targetFrame, targetIp);
    }

    void RtlCaptureContext(uint32_t context)
    {
        LOG_UNIMPLEMENTED();
    }
}

AR_IMPORT(RtlInitAnsiString);
AR_IMPORT(RtlInitUnicodeString);
AR_IMPORT(RtlFreeAnsiString);
AR_IMPORT(RtlUnicodeStringToAnsiString);
AR_IMPORT(RtlMultiByteToUnicodeN);
AR_IMPORT(RtlUnicodeToMultiByteN);
AR_IMPORT(RtlUpcaseUnicodeChar);
AR_IMPORT(RtlCompareStringN);
AR_IMPORT(RtlCompareMemoryUlong);
AR_IMPORT(RtlFillMemoryUlong);
AR_IMPORT(RtlTimeToTimeFields);
AR_IMPORT(RtlTimeFieldsToTime);
AR_IMPORT(RtlNtStatusToDosError);
AR_IMPORT(RtlImageXexHeaderField);
AR_IMPORT(RtlRaiseException);
AR_IMPORT(RtlUnwind);
AR_IMPORT(RtlCaptureContext);

// The formatting functions take variadic arguments, so they read the guest registers directly.

static void WriteFormatted(char* buffer, uint32_t count, const std::string& text, PPCContext& ctx)
{
    // _snprintf and _vsnprintf return -1 when the output does not fit, and do not terminate it in that case.
    if (text.size() >= count)
    {
        memcpy(buffer, text.data(), count);
        ctx.r3.s64 = -1;
    }
    else
    {
        memcpy(buffer, text.c_str(), text.size() + 1);
        ctx.r3.s64 = int64_t(text.size());
    }
}

PPC_FUNC(__imp__sprintf)
{
    GuestRegisterArgs args(ctx, base, 2);
    auto text = FormatGuestString(g_memory.Translate<const char>(ctx.r4.u32), args);
    memcpy(g_memory.Translate(ctx.r3.u32), text.c_str(), text.size() + 1);
    ctx.r3.s64 = int64_t(text.size());
}

PPC_FUNC(__imp___snprintf)
{
    GuestRegisterArgs args(ctx, base, 3);
    auto text = FormatGuestString(g_memory.Translate<const char>(ctx.r5.u32), args);
    WriteFormatted(g_memory.Translate<char>(ctx.r3.u32), ctx.r4.u32, text, ctx);
}

PPC_FUNC(__imp___vsnprintf)
{
    GuestVaListArgs args(ctx.r6.u32);
    auto text = FormatGuestString(g_memory.Translate<const char>(ctx.r5.u32), args);
    WriteFormatted(g_memory.Translate<char>(ctx.r3.u32), ctx.r4.u32, text, ctx);
}

PPC_FUNC(__imp__DbgPrint)
{
    GuestRegisterArgs args(ctx, base, 1);
    auto text = FormatGuestString(g_memory.Translate<const char>(ctx.r3.u32), args);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    LOGF_INFO("{}", text);
    ctx.r3.u64 = 0;
}
