// Kelvin, the NV2A's 3D class (0x97): register state as the push buffer sets it, and the draws it
// makes. Method numbers follow the nv097 names used by nouveau and xemu.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace gpu::kelvin {

enum : uint32_t {
    NO_OPERATION = 0x0100,
    WAIT_FOR_IDLE = 0x0110,
    SET_FLIP_READ = 0x0120,
    SET_FLIP_WRITE = 0x0124,
    SET_FLIP_MODULO = 0x0128,
    FLIP_INCREMENT_WRITE = 0x012C,
    FLIP_STALL = 0x0130,
    SET_CONTEXT_DMA_NOTIFIES = 0x0180,
    SET_CONTEXT_DMA_A = 0x0184,
    SET_CONTEXT_DMA_B = 0x0188,
    SET_CONTEXT_DMA_STATE = 0x0190,
    SET_CONTEXT_DMA_COLOR = 0x0194,
    SET_CONTEXT_DMA_ZETA = 0x0198,
    SET_CONTEXT_DMA_VERTEX_A = 0x019C,
    SET_CONTEXT_DMA_VERTEX_B = 0x01A0,
    SET_CONTEXT_DMA_SEMAPHORE = 0x01A4,
    SET_CONTEXT_DMA_REPORT = 0x01A8,
    SET_SURFACE_CLIP_HORIZONTAL = 0x0200,
    SET_SURFACE_CLIP_VERTICAL = 0x0204,
    SET_SURFACE_FORMAT = 0x0208,
    SET_SURFACE_PITCH = 0x020C,
    SET_SURFACE_COLOR_OFFSET = 0x0210,
    SET_SURFACE_ZETA_OFFSET = 0x0214,
    SET_COMBINER_ALPHA_ICW = 0x0260,  // 8 stages
    SET_COMBINER_SPECULAR_FOG_CW0 = 0x0288,
    SET_COMBINER_SPECULAR_FOG_CW1 = 0x028C,
    SET_CONTROL0 = 0x0290,  // bit 16: W-buffering (Z_PERSPECTIVE_ENABLE)
    SET_FOG_MODE = 0x029C,
    SET_FOG_GEN_MODE = 0x02A0,
    SET_FOG_ENABLE = 0x02A4,
    SET_FOG_COLOR = 0x02A8,
    SET_WINDOW_CLIP_TYPE = 0x02B4,
    SET_WINDOW_CLIP_HORIZONTAL = 0x02C0,
    SET_WINDOW_CLIP_VERTICAL = 0x02E0,
    SET_ALPHA_TEST_ENABLE = 0x0300,
    SET_BLEND_ENABLE = 0x0304,
    SET_CULL_FACE_ENABLE = 0x0308,
    SET_DEPTH_TEST_ENABLE = 0x030C,
    SET_DITHER_ENABLE = 0x0310,
    SET_LIGHTING_ENABLE = 0x0314,
    SET_STENCIL_TEST_ENABLE = 0x032C,
    SET_POLY_OFFSET_FILL_ENABLE = 0x0338,
    SET_ALPHA_FUNC = 0x033C,
    SET_ALPHA_REF = 0x0340,
    SET_BLEND_FUNC_SFACTOR = 0x0344,
    SET_BLEND_FUNC_DFACTOR = 0x0348,
    SET_BLEND_COLOR = 0x034C,
    SET_BLEND_EQUATION = 0x0350,
    SET_DEPTH_FUNC = 0x0354,
    SET_COLOR_MASK = 0x0358,
    SET_DEPTH_MASK = 0x035C,
    SET_STENCIL_MASK = 0x0360,
    SET_STENCIL_FUNC = 0x0364,
    SET_STENCIL_FUNC_REF = 0x0368,
    SET_STENCIL_FUNC_MASK = 0x036C,
    SET_STENCIL_OP_FAIL = 0x0370,
    SET_STENCIL_OP_ZFAIL = 0x0374,
    SET_STENCIL_OP_ZPASS = 0x0378,
    SET_SHADE_MODE = 0x037C,
    SET_POLYGON_OFFSET_SCALE_FACTOR = 0x0384,
    SET_POLYGON_OFFSET_BIAS = 0x0388,
    SET_FRONT_POLYGON_MODE = 0x038C,
    SET_BACK_POLYGON_MODE = 0x0390,
    SET_CLIP_MIN = 0x0394,
    SET_CLIP_MAX = 0x0398,
    SET_CULL_FACE = 0x039C,
    SET_FRONT_FACE = 0x03A0,
    SET_COMPOSITE_MATRIX = 0x0680,  // 16 floats
    SET_FOG_PARAMS = 0x09C0,        // 3 floats
    SET_VIEWPORT_OFFSET = 0x0A20,
    SET_COMBINER_FACTOR0 = 0x0A60,  // 8 stages
    SET_COMBINER_FACTOR1 = 0x0A80,
    SET_COMBINER_ALPHA_OCW = 0x0AA0,
    SET_COMBINER_COLOR_ICW = 0x0AC0,
    SET_VIEWPORT_SCALE = 0x0AF0,
    SET_TRANSFORM_PROGRAM = 0x0B00,   // 32 words, appended at the load slot
    SET_TRANSFORM_CONSTANT = 0x0B80,  // 32 words, appended at the constant load slot
    SET_VERTEX_DATA_ARRAY_OFFSET = 0x1720,  // 16 attributes
    SET_VERTEX_DATA_ARRAY_FORMAT = 0x1760,
    SET_LOGIC_OP_ENABLE = 0x17BC,
    SET_TWO_SIDE_LIGHT_EN = 0x17C4,
    SET_SHADER_CLIP_PLANE_MODE = 0x17F8,
    SET_BEGIN_END = 0x17FC,
    ARRAY_ELEMENT16 = 0x1800,
    ARRAY_ELEMENT32 = 0x1808,
    DRAW_ARRAYS = 0x1810,
    INLINE_ARRAY = 0x1818,
    SET_VERTEX_DATA2F_M = 0x1880,  // 16 attributes x 2 words
    SET_VERTEX_DATA2S = 0x1900,    // 16 x 1
    SET_VERTEX_DATA4UB = 0x1940,   // 16 x 1
    SET_VERTEX_DATA4S_M = 0x1980,  // 16 x 2
    SET_VERTEX_DATA4F_M = 0x1A00,  // 16 x 4
    SET_TEXTURE_OFFSET = 0x1B00,   // 4 stages, 0x40 apart
    SET_TEXTURE_FORMAT = 0x1B04,
    SET_TEXTURE_ADDRESS = 0x1B08,
    SET_TEXTURE_CONTROL0 = 0x1B0C,
    SET_TEXTURE_CONTROL1 = 0x1B10,
    SET_TEXTURE_FILTER = 0x1B14,
    SET_TEXTURE_IMAGE_RECT = 0x1B1C,
    SET_TEXTURE_PALETTE = 0x1B20,
    SET_TEXTURE_BORDER_COLOR = 0x1B24,
    SET_TEXTURE_BUMP_ENV_MAT = 0x1B28,
    CLEAR_REPORT_VALUE = 0x17C8,
    SET_SEMAPHORE_OFFSET = 0x1D6C,
    GET_REPORT = 0x17D0,  // offset in the report DMA (low 24 bits), type (high 8): 1 = z-pass pixel count
    SET_ZPASS_PIXEL_COUNT_ENABLE = 0x17CC,
    BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1D70,
    SET_ZSTENCIL_CLEAR_VALUE = 0x1D8C,
    SET_COLOR_CLEAR_VALUE = 0x1D90,
    CLEAR_SURFACE = 0x1D94,
    SET_CLEAR_RECT_HORIZONTAL = 0x1D98,
    SET_CLEAR_RECT_VERTICAL = 0x1D9C,
    SET_SPECULAR_FOG_FACTOR = 0x1E20,  // 2 words
    SET_COMBINER_COLOR_OCW = 0x1E40,   // 8 stages
    SET_COMBINER_CONTROL = 0x1E60,
    SET_SHADOW_COMPARE_FUNC = 0x1E6C,  // depth textures: r/q <op> texel, ops as the alpha test's
    SET_SHADER_STAGE_PROGRAM = 0x1E70,
    SET_DOT_RGBMAPPING = 0x1E74,
    SET_SHADER_OTHER_STAGE_INPUT = 0x1E78,
    SET_TRANSFORM_EXECUTION_MODE = 0x1E94,
    SET_TRANSFORM_PROGRAM_CXT_WRITE_EN = 0x1E98,
    SET_TRANSFORM_PROGRAM_LOAD = 0x1E9C,
    SET_TRANSFORM_PROGRAM_START = 0x1EA0,
    SET_TRANSFORM_CONSTANT_LOAD = 0x1EA4,
};

