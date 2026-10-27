#pragma once

// Xenos register indices (word offsets from gpu::REGISTER_BASE) and field layouts, as documented by Xenia
// (src/xenia/gpu/register_table.inc and registers.h).
namespace gpu::reg
{
    constexpr uint32_t CP_RB_RPTR = 0x01C4;
    constexpr uint32_t CP_RB_WPTR = 0x01C5;
    constexpr uint32_t SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t SCRATCH_REG0 = 0x0578;
    constexpr uint32_t COHER_STATUS_HOST = 0x0A31;

    constexpr uint32_t RB_SURFACE_INFO = 0x2000;
    constexpr uint32_t RB_COLOR_INFO = 0x2001;
    constexpr uint32_t RB_DEPTH_INFO = 0x2002;
    constexpr uint32_t RB_COLOR1_INFO = 0x2003;
    constexpr uint32_t RB_COLOR2_INFO = 0x2004;
    constexpr uint32_t RB_COLOR3_INFO = 0x2005;
    constexpr uint32_t COHER_DEST_BASE_0 = 0x2006;
    constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E;
    constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x200F;
    constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;
    constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
    constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;
    constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100;
    constexpr uint32_t VGT_MIN_VTX_INDX = 0x2101;
    constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
    constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
    constexpr uint32_t RB_COLOR_MASK = 0x2104;
    constexpr uint32_t RB_BLEND_RED = 0x2105;
    constexpr uint32_t RB_BLEND_GREEN = 0x2106;
    constexpr uint32_t RB_BLEND_BLUE = 0x2107;
    constexpr uint32_t RB_BLEND_ALPHA = 0x2108;
    constexpr uint32_t RB_STENCILREFMASK = 0x210D;
    constexpr uint32_t RB_ALPHA_REF = 0x210E;
    constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;
    constexpr uint32_t PA_CL_VPORT_XOFFSET = 0x2110;
    constexpr uint32_t PA_CL_VPORT_YSCALE = 0x2111;
    constexpr uint32_t PA_CL_VPORT_YOFFSET = 0x2112;
    constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113;
    constexpr uint32_t PA_CL_VPORT_ZOFFSET = 0x2114;
    constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
    constexpr uint32_t SQ_CONTEXT_MISC = 0x2181;
    constexpr uint32_t SQ_INTERPOLATOR_CNTL = 0x2182;
    constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
    constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
    constexpr uint32_t RB_COLORCONTROL = 0x2202;
    constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;
    constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
    constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
    constexpr uint32_t RB_MODECONTROL = 0x2208;
    constexpr uint32_t RB_BLENDCONTROL1 = 0x2209;
    constexpr uint32_t RB_BLENDCONTROL2 = 0x220A;
    constexpr uint32_t RB_BLENDCONTROL3 = 0x220B;
    constexpr uint32_t PA_SU_POINT_SIZE = 0x2280;
    constexpr uint32_t PA_SU_POINT_MINMAX = 0x2281;
    constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;
    constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
    constexpr uint32_t VGT_DMA_BASE = 0x21FA;
    constexpr uint32_t VGT_DMA_SIZE = 0x21FB;
    constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380;
    constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381;
    constexpr uint32_t PA_SU_POLY_OFFSET_BACK_SCALE = 0x2382;
    constexpr uint32_t PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2383;
    constexpr uint32_t PA_SC_AA_CONFIG = 0x2301;
    constexpr uint32_t PA_SC_AA_MASK = 0x2312;
    constexpr uint32_t RB_COPY_CONTROL = 0x2318;
    constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
    constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
    constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
    constexpr uint32_t RB_HIZ_CLEAR = 0x231C;
    constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
    constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
    constexpr uint32_t RB_COLOR_CLEAR_LO = 0x231F;
    constexpr uint32_t RB_COPY_FUNC = 0x2320;
    constexpr uint32_t RB_COPY_REF = 0x2321;
    constexpr uint32_t RB_COPY_MASK = 0x2322;
    constexpr uint32_t RB_COPY_SURFACE_SLICE = 0x2323;
    constexpr uint32_t RB_SAMPLE_COUNT_CTL = 0x2324;
    constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;

