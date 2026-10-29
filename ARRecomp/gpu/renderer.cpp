#include <stdafx.h>
#include "renderer.h"
#include <kernel/memory.h>
#include "d3d12_renderer.h"
#include "hlsl_translator.h"
#include "shader_compiler.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/shader.h"

namespace gpu
{
    using namespace reg;

    struct ShaderMicrocode
    {
        uint32_t type;
        uint64_t hash;
        std::vector<uint32_t> words; // Host byte order.

        // Translated on first use.
        std::unique_ptr<xe::gpu::Shader> shader;
        std::vector<uint8_t> code;
        bool translated = false;
    };

    static bool g_renderingDisabled;

    void DisableRendering()
    {
        g_renderingDisabled = true;
    }

    // Analyzes, translates and compiles a shader the first time a draw uses it.
    static bool PrepareShader(ShaderMicrocode& microcode)
    {
        if (microcode.translated)
            return !microcode.code.empty();

        microcode.translated = true;
        static thread_local xe::StringBuffer buffer;
        microcode.shader = std::make_unique<xe::gpu::Shader>(microcode.type == 0 ? xe::gpu::xenos::ShaderType::kVertex :
            xe::gpu::xenos::ShaderType::kPixel, microcode.hash, microcode.words.data(), microcode.words.size(), std::endian::little);
        microcode.shader->AnalyzeUcode(buffer);

        std::string hlsl = TranslateShaderToHlsl(*microcode.shader);
        for (const auto& error : microcode.shader->GetOrCreateTranslation(0)->errors())
            LOGF_WARNING("shader {:016X}: {}", microcode.hash, error.message);
        if (hlsl.empty())
            return false;

        auto name = fmt::format("{}_{:016X}", microcode.type == 0 ? "vs" : "ps", microcode.hash);
        microcode.code = CompileHlsl(hlsl, microcode.type == 0 ? L"vs_6_0" : L"ps_6_0", name.c_str());
        return !microcode.code.empty();
    }