constexpr uint32_t kControl0WBuffer = 1u << 16;
constexpr uint32_t kTextureAlphaKill = 1u << 2;  // SET_TEXTURE_CONTROL0

// SET_BEGIN_END primitives
enum : uint32_t { kEnd = 0, kPoints, kLines, kLineLoop, kLineStrip, kTriangles, kTriangleStrip, kTriangleFan, kQuads,
                  kQuadStrip, kPolygon };

// vertex attribute formats (SET_VERTEX_DATA_ARRAY_FORMAT type)
enum : uint32_t { kUbD3d = 0, kS1 = 1, kFloat = 2, kUbOgl = 4, kS32k = 5, kCmp = 6 };

constexpr int kAttributes = 16;
constexpr int kProgramSlots = 136;
constexpr int kConstants = 192;

struct State {
    uint32_t reg[0x2000 / 4] = {};  // last value of every method, by method / 4
    uint32_t program[kProgramSlots][4] = {};
    uint32_t program_load = 0, program_word = 0;
    float constants[kConstants][4] = {};
    uint32_t constant_load = 0, constant_word = 0;
    float attribute_value[kAttributes][4] = {};  // for attributes without an array
    uint64_t program_generation = 0;  // performance: advances when the program, its start or the mode change

    uint32_t Reg(uint32_t method) const { return reg[method / 4]; }
    float RegF(uint32_t method) const {
        float f;
        uint32_t v = reg[method / 4];
        std::memcpy(&f, &v, 4);
        return f;
    }
};

