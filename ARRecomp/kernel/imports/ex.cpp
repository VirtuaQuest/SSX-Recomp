// Executive, executable loader, configuration and other system services.
#include <stdafx.h>
#include <kernel/imports.h>
#include <kernel/xex_loader.h>
#include <xex.h>
#include <TinySHA1.hpp>

// Module handles for the system modules a title can look up. The executable's handle is its LDR entry.
static uint32_t g_xamModule;
static uint32_t g_kernelModule;

static void InitSystemModules()
{
    static std::once_flag once;
    std::call_once(once, []
        {
            g_xamModule = g_runtimeHeap.AllocGuest(0x80);
            g_kernelModule = g_runtimeHeap.AllocGuest(0x80);
        });
}

namespace imports
{
    uint32_t XexCheckExecutablePrivilege(uint32_t privilege)
    {
        // XEX_HEADER_SYSTEM_FLAGS holds the first 32 privilege bits.
        uint32_t field = FindXexHeaderField(g_module.xexHeader, XEX_HEADER_SYSTEM_FLAGS);
        if (field == 0 || privilege >= 32)
            return 0;

        return (GuestPtr<be<uint32_t>>(field)->get() >> privilege) & 1;
    }

    uint32_t XexGetModuleHandle(const char* name, be<uint32_t>* handle)
    {
        InitSystemModules();

        std::string module = name != nullptr ? name : "";
        std::transform(module.begin(), module.end(), module.begin(), ::tolower);

        if (module.empty() || module == "default.xex")
            *handle = g_module.ldrEntry;
        else if (module == "xam.xex")
            *handle = g_xamModule;
        else if (module == "xboxkrnl.exe")
            *handle = g_kernelModule;
        else
        {
            LOGF_WARNING("unknown module {}", module);
            *handle = 0;
            return X_STATUS_NO_SUCH_FILE;
        }

        return X_STATUS_SUCCESS;
    }

    uint32_t XexGetProcedureAddress(uint32_t module, uint32_t ordinal, be<uint32_t>* address)
    {
        InitSystemModules();

        uint32_t thunk = 0;
        if (module == g_xamModule)
            thunk = FindImportThunk(true, ordinal);
        else if (module == g_kernelModule)
            thunk = FindImportThunk(false, ordinal);

        *address = thunk;
        if (thunk == 0)
        {
            LOGF_WARNING("procedure {} of module 0x{:08X} is not available", ordinal, module);
            return X_STATUS_UNSUCCESSFUL;
        }

        return X_STATUS_SUCCESS;
    }

    uint32_t XexLoadImage(const char* path, uint32_t flags, uint32_t minimumVersion, be<uint32_t>* handle)
    {
        // System modules are always loaded; hand out their handles.
        std::string name = path != nullptr ? path : "";
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name == "xam.xex" || name == "xboxkrnl.exe")
            return XexGetModuleHandle(name.c_str(), handle);

        LOGF_WARNING("loading additional modules is not supported: {}", path != nullptr ? path : "(null)");
        if (handle != nullptr)
            *handle = 0;

