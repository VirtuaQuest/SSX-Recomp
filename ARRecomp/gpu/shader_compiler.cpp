#include <stdafx.h>
#include "shader_compiler.h"
#include <wrl/client.h>
#include <dxcapi.h>

using Microsoft::WRL::ComPtr;

namespace gpu
{
    static std::filesystem::path g_cacheFolder;
    static ComPtr<IDxcCompiler3> g_compiler;
    static ComPtr<IDxcUtils> g_utils;

    void SetShaderCacheFolder(const std::filesystem::path& folder)
    {
        g_cacheFolder = folder;
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
    }

    static bool InitializeCompiler()
    {
        static bool initialized = [] {
            HMODULE module = LoadLibraryW(L"dxcompiler.dll");
            if (module == nullptr)
            {
                LOGF_ERROR("dxcompiler.dll was not found; shaders cannot be compiled");
                return false;
            }

            auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
            if (create == nullptr || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_compiler))) ||
                FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils))))
            {
                LOGF_ERROR("failed to create the DXC compiler");
                return false;
            }

            return true;
        }();

        return initialized;
    }

    static uint64_t HashString(std::string_view text, std::wstring_view target)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (char c : text)
            hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
        for (wchar_t c : target)
            hash = (hash ^ uint16_t(c)) * 0x100000001B3ull;
        return hash;
    }

    std::vector<uint8_t> CompileHlsl(const std::string& source, const wchar_t* target, const char* debugName)
    {
        std::filesystem::path cachePath;
        if (!g_cacheFolder.empty())
        {
            cachePath = g_cacheFolder / fmt::format("{:016X}.dxil", HashString(source, target));
            std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
            if (file)
            {
                std::vector<uint8_t> cached(size_t(file.tellg()));
                file.seekg(0);
                file.read(reinterpret_cast<char*>(cached.data()), cached.size());
                if (!cached.empty())
                    return cached;
            }
        }

        if (!InitializeCompiler())
            return {};

        DxcBuffer buffer{ source.data(), source.size(), DXC_CP_UTF8 };
        const wchar_t* arguments[] = { L"-T", target, L"-E", L"main", L"-O2", L"-Qstrip_debug", L"-Qstrip_reflect" };

        ComPtr<IDxcResult> result;
        if (FAILED(g_compiler->Compile(&buffer, arguments, UINT32(std::size(arguments)), nullptr, IID_PPV_ARGS(&result))))
        {
            LOGF_ERROR("DXC failed to run for {}", debugName);
            return {};
        }

        HRESULT status;
        result->GetStatus(&status);

        ComPtr<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (FAILED(status))
        {
            LOGF_ERROR("shader {} failed to compile:\n{}", debugName, errors && errors->GetStringLength() ? errors->GetStringPointer() : "");
            if (!g_cacheFolder.empty())
                std::ofstream(g_cacheFolder / fmt::format("failed_{}.hlsl", debugName)) << source;
            return {};
        }

        ComPtr<IDxcBlob> object;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
        std::vector<uint8_t> code(bytes, bytes + object->GetBufferSize());

        if (!cachePath.empty())
        {
            std::ofstream file(cachePath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(code.data()), code.size());
        }

        return code;
    }
}
