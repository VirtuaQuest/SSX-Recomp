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
    float xe_resolution_scale; // Internal resolution scale: host pixels per guest pixel in each direction.
    float xe_padding;
    float4 xe_color_exp_bias;
    uint4 xe_textures[32];          // Descriptor index, sampler index, swizzle, sign/gamma flags.
    int4 xe_vertex_data_offsets[24]; // Byte offset from a guest address to the draw's vertex data.
    float4 xe_texture_transforms[32]; // UV scale (xy) and offset (zw) onto the host texture.
    float4 xe_point; // Point sprites: constant diameter (xy), per-vertex diameter range (zw), in pixels.
    float4 xe_point_ndc; // NDC radius of a point one pixel in diameter (xy).
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
static const uint XE_FLAG_W_NOT_RECIPROCAL = 4u;
static const uint XE_FLAG_POINT = 8u; // Drawing point sprites.
static const uint XE_FLAG_DEBUG_HIGHLIGHT = 16u; // Debugging: the draw outputs solid magenta.
static const uint XE_FLAG_DEPTH_24 = 32u; // The guest's depth buffer is 24 bit float (see xe_depth_24).

// The guest's float depth has 20 bits of mantissa and the host's 23, so surfaces almost in the same plane, as decals are, come out equal there and
// the one drawn later wins, while here the difference survives and which one wins changes with the camera. Keeps only the bits the guest has, rounding down
// like the guest words made from host depth, so depth restored from those words compares equal too. Only with AR_NO_EARLY_DEPTH: on a GTX 970 any depth output,
// SV_DepthLessEqual too, cost early rejection.
float xe_depth_24(float depth)
{
    return depth < 6.103515625e-05f ? floor(depth * 17179869184.0f) / 17179869184.0f : asfloat(asuint(depth) & 0xFFFFFFF8u);
}

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
        // Selects, not value[source]: indexing a vector by a value known only at run time puts it in local memory
        // (an alloca per fetch in the DXIL). Two levels of them, by the bits of the source.
        uint source = (swizzle >> (i * 3u)) & 7u;
        float2 pair = (source & 1u) ? value.yw : value.xz;
        float component = (source & 2u) ? pair.y : pair.x;
        result[i] = source < 4u ? component : (source == 5u ? 1.0f : 0.0f);
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