    // Shaders seen for the first time while playing are prepared on the spot (timed, to find hitches).
    static bool PrepareShaderTimed(ShaderMicrocode& microcode)
    {
        if (microcode.translated)
            return !microcode.code.empty();

        auto start = std::chrono::steady_clock::now();
        bool result = PrepareShader(microcode);
        LOGF_INFO("{} {:016X} prepared while playing in {:.1f} ms", microcode.type == 0 ? "vertex shader" : "pixel shader",
            microcode.hash, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        return result;
    }

    static std::unordered_map<uint64_t, std::unique_ptr<ShaderMicrocode>> g_shaders;
    static ShaderMicrocode* g_activeShaders[2];
    static std::filesystem::path g_microcodeFolder;
    static uint64_t HashWords(const uint32_t* words, size_t count);

    void PrecompileShaders(const std::filesystem::path& microcodeFolder)
    {
        g_microcodeFolder = microcodeFolder;
        std::error_code ec;
        std::filesystem::create_directories(microcodeFolder, ec);

        std::vector<std::unique_ptr<ShaderMicrocode>> shaders;
        for (const auto& entry : std::filesystem::directory_iterator(microcodeFolder, ec))
        {
            std::string name = entry.path().filename().string();
            if (name.size() != 23 || (name.rfind("vs_", 0) != 0 && name.rfind("ps_", 0) != 0) || entry.path().extension() != ".bin")
                continue;

            std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
            std::vector<uint32_t> words(size_t(file.tellg()) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(words.data()), words.size() * 4);
            uint32_t type = name[0] == 'v' ? 0 : 1;
            uint64_t hash = HashWords(words.data(), words.size()) ^ type;
            shaders.push_back(std::make_unique<ShaderMicrocode>(ShaderMicrocode{ type, hash, std::move(words) }));
        }
        if (shaders.empty())
            return;

        auto start = std::chrono::steady_clock::now();
        std::atomic<size_t> next = 0;
        std::vector<std::thread> threads;
        for (uint32_t i = 0; i < std::max(1u, std::thread::hardware_concurrency()); i++)
        {
            threads.emplace_back([&]
                {
                    for (size_t index; (index = next++) < shaders.size();)
                        PrepareShader(*shaders[index]);
                });
        }
        for (auto& thread : threads)
            thread.join();

        for (auto& shader : shaders)
        {
            uint64_t hash = shader->hash;
            g_shaders.emplace(hash, std::move(shader));
        }
        LOGF_INFO("{} shaders prepared in {:.1f} s", g_shaders.size(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }

    static uint64_t HashWords(const uint32_t* words, size_t count)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < count; i++)
        {
            hash ^= words[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    // ---- Survey ----

    static std::filesystem::path g_surveyFolder;
    static uint64_t g_surveyFrame;

    struct FrameSurvey
    {
        uint32_t draws;
        uint32_t indices;
        std::map<std::string, uint32_t> drawKinds;
        std::map<std::string, uint32_t> targets;
        std::vector<std::string> copies;
        std::set<std::pair<uint64_t, uint64_t>> shaderPairs;
    };

    static FrameSurvey g_survey;

    void EnableSurvey(const std::filesystem::path& shaderFolder)
    {
        g_surveyFolder = shaderFolder;
        std::error_code ec;
        std::filesystem::create_directories(shaderFolder, ec);
    }

    static bool SurveyThisFrame()
    {
        return !g_surveyFolder.empty() && g_surveyFrame % 300 == 0;
    }

    static void SurveyDraw(const be<uint32_t>* registers, const DrawCall& draw)
    {
        auto& survey = g_survey;
        survey.draws++;
        survey.indices += draw.initiator.NumIndices();

        auto mode = Bits<EdramMode>(registers[RB_MODECONTROL], 0, 3);
        SurfaceInfo surface{ registers[RB_SURFACE_INFO] };
        ColorInfo color{ registers[RB_COLOR_INFO] };
        DepthInfo depth{ registers[RB_DEPTH_INFO] };

        survey.drawKinds[fmt::format("mode {} prim {} src {}", uint32_t(mode), uint32_t(draw.initiator.PrimType()),
            uint32_t(draw.initiator.Source()))]++;

        if (mode == EdramMode::Copy)
        {
            CopyControl control{ registers[RB_COPY_CONTROL] };
            CopyDestInfo dest{ registers[RB_COPY_DEST_INFO] };
            CopyDestPitch pitch{ registers[RB_COPY_DEST_PITCH] };
            uint32_t source = control.SourceSelect();
            uint32_t sourceInfo = source < 4 ? registers[source == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + source - 1].get()
                : registers[RB_DEPTH_INFO].get();

            survey.copies.push_back(fmt::format("src {} (base {} fmt {} pitch {} msaa {}) cmd {} clear {}{} -> 0x{:08X} {}x{} fmt {} endian {} | scissor {:08X} {:08X} offset {:08X}",
                source, sourceInfo & 0xFFF, (sourceInfo >> 16) & 0xF, surface.Pitch(), surface.MsaaSamples(),
                control.Command(), control.ColorClear() ? "c" : "", control.DepthClear() ? "d" : "",
                registers[RB_COPY_DEST_BASE].get(), pitch.Pitch(), pitch.Height(), dest.Format(), dest.Endian(),
                registers[PA_SC_WINDOW_SCISSOR_TL].get(), registers[PA_SC_WINDOW_SCISSOR_BR].get(), registers[PA_SC_WINDOW_OFFSET].get()));
            return;
        }

        uint32_t mask = registers[RB_COLOR_MASK];
        std::string key = fmt::format("pitch {} msaa {}", surface.Pitch(), surface.MsaaSamples());
        for (uint32_t i = 0; i < 4; i++)
        {
            if ((mask >> (i * 4)) & 0xF)
            {
                ColorInfo info{ registers[i == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + i - 1] };
                key += fmt::format(" c{}(base {} fmt {})", i, info.Base(), info.Format());
            }
        }

        uint32_t depthControl = registers[RB_DEPTHCONTROL];
        if (depthControl & 0x7) // Stencil, depth test or depth write enabled.
            key += fmt::format(" z(base {} fmt {})", depth.Base(), depth.Format());

        survey.targets[key]++;
        survey.shaderPairs.emplace(g_activeShaders[0] ? g_activeShaders[0]->hash : 0, g_activeShaders[1] ? g_activeShaders[1]->hash : 0);
    }

    static void SurveySwap(const SwapInfo& swap)
    {
        auto& survey = g_survey;
        LOGF_INFO("survey frame {}: {} draws, {} indices, {} shader pairs, {} shaders loaded so far; front buffer 0x{:08X} {}x{} fetch {:08X} {:08X} {:08X}",
            g_surveyFrame, survey.draws, survey.indices, survey.shaderPairs.size(), g_shaders.size(), swap.frontBuffer,
            swap.width, swap.height, swap.fetchConstant[0], swap.fetchConstant[1], swap.fetchConstant[2]);
        for (auto& [kind, count] : survey.drawKinds)
            LOGF_INFO("  draws {}: {}", kind, count);
        for (auto& [target, count] : survey.targets)
            LOGF_INFO("  target {}: {} draws", target, count);
        for (auto& copy : survey.copies)
            LOGF_INFO("  copy {}", copy);
    }

    // ---- Interface ----

    void LoadShader(uint32_t type, const be<uint32_t>* code, uint32_t sizeWords)
    {
        if (type > 1)
            return;

        std::vector<uint32_t> words(sizeWords);
        for (uint32_t i = 0; i < sizeWords; i++)
            words[i] = code[i];

        uint64_t hash = HashWords(words.data(), words.size()) ^ type;
        auto& shader = g_shaders[hash];
        if (!shader)
        {
            shader = std::make_unique<ShaderMicrocode>(ShaderMicrocode{ type, hash, std::move(words) });

            if (!g_microcodeFolder.empty())
            {
                auto path = g_microcodeFolder / fmt::format("{}_{:016X}.bin", type == 0 ? "vs" : "ps", hash);
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(shader->words.data()), shader->words.size() * sizeof(uint32_t));
            }

            if (!g_surveyFolder.empty())
            {
                auto path = g_surveyFolder / fmt::format("{}_{:016X}.bin", type == 0 ? "vs" : "ps", hash);
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(shader->words.data()), shader->words.size() * sizeof(uint32_t));
            }
        }

        g_activeShaders[type] = shader.get();
    }

    void IssueDraw(const be<uint32_t>* registers, const DrawCall& draw)
    {
        if (SurveyThisFrame())
            SurveyDraw(registers, draw);

        if (g_renderingDisabled)
            return;

        // Resolves don't run shaders.
        auto mode = Bits<EdramMode>(registers[RB_MODECONTROL], 0, 3);
        auto* vertex = g_activeShaders[0];
        auto* pixel = g_activeShaders[1];
        d3d12::HostShader vertexShader{}, pixelShader{};
        if (mode != EdramMode::Copy)
        {
            if (!vertex || !PrepareShaderTimed(*vertex))
                return;
            vertexShader = { vertex->shader.get(), &vertex->code, vertex->hash };
            if (pixel && PrepareShaderTimed(*pixel))
                pixelShader = { pixel->shader.get(), &pixel->code, pixel->hash };
        }

        d3d12::Draw(registers, draw, vertexShader, pixelShader.code ? &pixelShader : nullptr);
    }

    void Swap(const be<uint32_t>* registers, const SwapInfo& swap)
    {
        if (SurveyThisFrame())
            SurveySwap(swap);

        g_survey = {};
        g_surveyFrame++;

        // Debugging the intro movie: log when its frame buffer changes (AR_WATCH_MOVIE).
        if (AR_ENV("AR_WATCH_MOVIE") && g_surveyFrame % 10 == 0)
        {
            static uint64_t lastHash;
            static uint32_t changes;
            const uint8_t* frame = g_memory.Translate<uint8_t>(PhysicalToVirtual(0x11E61000));
            uint64_t hash = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < 1280 * 720 * 4; i += 64)
                hash = (hash ^ frame[i]) * 0x100000001B3ull;
            if (hash != lastHash)
            {
                lastHash = hash;
                if (++changes % 10 == 1)
                    LOGF_INFO("movie frame buffer changed ({} changes) at frame {}", changes, g_surveyFrame);
            }
        }

        if (!g_renderingDisabled)
            d3d12::Present(registers, swap);
    }
}
