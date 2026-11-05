// Setup: the player points it at their copy of the game and its DLC, and it does the rest: takes the executable from the disc, unpacks the DLC,
// recompiles, compiles, and takes the icon from the game. Nothing of the game comes with this project. All of it stays on the player's own PC.
#include "theme.h"
#include "package.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // What this project's hooks were made for. Any other executable has its code at other addresses.
    constexpr const char* KNOWN_EXECUTABLE_SHA1 = "639e27f4d5c970b80a03b2c64932d5644a6c9b6d";
    constexpr uint32_t TITLE_ID = 0x5345083F;
    constexpr uint64_t NEEDED_DISK_BYTES = 5ull * 1024 * 1024 * 1024;
    constexpr int WINDOW_WIDTH = 760, WINDOW_HEIGHT = 640;

    enum Control
    {
        ID_GAME = 100,
        ID_GAME_BROWSE,
        ID_DLC,
        ID_DLC_BROWSE,
        ID_BUILD,
        ID_LAUNCHER,
        ID_LOG_FILE,
        ID_BAR,
        ID_LOG,
    };

    enum class Check
    {
        Good,
        Warning,
        Bad,
    };

    theme::Form g_form;
    HWND g_window, g_game, g_dlc, g_build, g_launcher, g_bar, g_log;
    int g_gameStatus, g_dlcStatus, g_stepLabel, g_timeLabel;
    int g_toolLabels[6];
    fs::path g_root, g_setupFolder, g_buildFolder, g_outFolder, g_runFolder, g_logPath;

    struct Toolchain
    {
        fs::path visualStudio, vcvars, clang, cmake, ninja, xenonRecomp;
        bool sdk{}, disk{};
        bool Ready() const { return !vcvars.empty() && !clang.empty() && !cmake.empty() && !ninja.empty() && !xenonRecomp.empty() && sdk && disk; }
    };
    Toolchain g_tools;

    Check g_gameCheck = Check::Bad;

    // ---- Between the build's thread and the window ----

    struct Shared
    {
        std::mutex mutex;
        int progress{};
        std::wstring step;
        std::wstring remaining;
        std::deque<std::wstring> lines;
    };
    Shared g_shared;
    std::atomic<bool> g_building, g_cancel, g_finished, g_succeeded;
    std::thread g_worker;
    HANDLE g_job;
    std::chrono::steady_clock::time_point g_started;
    std::ofstream g_logFile;
    std::mutex g_logMutex;

    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
            return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
        std::wstring wide(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
        return wide;
    }

    std::string Narrow(const std::wstring& text)
    {
        if (text.empty())
            return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), narrow.data(), size, nullptr, nullptr);
        return narrow;
    }

    void WriteLog(const std::string& line)
    {
        std::lock_guard lock(g_logMutex);
        if (g_logFile.is_open())
        {
            g_logFile << line << '\n';
            g_logFile.flush();
        }
    }

    // A line for the window as well as the log file.
    void Say(const std::wstring& line)
    {
        WriteLog(Narrow(line));
        std::lock_guard lock(g_shared.mutex);
        g_shared.lines.push_back(line);
    }

    void SetStep(const std::wstring& step, int progress)
    {
        std::lock_guard lock(g_shared.mutex);
        g_shared.step = step;
        g_shared.progress = progress;
    }

    void SetProgressOnly(int progress, const std::wstring& step = {}, const std::wstring& remaining = {})
    {
        std::lock_guard lock(g_shared.mutex);
        g_shared.progress = progress;
        if (!step.empty())
            g_shared.step = step;
        g_shared.remaining = remaining;
    }

    // ---- Running programs ----

    std::wstring Quote(const fs::path& path)
    {
        return L"\"" + path.wstring() + L"\"";
    }

    // Runs a program with its output going to a callback, line by line. Below normal priority, so the PC stays usable during a long compile.
    int Run(std::wstring command, const fs::path& folder, const std::function<void(const std::string&)>& onLine, bool lowPriority = true, HANDLE job = nullptr)
    {
        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        HANDLE readPipe, writePipe;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0))
            return -1;
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (lowPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
        BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, folder.empty() ? nullptr : folder.c_str(), &startup, &process);
        CloseHandle(writePipe);
        if (!started)
        {
            CloseHandle(readPipe);
            return -1;
        }
        if (job != nullptr)
            AssignProcessToJobObject(job, process.hProcess);
        ResumeThread(process.hThread);

        std::string pending;
        char buffer[4096];
        DWORD got;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
        {
            pending.append(buffer, got);
            size_t end;
            while ((end = pending.find_first_of("\r\n")) != std::string::npos)
            {
                if (end > 0 && onLine)
                    onLine(pending.substr(0, end));
                pending.erase(0, end + 1);
            }
        }
        if (!pending.empty() && onLine)
            onLine(pending);

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = DWORD(-1);
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        CloseHandle(readPipe);
        return int(code);
    }

    std::string Capture(const std::wstring& command)
    {
        std::string out;
        Run(command, {}, [&](const std::string& line) { out += line + "\n"; }, false);
        return out;
    }

    fs::path OnPath(const wchar_t* exe)
    {
        wchar_t found[MAX_PATH * 2];
        return SearchPathW(nullptr, exe, nullptr, DWORD(std::size(found)), found, nullptr) ? fs::path(found) : fs::path();
    }

    std::wstring GetEnv(const wchar_t* name)
    {
        wchar_t value[4096];
        DWORD size = GetEnvironmentVariableW(name, value, DWORD(std::size(value)));
        return size > 0 && size < std::size(value) ? value : L"";
    }

    bool Exists(const fs::path& path)
    {
        std::error_code ec;
        return fs::exists(path, ec);
    }

    // ---- What's needed to build ----

    Toolchain FindTools()
    {
        Toolchain tools;
        fs::path programs = GetEnv(L"ProgramFiles(x86)");
        fs::path vswhere = programs / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
        if (Exists(vswhere))
        {
            // The compiler here was tested with Visual Studio 2022's libraries, so those if they're here.
            for (const wchar_t* version : { L"-version [17.0,18.0) ", L"" })
            {
                std::string out = Capture(Quote(vswhere) + L" -products * " + version + L"-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath -utf8");
                std::wstring path = Widen(out.substr(0, out.find_first_of("\r\n")));
                if (!path.empty() && Exists(fs::path(path) / L"VC\\Auxiliary\\Build\\vcvars64.bat"))
                {
                    tools.visualStudio = path;
                    tools.vcvars = fs::path(path) / L"VC\\Auxiliary\\Build\\vcvars64.bat";
                    break;
                }
            }
        }

        std::vector<fs::path> clangFolders;
        if (auto dir = GetEnv(L"LLVM_DIR"); !dir.empty())
            clangFolders.push_back(fs::path(dir) / L"bin");
        clangFolders.push_back(g_root.parent_path() / L"_tools" / L"llvm" / L"bin");
        clangFolders.push_back(fs::path(GetEnv(L"ProgramFiles")) / L"LLVM" / L"bin");
        if (!tools.visualStudio.empty())
            clangFolders.push_back(tools.visualStudio / L"VC\\Tools\\Llvm\\x64\\bin");
        for (const fs::path& folder : clangFolders)
        {
            if (Exists(folder / L"clang-cl.exe"))
            {
                tools.clang = folder / L"clang-cl.exe";
                break;
            }
        }
        if (tools.clang.empty())
            tools.clang = OnPath(L"clang-cl.exe");

        fs::path bundled = tools.visualStudio / L"Common7\\IDE\\CommonExtensions\\Microsoft\\CMake";
        tools.cmake = !tools.visualStudio.empty() && Exists(bundled / L"CMake\\bin\\cmake.exe") ? bundled / L"CMake\\bin\\cmake.exe" : OnPath(L"cmake.exe");
        if (tools.cmake.empty() && Exists(fs::path(GetEnv(L"ProgramFiles")) / L"CMake\\bin\\cmake.exe"))
            tools.cmake = fs::path(GetEnv(L"ProgramFiles")) / L"CMake\\bin\\cmake.exe";
        tools.ninja = !tools.visualStudio.empty() && Exists(bundled / L"Ninja\\ninja.exe") ? bundled / L"Ninja\\ninja.exe" : OnPath(L"ninja.exe");

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(programs / L"Windows Kits\\10\\bin", ec))
            tools.sdk |= Exists(entry.path() / L"x64\\dxcompiler.dll");

        fs::path xenon = GetEnv(L"XENONRECOMP_ROOT").empty() ? g_root.parent_path() / L"XenonRecomp" : fs::path(GetEnv(L"XENONRECOMP_ROOT"));
        if (Exists(xenon / L"XenonUtils\\ppc_context.h") && !fs::is_empty(xenon / L"thirdparty\\simde", ec) && !ec)
            tools.xenonRecomp = xenon;

        ULARGE_INTEGER free{};
        tools.disk = GetDiskFreeSpaceExW(g_root.c_str(), &free, nullptr, nullptr) && free.QuadPart >= NEEDED_DISK_BYTES;
        return tools;
    }

    void ShowTools()
    {
        auto show = [](int i, bool good, const std::wstring& name, const std::wstring& missing)
            {
                g_form.SetLabel(g_toolLabels[i], (good ? L"✓  " : L"✗  ") + (good ? name : name + L": " + missing), good ? theme::GREEN : theme::BAD);
            };
        show(0, !g_tools.vcvars.empty(), L"Visual Studio C++ tools", L"install Build Tools");
        show(1, g_tools.sdk, L"Windows SDK", L"add it in that installer");
        show(2, !g_tools.clang.empty(), L"LLVM clang-cl", L"install LLVM");
        show(3, !g_tools.cmake.empty() && !g_tools.ninja.empty(), L"CMake and Ninja", L"add CMake in VS installer");
        show(4, !g_tools.xenonRecomp.empty(), L"XenonRecomp", L"clone next to this folder");
        show(5, g_tools.disk, L"5 GB of disk space", L"not enough free");
    }

    // ---- The player's files ----

    std::wstring GetText(HWND edit)
    {
        std::wstring text(size_t(GetWindowTextLengthW(edit)) + 1, L'\0');
        GetWindowTextW(edit, text.data(), int(text.size()));
        text.resize(wcslen(text.c_str()));
        return text;
    }

    void RefreshBuildButton();

    void CheckGame()
    {
        std::wstring path = GetText(g_game);
        g_gameCheck = Check::Bad;
        if (path.empty())
        {
            g_form.SetLabel(g_gameStatus, L"Choose your Xbox 360 disc image (.iso) or a folder with the game's files.", theme::DIM);
        }
        else
        {
            std::vector<uint8_t> xex;
            std::wstring error;
            if (!package::ReadGameFile(path, "default.xex", xex, error))
                g_form.SetLabel(g_gameStatus, error, theme::BAD);
            else if (package::Sha1(xex.data(), xex.size()) == KNOWN_EXECUTABLE_SHA1)
            {
                g_gameCheck = Check::Good;
                g_form.SetLabel(g_gameStatus, L"✓  Recognised: the version this project was made for.", theme::GREEN);
            }
            else
            {
                g_gameCheck = Check::Warning;
                g_form.SetLabel(g_gameStatus, L"A different version from the one this project was made for. It may not work.", theme::WARNING);
            }
        }
        RefreshBuildButton();
    }

    struct DlcPackage
    {
        fs::path path;
        std::wstring name;
    };

    std::vector<DlcPackage> FindDlc(const std::wstring& location)
    {
        std::vector<DlcPackage> found;
        if (location.empty())
            return found;
        auto consider = [&](const fs::path& path)
            {
                package::Info info = package::ReadInfo(path);
                if (info.valid && info.titleId == TITLE_ID && info.contentType == package::CONTENT_MARKETPLACE)
                    found.push_back({ path, info.name });
            };
        std::error_code ec;
        if (fs::is_regular_file(location, ec))
            consider(location);
        else
        {
            fs::recursive_directory_iterator it(location, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec))
            {
                if (it->is_regular_file(ec))
                    consider(it->path());
            }
        }
        return found;
    }

    void CheckDlc()
    {
        std::wstring location = GetText(g_dlc);
        if (location.empty())
        {
            g_form.SetLabel(g_dlcStatus, L"Optional. A folder with your DLC packages, or one package.", theme::DIM);
            return;
        }
        if (!Exists(location))
        {
            g_form.SetLabel(g_dlcStatus, L"This can't be found.", theme::BAD);
            return;
        }
        std::vector<DlcPackage> found = FindDlc(location);
        if (found.empty())
        {
            g_form.SetLabel(g_dlcStatus, L"No DLC for this game was found here.", theme::BAD);
            return;
        }
        std::wstring names;
        for (const DlcPackage& dlc : found)
        {
            if (!dlc.name.empty())
                names += (names.empty() ? L": " : L", ") + dlc.name;
        }
        g_form.SetLabel(g_dlcStatus, L"✓  " + std::to_wstring(found.size()) + (found.size() == 1 ? L" package" : L" packages") + names, theme::GREEN);
    }

    void RefreshBuildButton()
    {
        if (g_build != nullptr && !g_building)
            EnableWindow(g_build, g_tools.Ready() && g_gameCheck != Check::Bad);
    }

    // ---- Paths the launcher keeps, relative to itself ----

    std::wstring RelativeTo(const fs::path& path, const fs::path& folder)
    {
        fs::path relative = fs::absolute(path).lexically_normal().lexically_relative(fs::absolute(folder).lexically_normal());
        return relative.empty() ? path.wstring() : relative.wstring();
    }

    // ---- The icon ----