// Applies the fetch constant's per component sign (signed, unsigned biased or gamma) and swizzle to a sampled value.
float4 xe_texture_result(float4 value, uint4 info)
{
    // Most fetches have no sign, gamma, raw depth or exponent bias and the identity swizzle: one branch on the draw's
    // constants instead of a dozen per fetch.
    [branch] if ((info.w & 0x3FF3FFu) == 0u && (info.z & 0xFFFu) == 0x688u)
        return value;
    // Depth buffer words (resolved from a depth buffer, as bytes from the low one up) fetched as depth: 24 bit unorm
    // or float (4 bit exponent, 20 bit mantissa) depth above 8 bit stencil.
    uint rawDepth = (info.w >> 8u) & 3u;
    [branch] if (rawDepth != 0u)
    {
        uint4 bytes = uint4(round(saturate(value) * 255.0f));
        uint depth24 = bytes.y | (bytes.z << 8u) | (bytes.w << 16u);
        // The float one is the host float with the exponent rebased by 127 - 15 and the mantissa's low bits zero
        // (denormals: the 20 bit mantissa times 2^-34), built from its bits instead of with exp2.
        float depth = rawDepth == 1u ? float(depth24) * (1.0f / 16777215.0f) :
            (depth24 >> 20u) == 0u ? float(depth24) * 5.820766091346741e-11f : asfloat((depth24 << 3u) + 0x38000000u);
        value = float4(depth, depth, depth, depth);
    }

    // Sign, gamma, bias and exponent bias, each a branch on the draw's constants, and only when any is set.
    [branch] if ((info.w & 0x3FF0FFu) != 0u)
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
            else if ((info.w >> (i + 12u)) & 1u)
            {
                value[i] = value[i] * 2.0f - 1.0f;
            }
        }
        // Exponent bias of the fetch constant (6 bit signed power of two).
        int expAdjust = int(info.w << 10u) >> 26;
        if (expAdjust != 0)
            value *= exp2(float(expAdjust));
    }
    [branch] if ((info.z & 0xFFFu) == 0x688u)
        return value;
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
        m_sizes2d.reset();
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

    // Whether an operand may be an infinity or NaN: it reads a register component that may hold one at this point (in
    // program order) or anywhere in the shader. Constants, interpolators and fetched values count as finite.
    bool HlslShaderTranslator::OperandUnsafe(const InstructionOperand& operand) const
    {
        if (m_guardAll)
            return true;
        if (operand.storage_source != InstructionStorageSource::kRegister)
            return false;
        if (operand.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute)
            return m_unsafe.any();
        for (uint32_t i = 0; i < operand.component_count; i++)
        {
            SwizzleSource component = operand.components[i];
            if (component <= SwizzleSource::kW && m_unsafe[(operand.storage_index & 127) * 4 + uint32_t(component)])
                return true;
        }
        return false;
    }

    // Records a register write of a value that may (or can't) be infinite or NaN. While analyzing, the set only
    // grows. In program order (shaders without jumps, calls or loops), a finite value written unconditionally clears
    // the components it writes.
    void HlslShaderTranslator::TrackStore(const InstructionResult& result, bool unsafe)
    {
        if (result.storage_target != InstructionStorageTarget::kRegister || result.GetUsedWriteMask() == 0 ||
            (!m_analyzing && !m_programOrder))
            return;
        // Saturated results are finite (saturate turns NaN into 0), and so are the constants 0 and 1.
        unsafe = unsafe && !result.is_clamped;
        std::bitset<512> before = m_unsafe;
        if (result.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute)
        {
            if (unsafe)
                m_unsafe.set();
        }
        else
        {
            bool conditional = m_execConditional || m_predicated;
            for (uint32_t i = 0; i < 4; i++)
            {
                if (!(result.GetUsedWriteMask() & (1u << i)))
                    continue;
                uint32_t bit = (result.storage_index & 127) * 4 + i;
                if (unsafe && result.components[i] <= SwizzleSource::kW)
                    m_unsafe.set(bit);
                else if (!m_analyzing && !conditional)
                    m_unsafe.reset(bit);
            }
        }
        m_unsafeChanged |= m_unsafe != before;
    }

    // Shader model 3 multiplication makes zero times anything zero, also infinity and NaN, and xe_mul does that with
    // a select, which also keeps the host from fusing a multiply and an add. Operands that can't be infinite or NaN
    // multiply plainly: 0 times a finite value is 0 anyway.
    std::string HlslShaderTranslator::Mul(const std::string& a, const std::string& b, bool unsafe) const
    {
        return unsafe ? fmt::format("xe_mul({}, {})", a, b) : fmt::format("({} * {})", a, b);
    }

    std::string HlslShaderTranslator::Dot(const std::string& a, const std::string& b, bool unsafe) const
    {
        return unsafe ? fmt::format("xe_dp({}, {})", a, b) : fmt::format("dot({}, {})", a, b);
    }

    // A 2D texture's size, queried once at the start of the shader instead of at every fetch (on NVIDIA a size query
    // is a texture instruction too): xy in guest texels (resolved textures are larger by the resolution scale), zw in
    // host texels, and the reciprocals in <name>_rcp.
    std::string HlslShaderTranslator::TextureSize2D(uint32_t fetchConstant)
    {
        m_sizes2d.set(fetchConstant & 31);
        return fmt::format("xe_size2d_{}", fetchConstant & 31);
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
        TrackStore(result, m_storeUnsafe);
        m_storeUnsafe = false;
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
        m_predicated = predicated;
        if (!predicated)
            return;

        Line("if ({}xe_p0)", condition ? "" : "!");
        Line("{{");
        m_indent++;
    }

    void HlslShaderTranslator::EndPredicate(bool predicated)
    {
        m_predicated = false;
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
            value = fmt::format("({} * {:.6f}f)", value, std::ldexp(1.0f, attributes.exp_adjust));

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
                lod = fmt::format("xe_lod + {:.6f}f", attributes.lod_bias);
            else if (!pixel || !attributes.use_computed_lod)
                lod = fmt::format("{:.6f}f", attributes.lod_bias);

            auto sample = [&](const std::string& texture, const std::string& coords)
                {
                    std::string sampler = "xe_samplers[NonUniformResourceIndex(xe_texture.y)]";
                    if (!lod.empty())
                        return fmt::format("{}.SampleLevel({}, {}, {})", texture, sampler, coords, lod);
                    if (attributes.lod_bias != 0.0f)
                        return fmt::format("{}.SampleBias({}, {}, {:.6f}f)", texture, sampler, coords, attributes.lod_bias);
                    return fmt::format("{}.Sample({}, {})", texture, sampler, coords);
                };

            std::string value;
            switch (instr.dimension)
            {
            case xenos::FetchOpDimension::k1D:
            case xenos::FetchOpDimension::k2D:
            {
                // Sizes from the prologue (see TextureSize2D).
                std::string size = TextureSize2D(fetchConstant);
                std::string coords = instr.dimension == xenos::FetchOpDimension::k1D ? "float2(xe_coords.x, 0.5f)" : "xe_coords.xy";
                if (attributes.unnormalized_coordinates)
                    coords = fmt::format("({} * {}_rcp.xy)", coords, size);
                if (attributes.offset_x != 0.0f || attributes.offset_y != 0.0f)
                    coords = fmt::format("({} + float2({:.6f}f, {:.6f}f) * {}_rcp.xy)", coords, attributes.offset_x, attributes.offset_y, size);
                // Unnormalized coordinates are already in host texels; normalized ones span the fetch's size.
                if (attributes.unnormalized_coordinates)
                    coords = fmt::format("({} + xe_texture_transforms[{}].zw)", coords, fetchConstant);
                else
                    coords = fmt::format("({} * xe_texture_transforms[{}].xy + xe_texture_transforms[{}].zw)", coords, fetchConstant, fetchConstant);
                // Point sampling picks the texel the coordinates are in. The host converts normalized coordinates back to
                // texels with limited precision, so coordinates on a texel edge (games sample at whole pixel positions)
                // can land in the previous texel; they're moved to the texel's center.
                coords = fmt::format("(((xe_texture.w >> 11u) & 1u) ? (floor({0} * {1}.zw) + 0.5f) * {1}_rcp.zw : {0})", coords, size);
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

        case FetchOpcode::kGetTextureWeights:
        {
            // Bilinear weights: the fraction of the coordinates in texels, past the centers of the texels sampled
            // (shaders use them to filter comparisons themselves, as in percentage closer shadow filtering).
            if (!instr.has_result())
                break;

            uint32_t fetchConstant = instr.operands[1].storage_index;
            const auto& attributes = instr.attributes;
            bool point = attributes.mag_filter == xenos::TextureFilter::kPoint;
            if (point || instr.dimension == xenos::FetchOpDimension::kCube || instr.dimension == xenos::FetchOpDimension::k3DOrStacked)
            {
                StoreResult(instr.result, "float4(0.0f, 0.0f, 0.0f, 0.0f)");
                break;
            }

            Line("// getWeights tf{}", fetchConstant);
            Line("xe_texture = xe_textures[{}];", fetchConstant);
            Line("xe_coords = {};", Operand(instr.operands[0]));
            std::string size = TextureSize2D(fetchConstant);
            std::string coords = instr.dimension == xenos::FetchOpDimension::k1D ? "float2(xe_coords.x, 0.5f)" : "xe_coords.xy";
            if (attributes.unnormalized_coordinates)
                coords = fmt::format("({} * {}_rcp.xy + xe_texture_transforms[{}].zw)", coords, size, fetchConstant);
            else
                coords = fmt::format("({} * xe_texture_transforms[{}].xy + xe_texture_transforms[{}].zw)", coords, fetchConstant, fetchConstant);
            StoreResult(instr.result, fmt::format("float4(frac({} * {}.xy + float2({:.6f}f, {:.6f}f) - 0.5f), 0.0f, 0.0f)",
                coords, size, attributes.offset_x, attributes.offset_y));
            break;
        }

        case FetchOpcode::kGetTextureComputedLod:
        case FetchOpcode::kGetTextureGradients:
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
        auto vu = [&](uint32_t i) { return i < instr.vector_operand_count && OperandUnsafe(instr.vector_operands[i]); };
        auto su = [&](uint32_t i) { return i < instr.scalar_operand_count && OperandUnsafe(instr.scalar_operands[i]); };
        bool vectorUnsafe = false, scalarUnsafe = false;

        if (hasVector)
        {
            std::string a = instr.vector_operand_count > 0 ? v(0) : "";
            std::string b = instr.vector_operand_count > 1 ? v(1) : "";
            std::string c = instr.vector_operand_count > 2 ? v(2) : "";
            std::string value;

            switch (instr.vector_opcode)
            {
            case AluVectorOpcode::kAdd: value = fmt::format("{} + {}", a, b); break;
            case AluVectorOpcode::kMul: value = Mul(a, b, vu(0) || vu(1)); break;
            case AluVectorOpcode::kMax: value = a == b ? a : fmt::format("max({}, {})", a, b); break;
            case AluVectorOpcode::kMin: value = a == b ? a : fmt::format("min({}, {})", a, b); break;
            case AluVectorOpcode::kSeq: value = fmt::format("float4({} == {})", a, b); break;
            case AluVectorOpcode::kSgt: value = fmt::format("float4({} > {})", a, b); break;
            case AluVectorOpcode::kSge: value = fmt::format("float4({} >= {})", a, b); break;
            case AluVectorOpcode::kSne: value = fmt::format("float4({} != {})", a, b); break;
            case AluVectorOpcode::kFrc: value = fmt::format("frac({})", a); break;
            case AluVectorOpcode::kTrunc: value = fmt::format("trunc({})", a); break;
            case AluVectorOpcode::kFloor: value = fmt::format("floor({})", a); break;
            case AluVectorOpcode::kMad: value = fmt::format("{} + {}", Mul(a, b, vu(0) || vu(1)), c); break;
            case AluVectorOpcode::kCndEq: value = fmt::format("select({} == 0.0f, {}, {})", a, b, c); break;
            case AluVectorOpcode::kCndGe: value = fmt::format("select({} >= 0.0f, {}, {})", a, b, c); break;
            case AluVectorOpcode::kCndGt: value = fmt::format("select({} > 0.0f, {}, {})", a, b, c); break;
            case AluVectorOpcode::kDp4: value = fmt::format("({}).xxxx", Dot(a, b, vu(0) || vu(1))); break;
            case AluVectorOpcode::kDp3:
                value = fmt::format("({}).xxxx", Dot(fmt::format("float4(({}).xyz, 0.0f)", a), fmt::format("float4(({}).xyz, 0.0f)", b),
                    vu(0) || vu(1)));
                break;
            case AluVectorOpcode::kDp2Add:
                value = fmt::format("({} + ({}).x).xxxx", Dot(fmt::format("float4(({}).xy, 0.0f, 0.0f)", a),
                    fmt::format("float4(({}).xy, 0.0f, 0.0f)", b), vu(0) || vu(1)), c);
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
                value = fmt::format("float4(1.0f, {}, xe_temp.z, xe_temp2.w)", Mul("xe_temp.y", "xe_temp2.y", vu(0) || vu(1)));
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

            // Comparisons, kills and predicate sets give finite values; anything else may carry its operands'.
            bool compares = instr.vector_opcode == AluVectorOpcode::kSeq || instr.vector_opcode == AluVectorOpcode::kSgt ||
                instr.vector_opcode == AluVectorOpcode::kSge || instr.vector_opcode == AluVectorOpcode::kSne ||
                (instr.vector_opcode >= AluVectorOpcode::kSetpEqPush && instr.vector_opcode <= AluVectorOpcode::kSetpGePush) ||
                (instr.vector_opcode >= AluVectorOpcode::kKillEq && instr.vector_opcode <= AluVectorOpcode::kKillNe);
            vectorUnsafe = !compares && (vu(0) || vu(1) || vu(2));
        }

        if (hasScalar)
        {
            std::string value;
            switch (instr.scalar_opcode)
            {
            case AluScalarOpcode::kAdds: value = fmt::format("{} + {}", s(0, 0), s(0, 1)); break;
            case AluScalarOpcode::kAddsPrev: value = fmt::format("{} + xe_ps", s(0, 0)); break;
            case AluScalarOpcode::kMuls: value = Mul(s(0, 0), s(0, 1), su(0)); break;
            case AluScalarOpcode::kMulsPrev: value = Mul(s(0, 0), "xe_ps", su(0) || m_unsafePs || m_guardAll); break;
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
                value = Mul(s(0, 0), s(1, 0), su(0) || su(1)); break;
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

            switch (instr.scalar_opcode)
            {
            // Results that can be infinite or NaN from finite operands (or the largest float, which overflows to one).
            case AluScalarOpcode::kExp: case AluScalarOpcode::kLog: case AluScalarOpcode::kLogc:
            case AluScalarOpcode::kRcp: case AluScalarOpcode::kRcpc: case AluScalarOpcode::kRcpf:
            case AluScalarOpcode::kRsq: case AluScalarOpcode::kRsqc: case AluScalarOpcode::kRsqf:
            case AluScalarOpcode::kSqrt: case AluScalarOpcode::kMulsPrev2: case AluScalarOpcode::kSetpClr:
                scalarUnsafe = true;
                break;
            case AluScalarOpcode::kSeqs: case AluScalarOpcode::kSgts: case AluScalarOpcode::kSges: case AluScalarOpcode::kSnes:
            case AluScalarOpcode::kSetpEq: case AluScalarOpcode::kSetpNe: case AluScalarOpcode::kSetpGt: case AluScalarOpcode::kSetpGe:
            case AluScalarOpcode::kKillsEq: case AluScalarOpcode::kKillsGt: case AluScalarOpcode::kKillsGe:
            case AluScalarOpcode::kKillsNe: case AluScalarOpcode::kKillsOne:
                break;
            case AluScalarOpcode::kAddsPrev: case AluScalarOpcode::kMulsPrev: case AluScalarOpcode::kSubsPrev:
            case AluScalarOpcode::kRetainPrev:
                scalarUnsafe = su(0) || m_unsafePs;
                break;
            default:
                scalarUnsafe = su(0) || su(1);
                break;
            }
        }

        if (hasVector)
        {
            m_storeUnsafe = vectorUnsafe;
            StoreResult(instr.vector_and_constant_result, "xe_vector");
        }
        if (hasScalar)
        {
            Line("xe_ps = xe_ps_next;");
            m_storeUnsafe = scalarUnsafe;
            StoreResult(instr.scalar_result, "xe_ps.xxxx");
            m_unsafeChanged |= scalarUnsafe && !m_unsafePs;
            if (m_analyzing || (m_programOrder && (m_execConditional || instr.is_predicated)))
                m_unsafePs |= scalarUnsafe;
            else if (m_programOrder)
                m_unsafePs = scalarUnsafe;
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
        // AR_SHOW_EXPR=<HLSL float4>: what draws picked by AR_HIGHLIGHT_PS output instead of magenta, to look at a value
        // the shader computes. XE_R(n) is temporary register n as the shader leaves it (clamped to the shader's count).
        static const char* showExpression = getenv("AR_SHOW_EXPR");
        out += "#define XE_R(n) xe_r[min(n, XE_REGISTER_COUNT - 1)]\n";
        out += fmt::format("#define XE_HIGHLIGHT_COLOR ({})\n", showExpression ? showExpression : "float4(1.0f, 0.0f, 1.0f, 1.0f)");
        out += HLSL_PRELUDE;

        if (pixel)
        {
            out += R"(
struct XeInput
{
    noperspective centroid float4 position : SV_Position; // As SV_DepthLessEqual requires.
    float4 interpolators[16] : TEXCOORD0;
    float4 extra : TEXCOORD16; // Vertex shader point diameter (x), point sprite coordinates (zw).
    bool frontFace : SV_IsFrontFace;
};

struct XeOutput
{
    float4 color[4] : SV_Target0;
)";
            // Depth is output only by shaders that write it. Rounding every pixel's depth to the guest's 24 bit float
            // (xe_depth_24, for Anarchy Reigns' decals) cost early depth rejection on a GTX 970 even as
            // SV_DepthLessEqual: 2.3 ms of SSX's 9 ms of draws at 1x. AR_NO_EARLY_DEPTH rounds again.
            static const bool roundDepth = getenv("AR_NO_EARLY_DEPTH") != nullptr;
            bool depthOutput = current_shader().writes_depth() || roundDepth;
            if (depthOutput)
                out += current_shader().writes_depth() ? "    float depth : SV_Depth;\n" : "    float depth : SV_DepthLessEqual;\n";
            out += R"(};

XeOutput main(XeInput input)
{
    float4 xe_r[XE_REGISTER_COUNT];
    [unroll] for (int i = 0; i < XE_REGISTER_COUNT; i++)
        xe_r[i] = i < 16 ? input.interpolators[i] : float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (xe_param_gen_register >= 0 && xe_param_gen_register < XE_REGISTER_COUNT)
    {
        // Pixel position (negative x for back faces; for points, negative y) and point sprite coordinates.
        float2 pixel = floor(input.position.xy / xe_resolution_scale);
        float4 paramGen;
        if (xe_flags & XE_FLAG_POINT)
            paramGen = float4(abs(pixel.x), -abs(pixel.y), saturate(input.extra.zw));
        else
            // Back faces set the sign bit of x (also for x = 0); the magnitude stays the pixel position.
            paramGen = float4(input.frontFace ? pixel.x : asfloat(asuint(pixel.x) | 0x80000000u), pixel.y, 0.0f, 0.0f);
        // Written by constant indices: xe_r[xe_param_gen_register] would put every register in local memory.
        [unroll] for (int i = 0; i < XE_REGISTER_COUNT; i++)
            if (i == xe_param_gen_register)
                xe_r[i] = paramGen;
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
    float4 extra : TEXCOORD16; // Point diameter (x, negative when the shader doesn't write it).
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
    float4 xe_point_size = float4(-1.0f, 0.0f, 0.0f, 0.0f);
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
)";
        for (uint32_t index = 0; index < 32; index++)
        {
            if (!m_sizes2d[index])
                continue;
            out += fmt::format("    float4 xe_size2d_{0}, xe_size2d_{0}_rcp;\n", index);
            out += fmt::format("    xe_textures_2d[NonUniformResourceIndex(xe_textures[{0}].x)].GetDimensions(xe_size2d_{0}.z, xe_size2d_{0}.w);\n", index);
            out += fmt::format("    xe_size2d_{0}.xy = xe_size2d_{0}.zw / (((xe_textures[{0}].w >> 10u) & 1u) ? xe_resolution_scale : 1.0f);\n", index);
            out += fmt::format("    xe_size2d_{0}_rcp = 1.0f / xe_size2d_{0};\n", index);
        }
        out += R"(
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
    if (xe_flags & XE_FLAG_DEBUG_HIGHLIGHT)
        xe_color[0] = XE_HIGHLIGHT_COLOR;
    [unroll] for (int t = 0; t < 4; t++)
    {
        float4 color = xe_color[t] * xe_color_exp_bias[t];
        // 7e3 render targets (bit 4 + t) hold 0 to 31.875 colour and 2 bit alpha.
        if ((xe_gamma_targets >> (t + 4u)) & 1u)
            color = clamp(color, 0.0f, float4(31.875f, 31.875f, 31.875f, 1.0f));
        if ((xe_gamma_targets >> t) & 1u)
            color.rgb = float3(xe_linear_to_gamma(color.r), xe_linear_to_gamma(color.g), xe_linear_to_gamma(color.b));
        output.color[t] = color;
    }
)";
            if (current_shader().writes_depth() || getenv("AR_NO_EARLY_DEPTH"))
                out += "    output.depth = (xe_flags & XE_FLAG_DEPTH_24) ? xe_depth_24(xe_depth) : xe_depth;\n";
        }
        else
        {
            out += R"(
    // Guest viewport transform to host clip space.
    float4 position = xe_position;
    // PA_CL_VTE_CNTL.vtx_w0_fmt clear: the shader outputs 1/W.
    if (!(xe_flags & XE_FLAG_W_NOT_RECIPROCAL))
        position.w = rcp(position.w);
    if (xe_flags & XE_FLAG_XY_NOT_DIVIDED)
        position.xy *= position.w;
    if (xe_flags & XE_FLAG_Z_NOT_DIVIDED)
        position.z *= position.w;
    output.position = float4(position.xyz * xe_ndc_scale.xyz + position.w * xe_ndc_offset.xyz, position.w);
    output.interpolators = xe_o;
    output.extra = float4(xe_point_size.x, 0.0f, 0.0f, 0.0f);
)";
        }

        out += "    return output;\n}\n";
        return std::vector<uint8_t>(out.begin(), out.end());
    }

    std::string TranslateShaderToHlsl(Shader& shader)
    {
        HlslShaderTranslator translator;
        auto* translation = shader.GetOrCreateTranslation(0);
        // First find which register components may hold infinities or NaN anywhere, translating until that set stops
        // growing (loops can carry a value back to earlier instructions). Shaders addressing registers by a0 or aL guard every
        // multiplication, and AR_NO_FAST_MUL does everywhere.
        static const bool guardAll = getenv("AR_NO_FAST_MUL") != nullptr;
        translator.SetGuardAllMultiplies(guardAll || shader.uses_register_dynamic_addressing());
        for (int pass = 0; pass < 16; pass++)
        {
            translator.SetAnalyzing(true);
            translator.TranslateAnalyzedShader(*translation);
            if (!translator.UnsafeChanged())
                break;
        }
        // Without jumps, calls or loops the instructions run in program order, so the final translation can follow
        // values from the start instead (registers are reused: a component may be a reciprocal at one point and a
        // texture sample later).
        translator.SetAnalyzing(false);
        if (shader.label_addresses().empty())
            translator.FollowProgramOrder();
        if (!translator.TranslateAnalyzedShader(*translation))
            return {};

        return translation->GetTranslatedBinaryString();
    }
}