        return X_STATUS_NO_SUCH_FILE;
    }

    uint32_t XexUnloadImage(uint32_t handle)
    {
        return X_STATUS_SUCCESS;
    }

    uint32_t ExGetXConfigSetting(uint16_t category, uint16_t setting, void* buffer, uint16_t bufferSize, be<uint16_t>* requiredSize)
    {
        // Based on Unleashed Recompiled's implementation. Values are big endian, most settings are 32 bit.
        uint32_t value = 0;
        uint16_t size = 4;

        switch (category)
        {
        case 0x0002: // XCONFIG_SECURED_CATEGORY
            switch (setting)
            {
            case 0x0002: // XCONFIG_SECURED_AV_REGION
                value = 0x00001000; // NTSC-U
                break;
            default:
                LOGF_WARNING("unknown setting {}:{}", category, setting);
                return X_STATUS_INVALID_PARAMETER;
            }
            break;

        case 0x0003: // XCONFIG_USER_CATEGORY
            switch (setting)
            {
            case 0x0001: // XCONFIG_USER_TIME_ZONE_BIAS
            case 0x0002: // XCONFIG_USER_TIME_ZONE_STD_NAME
            case 0x0003: // XCONFIG_USER_TIME_ZONE_DLT_NAME
            case 0x0004: // XCONFIG_USER_TIME_ZONE_STD_DATE
            case 0x0005: // XCONFIG_USER_TIME_ZONE_DLT_DATE
            case 0x0006: // XCONFIG_USER_TIME_ZONE_STD_BIAS
            case 0x0007: // XCONFIG_USER_TIME_ZONE_DLT_BIAS
                value = 0;
                break;
            case 0x0009: // XCONFIG_USER_LANGUAGE
                value = 1; // English
                break;
            case 0x000A: // XCONFIG_USER_VIDEO_FLAGS
                value = 0x00040000; // Widescreen
                break;
            case 0x000B: // XCONFIG_USER_AUDIO_FLAGS
                value = 0x00010001; // Dolby Digital off, stereo
                break;
            case 0x000C: // XCONFIG_USER_RETAIL_FLAGS
                value = 1;
                break;
            case 0x000E: // XCONFIG_USER_COUNTRY
                value = 103; // United States
                size = 1;
                break;
            default:
                LOGF_WARNING("unknown setting {}:{}", category, setting);
                return X_STATUS_INVALID_PARAMETER;
            }
            break;

        default:
            LOGF_WARNING("unknown setting {}:{}", category, setting);
            return X_STATUS_INVALID_PARAMETER;
        }

        if (requiredSize != nullptr)
            *requiredSize = size;

        if (buffer != nullptr)
        {
            if (bufferSize < size)
                return X_STATUS_BUFFER_TOO_SMALL;

            if (size == 1)
                *static_cast<uint8_t*>(buffer) = uint8_t(value);
            else
                *static_cast<be<uint32_t>*>(buffer) = value;
        }

        return X_STATUS_SUCCESS;
    }

    void ExRegisterTitleTerminateNotification(uint32_t registration, uint32_t create)
    {
        // Notifications would run when the title exits; the process simply ends instead.
    }

    void HalReturnToFirmware(uint32_t routine)
    {
        LOGF_INFO("title requested a return to the dashboard ({})", routine);
        std::exit(0);
    }

    // XeCryptSha hashes up to three buffers in sequence.
    void XeCryptSha(const uint8_t* input1, uint32_t size1, const uint8_t* input2, uint32_t size2,
        const uint8_t* input3, uint32_t size3, uint8_t* output, uint32_t outputSize)
    {
        sha1::SHA1 sha;
        if (input1 != nullptr) sha.processBytes(input1, size1);
        if (input2 != nullptr) sha.processBytes(input2, size2);
        if (input3 != nullptr) sha.processBytes(input3, size3);

        sha1::SHA1::digest8_t digest;
        sha.finalize(digest);
        if (output != nullptr)
            memcpy(output, digest, std::min<uint32_t>(outputSize, sizeof(digest)));

        if (AR_ENV("AR_LOG_SHA"))
        {
            std::string hex;
            for (auto byte : digest)
                hex += fmt::format("{:02X}", byte);
            LOGF_INFO("sha of 0x{:08X}+{}, 0x{:08X}+{}, 0x{:08X}+{}: {}", g_memory.MapVirtual(input1), size1,
                input2 ? g_memory.MapVirtual(input2) : 0, size2, input3 ? g_memory.MapVirtual(input3) : 0, size3, hex);
        }
    }

    // Structured exception filter used by __try blocks. Exceptions are not raised in recompiled code, so it is only
    // reached through unwinding, which is not supported; continue searching.
    uint32_t __C_specific_handler(uint32_t exceptionRecord, uint32_t establisherFrame, uint32_t context, uint32_t dispatcherContext)
    {
        LOG_UNIMPLEMENTED();
        return 1; // ExceptionContinueSearch
    }
}

AR_IMPORT(XexCheckExecutablePrivilege);
AR_IMPORT(XexGetModuleHandle);
AR_IMPORT(XexGetProcedureAddress);
AR_IMPORT(XexLoadImage);
AR_IMPORT(XexUnloadImage);
AR_IMPORT(ExGetXConfigSetting);
AR_IMPORT(ExRegisterTitleTerminateNotification);
AR_IMPORT(HalReturnToFirmware);
AR_IMPORT(XeCryptSha);
AR_IMPORT(__C_specific_handler);