// one draw: vertices converted to float4 per enabled attribute, in submission order
struct Draw {
    uint32_t primitive = 0;
    uint16_t enabled = 0;  // attribute mask
    int stride = 0;        // floats per vertex, 4 per enabled attribute
    mutable std::vector<float> vertices;  // raw draws: empty until Vertices() converts them
    uint32_t count = 0;
    // ---- performance: raw draws (vertex arrays in memory) keep the attributes as the guest stores them; the
    // vertex shader decodes them (vsh::Generate with formats), the renderer draws them indexed ----
    bool raw = false;
    uint8_t format[kAttributes] = {};    // per enabled attribute: SET_VERTEX_DATA_ARRAY_FORMAT bits 0-7
    const uint8_t* data[kAttributes] = {};  // the attribute of the first vertex (number `first`)
    uint32_t data_stride[kAttributes] = {};
    uint32_t first = 0, span = 0;  // the vertex numbers used are first .. first + span - 1
    std::vector<uint32_t> ids;     // per vertex in submission order: its number - first
    void Reset() {  // as constructed, keeping the vectors' memory
        primitive = 0, enabled = 0, stride = 0, count = 0, raw = false, first = 0, span = 0;
        vertices.clear(), ids.clear();
    }
};

// a draw's vertices as float4 per enabled attribute (raw draws are converted on the first call)
const std::vector<float>& Vertices(const Draw& d);
// the bytes a raw attribute of this format reads per vertex (the vertex shader's fetch), 0 if it cannot be raw
uint32_t RawBytes(uint8_t format);

void Method(uint32_t subchannel, uint32_t method, uint32_t data);
// performance: a method with count data words at once (index and inline vertex data in bulk); the same as
// Method for each word with method advancing by 4 unless non_increasing
void Methods(uint32_t subchannel, uint32_t method, const uint32_t* words, uint32_t count, bool non_increasing);
const State& Current();
uint32_t ResolveDma(uint32_t handle);  // DMA object handle -> physical base address

// HLE graphics (xdk/d3d8.cpp): the XDK's push buffer is decoded on the host without the NV2A front end. DMA
// objects then do not exist in instance memory, software methods (NOPs with a parameter) go to the HLE layer
// instead of raising an interrupt, and flips present directly.
struct HleHooks {
    uint32_t (*resolve_dma)(uint32_t handle) = nullptr;
    void (*software_method)(uint32_t data) = nullptr;
};
void SetHle(const HleHooks& hooks);
// performance: a frame went to the display now (frame time statistics in the "render: N fps" lines)
void FramePresented();
// performance: time spent decoding/drawing (0) and presenting (1), QueryPerformanceCounter ticks, shown per frame
enum class Work { kDecode, kPresent, kIdle, kLimit, kCount };  // kIdle: the render thread waited for work, kLimit: fps_limit
void AddWork(Work what, int64_t ticks);
// the presenting guest thread's totals at a flip: ticks it waited for the CPU (and how often), for the render thread
void GuestWaits(int64_t cpu_ticks, uint64_t cpu_waits, int64_t render_ticks, int64_t kernel_ticks = 0,
                int64_t submit_ticks = 0);
// performance, FCI_TRACE_HITCHES: the guest's last frame (flip to flip) and its waits, for the render side's log
void GuestFrame(int64_t frame_ticks, int64_t cpu_wait_ticks, int64_t render_wait_ticks, const char* detail);
bool Hle();

}  // namespace gpu::kelvin