#pragma pack(push, 2)
    struct IconDirectoryEntry
    {
        BYTE width, height, colors, reserved;
        WORD planes, bits;
        DWORD bytes;
        WORD id;
    };
#pragma pack(pop)

    // Puts an icon file into a program as its icon, as Explorer shows it. The icon is only ever in programs built on this PC.
    bool StampIcon(const fs::path& exe, const fs::path& icon)
    {
        std::ifstream stream(icon, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (file.size() < 6)
            return false;
        uint16_t count = uint16_t(file[4] | file[5] << 8);
        if (file.size() < size_t(6) + size_t(count) * 16)
            return false;

        HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
        if (update == nullptr)
            return false;

        const WORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
        std::vector<uint8_t> group(6);
        memcpy(group.data(), file.data(), 6);
        bool good = true;
        for (uint16_t i = 0; i < count; i++)
        {
            const uint8_t* entry = file.data() + 6 + size_t(i) * 16;
            DWORD bytes, offset;
            memcpy(&bytes, entry + 8, 4);
            memcpy(&offset, entry + 12, 4);
            if (uint64_t(offset) + bytes > file.size())
            {
                good = false;
                break;
            }
            good &= UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(i + 1), language, file.data() + offset, bytes) != FALSE;

            IconDirectoryEntry item{ entry[0], entry[1], entry[2], entry[3], WORD(entry[4] | entry[5] << 8), WORD(entry[6] | entry[7] << 8), bytes, WORD(i + 1) };
            size_t at = group.size();
            group.resize(at + sizeof(item));
            memcpy(group.data() + at, &item, sizeof(item));
        }
        good = good && UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), language, group.data(), DWORD(group.size())) != FALSE;
        return EndUpdateResourceW(update, !good) != FALSE && good;
    }

    // A running program can't be replaced but can be renamed, so that one keeps running and the new one is built.
    void MoveAsideIfRunning(const fs::path& exe)
    {
        if (!Exists(exe))
            return;
        HANDLE handle = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
            return;
        }
        for (int i = 0; i < 20; i++)
        {
            fs::path aside = exe;
            aside.replace_extension(L".running" + (i == 0 ? std::wstring() : std::to_wstring(i)) + L".exe");
            if (MoveFileExW(exe.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                Say(exe.filename().wstring() + L" is running: it was renamed " + aside.filename().wstring() + L" and goes on running.");
                return;
            }
        }
    }

    // ---- The build ----

    struct Choices
    {
        fs::path game;
        std::wstring dlc;
    };

    // Parts of the bar, out of 1000, by about how long each takes.
    constexpr int BAR_PREPARE = 15, BAR_TOOL_SETUP = 30, BAR_TOOL = 80, BAR_GENERATE = 160, BAR_CONFIGURE = 175, BAR_COMPILE = 955, BAR_ICON = 995;

    void ApplyToolchainEnvironment()
    {
        // vcvars64 looks for this on the PATH.
        std::wstring path = (fs::path(GetEnv(L"ProgramFiles(x86)")) / L"Microsoft Visual Studio\\Installer").wstring() + L";" + GetEnv(L"PATH");
        SetEnvironmentVariableW(L"PATH", path.c_str());

        std::wstring command = GetEnv(L"ComSpec") + L" /d /s /c \"\"" + g_tools.vcvars.wstring() + L"\" >nul 2>&1 && set\"";
        std::string out = Capture(command);
        size_t at = 0;
        while (at < out.size())
        {
            size_t end = out.find('\n', at);
            std::string line = out.substr(at, end - at);
            at = end == std::string::npos ? out.size() : end + 1;
            size_t equals = line.find('=');
            if (equals != std::string::npos && equals > 0)
                SetEnvironmentVariableW(Widen(line.substr(0, equals)).c_str(), Widen(line.substr(equals + 1)).c_str());
        }

        path = g_tools.clang.parent_path().wstring() + L";" + g_tools.cmake.parent_path().wstring() + L";" + g_tools.ninja.parent_path().wstring() + L";" + GetEnv(L"PATH");
        SetEnvironmentVariableW(L"PATH", path.c_str());
    }

    // Ninja's "[12/345] ..." lines, as a part of the bar.
    std::function<void(const std::string&)> NinjaProgress(int from, int to, const std::wstring& what, bool estimate)
    {
        auto started = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        return [=](const std::string& line)
            {
                WriteLog(line);
                unsigned done, total;
                if (line.size() > 1 && line[0] == '[' && sscanf_s(line.c_str(), "[%u/%u]", &done, &total) == 2 && total > 0)
                {
                    double part = double(done) / total;
                    std::wstring remaining;
                    if (estimate && part > 0.03)
                    {
                        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - *started).count() * (1.0 - part) / part;
                        int minutes = int(seconds / 60.0 + 0.999);
                        remaining = minutes <= 1 ? L"about a minute left" : L"about " + std::to_wstring(minutes) + L" minutes left";
                    }
                    SetProgressOnly(from + int((to - from) * part), what + L" (" + std::to_wstring(done) + L" of " + std::to_wstring(total) + L")", remaining);
                }
                else if (line.find("error") != std::string::npos || line.find("FAILED") != std::string::npos)
                    Say(Widen(line.substr(0, 200)));
            };
    }

    bool Failed(const std::wstring& message)
    {
        Say(L"Stopped: " + message);
        return false;
    }

    bool WriteIfDifferent(const fs::path& path, const std::vector<uint8_t>& data)
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing)
        {
            std::vector<uint8_t> current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (current == data)
                return true;
            existing.close();
        }
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return output.good();
    }

    bool InstallDlc(const std::wstring& location)
    {
        std::vector<DlcPackage> found = FindDlc(location);
        fs::path content = g_runFolder / L"user\\content\\00000002";
        std::error_code ec;
        fs::create_directories(content, ec);
        int done = 0;
        for (const DlcPackage& dlc : found)
        {
            // The game has room for 42 characters in a package's name, which is what real ones have.
            std::wstring name = dlc.path.filename().wstring().substr(0, 42);
            std::wstring shown = dlc.name.empty() ? name.substr(0, 12) + L"..." : dlc.name;
            fs::path target = content / name;
            fs::path unpacking = g_setupFolder / L"unpacking";
            fs::remove_all(unpacking, ec);
            std::wstring error;
            if (!package::Extract(dlc.path, unpacking, error))
            {
                fs::remove_all(unpacking, ec);
                return Failed(L"DLC " + shown + L": " + error + L".");
            }
            fs::remove_all(target, ec);
            fs::rename(unpacking, target, ec);
            if (ec)
                return Failed(L"DLC " + shown + L" couldn't be put in place.");
            Say(L"DLC installed: " + shown);
            SetProgressOnly(BAR_PREPARE + 5 * ++done / int(found.size()));
        }
        return true;
    }

    bool BuildAll(const Choices& choices)
    {
        std::error_code ec;
        fs::create_directories(g_setupFolder, ec);
        fs::create_directories(g_runFolder / L"user", ec);

        SetStep(L"Reading the game", 2);
        ApplyToolchainEnvironment();
        if (g_cancel)
            return false;

        std::vector<uint8_t> xex;
        std::wstring error;
        if (!package::ReadGameFile(choices.game, "default.xex", xex, error))
            return Failed(error);
        fs::create_directories(g_root / L"ARRecompLib\\private", ec);
        if (!WriteIfDifferent(g_root / L"ARRecompLib\\private\\default.xex", xex))
            return Failed(L"The game's executable couldn't be saved. Is the disk full?");
        Say(L"Took the executable from the game.");

        SetStep(L"Unpacking DLC", BAR_PREPARE);
        if (!choices.dlc.empty() && !InstallDlc(choices.dlc))
            return false;

        if (g_cancel)
            return false;

        std::wstring configure = Quote(g_tools.cmake) + L" -S " + Quote(g_root) + L" -B " + Quote(g_buildFolder) +
            L" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DXENONRECOMP_ROOT=" + Quote(g_tools.xenonRecomp);
        auto logOnly = [](const std::string& line) { WriteLog(line); };

        SetStep(L"Preparing the recompiler", BAR_PREPARE + 5);
        if (Run(configure, g_root, logOnly, true, g_job) != 0)
            return g_cancel ? false : Failed(L"CMake couldn't set up the build. See the log.");

        SetProgressOnly(BAR_TOOL_SETUP, L"Building the recompiler");
        if (Run(Quote(g_tools.cmake) + L" --build " + Quote(g_buildFolder) + L" --target XenonRecomp", g_root,
            NinjaProgress(BAR_TOOL_SETUP, BAR_TOOL, L"Building the recompiler", false), true, g_job) != 0)
            return g_cancel ? false : Failed(L"The recompiler didn't build. See the log.");

        // It leaves files that come out the same as they were, so an unchanged game doesn't compile again.
        SetProgressOnly(BAR_TOOL, L"Recompiling the game's code");
        fs::create_directories(g_root / L"ARRecompLib\\ppc", ec);
        fs::path recompiler = g_buildFolder / L"XenonRecomp\\XenonRecomp\\XenonRecomp.exe";
        int recompiled = Run(Quote(recompiler) + L" " + Quote(g_root / L"ARRecompLib\\config\\AR.toml") + L" " + Quote(g_tools.xenonRecomp / L"XenonUtils\\ppc_context.h"), g_root,
            [](const std::string& line)
            {
                float percent;
                if (sscanf_s(line.c_str(), "Recompiling functions... %f%%", &percent) == 1)
                    SetProgressOnly(BAR_TOOL + int((BAR_GENERATE - BAR_TOOL) * percent / 100.0f), L"Recompiling the game's code");
                else
                    WriteLog(line);
            }, true, g_job);
        if (recompiled != 0)
            return g_cancel ? false : Failed(L"The recompiler stopped with an error. See the log.");
        Say(L"Recompiled the game's code.");

        SetProgressOnly(BAR_GENERATE, L"Setting up the compile");
        if (Run(configure, g_root, logOnly, true, g_job) != 0)
            return g_cancel ? false : Failed(L"CMake couldn't set up the build. See the log.");

        fs::path outputFolder = g_buildFolder / L"ARRecomp";
        MoveAsideIfRunning(outputFolder / L"ARRecomp.exe");
        MoveAsideIfRunning(outputFolder / L"ARLauncher.exe");

        SetProgressOnly(BAR_CONFIGURE, L"Compiling");
        if (Run(Quote(g_tools.cmake) + L" --build " + Quote(g_buildFolder) + L" --target ARRecomp ARLauncher", g_root,
            NinjaProgress(BAR_CONFIGURE, BAR_COMPILE, L"Compiling", true), true, g_job) != 0)
            return g_cancel ? false : Failed(L"The compile failed. See the log.");
        Say(L"Compiled.");

        // The launcher's settings: where the game is, and where it keeps its things. The player's other choices stay.
        fs::path ini = outputFolder / L"launcher.ini";
        WritePrivateProfileStringW(L"Launcher", L"Game", RelativeTo(choices.game, outputFolder).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Launcher", L"WorkingFolder", RelativeTo(g_runFolder, outputFolder).c_str(), ini.c_str());

        // The icon: cut from the player's game by running it for a moment, unseen and silent, in a folder of its own, so saves aren't touched.
        SetProgressOnly(BAR_COMPILE, L"Taking the icon from your game", L"");
        fs::path icon = g_runFolder / L"user\\game_icon.ico";
        if (!Exists(icon))
        {
            fs::path scratch = g_setupFolder / L"icon";
            fs::remove_all(scratch, ec);
            fs::create_directories(scratch, ec);
            Run(Quote(outputFolder / L"ARRecomp.exe") + L" --game " + Quote(fs::absolute(choices.game)) + L" --make-icon", scratch, logOnly, true, g_job);
            if (g_cancel)
                return false;
            if (Exists(scratch / L"user\\game_icon.ico"))
                fs::copy_file(scratch / L"user\\game_icon.ico", icon, fs::copy_options::overwrite_existing, ec);
            fs::remove_all(scratch, ec);
        }
        if (Exists(icon))
        {
            bool stamped = StampIcon(outputFolder / L"ARRecomp.exe", icon);
            stamped &= StampIcon(outputFolder / L"ARLauncher.exe", icon);
            Say(stamped ? L"The programs have the icon from your game." : L"The icon couldn't be put in the programs. They'll still work.");
        }
        else
            Say(L"The icon couldn't be taken from the game now. The game will make it the first time it reaches the title screen.");
        SetProgressOnly(BAR_ICON, L"Finishing");
        return true;
    }

    void StartBuild()
    {
        std::wstring gamePath = GetText(g_game);

        if (g_gameCheck == Check::Warning && MessageBoxW(g_window,
            L"This isn't the version of the game this project was made for.\n\nThe fixes in this project are tied to exact places in the game's code, so it may crash or misbehave. Build it anyway?",
            L"Setup", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
        // What was chosen is remembered.
        fs::path settings = g_setupFolder / L"setup.ini";
        std::error_code ec;
        fs::create_directories(g_setupFolder, ec);
        WritePrivateProfileStringW(L"Setup", L"Game", gamePath.c_str(), settings.c_str());
        WritePrivateProfileStringW(L"Setup", L"Dlc", GetText(g_dlc).c_str(), settings.c_str());

        {
            std::lock_guard lock(g_logMutex);
            g_logFile.close();
            g_logFile.open(g_logPath, std::ios::binary | std::ios::trunc);
        }
        SetWindowTextW(g_log, L"");
        g_building = true;
        g_cancel = false;
        g_finished = false;
        g_succeeded = false;
        g_started = std::chrono::steady_clock::now();
        SetWindowTextW(g_build, L"Cancel");
        InvalidateRect(g_build, nullptr, FALSE);
        EnableWindow(g_launcher, FALSE);
        EnableWindow(g_game, FALSE);
        EnableWindow(g_dlc, FALSE);

        // Everything started goes when the job does: on Cancel, and if this window goes away.
        if (g_job != nullptr)
            CloseHandle(g_job);
        g_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        Choices choices{ gamePath, GetText(g_dlc) };
        g_worker = std::thread([choices]
            {
                bool good = BuildAll(choices);
                if (g_cancel)
                    Say(L"Cancelled.");
                g_succeeded = good && !g_cancel;
                if (g_succeeded)
                    SetProgressOnly(1000, L"Finished", L"");
                g_finished = true;
            });
        SetTimer(g_window, 1, 150, nullptr);
    }

    void AppendLog(const std::wstring& line)
    {
        int length = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, length, length);
        std::wstring text = (length > 0 ? L"\r\n" : L"") + line;
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    }

    std::wstring TimeText()
    {
        int seconds = int(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count());
        wchar_t text[32];
        swprintf_s(text, L"%d:%02d", seconds / 60, seconds % 60);
        return text;
    }

    void Tick()
    {
        std::wstring step, remaining;
        int progress;
        std::deque<std::wstring> lines;
        {
            std::lock_guard lock(g_shared.mutex);
            step = g_shared.step;
            remaining = g_shared.remaining;
            progress = g_shared.progress;
            lines.swap(g_shared.lines);
        }
        for (const std::wstring& line : lines)
            AppendLog(line);
        theme::SetProgress(g_bar, progress);
        wchar_t percent[16];
        swprintf_s(percent, L"%d%%", progress / 10);

        if (g_finished)
        {
            KillTimer(g_window, 1);
            if (g_worker.joinable())
                g_worker.join();
            g_building = false;
            SetWindowTextW(g_build, L"Build");
            InvalidateRect(g_build, nullptr, FALSE);
            EnableWindow(g_game, TRUE);
            EnableWindow(g_dlc, TRUE);
            EnableWindow(g_launcher, Exists(g_buildFolder / L"ARRecomp\\ARLauncher.exe"));
            RefreshBuildButton();
            if (g_succeeded)
            {
                g_form.SetLabel(g_stepLabel, L"Finished. Open the launcher to play.", theme::LIME);
                g_form.SetLabel(g_timeLabel, L"100%   " + TimeText(), theme::LIME);
            }
            else
            {
                g_form.SetLabel(g_stepLabel, g_cancel ? L"Cancelled." : L"That didn't work: see the messages below.", g_cancel ? theme::WARNING : theme::BAD);
                g_form.SetLabel(g_timeLabel, TimeText(), theme::DIM);
            }
            return;
        }

        g_form.SetLabel(g_stepLabel, step, theme::TEXT);
        g_form.SetLabel(g_timeLabel, std::wstring(percent) + L"   " + TimeText() + (remaining.empty() ? L"" : L"   " + remaining), theme::DIM);
    }

    // ---- Choosing files ----

    void BrowseFile(HWND window, HWND edit, const wchar_t* filter)
    {
        wchar_t path[MAX_PATH * 2] = {};
        wcsncpy_s(path, GetText(edit).c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = window;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = path;
        dialog.nMaxFile = DWORD(std::size(path));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog))
            SetWindowTextW(edit, path);
    }

    void BrowseFolder(HWND window, HWND edit)
    {
        IFileOpenDialog* dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        DWORD options;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS);
        if (SUCCEEDED(dialog->Show(window)))
        {
            IShellItem* item;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    SetWindowTextW(edit, path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    std::wstring ReadSetting(const fs::path& file, const wchar_t* section, const wchar_t* key)
    {
        wchar_t value[MAX_PATH * 2] = {};
        GetPrivateProfileStringW(section, key, L"", value, DWORD(std::size(value)), file.c_str());
        return value;
    }

    // What was chosen last time, or what's already known: the launcher's game, and a DLC folder where consoles keep it, by the game.
    void LoadChoices()
    {
        fs::path settings = g_setupFolder / L"setup.ini";
        std::wstring game = ReadSetting(settings, L"Setup", L"Game");
        std::wstring dlc = ReadSetting(settings, L"Setup", L"Dlc");
        if (game.empty())
        {
            fs::path launcherFolder = g_buildFolder / L"ARRecomp";
            std::wstring known = ReadSetting(launcherFolder / L"launcher.ini", L"Launcher", L"Game");
            if (!known.empty())
            {
                fs::path resolved = (launcherFolder / known).lexically_normal();
                if (Exists(resolved))
                    game = resolved.wstring();
            }
        }
        if (dlc.empty() && !game.empty())
        {
            wchar_t folder[16];
            swprintf_s(folder, L"%08X", TITLE_ID);
            fs::path guess = fs::path(game).parent_path() / folder / L"00000002";
            if (Exists(guess))
                dlc = guess.wstring();
        }
        SetWindowTextW(g_game, game.c_str());
        SetWindowTextW(g_dlc, dlc.c_str());
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LRESULT handled;
        if (g_form.Handle(message, wParam, lParam, handled))
            return handled;

        switch (message)
        {
        case WM_CREATE:
        {
            g_window = window;
            g_form.Attach(window, WINDOW_WIDTH, WINDOW_HEIGHT, L"Setup", L"Anarchy Reigns Recompiled: builds the game on this PC from your own copy");

            constexpr int x = 24, width = WINDOW_WIDTH - 48, labelWidth = 104, browseWidth = 84;
            int y = 96;
            g_form.AddSection(x, y, width, 30 + 2 * 52 + 2, L"Your game");
            y += 30;
            auto row = [&](const wchar_t* label, int id, int browse, HWND& edit, int& status)
                {
                    g_form.AddLabel(x + 2, y, labelWidth, 28, label);
                    edit = g_form.AddEdit(id, x + labelWidth, y, width - labelWidth - browseWidth - 12, L"");
                    g_form.AddButton(browse, x + width - browseWidth - 6, y, browseWidth, 28, L"Browse");
                    status = g_form.AddLabel(x + labelWidth + 2, y + 29, width - labelWidth - 10, 18, L"", theme::DIM, theme::Font::Small);
                    y += 52;
                };
            row(L"Game", ID_GAME, ID_GAME_BROWSE, g_game, g_gameStatus);
            row(L"DLC", ID_DLC, ID_DLC_BROWSE, g_dlc, g_dlcStatus);

            y += 12;
            g_form.AddSection(x, y, width, 30 + 3 * 24 + 4, L"What's needed");
            y += 30;
            for (int i = 0; i < 6; i++)
                g_toolLabels[i] = g_form.AddLabel(x + 4 + (i % 2) * (width / 2), y + (i / 2) * 24, width / 2 - 8, 22, L"", theme::DIM, theme::Font::Body);
            y += 3 * 24 + 16;

            g_form.AddSection(x, y, width, WINDOW_HEIGHT - y - 16, L"Build");
            y += 30;
            g_stepLabel = g_form.AddLabel(x + 2, y, 400, 22, L"Ready. This takes a while, but the PC stays usable.", theme::DIM);
            g_timeLabel = g_form.AddLabel(x + 402, y, width - 408, 22, L"", theme::DIM, theme::Font::Body, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            y += 26;
            g_bar = g_form.AddProgress(ID_BAR, x + 2, y, width - 8, 26);
            y += 34;
            g_log = g_form.AddLog(ID_LOG, x + 2, y, width - 8 - 186, WINDOW_HEIGHT - y - 26);
            int buttonX = x + width - 178;
            g_build = g_form.AddButton(ID_BUILD, buttonX, y, 172, 44, L"Build", true);
            g_launcher = g_form.AddButton(ID_LAUNCHER, buttonX, y + 50, 172, 28, L"Open launcher");
            g_form.AddButton(ID_LOG_FILE, buttonX, y + 84, 172, 28, L"Open full log");

            g_tools = FindTools();
            ShowTools();
            LoadChoices();
            CheckGame();
            CheckDlc();
            EnableWindow(g_launcher, Exists(g_buildFolder / L"ARRecomp\\ARLauncher.exe"));
            return 0;
        }

        case WM_TIMER:
            Tick();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case ID_GAME_BROWSE:
                BrowseFile(window, g_game, L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0");
                CheckGame();
                break;
            case ID_DLC_BROWSE:
                BrowseFolder(window, g_dlc);
                CheckDlc();
                break;
            case ID_GAME:
                if (HIWORD(wParam) == EN_KILLFOCUS)
                    CheckGame();
                break;
            case ID_DLC:
                if (HIWORD(wParam) == EN_KILLFOCUS)
                    CheckDlc();
                break;
            case ID_BUILD:
            case IDOK:
                if (g_building)
                {
                    g_cancel = true;
                    if (g_job != nullptr)
                        TerminateJobObject(g_job, 1);
                }
                else if (IsWindowEnabled(g_build))
                    StartBuild();
                break;
            case ID_LAUNCHER:
            {
                fs::path launcher = g_buildFolder / L"ARRecomp\\ARLauncher.exe";
                ShellExecuteW(window, L"open", launcher.c_str(), nullptr, launcher.parent_path().c_str(), SW_SHOWNORMAL);
                break;
            }
            case ID_LOG_FILE:
                if (Exists(g_logPath))
                    ShellExecuteW(window, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            return 0;

        case WM_CLOSE:
            if (g_building)
            {
                if (MessageBoxW(window, L"A build is running. Stop it and close?", L"Setup", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                    return 0;
                g_cancel = true;
                if (g_job != nullptr)
                    TerminateJobObject(g_job, 1);
                if (g_worker.joinable())
                    g_worker.join();
            }
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    // The folder of this project, above this program, wherever it is.
    bool FindRoot()
    {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        for (fs::path folder = fs::path(exe).parent_path(); ; folder = folder.parent_path())
        {
            if (Exists(folder / L"ARRecompLib\\config\\AR.toml"))
            {
                g_root = folder;
                return true;
            }
            if (folder == folder.parent_path())
                return false;
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!FindRoot())
    {
        MessageBoxW(nullptr, L"Setup has to run from inside the project's folder. Use Setup.bat there.", L"Setup", MB_ICONERROR);
        return 1;
    }
    g_outFolder = g_root / L"out";
    g_setupFolder = g_outFolder / L"setup";
    g_buildFolder = g_outFolder / L"build";
    g_runFolder = g_root / L"run";
    g_logPath = g_setupFolder / L"setup.log";

    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"ARSetup";
    windowClass.hIcon = theme::MakeIcon(GetSystemMetrics(SM_CXICON));
    windowClass.hIconSm = theme::MakeIcon(GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&windowClass);

    RECT rect{ 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Anarchy Reigns Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}