    // Shader constants: 512 float4 (256 each for the vertex and pixel shaders), then the fetch constants
    // (32 texture fetch constants of 6 words, which alias 96 vertex fetch constants of 2 words), the boolean
    // constants (256 bits) and the loop constants (32 words).
    constexpr uint32_t SHADER_CONSTANT_000_X = 0x4000;
    constexpr uint32_t SHADER_CONSTANT_FETCH_00_0 = 0x4800;
    constexpr uint32_t SHADER_CONSTANT_BOOL_000_031 = 0x4900;
    constexpr uint32_t SHADER_CONSTANT_LOOP_00 = 0x4908;
    constexpr uint32_t REGISTER_COUNT = 0x6000;

    template<typename T>
    inline T Bits(uint32_t value, uint32_t shift, uint32_t count)
    {
        return T((value >> shift) & ((1u << count) - 1));
    }

    enum class PrimitiveType : uint32_t
    {
        None = 0x00,
        PointList = 0x01,
        LineList = 0x02,
        LineStrip = 0x03,
        TriangleList = 0x04,
        TriangleFan = 0x05,
        TriangleStrip = 0x06,
        TriangleWithWFlags = 0x07,
        RectangleList = 0x08,
        LineLoop = 0x0C,
        QuadList = 0x0D,
        QuadStrip = 0x0E,
        Polygon = 0x0F,
    };

    enum class SourceSelect : uint32_t
    {
        DMA = 0,
        Immediate = 1,
        AutoIndex = 2,
    };

    enum class EdramMode : uint32_t
    {
        NoOperation = 0,
        ColorDepth = 4,
        DepthOnly = 5,
        Copy = 6,
    };

    // VGT_DRAW_INITIATOR.
    struct DrawInitiator
    {
        uint32_t value;

        PrimitiveType PrimType() const { return Bits<PrimitiveType>(value, 0, 6); }
        SourceSelect Source() const { return Bits<SourceSelect>(value, 6, 2); }
        bool Index32() const { return (value >> 11) & 1; }
        uint32_t NumIndices() const { return value >> 16; }
    };

    // RB_SURFACE_INFO.
    struct SurfaceInfo
    {
        uint32_t value;

        uint32_t Pitch() const { return value & 0x3FFF; }
        uint32_t MsaaSamples() const { return (value >> 16) & 3; } // 0: 1x, 1: 2x, 2: 4x.
    };

    // RB_COLOR_INFO / RB_COLORn_INFO.
    struct ColorInfo
    {
        uint32_t value;

        uint32_t Base() const { return value & 0xFFF; } // EDRAM tiles of 80x16 samples (5120 bytes).
        uint32_t Format() const { return (value >> 16) & 0xF; }
        int32_t ExpBias() const { return int32_t(value << 6) >> 26; }
    };

    // RB_DEPTH_INFO.
    struct DepthInfo
    {
        uint32_t value;

        uint32_t Base() const { return value & 0xFFF; }
        uint32_t Format() const { return (value >> 16) & 1; } // 0: D24S8, 1: D24FS8.
    };

    // RB_COPY_CONTROL.
    struct CopyControl
    {
        uint32_t value;

        uint32_t SourceSelect() const { return value & 7; } // Color target 0-3, or 4 for depth.
        uint32_t SampleSelect() const { return (value >> 4) & 7; }
        bool ColorClear() const { return (value >> 8) & 1; }
        bool DepthClear() const { return (value >> 9) & 1; }
        uint32_t Command() const { return (value >> 20) & 3; }
    };

    // RB_COPY_DEST_INFO.
    struct CopyDestInfo
    {
        uint32_t value;

        uint32_t Endian() const { return value & 7; }
        uint32_t Format() const { return (value >> 7) & 0x3F; } // ColorFormat, same values as TextureFormat.
        int32_t ExpBias() const { return int32_t(value << 10) >> 26; }
        bool Swap() const { return (value >> 24) & 1; }
    };

    // RB_COPY_DEST_PITCH.
    struct CopyDestPitch
    {
        uint32_t value;

        uint32_t Pitch() const { return value & 0x3FFF; }
        uint32_t Height() const { return (value >> 16) & 0x3FFF; }
    };
}
