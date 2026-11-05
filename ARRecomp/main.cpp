#include <stdafx.h>
#include <kernel/keyboard_mouse.h>
#include <cpu/guest_thread.h>
#include <gpu/command_processor.h>
#include <gpu/renderer.h>
#include <gpu/d3d12_renderer.h>
#include <gpu/video_settings.h>
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
void InstallMemoryWatch();

static void PrintUsage()
{
    fmt::print(stderr,
        "Usage: ARRecomp --game <path> [--xex <path>] [--user <path>] [--trace]\n"
        "  --game  The Xbox 360 disc image (.iso) or a folder with its extracted files.\n"
        "  --xex   The executable to load (default: default.xex from the game).\n"
        "  --user  Folder for save data and caches (default: ./user).\n"
        "  --resolution WxH       Window or display resolution (default 1280x720).\n"
        "  --display MODE         windowed, borderless or fullscreen (default windowed).\n"
        "  --fps N                Frame rate: 30 (default), 60 or 120.\n"
        "  --antialiasing N       Anti-aliasing: 0 off (default), 1 FXAA.\n"
        "  --anisotropic N        Anisotropic filtering: 0 as the game asks (default), 2, 4, 8 or 16.\n"
        "  --texture-lod-bias F   Texture detail: 0 default, below 0 sharper.\n"
        "  --draw-distance F      How much farther objects keep their detail and are drawn: 1 default.\n"
        "  --sharpen N            Sharpening: 0 off (default) to 3.\n"
        "  --ultrawide            A wider view on resolutions wider than 16:9, instead of bars.\n"
        "  --vsync                Wait for the display's refresh.\n"
        "  --render-scale N       Internal resolution scale: 1 (720p, default), 2 (1440p), 3 (2160p).\n"
        "  --shadows on|off       Dynamic shadows (default on).\n"
        "  --mouse-sensitivity N  Camera speed with the mouse (default 1).\n"
        "  --invert-mouse         Mouse down looks up.\n"
        "  --online               Multiplayer between copies of the game, experimental (ports 36000-36127).\n"
        "  --online-peer IP       Also look for games at these addresses (comma separated).\n"
        "  --player-name NAME     Your name in multiplayer.\n"
        "  --controller N         Which controller this copy reads, 1 to 4 (default 1).\n");
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
    float mouseSensitivity = 1.0f;
    bool invertMouse = false;
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
        else if (arg == "--resolution" && i + 1 < argc)
        {
            unsigned width = 0, height = 0;
            if (sscanf(argv[++i], "%ux%u", &width, &height) == 2 && width >= 320 && height >= 240 && width <= 7680 && height <= 4320)
            {
                gpu::g_videoSettings.width = width;
                gpu::g_videoSettings.height = height;
            }
        }
        else if (arg == "--display" && i + 1 < argc)
        {
            std::string_view mode = argv[++i];
            gpu::g_videoSettings.mode = mode == "fullscreen" ? gpu::DisplayMode::Fullscreen :
                mode == "borderless" ? gpu::DisplayMode::Borderless : gpu::DisplayMode::Windowed;
        }
        else if (arg == "--controller" && i + 1 < argc)
            _putenv_s("AR_CONTROLLER", argv[++i]);
        else if (arg == "--online")
            _putenv_s("AR_ONLINE", "1");
        else if (arg == "--online-peer" && i + 1 < argc)
            _putenv_s("AR_ONLINE_PEER", argv[++i]);
        else if (arg == "--player-name" && i + 1 < argc)
            _putenv_s("AR_PLAYER_NAME", argv[++i]);
        else if (arg == "--fps" && i + 1 < argc)
            gpu::g_videoSettings.frameRate = std::clamp(atoi(argv[++i]), 30, 120);
        else if (arg == "--render-scale" && i + 1 < argc)
            gpu::g_videoSettings.renderScale = std::clamp(atoi(argv[++i]), 1, 4);
        else if (arg == "--antialiasing" && i + 1 < argc)
            gpu::g_videoSettings.antiAliasing = atoi(argv[++i]);
        else if (arg == "--anisotropic" && i + 1 < argc)
            gpu::g_videoSettings.anisotropy = std::clamp(atoi(argv[++i]), 0, 16);
        else if (arg == "--texture-lod-bias" && i + 1 < argc)
            gpu::g_videoSettings.textureLodBias = std::clamp(float(atof(argv[++i])), -3.0f, 3.0f);
        else if (arg == "--draw-distance" && i + 1 < argc)
            gpu::g_videoSettings.drawDistance = std::clamp(float(atof(argv[++i])), 1.0f, 1000.0f);
        else if (arg == "--sharpen" && i + 1 < argc)
            gpu::g_videoSettings.sharpen = std::clamp(atoi(argv[++i]), 0, 3);
        else if (arg == "--ultrawide")
            gpu::g_videoSettings.ultrawide = true;
        else if (arg == "--vsync")
            gpu::g_videoSettings.vsync = true;
        else if (arg == "--shadows" && i + 1 < argc)
            gpu::g_videoSettings.shadows = std::string_view(argv[++i]) != "off";
        else if (arg == "--mouse-sensitivity" && i + 1 < argc)
            mouseSensitivity = std::clamp(float(atof(argv[++i])), 0.05f, 20.0f);
        else if (arg == "--invert-mouse")
            invertMouse = true;
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
    kbm::SetMouseOptions(mouseSensitivity, invertMouse);

    if (gpuSurvey)
        gpu::EnableSurvey(userPath / "shaders");
    gpu::SetShaderCacheFolder(userPath / "shader_cache");
    gpu::PrecompileShaders(userPath / "shader_cache" / "microcode");
    gpu::d3d12::SetPipelineCacheFile(userPath / "shader_cache" / "pipelines.bin");
    StartThreadProfiler();
    StartHostProfiler();

    if (g_memory.base == nullptr)
    {
        LOGF_ERROR("failed to reserve 4 GB of guest memory");
        return 1;
    }

    g_runtimeHeap.Init();
    InstallMemoryWatch();

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

    // For reverse engineering: the loaded image, as the recompiled code sees it (AR_DUMP_IMAGE=<path>).
    if (const char* dumpPath = getenv("AR_DUMP_IMAGE"))
    {
        std::ofstream file(dumpPath, std::ios::binary);
        file.write(g_memory.Translate<const char>(g_module.imageBase), g_module.imageSize);
        LOGF_INFO("image dumped to {}", dumpPath);
    }

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
