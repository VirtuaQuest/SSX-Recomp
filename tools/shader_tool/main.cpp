// Development tool for the shader translator: disassembles Xenos shader microcode saved by --gpu-survey
// (host byte order words, vs_*.bin / ps_*.bin) and prints what the analysis found.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fmt/format.h>
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/shader.h"
#include <gpu/hlsl_translator.h>

using namespace xe::gpu;

static std::vector<uint32_t> ReadWords(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    std::vector<uint32_t> words(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), words.size() * 4);
    return words;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fmt::print(stderr, "Usage: ARShaderTool <shader.bin>...\n");
        return 1;
    }

    xe::StringBuffer buffer;
    std::filesystem::path hlslFolder;
    for (int i = 1; i < argc; i++)
    {
        if (std::string_view(argv[i]) == "--hlsl" && i + 1 < argc)
        {
            hlslFolder = argv[++i];
            std::filesystem::create_directories(hlslFolder);
            continue;
        }

        std::filesystem::path path = argv[i];
        auto words = ReadWords(path);
        bool pixel = path.filename().string().starts_with("ps");
        Shader shader(pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0, words.data(), words.size(),
            std::endian::little);
        shader.AnalyzeUcode(buffer);

        fmt::print("==== {} ({} words)\n", path.filename().string(), words.size());
        fmt::print("vertex fetches {}, texture fetches {}, memexport {:#x}, color targets {:#x}, kills {}, writes depth {}\n",
            shader.vertex_bindings().size(), shader.texture_bindings().size(), shader.memexport_eM_written(),
            shader.writes_color_targets(), shader.kills_pixels(), shader.writes_depth());
        fmt::print("{}\n", shader.ucode_disassembly());

        // With --hlsl <folder>, the translation is written there for compiling with dxc.
        if (!hlslFolder.empty())
        {
            std::string hlsl = gpu::TranslateShaderToHlsl(shader);
            for (auto& error : shader.GetOrCreateTranslation(0)->errors())
                fmt::print("translation {}: {}\n", error.is_fatal ? "error" : "warning", error.message);
            std::ofstream(hlslFolder / path.filename().replace_extension(".hlsl")) << hlsl;
        }
    }

    return 0;
}
