#include <stdafx.h>
#include "renderer.h"

namespace gpu
{
    using namespace reg;

    struct ShaderMicrocode
    {
        uint32_t type;
        uint64_t hash;
        std::vector<uint32_t> words; // Host byte order.
    };

    static std::unordered_map<uint64_t, std::unique_ptr<ShaderMicrocode>> g_shaders;
    static ShaderMicrocode* g_activeShaders[2];

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
    }

    void Swap(const be<uint32_t>* registers, const SwapInfo& swap)
    {
        if (SurveyThisFrame())
            SurveySwap(swap);

        g_survey = {};
        g_surveyFrame++;
    }
}
