#include <stdafx.h>
#include "variables.h"
#include "heap.h"
#include "memory.h"
#include "objects.h"
#include "xex_loader.h"

static std::mutex g_variablesMutex;
static std::unordered_map<std::string, uint32_t> g_variables;

static void Write32(uint32_t address, uint32_t value)
{
    *g_memory.Translate<be<uint32_t>>(address) = value;
}

static void Write16(uint32_t address, uint16_t value)
{
    *g_memory.Translate<be<uint16_t>>(address) = value;
}

static uint32_t CreateVariable(std::string_view name)
{
    if (name == "XboxHardwareInfo")
    {
        // Flags, NumberOfProcessors, PCIBridgeRevisionID, Reserved[6], BldrMagic, BldrFlags.
        uint32_t address = g_runtimeHeap.AllocGuest(0x10);
        Write32(address, 0x00000020);
        *g_memory.Translate<uint8_t>(address + 4) = 6;
        return address;
    }

    if (name == "XboxKrnlVersion")
    {
        // Major, Minor, Build, Qfe; matches the kernel version the executable was linked against (2.0.21076.0).
        uint32_t address = g_runtimeHeap.AllocGuest(8);
        Write16(address + 0, 2);
        Write16(address + 2, 0);
        Write16(address + 4, 21076);
        Write16(address + 6, 0);
        return address;
    }

    if (name == "KeTimeStampBundle")
    {
        // InterruptTime, SystemTime, TickCount; updated by StartTimeStampBundleUpdates.
        return g_runtimeHeap.AllocGuest(0x18);
    }

    if (name == "ExLoadedCommandLine")
    {
        uint32_t address = g_runtimeHeap.AllocGuest(1024);
        strcpy(g_memory.Translate<char>(address), "\"default.xex\"");
        return address;
    }

    if (name == "XexExecutableModuleHandle")
    {
        // A pointer to the module handle (the LDR_DATA_TABLE_ENTRY).
        uint32_t address = g_runtimeHeap.AllocGuest(4);
        Write32(address, g_module.ldrEntry);
        return address;
    }

    if (name == "KeDebugMonitorData" || name == "KeCertMonitorData")
        return g_runtimeHeap.AllocGuest(0x100);

    if (name == "ExThreadObjectType" || name == "ExEventObjectType" || name == "ExSemaphoreObjectType" ||
        name == "ExMutantObjectType" || name == "ExTimerObjectType" || name == "IoFileObjectType")
    {
        // Object type descriptors are only compared by address.
        return g_runtimeHeap.AllocGuest(0x40);
    }

    if (name == "VdGlobalDevice" || name == "VdGlobalXamDevice")
    {
        // Holds a pointer to the Direct3D device, written by the game.
        return g_runtimeHeap.AllocGuest(4);
    }

    if (name == "VdGpuClockInMHz")
    {
        uint32_t address = g_runtimeHeap.AllocGuest(4);
        Write32(address, 500);
        return address;
    }

    if (name == "VdHSIOCalibrationLock")
    {
        // RTL_CRITICAL_SECTION: synchronization event header, LockCount = -1, RecursionCount, OwningThread.
        uint32_t address = g_runtimeHeap.AllocGuest(0x1C);
        auto* header = g_memory.Translate<XDISPATCHER_HEADER>(address);
        header->Type = DISPATCHER_SYNCHRONIZATION_EVENT;
        header->Size = 4;
        Write32(address + 0x10, 0xFFFFFFFF);
        return address;
    }

    return 0;
}

uint32_t GetKernelVariable(std::string_view name)
{
    std::lock_guard lock(g_variablesMutex);

    auto it = g_variables.find(std::string(name));
    if (it != g_variables.end())
        return it->second;

    uint32_t address = CreateVariable(name);
    if (address != 0)
        g_variables.emplace(std::string(name), address);

    return address;
}

void StartTimeStampBundleUpdates()
{
    uint32_t bundle = GetKernelVariable("KeTimeStampBundle");

    std::thread([bundle]()
        {
            auto start = std::chrono::steady_clock::now();
            while (true)
            {
                auto elapsed = std::chrono::steady_clock::now() - start;
                uint64_t interruptTime = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
                uint32_t tickCount = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

                *g_memory.Translate<be<uint64_t>>(bundle + 0x00) = interruptTime;
                *g_memory.Translate<be<uint64_t>>(bundle + 0x08) = uint64_t(GetGuestSystemTime());
                *g_memory.Translate<be<uint32_t>>(bundle + 0x10) = tickCount;

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
}
