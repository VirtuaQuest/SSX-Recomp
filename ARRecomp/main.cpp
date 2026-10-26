#include <stdafx.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/variables.h>
#include <kernel/vfs.h>
#include <kernel/xex_loader.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

Memory g_memory;

void InstallCrashHandler();

static void PrintUsage()
{
    fmt::print(stderr,
        "Usage: ARRecomp --game <path> [--xex <path>] [--user <path>] [--trace]\n"
        "  --game  The Xbox 360 disc image (.iso) or a folder with its extracted files.\n"
        "  --xex   The executable to load (default: default.xex from the game).\n"
        "  --user  Folder for save data and caches (default: ./user).\n");
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    InstallCrashHandler();

    std::filesystem::path gamePath;
    std::filesystem::path xexPath;
    std::filesystem::path userPath = "user";

    for (int i = 1; i < argc; i++)
    {
        std::string_view arg = argv[i];
        if (arg == "--game" && i + 1 < argc)
            gamePath = argv[++i];
        else if (arg == "--xex" && i + 1 < argc)
            xexPath = argv[++i];
        else if (arg == "--user" && i + 1 < argc)
            userPath = argv[++i];
        else if (arg == "--trace")
            logger::g_minimumLevel = logger::Level::Trace;
        else
        {
            PrintUsage();
            return 1;
        }
    }

    if (gamePath.empty())
    {
        PrintUsage();
        return 1;
    }

    if (g_memory.base == nullptr)
    {
        LOGF_ERROR("failed to reserve 4 GB of guest memory");
        return 1;
    }

    g_runtimeHeap.Init();

    if (!vfs::Mount(gamePath, userPath))
        return 1;

    // The executable is loaded from the host. Without --xex it is extracted from the game.
    if (xexPath.empty())
    {
        xexPath = userPath / "default.xex";
        if (!vfs::ExtractFile("game:\\default.xex", xexPath))
        {
            LOGF_ERROR("could not find default.xex in {}", gamePath.string());
            return 1;
        }
    }

    if (!LoadExecutable(xexPath))
        return 1;

    StartTimeStampBundleUpdates();

    ThreadStartParams params{};
    params.startAddress = g_module.entryPoint;
    params.stackSize = g_defaultStackSize;

    auto mainThread = CreateGuestThread(params);
    if (!mainThread)
        return 1;

    XDISPATCHER_HEADER* header = mainThread->Header();
    WaitForDispatcherObjects({ &header, 1 }, false, std::nullopt, false);

    LOGF_INFO("main thread exited with code 0x{:X}", mainThread->exitCode);
    return int(mainThread->exitCode);
}
