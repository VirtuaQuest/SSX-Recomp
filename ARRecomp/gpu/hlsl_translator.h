#pragma once

#include <string>
#include "xenia/gpu/shader_translator.h"

// Translates Xenos shader microcode to HLSL (shader model 5.1) for the Direct3D 12 renderer.
//
// The microcode's control flow becomes a state machine: every label (jump, call and loop target) starts a case of
// a switch on a program counter, inside a loop that runs until the shader ends. Registers, the address register,
// the loop registers and the predicate are local variables. All Xenos state the shader reads comes from these
// resources, laid out by the renderer (see HLSL_PRELUDE in hlsl_translator.cpp):
//
//   b0          XeSystemConstants: boolean, loop and fetch constants, and host values (viewport transform,
//               texture and sampler descriptor indices, vertex data offsets, alpha test, render target state)
//   b1          XeFloatConstants: the 256 float constants of the shader's stage
//   t0 space3   the vertex data the draw uses, copied from guest memory (raw buffer)
//   t0 space0-2 every texture in the descriptor heap, as 2D, 3D and cube arrays
//   s0          every sampler in the sampler heap
//
// The vertex shader's outputs are its position and 16 interpolators; the pixel shader reads the same 16.
namespace gpu
{
    class HlslShaderTranslator : public xe::gpu::ShaderTranslator
    {
    public:
        std::string TakeSource() { return std::move(m_out); }

    protected:
        void Reset() override;
        void StartTranslation() override;
        std::vector<uint8_t> CompleteTranslation() override;

        void ProcessLabel(uint32_t cfIndex) override;
        void ProcessExecInstructionBegin(const xe::gpu::ParsedExecInstruction& instr) override;
        void ProcessExecInstructionEnd(const xe::gpu::ParsedExecInstruction& instr) override;
        void ProcessLoopStartInstruction(const xe::gpu::ParsedLoopStartInstruction& instr) override;
        void ProcessLoopEndInstruction(const xe::gpu::ParsedLoopEndInstruction& instr) override;
        void ProcessCallInstruction(const xe::gpu::ParsedCallInstruction& instr) override;
        void ProcessReturnInstruction(const xe::gpu::ParsedReturnInstruction& instr) override;
        void ProcessJumpInstruction(const xe::gpu::ParsedJumpInstruction& instr) override;
        void ProcessVertexFetchInstruction(const xe::gpu::ParsedVertexFetchInstruction& instr) override;
        void ProcessTextureFetchInstruction(const xe::gpu::ParsedTextureFetchInstruction& instr) override;
        void ProcessAluInstruction(const xe::gpu::ParsedAluInstruction& instr, uint8_t memexportWrittenBefore) override;

    private:
        template<typename... Args>
        void Line(fmt::format_string<Args...> format, Args&&... args);

        std::string Operand(const xe::gpu::InstructionOperand& operand);
        std::string OperandComponent(const xe::gpu::InstructionOperand& operand, uint32_t component);
        std::string ResultTarget(const xe::gpu::InstructionResult& result);
        void StoreResult(const xe::gpu::InstructionResult& result, const std::string& value);
        void BeginPredicate(bool predicated, bool condition);
        void EndPredicate(bool predicated);
        void OpenCase(uint32_t cfIndex);
        void JumpTo(uint32_t cfIndex);
        static std::string BoolConstant(uint32_t index);

        std::string m_body;
        std::string m_out;
        int m_indent = 0;
        bool m_caseOpen = false;
        uint32_t m_openCase = 0;
        bool m_execConditional = false;
    };

    // Translates analyzed microcode. Returns an empty string if the shader uses something not supported.
    std::string TranslateShaderToHlsl(xe::gpu::Shader& shader);
}
