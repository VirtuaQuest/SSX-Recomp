#pragma once

#include "registers.h"

// Interface between the command processor and the host renderer. All calls are made on the command processor
// thread, in command stream order, with the register file as it is at that point of the stream.
namespace gpu
{
    struct DrawCall
    {
        reg::DrawInitiator initiator;
        uint32_t indexBase;     // Physical address, for DMA index sources.
        uint32_t indexWords;
        uint32_t indexEndian;
        std::vector<uint32_t> immediateIndices; // Packet words, for immediate index sources.
    };

    struct SwapInfo
    {
        uint32_t frontBuffer; // Physical address.
        uint32_t width;
        uint32_t height;
        uint32_t fetchConstant[6]; // Texture fetch constant describing the front buffer.
    };

    // Shader microcode loaded with IM_LOAD; type 0 is a vertex shader, 1 a pixel shader.
    void LoadShader(uint32_t type, const be<uint32_t>* code, uint32_t sizeWords);
    void IssueDraw(const be<uint32_t>* registers, const DrawCall& draw);
    void Swap(const be<uint32_t>* registers, const SwapInfo& swap);

    // Logs a summary of the GPU work of every 300th frame and saves the shaders to the folder (empty: off).
    void EnableSurvey(const std::filesystem::path& shaderFolder);
}
