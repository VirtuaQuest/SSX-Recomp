#include <stdafx.h>
#include <cpu/guest_thread.h>
#include <gpu/command_processor.h>
#include <gpu/renderer.h>
#include <gpu/d3d12_renderer.h>
#include <gpu/shader_compiler.h>
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
    int dumpThreadsAfter = 0;
    bool gpuSurvey = false;

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
        else if (arg == "--gpu-survey")
            gpuSurvey = true;
        else if (arg == "--screenshots")
            gpu::d3d12::EnableScreenshots("screenshots");
        else if (arg == "--headless")
            gpu::DisableRendering();
        else if (arg == "--dump-threads" && i + 1 < argc)
            dumpThreadsAfter = atoi(argv[++i]);
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

    if (gpuSurvey)
        gpu::EnableSurvey(userPath / "shaders");
    gpu::SetShaderCacheFolder(userPath / "shader_cache");

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

    if (dumpThreadsAfter > 0)
    {
        std::thread([dumpThreadsAfter]
            {
                std::this_thread::sleep_for(std::chrono::seconds(dumpThreadsAfter));
                DumpGuestThreads();
                gpu::DumpState();

                // The title's Direct3D device: fence counter (+0x2B1C) and the completed fence it points at (+0x2B10).
                uint32_t device = *g_memory.Translate<be<uint32_t>>(GetKernelVariable("VdGlobalDevice"));
                if (device != 0)
                {
                    uint32_t fenceAddress = *g_memory.Translate<be<uint32_t>>(device + 0x2B10);
                    LOGF_INFO("D3D device 0x{:08X}: fence counter {}, completed fence at 0x{:08X} = {}", device,
                        g_memory.Translate<be<uint32_t>>(device + 0x2B1C)->get(), fenceAddress,
                        fenceAddress != 0 ? g_memory.Translate<be<uint32_t>>(fenceAddress)->get() : 0);
                }
            }).detach();
    }

    XDISPATCHER_HEADER* header = mainThread->Header();
    WaitForDispatcherObjects({ &header, 1 }, false, std::nullopt, false);

    LOGF_INFO("main thread exited with code 0x{:X}", mainThread->exitCode);
    return int(mainThread->exitCode);
}
