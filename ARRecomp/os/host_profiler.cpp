#include <stdafx.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <unordered_set>

// Diagnostics (AR_HOST_PROFILE=<thread name substring>): samples the named host threads (such as "GPU command
// processor") about every millisecond and logs, every 10 seconds, the functions they spend time in: exclusive (the
// function executing) and inclusive (anywhere on the stack).
void StartHostProfiler()
{
    const char* filter = AR_ENV("AR_HOST_PROFILE");
    if (!filter)
        return;

    std::wstring wideFilter(filter, filter + strlen(filter));
    std::thread([wideFilter]
        {
            SetThreadDescription(GetCurrentThread(), L"Host profiler");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            timeBeginPeriod(1);
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);

            std::vector<HANDLE> threads;
            std::unordered_map<uint64_t, uint32_t> exclusive, inclusive;
            uint32_t samples = 0;
            auto lastReport = std::chrono::steady_clock::now();
            auto lastScan = std::chrono::steady_clock::time_point{};

            auto name = [](uint64_t address)
                {
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
                    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                    symbol->MaxNameLen = 255;
                    DWORD64 displacement;
                    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
                        return std::pair{ uint64_t(symbol->Address), std::string(symbol->Name) };
                    return std::pair{ address, fmt::format("{:X}", address) };
                };

            while (true)
            {
                Sleep(1);
                auto now = std::chrono::steady_clock::now();

                // Threads are looked up again now and then: the ones named are created after startup.
                if (now - lastScan > std::chrono::seconds(5))
                {
                    lastScan = now;
                    for (HANDLE thread : threads)
                        CloseHandle(thread);
                    threads.clear();
                    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    THREADENTRY32 entry{ sizeof(entry) };
                    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                    {
                        if (entry.th32OwnerProcessID != GetCurrentProcessId())
                            continue;
                        HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, entry.th32ThreadID);
                        if (!thread)
                            continue;
                        PWSTR description = nullptr;
                        bool match = SUCCEEDED(GetThreadDescription(thread, &description)) && description &&
                            std::wstring_view(description).find(wideFilter) != std::wstring_view::npos;
                        LocalFree(description);
                        if (match)
                            threads.push_back(thread);
                        else
                            CloseHandle(thread);
                    }
                    CloseHandle(snapshot);
                }

                for (HANDLE thread : threads)
                {
                    uint64_t frames[24];
                    uint32_t count = 0;
                    if (SuspendThread(thread) == DWORD(-1))
                        continue;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    if (GetThreadContext(thread, &context))
                    {
                        // Unwinding reads only the suspended thread's stack and the unwind tables (no allocation).
                        while (count < std::size(frames) && context.Rip != 0)
                        {
                            frames[count++] = context.Rip;
                            DWORD64 imageBase;
                            auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                            if (!function)
                            {
                                context.Rip = *reinterpret_cast<const uint64_t*>(context.Rsp);
                                context.Rsp += 8;
                                continue;
                            }
                            void* handlerData;
                            DWORD64 establisher;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
                        }
                    }
                    ResumeThread(thread);
                    if (count == 0)
                        continue;

                    samples++;
                    std::unordered_set<uint64_t> seen;
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint64_t function = name(frames[i]).first;
                        if (i == 0)
                            exclusive[function]++;
                        if (seen.insert(function).second)
                            inclusive[function]++;
                    }
                }

                if (now - lastReport < std::chrono::seconds(10) || samples == 0)
                    continue;
                lastReport = now;

                auto report = [&](const char* title, std::unordered_map<uint64_t, uint32_t>& counts, size_t limit)
                    {
                        std::vector<std::pair<uint32_t, uint64_t>> top;
                        for (auto [function, n] : counts)
                            top.emplace_back(n, function);
                        std::sort(top.begin(), top.end(), std::greater<>());
                        std::string text;
                        for (size_t i = 0; i < std::min(top.size(), limit); i++)
                            text += fmt::format("\n    {:5.1f}% {}", top[i].first * 100.0 / samples, name(top[i].second).second);
                        LOGF_INFO("host profile {} ({} samples):{}", title, samples, text);
                    };
                report("exclusive", exclusive, 40);
                report("inclusive", inclusive, 40);
                exclusive.clear();
                inclusive.clear();
                samples = 0;
            }
        }).detach();
}
