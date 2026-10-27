// Video driver: GPU setup, display information and presentation.
#include <stdafx.h>
#include <kernel/imports.h>
#include <gpu/command_processor.h>

static uint32_t MakePacketType3(uint32_t opcode, uint32_t count)
{
    return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8);
}

namespace imports
{
    uint32_t VdInitializeEngines(uint32_t unknown, uint32_t callback, uint32_t argument, uint32_t pfpMicrocode, uint32_t meMicrocode)
    {
        return 1;
    }

    void VdShutdownEngines()
    {
    }

    void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
    {
        gpu::SetInterruptCallback(callback, userData);
    }

    void VdInitializeRingBuffer(uint32_t address, uint32_t sizeLog2)
    {
        gpu::InitializeRingBuffer(address, sizeLog2);
    }

    void VdEnableRingBufferRPtrWriteBack(uint32_t address, uint32_t blockSizeLog2)
    {
        gpu::EnableReadPointerWriteBack(address, blockSizeLog2);
    }

    void VdGetSystemCommandBuffer(be<uint32_t>* buffer, be<uint32_t>* unknown)
    {
        memset(buffer, 0, 0x94);
        *buffer = 0xBEEF0000;
        *unknown = 0xBEEF0001;
    }

    void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t address)
    {
    }

    void VdQueryVideoMode(XVIDEO_MODE* mode)
    {
        // Based on Unleashed Recompiled's implementation.
        memset(mode, 0, sizeof(*mode));
        mode->DisplayWidth = 1280;
        mode->DisplayHeight = 720;
        mode->IsInterlaced = false;
        mode->IsWidescreen = true;
        mode->IsHighDefinition = true;
        mode->RefreshRate = 0x42700000; // 60.0f
        mode->VideoStandard = 1;
        mode->Unknown4A = 0x4A;
        mode->Unknown01 = 0x01;
    }

    uint32_t VdQueryVideoFlags()
    {
        return 0x7; // Widescreen, 720p, 1080i capable.
    }

    // X_DISPLAY_INFO: front buffer size and formats, scaler parameters, display size and refresh rate.
    void VdGetCurrentDisplayInformation(uint8_t* info)
    {
        memset(info, 0, 0x58);
        *reinterpret_cast<be<uint16_t>*>(info + 0x00) = 1280;
        *reinterpret_cast<be<uint16_t>*>(info + 0x02) = 720;
        info[0x04] = 6;    // Color format.
        info[0x05] = 0x1A; // Pixel format.
        *reinterpret_cast<be<uint16_t>*>(info + 0x50) = 1280;
        *reinterpret_cast<be<uint16_t>*>(info + 0x52) = 720;
        *reinterpret_cast<be<uint16_t>*>(info + 0x56) = 60;
    }

    void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
    {
        *type = 2;
        *power = 2.22222233f;
    }

    uint32_t VdSetDisplayMode(uint32_t mode)
    {
        return 0;
    }

    uint32_t VdSetDisplayModeOverride(uint32_t unknown0, uint32_t unknown1, uint32_t unknown2, uint32_t unknown3, uint32_t unknown4)
    {
        return 0;
    }

    uint32_t VdPersistDisplay(uint32_t unknown, be<uint32_t>* result)
    {
        if (result != nullptr)
            *result = 0;

        return 1;
    }

    uint32_t VdRetrainEDRAMWorker(uint32_t unknown)
    {
        return 0;
    }

    uint32_t VdRetrainEDRAM(uint32_t unknown0, uint32_t unknown1, uint32_t unknown2, uint32_t unknown3, uint32_t unknown4, uint32_t unknown5)
    {
        return 0;
    }

    uint32_t VdIsHSIOTrainingSucceeded()
    {
        return 1;
    }

    void VdEnableDisableClockGating(uint32_t enabled)
    {
    }

    uint32_t VdCallGraphicsNotificationRoutines(uint32_t unknown, uint32_t arguments)
    {
        return 0;
    }

    // Writes the display scaler's commands into the buffer passed last (11 arguments, the last three on the stack)
    // and returns how many words it wrote; the title copies that many words into its command stream. The scaler
    // only affects video output, so no commands are produced.
    uint32_t VdInitializeScalerCommandBuffer(uint32_t sourceXy, uint32_t sourceWh, uint32_t outputXy, uint32_t outputWh,
        uint32_t unknown0, uint32_t unknown1, uint32_t unknown2, uint32_t unknown3, uint32_t unknown4, uint32_t unknown5, uint32_t buffer)
    {
        return 0;
    }

    // The title reserves 64 words in its command stream for the swap; fill them with a swap packet.
    void VdSwap(be<uint32_t>* buffer, uint32_t fetchConstant, uint32_t unknown2, uint32_t unknown3, uint32_t unknown4,
        uint32_t unknown5, be<uint32_t>* frontBuffer, be<uint32_t>* textureFormat, be<uint32_t>* colorSpace, uint32_t unknown9)
    {
        buffer[0] = MakePacketType3(0x64, 63);
        buffer[1] = 0x53574150; // 'SWAP'
        buffer[2] = frontBuffer != nullptr ? frontBuffer->get() & 0x1FFFFFFF : 0;
        buffer[3] = 1280;
        buffer[4] = 720;
        for (uint32_t i = 5; i < 64; i++)
            buffer[i] = 0;

        // The front buffer's texture fetch constant tells the renderer its format and layout.
        if (fetchConstant != 0)
        {
            auto* words = g_memory.Translate<be<uint32_t>>(fetchConstant);
            for (uint32_t i = 0; i < 6; i++)
                buffer[5 + i] = words[i];
        }
    }
}

AR_IMPORT(VdInitializeEngines);
AR_IMPORT(VdShutdownEngines);
AR_IMPORT(VdSetGraphicsInterruptCallback);
AR_IMPORT(VdInitializeRingBuffer);
AR_IMPORT(VdEnableRingBufferRPtrWriteBack);
AR_IMPORT(VdGetSystemCommandBuffer);
AR_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress);
AR_IMPORT(VdQueryVideoMode);
AR_IMPORT(VdQueryVideoFlags);
AR_IMPORT(VdGetCurrentDisplayInformation);
AR_IMPORT(VdGetCurrentDisplayGamma);
AR_IMPORT(VdSetDisplayMode);
AR_IMPORT(VdSetDisplayModeOverride);
AR_IMPORT(VdPersistDisplay);
AR_IMPORT(VdRetrainEDRAMWorker);
AR_IMPORT(VdRetrainEDRAM);
AR_IMPORT(VdIsHSIOTrainingSucceeded);
AR_IMPORT(VdEnableDisableClockGating);
AR_IMPORT(VdCallGraphicsNotificationRoutines);
AR_IMPORT(VdInitializeScalerCommandBuffer);
AR_IMPORT(VdSwap);
