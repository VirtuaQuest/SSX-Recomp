#include <cstdarg>
#include <fmt/format.h>
#include "hlsl_translator.h"

using namespace xe::gpu;
using namespace xe::gpu::ucode;

namespace gpu
{
    // Declarations and helpers shared by all translated shaders. The constant buffer layout must match
    // SystemConstants in shader_constants.h.
    static constexpr const char* HLSL_PRELUDE = R"(
cbuffer XeSystemConstants : register(b0)
{
    uint4 xe_bool_constants[2];
    uint4 xe_loop_constants[8];
    uint4 xe_fetch_constants[48];
    float4 xe_ndc_scale;
    float4 xe_ndc_offset;
    uint xe_flags;
    int xe_vertex_index_offset;
    int xe_param_gen_register;
    float xe_alpha_ref;
    uint xe_alpha_func;
    uint xe_gamma_targets;
    float2 xe_padding;
    float4 xe_color_exp_bias;
    uint4 xe_textures[32];          // Descriptor index, sampler index, swizzle, sign/gamma flags.
    int4 xe_vertex_data_offsets[24]; // Byte offset from a guest address to the draw's vertex data.
};

cbuffer XeFloatConstants : register(b1)
{
    float4 xe_c[256];
};

ByteAddressBuffer xe_vertex_data : register(t0, space3);
Texture2D<float4> xe_textures_2d[] : register(t0, space0);
Texture3D<float4> xe_textures_3d[] : register(t0, space1);
TextureCube<float4> xe_textures_cube[] : register(t0, space2);
SamplerState xe_samplers[] : register(s0, space0);

static const uint XE_FLAG_XY_NOT_DIVIDED = 1u;
static const uint XE_FLAG_Z_NOT_DIVIDED = 2u;
static const uint XE_FLAG_W_RECIPROCAL = 4u;

// Shader model 3 multiplication: zero times anything (including infinity and NaN) is zero.
float4 xe_mul(float4 a, float4 b) { return select(or(a == 0.0f, b == 0.0f), 0.0f, a * b); }
float xe_mul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float xe_dp(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y + m.z + m.w; }

bool xe_bool(uint index)
{
    return ((xe_bool_constants[index >> 7u][(index >> 5u) & 3u] >> (index & 31u)) & 1u) != 0u;
}

uint xe_loop(uint index)
{
    return xe_loop_constants[index >> 2u][index & 3u];
}

uint xe_fetch(uint word)
{
    return xe_fetch_constants[word >> 2u][word & 3u];
}

uint xe_swap(uint value, uint endian)
{
    if (endian == 1u)
        value = ((value & 0x00FF00FFu) << 8u) | ((value >> 8u) & 0x00FF00FFu);
    else if (endian == 2u)
        value = (value << 24u) | ((value & 0xFF00u) << 8u) | ((value >> 8u) & 0xFF00u) | (value >> 24u);
    else if (endian == 3u)
        value = (value << 16u) | (value >> 16u);
    return value;
}

// Loads the words of a vertex element: the fetch constant gives the buffer's guest address and byte order.
uint4 xe_vertex_load(uint fetchConstant, int elementDwords, uint wordCount)
{
    uint word0 = xe_fetch(fetchConstant * 2u);
    uint endian = xe_fetch(fetchConstant * 2u + 1u) & 3u;
    int address = int(word0 & 0xFFFFFFFCu) + elementDwords * 4 + xe_vertex_data_offsets[fetchConstant >> 2u][fetchConstant & 3u];
    uint4 words = uint4(0u, 0u, 0u, 0u);
    words.x = xe_swap(xe_vertex_data.Load(address), endian);
    if (wordCount > 1u) words.y = xe_swap(xe_vertex_data.Load(address + 4), endian);
    if (wordCount > 2u) words.z = xe_swap(xe_vertex_data.Load(address + 8), endian);
    if (wordCount > 3u) words.w = xe_swap(xe_vertex_data.Load(address + 12), endian);
    return words;
}

// Unpacks packed integer components; bits holds the widths, offsets the positions.
float4 xe_unpack(uint word, uint4 bits, uint4 offsets, bool isSigned, bool isInteger, bool noZero)
{
    float4 result;
    [unroll] for (uint i = 0u; i < 4u; i++)
    {
        if (bits[i] == 0u) { result[i] = i == 3u ? 1.0f : 0.0f; continue; }
        uint mask = bits[i] >= 32u ? 0xFFFFFFFFu : ((1u << bits[i]) - 1u);
        uint value = (word >> offsets[i]) & mask;
        if (isSigned)
        {
            int signedValue = int(value << (32u - bits[i])) >> (32u - bits[i]);
            float maxValue = float((1u << (bits[i] - 1u)) - 1u);
            if (isInteger) result[i] = float(signedValue);
            else if (noZero) result[i] = (float(signedValue) * 2.0f + 1.0f) / (maxValue * 2.0f + 1.0f);
            else result[i] = max(float(signedValue) / maxValue, -1.0f);
        }
        else
        {
            result[i] = isInteger ? float(value) : float(value) / float(mask);
        }
    }
    return result;
}

float4 xe_swizzle(float4 value, uint swizzle)
{
    float4 result;
    [unroll] for (uint i = 0u; i < 4u; i++)
    {
        uint source = (swizzle >> (i * 3u)) & 7u;
        result[i] = source < 4u ? value[source] : (source == 5u ? 1.0f : 0.0f);
    }
    return result;
}

float xe_gamma_to_linear(float g)
{
    // Xenos piecewise linear gamma, inverted.
    g = saturate(g) * 1023.0f;
    float l;
    if (g < 256.0f) l = g * 0.25f;
    else if (g < 384.0f) l = (g - 128.0f) * 0.5f;
    else if (g < 768.0f) l = g - 256.0f;
    else l = (g - 512.0f) * 2.0f;
    return l / 1023.0f;
}

float xe_linear_to_gamma(float l)
{
    l = saturate(l) * 1023.0f;
    float g;
    if (l < 64.0f) g = l * 4.0f;
    else if (l < 128.0f) g = l * 2.0f + 128.0f;
    else if (l < 512.0f) g = l + 256.0f;
    else g = l * 0.5f + 512.0f;
    return g / 1023.0f;
}

// Applies the fetch constant's per component sign (signed or gamma) and swizzle to a sampled value.
float4 xe_texture_result(float4 value, uint4 info)
{
    [unroll] for (uint i = 0u; i < 4u; i++)
    {
        if ((info.w >> i) & 1u)
        {
            float s = value[i] * 255.0f;
            s = s >= 127.5f ? s - 256.0f : s;
            value[i] = max(s / 127.0f, -1.0f);
        }
        else if ((info.w >> (i + 4u)) & 1u)
        {
            value[i] = xe_gamma_to_linear(value[i]);
        }
    }
    return xe_swizzle(value, info.z);
}

// Xenos cube coordinates (face s and t in 1-2 and the face index, from the cube instruction) to a direction.
float3 xe_cube_direction(float3 c)
{
    float2 st = (c.xy - 1.5f) * 2.0f;
    uint face = uint(c.z);
    if (face == 0u) return float3(1.0f, -st.y, -st.x);
    if (face == 1u) return float3(-1.0f, -st.y, st.x);
    if (face == 2u) return float3(st.x, 1.0f, st.y);
    if (face == 3u) return float3(st.x, -1.0f, -st.y);
    if (face == 4u) return float3(st.x, -st.y, 1.0f);
    return float3(-st.x, -st.y, -1.0f);
}

float4 xe_cube(float4 a, float4 b)
{
    // Inputs are source.zzxy and source.yxzz.
    float x = b.y, y = b.x, z = a.x;
    float4 result;
    if (abs(z) >= abs(x) && abs(z) >= abs(y))
        result = float4(-y, z < 0.0f ? -x : x, 2.0f * z, z < 0.0f ? 5.0f : 4.0f);
    else if (abs(y) >= abs(x))
        result = float4(y < 0.0f ? -z : z, x, 2.0f * y, y < 0.0f ? 3.0f : 2.0f);
    else
        result = float4(-y, x < 0.0f ? z : -z, 2.0f * x, x < 0.0f ? 1.0f : 0.0f);
    return result;
}
)";

    static constexpr char COMPONENTS[] = { 'x', 'y', 'z', 'w' };

    template<typename... Args>
    void HlslShaderTranslator::Line(fmt::format_string<Args...> format, Args&&... args)
    {
        m_body.append(size_t(m_indent) * 4, ' ');
        m_body += fmt::format(format, std::forward<Args>(args)...);
        m_body += '\n';
    }

    void HlslShaderTranslator::Reset()
    {
        ShaderTranslator::Reset();
        m_body.clear();
        m_out.clear();
        m_indent = 0;
        m_caseOpen = false;
        m_execConditional = false;
    }

    void HlslShaderTranslator::StartTranslation()
    {
        m_indent = 2;
        OpenCase(0);
    }

    std::string HlslShaderTranslator::BoolConstant(uint32_t index)
    {
        return fmt::format("xe_bool({}u)", index);
    }

    void HlslShaderTranslator::OpenCase(uint32_t cfIndex)
    {
        m_indent = 2;
        Line("case {}u:", cfIndex);
        Line("{{");
        m_indent = 3;
        m_caseOpen = true;
        m_openCase = cfIndex;
    }

    void HlslShaderTranslator::JumpTo(uint32_t cfIndex)
    {
        Line("xe_pc = {}u;", cfIndex);
        Line("break;");
    }

    void HlslShaderTranslator::ProcessLabel(uint32_t cfIndex)
    {
        // A case for the instruction after a loop start, call or jump may already be open.
        if (m_caseOpen && m_openCase == cfIndex)
            return;

        // Cases cannot fall through, so the previous one continues at the label explicitly.
        if (m_caseOpen)
        {
            JumpTo(cfIndex);
            m_indent = 2;
            Line("}}");
        }

        OpenCase(cfIndex);
    }

    std::string HlslShaderTranslator::Operand(const InstructionOperand& operand)
    {
        std::string index = std::to_string(operand.storage_index);
        if (operand.storage_addressing_mode == InstructionStorageAddressingMode::kAddressRegisterRelative)
            index = fmt::format("{} + xe_a0", operand.storage_index);
        else if (operand.storage_addressing_mode == InstructionStorageAddressingMode::kLoopRelative)
            index = fmt::format("{} + xe_aL", operand.storage_index);

        std::string value;
        switch (operand.storage_source)
        {
        case InstructionStorageSource::kRegister:
            value = operand.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute
                ? fmt::format("xe_r[{}]", index) : fmt::format("xe_r[clamp({}, 0, XE_REGISTER_COUNT - 1)]", index);
            break;
        case InstructionStorageSource::kConstantFloat:
            value = fmt::format("xe_c[({}) & 255]", index);
            break;
        default:
            value = "float4(0.0f, 0.0f, 0.0f, 0.0f)";
            break;
        }

        std::string swizzle = ".";
        for (uint32_t i = 0; i < 4; i++)
        {
            SwizzleSource component = operand.GetComponent(i);
            swizzle += component <= SwizzleSource::kW ? COMPONENTS[uint32_t(component)] : 'x';
        }

        // Swizzles to constants (used only by fetch results) are not valid here; they read x.
        value += swizzle;
        if (operand.is_absolute_value)
            value = fmt::format("abs({})", value);
        if (operand.is_negated)
            value = fmt::format("(-{})", value);
        return value;
    }

    std::string HlslShaderTranslator::OperandComponent(const InstructionOperand& operand, uint32_t component)
    {
        return fmt::format("{}.{}", Operand(operand), COMPONENTS[component]);
    }

    std::string HlslShaderTranslator::ResultTarget(const InstructionResult& result)
    {
        std::string index = std::to_string(result.storage_index);
        if (result.storage_addressing_mode == InstructionStorageAddressingMode::kAddressRegisterRelative)
            index = fmt::format("clamp({} + xe_a0, 0, XE_REGISTER_COUNT - 1)", result.storage_index);
        else if (result.storage_addressing_mode == InstructionStorageAddressingMode::kLoopRelative)
            index = fmt::format("clamp({} + xe_aL, 0, XE_REGISTER_COUNT - 1)", result.storage_index);

        switch (result.storage_target)
        {
        case InstructionStorageTarget::kRegister: return fmt::format("xe_r[{}]", index);
        case InstructionStorageTarget::kInterpolator: return fmt::format("xe_o[{}]", result.storage_index & 15);
        case InstructionStorageTarget::kPosition: return "xe_position";
        case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex: return "xe_point_size";
        case InstructionStorageTarget::kColor: return fmt::format("xe_color[{}]", result.storage_index & 3);
        case InstructionStorageTarget::kDepth: return "xe_depth";
        default: return {};
        }
    }

    // Writes a float4 value through the result's write mask; components can also be set to 0 or 1.
    void HlslShaderTranslator::StoreResult(const InstructionResult& result, const std::string& value)
    {
        uint32_t mask = result.GetUsedWriteMask();
        std::string target = ResultTarget(result);
        if (mask == 0 || target.empty())
            return;

        Line("xe_value = {};", value);
        if (result.is_clamped)
            Line("xe_value = saturate(xe_value);");

        if (result.storage_target == InstructionStorageTarget::kDepth)
        {
            Line("xe_depth = xe_value.x;");
            return;
        }

        for (uint32_t i = 0; i < 4; i++)
        {
            if (!(mask & (1u << i)))
                continue;

            SwizzleSource component = result.components[i];
            if (component <= SwizzleSource::kW)
                Line("{}.{} = xe_value.{};", target, COMPONENTS[i], COMPONENTS[uint32_t(component)]);
            else
                Line("{}.{} = {};", target, COMPONENTS[i], component == SwizzleSource::k1 ? "1.0f" : "0.0f");
        }
    }

    void HlslShaderTranslator::BeginPredicate(bool predicated, bool condition)
    {
        if (!predicated)
            return;

        Line("if ({}xe_p0)", condition ? "" : "!");
        Line("{{");
        m_indent++;
    }

    void HlslShaderTranslator::EndPredicate(bool predicated)
    {
        if (!predicated)
            return;

        m_indent--;
        Line("}}");
    }

    void HlslShaderTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr)
    {
        Line("// {} {} at {}", instr.opcode_name, instr.instruction_count, instr.instruction_address);
        m_execConditional = instr.type != ParsedExecInstruction::Type::kUnconditional;
        if (instr.type == ParsedExecInstruction::Type::kConditional)
            Line("if ({}{})", instr.condition ? "" : "!", BoolConstant(instr.bool_constant_index));
        else if (instr.type == ParsedExecInstruction::Type::kPredicated)
            Line("if ({}xe_p0)", instr.condition ? "" : "!");

        if (m_execConditional)
        {
            Line("{{");
            m_indent++;
        }
    }

    void HlslShaderTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr)
    {
        if (m_execConditional)
        {
            m_indent--;
            Line("}}");
            m_execConditional = false;
        }

        if (instr.is_end)
            JumpTo(0xFFFF);
    }

    void HlslShaderTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr)
    {
        // The loop counter and aL are pushed; a loop with a count of zero is skipped.
        Line("// loop {} start", instr.loop_constant_index);
        Line("xe_loop_depth = min(xe_loop_depth + 1, 3);");
        Line("xe_loop_count[xe_loop_depth] = xe_loop({}u) & 0xFFu;", instr.loop_constant_index);
        Line("xe_loop_aL[xe_loop_depth] = xe_aL;");
        if (!instr.is_repeat)
            Line("xe_aL = int((xe_loop({}u) >> 8u) & 0xFFu);", instr.loop_constant_index);
        Line("if (xe_loop_count[xe_loop_depth] == 0u)");
        Line("{{");
        m_indent++;
        Line("xe_aL = xe_loop_aL[xe_loop_depth];");
        Line("xe_loop_depth = max(xe_loop_depth - 1, 0);");
        JumpTo(instr.loop_skip_address);
        m_indent--;
        Line("}}");
        JumpTo(instr.dword_index + 1);
        m_indent = 2;
        Line("}}");
        OpenCase(instr.dword_index + 1);
    }

    void HlslShaderTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr)
    {
        Line("// loop {} end", instr.loop_constant_index);
        Line("xe_loop_count[xe_loop_depth] -= 1u;");
        Line("xe_aL += int(xe_loop({}u) << 8u) >> 24;", instr.loop_constant_index);
        if (instr.is_predicated_break)
            Line("if (xe_loop_count[xe_loop_depth] != 0u && xe_p0 != {})", instr.predicate_condition ? "true" : "false");
        else
            Line("if (xe_loop_count[xe_loop_depth] != 0u)");
        Line("{{");
        m_indent++;
        JumpTo(instr.loop_body_address);
        m_indent--;
        Line("}}");
        Line("xe_aL = xe_loop_aL[xe_loop_depth];");
        Line("xe_loop_depth = max(xe_loop_depth - 1, 0);");
    }

    void HlslShaderTranslator::ProcessCallInstruction(const ParsedCallInstruction& instr)
    {
        std::string condition = "true";
        if (instr.type == ParsedCallInstruction::Type::kConditional)
            condition = fmt::format("{}{}", instr.condition ? "" : "!", BoolConstant(instr.bool_constant_index));
        else if (instr.type == ParsedCallInstruction::Type::kPredicated)
            condition = fmt::format("{}xe_p0", instr.condition ? "" : "!");

        Line("if ({})", condition);
        Line("{{");
        m_indent++;
        Line("xe_call_depth = min(xe_call_depth + 1, 3);");
        Line("xe_call_return[xe_call_depth] = {}u;", instr.dword_index + 1);
        JumpTo(instr.target_address);
        m_indent--;
        Line("}}");
        JumpTo(instr.dword_index + 1);
        m_indent = 2;
        Line("}}");
        OpenCase(instr.dword_index + 1);
    }

    void HlslShaderTranslator::ProcessReturnInstruction(const ParsedReturnInstruction& instr)
    {
        Line("xe_pc = xe_call_return[xe_call_depth];");
        Line("xe_call_depth = max(xe_call_depth - 1, 0);");
        Line("break;");
        m_indent = 2;
        Line("}}");
        OpenCase(instr.dword_index + 1);
    }

    void HlslShaderTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr)
    {
        if (instr.type == ParsedJumpInstruction::Type::kUnconditional)
        {
            JumpTo(instr.target_address);
            m_indent = 2;
            Line("}}");
            OpenCase(instr.dword_index + 1);
            return;
        }

        if (instr.type == ParsedJumpInstruction::Type::kConditional)
            Line("if ({}{})", instr.condition ? "" : "!", BoolConstant(instr.bool_constant_index));
        else
            Line("if ({}xe_p0)", instr.condition ? "" : "!");
        Line("{{");
        m_indent++;
        JumpTo(instr.target_address);
        m_indent--;
        Line("}}");
    }

    struct VertexFormatInfo
    {
        uint32_t words;
        bool isFloat;
        uint32_t bits[4];
        uint32_t offsets[4];
    };

    static bool GetVertexFormatInfo(xenos::VertexFormat format, VertexFormatInfo& info)
    {
        switch (format)
        {
        case xenos::VertexFormat::k_8_8_8_8: info = { 1, false, { 8, 8, 8, 8 }, { 0, 8, 16, 24 } }; return true;
        case xenos::VertexFormat::k_2_10_10_10: info = { 1, false, { 10, 10, 10, 2 }, { 0, 10, 20, 30 } }; return true;
        case xenos::VertexFormat::k_10_11_11: info = { 1, false, { 11, 11, 10, 0 }, { 0, 11, 22, 0 } }; return true;
        case xenos::VertexFormat::k_11_11_10: info = { 1, false, { 10, 11, 11, 0 }, { 0, 10, 21, 0 } }; return true;
        case xenos::VertexFormat::k_16_16: info = { 1, false, { 16, 16, 0, 0 }, { 0, 16, 0, 0 } }; return true;
        case xenos::VertexFormat::k_16_16_16_16: info = { 2, false, { 16, 16, 16, 16 }, { 0, 16, 0, 16 } }; return true;
        case xenos::VertexFormat::k_16_16_FLOAT: info = { 1, true }; return true;
        case xenos::VertexFormat::k_16_16_16_16_FLOAT: info = { 2, true }; return true;
        case xenos::VertexFormat::k_32: info = { 1, false, { 32, 0, 0, 0 } }; return true;
        case xenos::VertexFormat::k_32_32: info = { 2, false, { 32, 32, 0, 0 } }; return true;
        case xenos::VertexFormat::k_32_32_32_32: info = { 4, false, { 32, 32, 32, 32 } }; return true;
        case xenos::VertexFormat::k_32_FLOAT: info = { 1, true }; return true;
        case xenos::VertexFormat::k_32_32_FLOAT: info = { 2, true }; return true;
        case xenos::VertexFormat::k_32_32_32_FLOAT: info = { 3, true }; return true;
        case xenos::VertexFormat::k_32_32_32_32_FLOAT: info = { 4, true }; return true;
        default: return false;
        }
    }

    void HlslShaderTranslator::ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& instr)
    {
        if (instr.result.GetUsedWriteMask() == 0)
            return;

        const auto& attributes = instr.attributes;
        VertexFormatInfo format;
        if (!GetVertexFormatInfo(attributes.data_format, format))
        {
            EmitTranslationError(fmt::format("unsupported vertex format {}", uint32_t(attributes.data_format)).c_str(), false);
            return;
        }

        uint32_t fetchConstant = instr.operands[1].storage_index;
        BeginPredicate(instr.is_predicated, instr.predicate_condition);
        Line("// vfetch vf{}, format {}, stride {}, offset {}", fetchConstant, uint32_t(attributes.data_format),
            attributes.stride, attributes.offset);

        std::string index = OperandComponent(instr.operands[0], 0);
        Line("xe_index = int({}({}));", attributes.is_index_rounded ? "round" : "floor", index);
        Line("xe_words = xe_vertex_load({}u, xe_index * {} + {}, {}u);", fetchConstant, attributes.stride, attributes.offset, format.words);

        // Components missing from the format read as 0, except w, which reads as 1.
        std::string value;
        if (format.isFloat)
        {
            switch (attributes.data_format)
            {
            case xenos::VertexFormat::k_16_16_FLOAT:
                value = "float4(f16tof32(xe_words.x), f16tof32(xe_words.x >> 16u), 0.0f, 1.0f)"; break;
            case xenos::VertexFormat::k_16_16_16_16_FLOAT:
                value = "float4(f16tof32(xe_words.x), f16tof32(xe_words.x >> 16u), f16tof32(xe_words.y), f16tof32(xe_words.y >> 16u))"; break;
            case xenos::VertexFormat::k_32_FLOAT: value = "float4(asfloat(xe_words.x), 0.0f, 0.0f, 1.0f)"; break;
            case xenos::VertexFormat::k_32_32_FLOAT: value = "float4(asfloat(xe_words.xy), 0.0f, 1.0f)"; break;
            case xenos::VertexFormat::k_32_32_32_FLOAT: value = "float4(asfloat(xe_words.xyz), 1.0f)"; break;
            default: value = "asfloat(xe_words)"; break;
            }
        }
        else if (attributes.data_format == xenos::VertexFormat::k_16_16_16_16)
        {
            value = fmt::format("float4(xe_unpack(xe_words.x, uint4(16u, 16u, 0u, 0u), uint4(0u, 16u, 0u, 0u), {0}, {1}, {2}).xy, "
                "xe_unpack(xe_words.y, uint4(16u, 16u, 0u, 0u), uint4(0u, 16u, 0u, 0u), {0}, {1}, {2}).xy)",
                attributes.is_signed, attributes.is_integer,
                attributes.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero);
        }
        else if (format.words > 1)
        {
            // 32 bit integer components, one per word.
            std::string components[4];
            for (uint32_t i = 0; i < 4; i++)
            {
                if (i < format.words)
                    components[i] = fmt::format("xe_unpack(xe_words.{}, uint4(32u, 0u, 0u, 0u), uint4(0u, 0u, 0u, 0u), {}, {}, false).x",
                        COMPONENTS[i], attributes.is_signed, attributes.is_integer);
                else
                    components[i] = i == 3 ? "1.0f" : "0.0f";
            }
            value = fmt::format("float4({}, {}, {}, {})", components[0], components[1], components[2], components[3]);
        }
        else
        {
            value = fmt::format("xe_unpack(xe_words.x, uint4({}u, {}u, {}u, {}u), uint4({}u, {}u, {}u, {}u), {}, {}, {})",
                format.bits[0], format.bits[1], format.bits[2], format.bits[3],
                format.offsets[0], format.offsets[1], format.offsets[2], format.offsets[3],
                attributes.is_signed, attributes.is_integer,
                attributes.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero);
        }

        if (attributes.exp_adjust != 0)
            value = fmt::format("({} * {}f)", value, std::ldexp(1.0f, attributes.exp_adjust));

        StoreResult(instr.result, value);
        EndPredicate(instr.is_predicated);
    }

    void HlslShaderTranslator::ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& instr)
    {
        BeginPredicate(instr.is_predicated, instr.predicate_condition);

        switch (instr.opcode)
        {
        case FetchOpcode::kSetTextureLod:
            Line("xe_lod = {};", OperandComponent(instr.operands[0], 0));
            break;

        case FetchOpcode::kTextureFetch:
        {
            if (!instr.has_result() || instr.result.GetUsedWriteMask() == 0)
                break;

            uint32_t fetchConstant = instr.operands[1].storage_index;
            const auto& attributes = instr.attributes;
            Line("// tfetch tf{}", fetchConstant);
            Line("xe_texture = xe_textures[{}];", fetchConstant);
            Line("xe_coords = {};", Operand(instr.operands[0]));

            bool pixel = is_pixel_shader();
            std::string lod;
            if (attributes.use_register_lod)
                lod = fmt::format("xe_lod + {}f", attributes.lod_bias);
            else if (!pixel || !attributes.use_computed_lod)
                lod = fmt::format("{}f", attributes.lod_bias);

            auto sample = [&](const std::string& texture, const std::string& coords)
                {
                    std::string sampler = "xe_samplers[NonUniformResourceIndex(xe_texture.y)]";
                    if (!lod.empty())
                        return fmt::format("{}.SampleLevel({}, {}, {})", texture, sampler, coords, lod);
                    if (attributes.lod_bias != 0.0f)
                        return fmt::format("{}.SampleBias({}, {}, {}f)", texture, sampler, coords, attributes.lod_bias);
                    return fmt::format("{}.Sample({}, {})", texture, sampler, coords);
                };

            std::string value;
            switch (instr.dimension)
            {
            case xenos::FetchOpDimension::k1D:
            case xenos::FetchOpDimension::k2D:
            {
                Line("xe_textures_2d[NonUniformResourceIndex(xe_texture.x)].GetDimensions(xe_size.x, xe_size.y);");
                std::string coords = instr.dimension == xenos::FetchOpDimension::k1D ? "float2(xe_coords.x, 0.5f)" : "xe_coords.xy";
                if (attributes.unnormalized_coordinates)
                    coords = fmt::format("({} / xe_size.xy)", coords);
                if (attributes.offset_x != 0.0f || attributes.offset_y != 0.0f)
                    coords = fmt::format("({} + float2({}f, {}f) / xe_size.xy)", coords, attributes.offset_x, attributes.offset_y);
                value = sample("xe_textures_2d[NonUniformResourceIndex(xe_texture.x)]", coords);
                break;
            }
            case xenos::FetchOpDimension::k3DOrStacked:
            {
                Line("xe_textures_3d[NonUniformResourceIndex(xe_texture.x)].GetDimensions(xe_size.x, xe_size.y, xe_size.z);");
                std::string coords = attributes.unnormalized_coordinates ? "(xe_coords.xyz / xe_size)" : "xe_coords.xyz";
                value = sample("xe_textures_3d[NonUniformResourceIndex(xe_texture.x)]", coords);
                break;
            }
            case xenos::FetchOpDimension::kCube:
                value = sample("xe_textures_cube[NonUniformResourceIndex(xe_texture.x)]", "xe_cube_direction(xe_coords.xyz)");
                break;
            }

            StoreResult(instr.result, fmt::format("xe_texture_result({}, xe_texture)", value));
            break;
        }

        case FetchOpcode::kGetTextureComputedLod:
        case FetchOpcode::kGetTextureGradients:
        case FetchOpcode::kGetTextureWeights:
        case FetchOpcode::kGetTextureBorderColorFrac:
            if (instr.has_result())
                StoreResult(instr.result, "float4(0.0f, 0.0f, 0.0f, 0.0f)");
            break;

        default:
            break;
        }

        EndPredicate(instr.is_predicated);
    }

    void HlslShaderTranslator::ProcessAluInstruction(const ParsedAluInstruction& instr, uint8_t memexportWrittenBefore)
    {
        if (instr.IsNop())
            return;

        BeginPredicate(instr.is_predicated, instr.predicate_condition);

        // Both operations read their operands before either result is written.
        bool hasVector = !instr.IsVectorOpDefaultNop() || instr.vector_and_constant_result.GetUsedWriteMask() != 0;
        bool hasScalar = !instr.IsScalarOpDefaultNop() || instr.scalar_result.GetUsedWriteMask() != 0;
        bool writesPredicate = false;

        auto v = [&](uint32_t i) { return Operand(instr.vector_operands[i]); };
        auto s = [&](uint32_t i, uint32_t component) { return OperandComponent(instr.scalar_operands[i], component); };

        if (hasVector)
        {
            std::string a = instr.vector_operand_count > 0 ? v(0) : "";
            std::string b = instr.vector_operand_count > 1 ? v(1) : "";
            std::string c = instr.vector_operand_count > 2 ? v(2) : "";
            std::string value;

            switch (instr.vector_opcode)
            {
            case AluVectorOpcode::kAdd: value = fmt::format("{} + {}", a, b); break;
            case AluVectorOpcode::kMul: value = fmt::format("xe_mul({}, {})", a, b); break;
            case AluVectorOpcode::kMax: value = a == b ? a : fmt::format("max({}, {})", a, b); break;
            case AluVectorOpcode::kMin: value = a == b ? a : fmt::format("min({}, {})", a, b); break;
            case AluVectorOpcode::kSeq: value = fmt::format("float4({} == {})", a, b); break;
            case AluVectorOpcode::kSgt: value = fmt::format("float4({} > {})", a, b); break;
            case AluVectorOpcode::kSge: value = fmt::format("float4({} >= {})", a, b); break;
            case AluVectorOpcode::kSne: value = fmt::format("float4({} != {})", a, b); break;
            case AluVectorOpcode::kFrc: value = fmt::format("frac({})", a); break;
            case AluVectorOpcode::kTrunc: value = fmt::format("trunc({})", a); break;
            case AluVectorOpcode::kFloor: value = fmt::format("floor({})", a); break;
            case AluVectorOpcode::kMad: value = fmt::format("xe_mul({}, {}) + {}", a, b, c); break;
            case AluVectorOpcode::kCndEq: value = fmt::format("select({} == 0.0f, {}, {})", a, b, c); break;
            case AluVectorOpcode::kCndGe: value = fmt::format("select({} >= 0.0f, {}, {})", a, b, c); break;
            case AluVectorOpcode::kCndGt: value = fmt::format("select({} > 0.0f, {}, {})", a, b, c); break;
            case AluVectorOpcode::kDp4: value = fmt::format("xe_dp({}, {}).xxxx", a, b); break;
            case AluVectorOpcode::kDp3: value = fmt::format("xe_dp(float4(({}).xyz, 0.0f), float4(({}).xyz, 0.0f)).xxxx", a, b); break;
            case AluVectorOpcode::kDp2Add:
                value = fmt::format("(xe_dp(float4(({}).xy, 0.0f, 0.0f), float4(({}).xy, 0.0f, 0.0f)) + ({}).x).xxxx", a, b, c);
                break;
            case AluVectorOpcode::kCube: value = fmt::format("xe_cube({}, {})", a, b); break;
            case AluVectorOpcode::kMax4:
                Line("xe_temp = {};", a);
                value = "max(max(xe_temp.x, xe_temp.y), max(xe_temp.z, xe_temp.w)).xxxx";
                break;
            case AluVectorOpcode::kSetpEqPush:
            case AluVectorOpcode::kSetpNePush:
            case AluVectorOpcode::kSetpGtPush:
            case AluVectorOpcode::kSetpGePush:
            {
                const char* op = instr.vector_opcode == AluVectorOpcode::kSetpEqPush ? "==" :
                    instr.vector_opcode == AluVectorOpcode::kSetpNePush ? "!=" :
                    instr.vector_opcode == AluVectorOpcode::kSetpGtPush ? ">" : ">=";
                Line("xe_temp = {};", a);
                Line("xe_temp2 = {};", b);
                Line("xe_new_p0 = xe_temp.w == 0.0f && xe_temp2.w {} 0.0f;", op);
                value = fmt::format("((xe_temp.x == 0.0f && xe_temp2.x {} 0.0f) ? 0.0f : xe_temp.x + 1.0f).xxxx", op);
                writesPredicate = true;
                break;
            }
            case AluVectorOpcode::kKillEq:
            case AluVectorOpcode::kKillGt:
            case AluVectorOpcode::kKillGe:
            case AluVectorOpcode::kKillNe:
            {
                const char* op = instr.vector_opcode == AluVectorOpcode::kKillEq ? "==" :
                    instr.vector_opcode == AluVectorOpcode::kKillGt ? ">" :
                    instr.vector_opcode == AluVectorOpcode::kKillGe ? ">=" : "!=";
                Line("xe_kill = any({} {} {});", a, op, b);
                value = "float4(xe_kill, xe_kill, xe_kill, xe_kill)";
                break;
            }
            case AluVectorOpcode::kDst:
                Line("xe_temp = {};", a);
                Line("xe_temp2 = {};", b);
                value = "float4(1.0f, xe_mul(xe_temp.y, xe_temp2.y), xe_temp.z, xe_temp2.w)";
                break;
            case AluVectorOpcode::kMaxA:
                Line("xe_temp = {};", a);
                Line("xe_new_a0 = int(clamp(floor(xe_temp.w + 0.5f), -256.0f, 255.0f));");
                value = fmt::format("max(xe_temp, {})", b);
                break;
            default:
                EmitTranslationError(fmt::format("unsupported vector opcode {}", uint32_t(instr.vector_opcode)).c_str(), false);
                value = "float4(0.0f, 0.0f, 0.0f, 0.0f)";
                break;
            }

            Line("xe_vector = {};", value);
        }

        if (hasScalar)
        {
            std::string value;
            switch (instr.scalar_opcode)
            {
            case AluScalarOpcode::kAdds: value = fmt::format("{} + {}", s(0, 0), s(0, 1)); break;
            case AluScalarOpcode::kAddsPrev: value = fmt::format("{} + xe_ps", s(0, 0)); break;
            case AluScalarOpcode::kMuls: value = fmt::format("xe_mul({}, {})", s(0, 0), s(0, 1)); break;
            case AluScalarOpcode::kMulsPrev: value = fmt::format("xe_mul({}, xe_ps)", s(0, 0)); break;
            case AluScalarOpcode::kMulsPrev2:
                Line("xe_temp.xy = float2({}, {});", s(0, 0), s(0, 1));
                value = "(xe_ps == -3.402823466e+38f || isinf(xe_ps) || isnan(xe_ps) || isinf(xe_temp.y) || isnan(xe_temp.y) || xe_temp.y <= 0.0f) ? -3.402823466e+38f : xe_mul(xe_temp.x, xe_ps)";
                break;
            case AluScalarOpcode::kMaxs: value = fmt::format("max({}, {})", s(0, 0), s(0, 1)); break;
            case AluScalarOpcode::kMins: value = fmt::format("min({}, {})", s(0, 0), s(0, 1)); break;
            case AluScalarOpcode::kSeqs: value = fmt::format("float({} == 0.0f)", s(0, 0)); break;
            case AluScalarOpcode::kSgts: value = fmt::format("float({} > 0.0f)", s(0, 0)); break;
            case AluScalarOpcode::kSges: value = fmt::format("float({} >= 0.0f)", s(0, 0)); break;
            case AluScalarOpcode::kSnes: value = fmt::format("float({} != 0.0f)", s(0, 0)); break;
            case AluScalarOpcode::kFrcs: value = fmt::format("frac({})", s(0, 0)); break;
            case AluScalarOpcode::kTruncs: value = fmt::format("trunc({})", s(0, 0)); break;
            case AluScalarOpcode::kFloors: value = fmt::format("floor({})", s(0, 0)); break;
            case AluScalarOpcode::kExp: value = fmt::format("exp2({})", s(0, 0)); break;
            case AluScalarOpcode::kLogc: value = fmt::format("max(log2({}), -3.402823466e+38f)", s(0, 0)); break;
            case AluScalarOpcode::kLog: value = fmt::format("log2({})", s(0, 0)); break;
            case AluScalarOpcode::kRcpc: value = fmt::format("clamp(rcp({}), -3.402823466e+38f, 3.402823466e+38f)", s(0, 0)); break;
            case AluScalarOpcode::kRcpf:
                Line("xe_scalar = rcp({});", s(0, 0));
                value = "isinf(xe_scalar) ? 0.0f : xe_scalar";
                break;
            case AluScalarOpcode::kRcp: value = fmt::format("rcp({})", s(0, 0)); break;
            case AluScalarOpcode::kRsqc: value = fmt::format("clamp(rsqrt({}), -3.402823466e+38f, 3.402823466e+38f)", s(0, 0)); break;
            case AluScalarOpcode::kRsqf:
                Line("xe_scalar = rsqrt({});", s(0, 0));
                value = "isinf(xe_scalar) ? 0.0f : xe_scalar";
                break;
            case AluScalarOpcode::kRsq: value = fmt::format("rsqrt({})", s(0, 0)); break;
            case AluScalarOpcode::kMaxAs:
                Line("xe_temp.xy = float2({}, {});", s(0, 0), s(0, 1));
                Line("xe_new_a0 = int(clamp(floor(xe_temp.x + 0.5f), -256.0f, 255.0f));");
                value = "max(xe_temp.x, xe_temp.y)";
                break;
            case AluScalarOpcode::kMaxAsf:
                Line("xe_temp.xy = float2({}, {});", s(0, 0), s(0, 1));
                Line("xe_new_a0 = int(clamp(floor(xe_temp.x), -256.0f, 255.0f));");
                value = "max(xe_temp.x, xe_temp.y)";
                break;
            case AluScalarOpcode::kSubs: value = fmt::format("{} - {}", s(0, 0), s(0, 1)); break;
            case AluScalarOpcode::kSubsPrev: value = fmt::format("{} - xe_ps", s(0, 0)); break;
            case AluScalarOpcode::kSetpEq:
            case AluScalarOpcode::kSetpNe:
            case AluScalarOpcode::kSetpGt:
            case AluScalarOpcode::kSetpGe:
            {
                const char* op = instr.scalar_opcode == AluScalarOpcode::kSetpEq ? "==" :
                    instr.scalar_opcode == AluScalarOpcode::kSetpNe ? "!=" :
                    instr.scalar_opcode == AluScalarOpcode::kSetpGt ? ">" : ">=";
                Line("xe_new_p0 = {} {} 0.0f;", s(0, 0), op);
                value = "xe_new_p0 ? 0.0f : 1.0f";
                writesPredicate = true;
                break;
            }
            case AluScalarOpcode::kSetpInv:
                Line("xe_scalar = {};", s(0, 0));
                Line("xe_new_p0 = xe_scalar == 1.0f;");
                value = "xe_new_p0 ? 0.0f : (xe_scalar == 0.0f ? 1.0f : xe_scalar)";
                writesPredicate = true;
                break;
            case AluScalarOpcode::kSetpPop:
                Line("xe_scalar = {} - 1.0f;", s(0, 0));
                Line("xe_new_p0 = xe_scalar <= 0.0f;");
                value = "xe_new_p0 ? 0.0f : xe_scalar";
                writesPredicate = true;
                break;
            case AluScalarOpcode::kSetpClr:
                Line("xe_new_p0 = false;");
                value = "3.402823466e+38f";
                writesPredicate = true;
                break;
            case AluScalarOpcode::kSetpRstr:
                Line("xe_scalar = {};", s(0, 0));
                Line("xe_new_p0 = xe_scalar == 0.0f;");
                value = "xe_new_p0 ? 0.0f : xe_scalar";
                writesPredicate = true;
                break;
            case AluScalarOpcode::kKillsEq:
            case AluScalarOpcode::kKillsGt:
            case AluScalarOpcode::kKillsGe:
            case AluScalarOpcode::kKillsNe:
            case AluScalarOpcode::kKillsOne:
            {
                const char* condition = instr.scalar_opcode == AluScalarOpcode::kKillsEq ? "== 0.0f" :
                    instr.scalar_opcode == AluScalarOpcode::kKillsGt ? "> 0.0f" :
                    instr.scalar_opcode == AluScalarOpcode::kKillsGe ? ">= 0.0f" :
                    instr.scalar_opcode == AluScalarOpcode::kKillsNe ? "!= 0.0f" : "== 1.0f";
                Line("xe_kill = xe_kill || ({} {});", s(0, 0), condition);
                value = "xe_kill ? 1.0f : 0.0f";
                break;
            }
            case AluScalarOpcode::kSqrt: value = fmt::format("sqrt({})", s(0, 0)); break;
            case AluScalarOpcode::kMulsc0:
            case AluScalarOpcode::kMulsc1:
                value = fmt::format("xe_mul({}, {})", s(0, 0), s(1, 0)); break;
            case AluScalarOpcode::kAddsc0:
            case AluScalarOpcode::kAddsc1:
                value = fmt::format("{} + {}", s(0, 0), s(1, 0)); break;
            case AluScalarOpcode::kSubsc0:
            case AluScalarOpcode::kSubsc1:
                value = fmt::format("{} - {}", s(0, 0), s(1, 0)); break;
            case AluScalarOpcode::kSin: value = fmt::format("sin({})", s(0, 0)); break;
            case AluScalarOpcode::kCos: value = fmt::format("cos({})", s(0, 0)); break;
            case AluScalarOpcode::kRetainPrev: value = "xe_ps"; break;
            default:
                EmitTranslationError(fmt::format("unsupported scalar opcode {}", uint32_t(instr.scalar_opcode)).c_str(), false);
                value = "0.0f";
                break;
            }

            Line("xe_ps_next = {};", value);
        }

        if (hasVector)
            StoreResult(instr.vector_and_constant_result, "xe_vector");
        if (hasScalar)
        {
            Line("xe_ps = xe_ps_next;");
            StoreResult(instr.scalar_result, "xe_ps.xxxx");
        }

        if (writesPredicate)
            Line("xe_p0 = xe_new_p0;");
        if (instr.vector_opcode == AluVectorOpcode::kMaxA || instr.scalar_opcode == AluScalarOpcode::kMaxAs ||
            instr.scalar_opcode == AluScalarOpcode::kMaxAsf)
        {
            Line("xe_a0 = xe_new_a0;");
        }

        if (is_pixel_shader() && (instr.vector_opcode >= AluVectorOpcode::kKillEq && instr.vector_opcode <= AluVectorOpcode::kKillNe ||
            instr.scalar_opcode >= AluScalarOpcode::kKillsEq && instr.scalar_opcode <= AluScalarOpcode::kKillsOne))
        {
            Line("if (xe_kill) discard;");
        }

        EndPredicate(instr.is_predicated);
    }

    std::vector<uint8_t> HlslShaderTranslator::CompleteTranslation()
    {
        if (m_caseOpen)
        {
            JumpTo(0xFFFF);
            m_indent = 2;
            Line("}}");
        }

        bool pixel = is_pixel_shader();
        uint32_t registerCount = std::max(register_count(), 16u);
        std::string& out = m_out;

        out = fmt::format("#define XE_REGISTER_COUNT {}\n", registerCount);
        out += HLSL_PRELUDE;

        if (pixel)
        {
            out += R"(
struct XeInput
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
    bool frontFace : SV_IsFrontFace;
};

struct XeOutput
{
    float4 color[4] : SV_Target0;
)";
            if (current_shader().writes_depth())
                out += "    float depth : SV_Depth;\n";
            out += R"(};

XeOutput main(XeInput input)
{
    float4 xe_r[XE_REGISTER_COUNT];
    [unroll] for (int i = 0; i < XE_REGISTER_COUNT; i++)
        xe_r[i] = i < 16 ? input.interpolators[i] : float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (xe_param_gen_register >= 0 && xe_param_gen_register < XE_REGISTER_COUNT)
    {
        // Pixel position (negative x for back faces) and point sprite coordinates.
        float2 pixel = input.position.xy - 0.5f;
        xe_r[xe_param_gen_register] = float4(input.frontFace ? pixel.x : -pixel.x - 1.0f, pixel.y, 0.0f, 0.0f);
    }
    float4 xe_color[4] = { float4(0.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 0.0f),
        float4(0.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 0.0f) };
    float xe_depth = input.position.z;
)";
        }
        else
        {
            out += R"(
struct XeOutput
{
    float4 position : SV_Position;
    float4 interpolators[16] : TEXCOORD0;
};

XeOutput main(uint vertexId : SV_VertexID)
{
    float4 xe_r[XE_REGISTER_COUNT];
    [unroll] for (int i = 0; i < XE_REGISTER_COUNT; i++)
        xe_r[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    xe_r[0].x = float(int(vertexId) + xe_vertex_index_offset);
    float4 xe_o[16];
    [unroll] for (int j = 0; j < 16; j++)
        xe_o[j] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 xe_position = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float4 xe_point_size = float4(0.0f, 0.0f, 0.0f, 0.0f);
    int xe_index = 0;
    uint4 xe_words = uint4(0u, 0u, 0u, 0u);
)";
        }

        out += R"(
    float4 xe_value = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 xe_vector = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 xe_temp = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 xe_temp2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 xe_coords = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 xe_size = float3(1.0f, 1.0f, 1.0f);
    uint4 xe_texture = uint4(0u, 0u, 0u, 0u);
    float xe_ps = 0.0f;
    float xe_ps_next = 0.0f;
    float xe_scalar = 0.0f;
    float xe_lod = 0.0f;
    bool xe_p0 = false;
    bool xe_new_p0 = false;
    bool xe_kill = false;
    int xe_a0 = 0;
    int xe_new_a0 = 0;
    int xe_aL = 0;
    int xe_loop_depth = 0;
    uint xe_loop_count[4] = { 0u, 0u, 0u, 0u };
    int xe_loop_aL[4] = { 0, 0, 0, 0 };
    int xe_call_depth = 0;
    uint xe_call_return[4] = { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu };

    uint xe_pc = 0u;
    [loop] while (xe_pc != 0xFFFFu)
    {
        [forcecase] switch (xe_pc)
        {
)";
        out += m_body;
        out += R"(
        default:
            xe_pc = 0xFFFFu;
            break;
        }
    }

    XeOutput output;
)";

        if (pixel)
        {
            out += R"(
    // Alpha test, exponent bias and gamma conversion of the render targets.
    float alpha = xe_color[0].a;
    bool alphaPass = true;
    switch (xe_alpha_func)
    {
    case 0u: alphaPass = false; break;
    case 1u: alphaPass = alpha < xe_alpha_ref; break;
    case 2u: alphaPass = alpha == xe_alpha_ref; break;
    case 3u: alphaPass = alpha <= xe_alpha_ref; break;
    case 4u: alphaPass = alpha > xe_alpha_ref; break;
    case 5u: alphaPass = alpha != xe_alpha_ref; break;
    case 6u: alphaPass = alpha >= xe_alpha_ref; break;
    }
    if (!alphaPass)
        discard;
    [unroll] for (int t = 0; t < 4; t++)
    {
        float4 color = xe_color[t] * xe_color_exp_bias[t];
        if ((xe_gamma_targets >> t) & 1u)
            color.rgb = float3(xe_linear_to_gamma(color.r), xe_linear_to_gamma(color.g), xe_linear_to_gamma(color.b));
        output.color[t] = color;
    }
)";
            if (current_shader().writes_depth())
                out += "    output.depth = xe_depth;\n";
        }
        else
        {
            out += R"(
    // Guest viewport transform to host clip space.
    float4 position = xe_position;
    if (xe_flags & XE_FLAG_W_RECIPROCAL)
        position.w = rcp(position.w);
    if (xe_flags & XE_FLAG_XY_NOT_DIVIDED)
        position.xy *= position.w;
    if (xe_flags & XE_FLAG_Z_NOT_DIVIDED)
        position.z *= position.w;
    output.position = float4(position.xyz * xe_ndc_scale.xyz + position.w * xe_ndc_offset.xyz, position.w);
    output.interpolators = xe_o;
)";
        }

        out += "    return output;\n}\n";
        return std::vector<uint8_t>(out.begin(), out.end());
    }

    std::string TranslateShaderToHlsl(Shader& shader)
    {
        HlslShaderTranslator translator;
        auto* translation = shader.GetOrCreateTranslation(0);
        if (!translator.TranslateAnalyzedShader(*translation))
            return {};

        return translation->GetTranslatedBinaryString();
    }
}
