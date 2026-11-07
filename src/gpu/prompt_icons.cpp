// Button prompts for the input device in use (GitHub #15): our own copy of the font page that holds the game's
// button glyphs, with each glyph drawn anew for the device (see gpu/prompt_icons.h). The shapes are signed distance
// functions with one pixel of antialiasing; key names come from GDI (a system font, white on black as coverage).
#include "gpu/prompt_icons.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/texture_pack.h"
#include "hle/host_pad.h"
#include "hle/pc_input.h"
#include "hle/prompts.h"
#include "runtime/log.h"

namespace gpu::prompt_icons {

namespace {

using Microsoft::WRL::ComPtr;
using hle::prompts::Style;

// The page: 256x128 DXT3 (NV2A color 0x0E), one level, of the condensed font the prompts, menus' footers and hints
// use. Its name is texpack::ContentKey (texture dumps name it the same: textures\dump\9c901965508be13d.png).
constexpr uint64_t kPageKey = 0x9c901965508be13dull;
constexpr uint32_t kPageWidth = 256, kPageHeight = 128;

// what a glyph stands for (the strings' tokens: {A} {B} {X} {Y} {WH} {BL} {TRIGL} {TRIGR} {START} {BACK} {ASL}
// {ASR} {DPU} {DPD} {DPL} {DPR} {DPLR} {DPUD})
enum Role : uint8_t {
    kA, kB, kX, kY, kWhite, kBlack, kLT, kRT, kStart, kBack, kLS, kRS,
    kUp, kDown, kLeft, kRight, kLeftRight, kUpDown, kAll, kUpLeftRight, kDownLeftRight, kDpad,
};
// the binding (bind_<name>, pad_bind_<name>) behind each single role
const char* const kTargetOf[] = {"a", "b", "x", "y", "white", "black", "lt", "rt", "start", "back", "lthumb", "rthumb",
                                 "up", "down", "left", "right"};

// The glyphs' boxes on the page (texels, inclusive), found from the alpha of the dumped page. The sticks and
// triggers come with the language's letter (L/R, G/D, I/D, S/D): all of them stand for the same stick or trigger.
// Left alone: the unlabelled stick and trigger (no token found that uses them) and the HUD's icons on the page.
struct Cell {
    uint8_t x0, y0, x1, y1;
    Role role;
};
constexpr Cell kCells[] = {
    {27, 2, 50, 25, kUpLeftRight},  {52, 2, 75, 25, kDpad},     {123, 2, 142, 21, kB},  {145, 2, 164, 21, kWhite},
    {100, 3, 121, 23, kLS},         {145, 24, 164, 43, kBlack}, {123, 25, 142, 44, kX}, {77, 26, 98, 46, kRS},
    {100, 26, 121, 46, kLS},        {183, 27, 200, 50, kLT},    {27, 29, 50, 52, kAll}, {52, 29, 75, 52, kLeftRight},
    {145, 45, 162, 68, kLT},        {164, 45, 181, 68, kRT},    {123, 48, 142, 67, kY}, {77, 50, 98, 70, kLS},
    {100, 50, 121, 70, kRS},        {184, 53, 201, 76, kLT},    {27, 56, 50, 79, kDownLeftRight},
    {52, 56, 75, 79, kLeft},        {124, 71, 143, 90, kStart}, {165, 71, 182, 94, kLT}, {101, 74, 122, 94, kLS},
    {184, 79, 201, 102, kLT},       {76, 82, 99, 105, kRight},  {1, 83, 24, 106, kUpDown}, {26, 83, 49, 106, kDown},
    {51, 83, 74, 106, kUp},         {144, 93, 163, 112, kBack}, {165, 97, 182, 120, kRT}, {101, 99, 120, 118, kA},
};

// ---- colours: the game's own icons are dark grey caps with a black rim; its D-pad arrows are orange ----
struct Color {
    float r, g, b;
};
constexpr Color kRim{0.07f, 0.07f, 0.06f};
constexpr Color kCap{0.30f, 0.30f, 0.28f};
constexpr Color kCapTop{0.40f, 0.40f, 0.37f};
constexpr Color kLabel{0.95f, 0.94f, 0.88f};
constexpr Color kAccent{0.95f, 0.58f, 0.12f};
constexpr Color kDisc{0.12f, 0.12f, 0.13f};
constexpr Color kDiscRim{0.62f, 0.62f, 0.60f};
constexpr Color kPsCross{0.49f, 0.64f, 0.96f}, kPsCircle{0.96f, 0.38f, 0.42f}, kPsSquare{0.90f, 0.55f, 0.82f},
    kPsTriangle{0.27f, 0.82f, 0.66f};

// ---- the canvas: straight alpha, float ----
struct Canvas {
    int w = 0, h = 0;
    std::vector<std::array<float, 4>> px;  // r g b a
    std::array<float, 4>& At(int x, int y) { return px[size_t(y) * w + x]; }
    // colour over what is there, with this coverage
    void Blend(int x, int y, Color c, float a) {
        if (a <= 0 || x < 0 || y < 0 || x >= w || y >= h) return;
        a = std::min(a, 1.0f);
        auto& d = At(x, y);
        float out = a + d[3] * (1 - a);
        if (out <= 0) return;
        float k = d[3] * (1 - a);
        d[0] = (c.r * a + d[0] * k) / out, d[1] = (c.g * a + d[1] * k) / out, d[2] = (c.b * a + d[2] * k) / out;
        d[3] = out;
    }
};

// a box in canvas pixels, x0/y0 inclusive, x1/y1 exclusive
struct Box {
    float x0, y0, x1, y1;
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
    float Cx() const { return (x0 + x1) / 2; }
    float Cy() const { return (y0 + y1) / 2; }
    Box Inset(float d) const { return {x0 + d, y0 + d, x1 - d, y1 - d}; }
};

// fills where sd (signed distance in pixels, < 0 inside) is inside, over the box's pixels
template <class F>
void Fill(Canvas& c, const Box& b, Color col, F sd, float alpha = 1) {
    int x0 = std::max(0, int(std::floor(b.x0)) - 1), x1 = std::min(c.w, int(std::ceil(b.x1)) + 1);
    int y0 = std::max(0, int(std::floor(b.y0)) - 1), y1 = std::min(c.h, int(std::ceil(b.y1)) + 1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) c.Blend(x, y, col, std::clamp(0.5f - sd(x + 0.5f, y + 0.5f), 0.0f, 1.0f) * alpha);
}

float RoundRect(float x, float y, const Box& b, float r) {
    r = std::min({r, b.W() / 2, b.H() / 2});
    float qx = std::fabs(x - b.Cx()) - (b.W() / 2 - r), qy = std::fabs(y - b.Cy()) - (b.H() / 2 - r);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

float Circle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }

float Segment(float x, float y, float ax, float ay, float bx, float by, float half_width) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float t = std::clamp((px * dx + py * dy) / std::max(dx * dx + dy * dy, 1e-6f), 0.0f, 1.0f);
    return std::hypot(px - dx * t, py - dy * t) - half_width;
}

// a convex polygon given clockwise in screen space (y down): the largest of the edges' distances
float Polygon(float x, float y, const float (*p)[2], int n) {
    float d = -1e9f;
    for (int i = 0; i < n; i++) {
        const float* a = p[i];
        const float* b = p[(i + 1) % n];
        float ex = b[0] - a[0], ey = b[1] - a[1], len = std::max(std::hypot(ex, ey), 1e-6f);
        // outward normal of a clockwise (screen) polygon: (ey, -ex)
        d = std::max(d, ((x - a[0]) * ey - (y - a[1]) * ex) / len);
    }
    return d;
}

// ---- text: GDI's antialiased glyphs of a system font as coverage ----

std::mutex g_gdi_lock;

// the label centred in the box, as large as fits (height share hs of the box), condensed before it shrinks
void Text(Canvas& c, const Box& b, const std::string& label, Color col, float hs = 0.62f) {
    const int w = std::max(1, int(std::lround(b.W()))), h = std::max(1, int(std::lround(b.H())));
    std::lock_guard lock(g_gdi_lock);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HGDIOBJ old_font = GetCurrentObject(dc, OBJ_FONT);
    std::memset(bits, 0, size_t(w) * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::wstring text(label.begin(), label.end());
    const float avail = w * 0.86f;
    int height = std::max(4, int(h * hs));
    HFONT font = nullptr;
    SIZE size{};
    TEXTMETRICW tm{};
    for (int attempt = 0; attempt < 12 && !font; attempt++) {
        LOGFONTW lf{};
        lf.lfHeight = -height;  // character height (cap height plus descender room)
        lf.lfWeight = FW_BOLD;
        lf.lfQuality = ANTIALIASED_QUALITY;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Arial");
        HFONT f = CreateFontIndirectW(&lf);
        SelectObject(dc, f);
        GetTextMetricsW(dc, &tm);
        GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
        if (size.cx > avail) {
            // condensed: narrower characters, down to 60% of their width
            float squeeze = std::max(0.6f, avail / float(size.cx));
            lf.lfWidth = std::max(1, int(tm.tmAveCharWidth * squeeze));
            SelectObject(dc, old_font);
            DeleteObject(f);
            f = CreateFontIndirectW(&lf);
            SelectObject(dc, f);
            GetTextMetricsW(dc, &tm);
            GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
        }
        if (size.cx <= avail || height <= 4) {
            font = f;
        } else {
            SelectObject(dc, old_font);
            DeleteObject(f);
            height = std::max(4, int(height * 0.88f));
        }
    }
    if (font) {
        // capitals centred: their height is the ascent without the internal leading
        int cap = tm.tmAscent - tm.tmInternalLeading;
        int y = (h - cap) / 2 - tm.tmInternalLeading;
        TextOutW(dc, (w - size.cx) / 2, y, text.c_str(), int(text.size()));
        GdiFlush();
        const uint8_t* p = static_cast<const uint8_t*>(bits);
        const int ox = int(std::lround(b.x0)), oy = int(std::lround(b.y0));
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
                const uint8_t* q = p + (size_t(yy) * w + xx) * 4;
                float a = std::max({q[0], q[1], q[2]}) / 255.0f;
                if (a > 0) c.Blend(ox + xx, oy + yy, col, a);
            }
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---- the icons ----

// a key cap filling the box: black rim, grey cap with a lighter top face (a key seen from slightly above)
void KeyCap(Canvas& c, const Box& b, float s) {
    float r = 3.2f * s;
    Fill(c, b, kRim, [&](float x, float y) { return RoundRect(x, y, b, r); });
    Box body = b.Inset(1.0f * s);
    Fill(c, body, kCap, [&](float x, float y) { return RoundRect(x, y, body, r - s); });
    Box top{body.x0 + 0.9f * s, body.y0 + 0.6f * s, body.x1 - 0.9f * s, body.y1 - 2.0f * s};
    Fill(c, top, kCapTop, [&](float x, float y) { return RoundRect(x, y, top, r - 1.5f * s); });
}

// the face of a cap, where its label goes
Box KeyFace(const Box& b, float s) { return {b.x0 + 1.9f * s, b.y0 + 1.6f * s, b.x1 - 1.9f * s, b.y1 - 3.0f * s}; }

// a filled triangle pointing in a direction (0 up, 1 down, 2 left, 3 right) around a centre
void Arrow(Canvas& c, float cx, float cy, float size, int dir, Color col) {
    float h = size * 0.5f;
    float p[3][2];
    switch (dir) {
    case 0: p[0][0] = cx, p[0][1] = cy - h, p[1][0] = cx + h, p[1][1] = cy + h * 0.6f, p[2][0] = cx - h, p[2][1] = cy + h * 0.6f; break;
    case 1: p[0][0] = cx, p[0][1] = cy + h, p[1][0] = cx - h, p[1][1] = cy - h * 0.6f, p[2][0] = cx + h, p[2][1] = cy - h * 0.6f; break;
    case 2: p[0][0] = cx - h, p[0][1] = cy, p[1][0] = cx + h * 0.6f, p[1][1] = cy - h, p[2][0] = cx + h * 0.6f, p[2][1] = cy + h; break;
    default: p[0][0] = cx + h, p[0][1] = cy, p[1][0] = cx - h * 0.6f, p[1][1] = cy + h, p[2][0] = cx - h * 0.6f, p[2][1] = cy - h; break;
    }
    Box bb{cx - h - 1, cy - h - 1, cx + h + 1, cy + h + 1};
    Fill(c, bb, col, [&](float x, float y) { return Polygon(x, y, p, 3); });
}

// a cap with arrows for these directions (the arrow keys): one large arrow, or the D-pad's cross of small ones
void ArrowCap(Canvas& c, const Box& b, float s, const std::vector<int>& dirs) {
    KeyCap(c, b, s);
    Box f = KeyFace(b, s);
    float m = std::min(f.W(), f.H());
    if (dirs.size() == 1) {
        Arrow(c, f.Cx(), f.Cy(), m * 0.62f, dirs[0], kLabel);
        return;
    }
    if (dirs.size() == 2 && ((dirs[0] >= 2) == (dirs[1] >= 2))) {  // left/right or up/down: side by side
        bool horizontal = dirs[0] >= 2;
        float off = m * 0.24f;
        for (int d : dirs) {
            float sign = (d == 0 || d == 2) ? -1.0f : 1.0f;
            Arrow(c, f.Cx() + (horizontal ? sign * off : 0), f.Cy() + (horizontal ? 0 : sign * off), m * 0.42f, d, kLabel);
        }
        return;
    }
    const float off = m * 0.29f;
    const float dx[] = {0, 0, -off, off}, dy[] = {-off, off, 0, 0};
    for (int d : dirs) Arrow(c, f.Cx() + dx[d], f.Cy() + dy[d], m * 0.34f, d, kLabel);
}

// a mouse with the bound button lit: 1 left, 2 right, 3 middle (wheel), 4 wheel up, 5 wheel down
void Mouse(Canvas& c, const Box& b, float s, int button) {
    float w = std::min(b.W() * 0.86f, b.H() * 0.68f), h = std::min(b.H() * 0.96f, w * 1.45f);
    Box body{b.Cx() - w / 2, b.Cy() - h / 2, b.Cx() + w / 2, b.Cy() + h / 2};
    float r = w * 0.46f;
    Fill(c, body, kRim, [&](float x, float y) { return RoundRect(x, y, body, r); });
    Box in = body.Inset(1.0f * s);
    float split = in.y0 + in.H() * 0.42f;
    auto inside = [&](float x, float y) { return RoundRect(x, y, in, r - s); };
    Fill(c, in, kCap, inside);
    if (button == 1 || button == 2) {
        // the lit half of the top: inside the body, above the split, on that side of the middle
        Fill(c, in, kAccent, [&](float x, float y) {
            float side = button == 1 ? x - in.Cx() : in.Cx() - x;
            return std::max({inside(x, y), y - split, side});
        });
    }
    // the seams: across under the buttons, down between them
    float seam = std::max(0.5f, 0.45f * s);
    Fill(c, in, kRim, [&](float x, float y) { return std::max(std::fabs(y - split) - seam, inside(x, y)); });
    Fill(c, in, kRim, [&](float x, float y) { return std::max({std::fabs(x - in.Cx()) - seam, y - split, inside(x, y)}); });
    // the wheel
    float ww = std::max(1.6f * s, w * 0.2f), wh = in.H() * 0.24f;
    Box wheel{in.Cx() - ww / 2, in.y0 + in.H() * 0.1f, in.Cx() + ww / 2, in.y0 + in.H() * 0.1f + wh};
    bool lit = button >= 3;
    Fill(c, wheel, lit ? kAccent : kCapTop, [&](float x, float y) { return RoundRect(x, y, wheel, ww / 2); });
    if (button == 4 || button == 5) Arrow(c, in.Cx(), split + (in.y1 - split) * 0.48f, w * 0.42f, button == 4 ? 0 : 1, kAccent);
}

// a round button: dark disc with a light rim
void Disc(Canvas& c, const Box& b, float s) {
    float r = std::min(b.W(), b.H()) / 2 - 0.3f * s;
    Fill(c, b, kRim, [&](float x, float y) { return Circle(x, y, b.Cx(), b.Cy(), r); });
    Fill(c, b, kDiscRim, [&](float x, float y) { return Circle(x, y, b.Cx(), b.Cy(), r - 0.9f * s); });
    Fill(c, b, kDisc, [&](float x, float y) { return Circle(x, y, b.Cx(), b.Cy(), r - 1.9f * s); });
}

// a round button with a label
void LabelDisc(Canvas& c, const Box& b, float s, const std::string& label) {
    Disc(c, b, s);
    float r = std::min(b.W(), b.H()) / 2;
    Box inner{b.Cx() - r * 0.72f, b.Cy() - r * 0.72f, b.Cx() + r * 0.72f, b.Cy() + r * 0.72f};
    Text(c, inner, label, kLabel, label.size() == 1 ? 0.78f : 0.62f);
}

// a shoulder button or pill (start/select): dark rounded box with a label
void LabelPill(Canvas& c, const Box& b, float s, const std::string& label) {
    float h = std::min(b.H(), std::max(b.W() * 0.62f, 8.0f * s));
    Box p{b.x0, b.Cy() - h / 2, b.x1, b.Cy() + h / 2};
    float r = h * 0.38f;
    Fill(c, p, kRim, [&](float x, float y) { return RoundRect(x, y, p, r); });
    Box in = p.Inset(0.9f * s);
    Fill(c, in, kDiscRim, [&](float x, float y) { return RoundRect(x, y, in, r - 0.9f * s); });
    Box in2 = in.Inset(0.9f * s);
    Fill(c, in2, kDisc, [&](float x, float y) { return RoundRect(x, y, in2, r - 1.8f * s); });
    Text(c, in2.Inset(0.3f * s), label, kLabel, 0.72f);
}

// PlayStation's face buttons: the symbol in its colour on a dark disc
void PsFace(Canvas& c, const Box& b, float s, int which) {  // 0 cross, 1 circle, 2 square, 3 triangle
    Disc(c, b, s);
    float r = std::min(b.W(), b.H()) / 2;
    float k = r * 0.42f, line = std::max(0.7f, 0.85f * s);
    float cx = b.Cx(), cy = b.Cy();
    switch (which) {
    case 0:
        Fill(c, b, kPsCross, [&](float x, float y) {
            return std::min(Segment(x, y, cx - k, cy - k, cx + k, cy + k, line), Segment(x, y, cx - k, cy + k, cx + k, cy - k, line));
        });
        break;
    case 1:
        Fill(c, b, kPsCircle, [&](float x, float y) { return std::fabs(Circle(x, y, cx, cy, k * 1.05f)) - line; });
        break;
    case 2: {
        Box sq{cx - k * 0.9f, cy - k * 0.9f, cx + k * 0.9f, cy + k * 0.9f};
        Fill(c, b, kPsSquare, [&](float x, float y) { return std::fabs(RoundRect(x, y, sq, 0.3f * s)) - line; });
        break;
    }
    default: {
        const float p[3][2] = {{cx, cy - k * 1.1f}, {cx + k * 1.05f, cy + k * 0.72f}, {cx - k * 1.05f, cy + k * 0.72f}};
        Fill(c, b, kPsTriangle, [&](float x, float y) { return std::fabs(Polygon(x, y, p, 3)) - line; });
        break;
    }
    }
}

// ---- what a role shows ----

// The bindings the icons show, gathered on the renderer's thread (the pages are drawn on a worker): per Xbox
// button of kTargetOf the first key (bind_<name>, 0 for none) and the first controller button (pad_bind_<name>,
// its ini name: "a", "lb", "lt", ...; empty for none)
constexpr int kTargets = int(std::size(kTargetOf));
struct Inputs {
    int keys[kTargets] = {};
    std::string pad[kTargets];
    int generation = 4;  // PlayStation 3/4/5
};

Inputs Gather() {
    Inputs in;
    for (int t = 0; t < kTargets; t++) {
        for (int i = 0; i < hle::pc::BindingCount(); i++)
            if (!std::strcmp(hle::pc::BindingName(i), kTargetOf[t])) {
                std::vector<int> keys = hle::pc::BindingKeys(i);
                in.keys[t] = keys.empty() ? 0 : keys[0];
            }
        for (int i = 0; i < hle::host::BindingCount(); i++)
            if (!std::strcmp(hle::host::BindingName(i), kTargetOf[t])) {
                std::vector<int> buttons = hle::host::BindingButtons(i);
                if (!buttons.empty()) in.pad[t] = hle::host::ButtonName(buttons[0]);
            }
    }
    in.generation = hle::prompts::PlayStationGeneration();
    return in;
}

int ArrowDir(int vk) { return vk == VK_UP ? 0 : vk == VK_DOWN ? 1 : vk == VK_LEFT ? 2 : vk == VK_RIGHT ? 3 : -1; }
int MouseButton(int vk) {
    if (vk == VK_LBUTTON) return 1;
    if (vk == VK_RBUTTON) return 2;
    if (vk == VK_MBUTTON) return 3;
    if (vk == hle::pc::WheelCode(true)) return 4;
    if (vk == hle::pc::WheelCode(false)) return 5;
    return 0;
}

// what a key cap says: short names in capitals ("SPACE", "SHIFT", "ESC", "F5", "E")
std::string KeyText(int vk) {
    struct Short {
        int vk;
        const char* text;
    };
    static const Short kShort[] = {
        {VK_SPACE, "SPACE"}, {VK_RETURN, "ENTER"}, {VK_ESCAPE, "ESC"}, {VK_TAB, "TAB"}, {VK_BACK, "BKSP"},
        {VK_CAPITAL, "CAPS"}, {VK_SHIFT, "SHIFT"}, {VK_LSHIFT, "SHIFT"}, {VK_RSHIFT, "RSHIFT"}, {VK_CONTROL, "CTRL"},
        {VK_LCONTROL, "CTRL"}, {VK_RCONTROL, "RCTRL"}, {VK_MENU, "ALT"}, {VK_LMENU, "ALT"}, {VK_RMENU, "ALT GR"},
        {VK_INSERT, "INS"}, {VK_DELETE, "DEL"}, {VK_HOME, "HOME"}, {VK_END, "END"}, {VK_PRIOR, "PG UP"},
        {VK_NEXT, "PG DN"}, {VK_OEM_MINUS, "-"}, {VK_OEM_PLUS, "="}, {VK_OEM_COMMA, ","}, {VK_OEM_PERIOD, "."},
        {VK_OEM_2, "/"}, {VK_OEM_1, ";"}, {VK_OEM_7, "'"}, {VK_OEM_4, "["}, {VK_OEM_6, "]"}, {VK_OEM_5, "\\"},
        {VK_OEM_3, "`"}, {VK_MULTIPLY, "NUM *"}, {VK_ADD, "NUM +"}, {VK_SUBTRACT, "NUM -"}, {VK_DECIMAL, "NUM ."},
        {VK_DIVIDE, "NUM /"}, {VK_XBUTTON1, "M4"}, {VK_XBUTTON2, "M5"},
    };
    for (const Short& k : kShort)
        if (k.vk == vk) return k.text;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
    std::string name = hle::pc::KeyName(vk);
    for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return name;
}

// the directions of the D-pad roles (0 up, 1 down, 2 left, 3 right), in the order a label lists them (W A S D)
std::vector<int> Directions(Role r) {
    switch (r) {
    case kUp: return {0};
    case kDown: return {1};
    case kLeft: return {2};
    case kRight: return {3};
    case kLeftRight: return {2, 3};
    case kUpDown: return {0, 1};
    case kUpLeftRight: return {0, 2, 3};
    case kDownLeftRight: return {2, 1, 3};
    default: return {0, 2, 1, 3};
    }
}

// keyboard and mouse: true when drawn (false: nothing bound, the game's glyph stays)
bool DrawKeyboard(Canvas& c, const Box& b, float s, Role role, const Inputs& in) {
    if (role >= kUp) {
        std::vector<int> dirs = Directions(role), keys;
        bool arrows = true;
        for (int d : dirs) {
            int vk = in.keys[kUp + d];
            if (!vk) return false;
            keys.push_back(vk);
            arrows = arrows && ArrowDir(vk) == d;
        }
        if (arrows) {
            ArrowCap(c, b, s, dirs);
            return true;
        }
        if (keys.size() == 1) {
            int vk = keys[0];
            if (int m = MouseButton(vk)) Mouse(c, b, s, m);
            else if (int a = ArrowDir(vk); a >= 0) ArrowCap(c, b, s, {a});
            else KeyCap(c, b, s), Text(c, KeyFace(b, s), KeyText(vk), kLabel);
            return true;
        }
        // several keys: their names together (W A S D -> "WASD", else "Q/E")
        std::string label;
        bool letters = true;
        for (int vk : keys) letters = letters && KeyText(vk).size() == 1;
        for (size_t i = 0; i < keys.size(); i++) label += (i && !letters ? "/" : "") + KeyText(keys[i]);
        KeyCap(c, b, s);
        Text(c, KeyFace(b, s), label, kLabel);
        return true;
    }
    int vk = in.keys[role];
    if (!vk) return false;
    if (int m = MouseButton(vk)) {
        Mouse(c, b, s, m);
    } else if (int a = ArrowDir(vk); a >= 0) {
        ArrowCap(c, b, s, {a});
    } else {
        KeyCap(c, b, s);
        Text(c, KeyFace(b, s), KeyText(vk), kLabel);
    }
    return true;
}

// PlayStation and Switch: the controller button bound to the Xbox button, in that pad's names and symbols. The
// D-pad looks alike on every pad: its glyphs stay the game's
bool DrawPad(Canvas& c, const Box& b, float s, Role role, Style style, const Inputs& in) {
    if (role >= kUp) return false;
    const std::string button = in.pad[role];
    if (button.empty()) return false;
    const bool ps = style == Style::kPlayStation;
    const int generation = in.generation;
    // square box for the round ones (the triggers' boxes are tall)
    float m = std::min(b.W(), b.H());
    Box sq{b.Cx() - m / 2, b.Cy() - m / 2, b.Cx() + m / 2, b.Cy() + m / 2};
    static const char* const kFaces[] = {"a", "b", "x", "y"};
    for (int f = 0; f < 4; f++)
        if (button == kFaces[f]) {
            if (ps) PsFace(c, sq, s, f == 0 ? 0 : f == 1 ? 1 : f == 2 ? 2 : 3);
            else LabelDisc(c, sq, s, f == 0 ? "B" : f == 1 ? "A" : f == 2 ? "Y" : "X");  // Switch: by position
            return true;
        }
    struct Name {
        const char* button;
        const char* ps;
        const char* nintendo;
        bool round;
    };
    static const Name kNames[] = {
        {"lb", "L1", "L", false},     {"rb", "R1", "R", false},      {"lt", "L2", "ZL", false},
        {"rt", "R2", "ZR", false},    {"ls", "L3", "LS", true},      {"rs", "R3", "RS", true},
        {"guide", "PS", "HOME", true}, {"touchpad", "TOUCH", "", false}, {"misc1", "MIC", "CAP", true},
        {"up", "UP", "UP", false},    {"down", "DOWN", "DOWN", false}, {"left", "LEFT", "LEFT", false},
        {"right", "RIGHT", "RIGHT", false},
    };
    std::string label;
    bool round = false;
    if (button == "start") label = ps ? (generation == 3 ? "START" : "OPTIONS") : "+", round = !ps;
    else if (button == "back") label = ps ? (generation == 3 ? "SELECT" : generation == 5 ? "CREATE" : "SHARE") : "-", round = !ps;
    for (const Name& n : kNames)
        if (button == n.button) label = ps ? n.ps : n.nintendo, round = n.round;
    if (label.empty()) label = button, round = false;
    for (char& ch : label) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (round) LabelDisc(c, sq, s, label);
    else LabelPill(c, b, s, label);
    return true;
}

// ---- the page ----

std::mutex g_lock;  // g_built; g_original is written once, before any page is drawn
std::vector<uint32_t> g_original;  // the game's page, BGRA8, 256x128
struct Built {
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t revision = 0;  // hle::prompts::Revision() it was drawn for
    int scale = 0;
    bool busy = false;      // a worker draws it
};
Built g_built[4];  // per style (kXbox stays empty: the game's own page)

uint32_t Sample(float x, float y) {  // the original, bilinear, clamped, texel centres at +0.5
    x -= 0.5f, y -= 0.5f;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - x0, fy = y - y0;
    float acc[4] = {};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xx = std::clamp(x0 + i, 0, int(kPageWidth) - 1), yy = std::clamp(y0 + j, 0, int(kPageHeight) - 1);
            uint32_t p = g_original[size_t(yy) * kPageWidth + xx];
            float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            float a = (p >> 24) / 255.0f;
            // premultiplied while filtering, so transparent texels do not darken the edges
            acc[0] += wgt * a * ((p >> 16) & 0xFF), acc[1] += wgt * a * ((p >> 8) & 0xFF), acc[2] += wgt * a * (p & 0xFF);
            acc[3] += wgt * a;
        }
    if (acc[3] <= 0) return 0;
    auto ch = [&](float v) { return uint32_t(std::clamp(v / acc[3], 0.0f, 255.0f) + 0.5f); };
    return uint32_t(std::clamp(acc[3] * 255.0f + 0.5f, 0.0f, 255.0f)) << 24 | ch(acc[0]) << 16 | ch(acc[1]) << 8 | ch(acc[2]);
}

// FCI_PROMPTS_DUMP=1: each page drawn goes to prompts_<style>.bmp beside the exe (32 bit with alpha)
void DumpPage(const std::vector<uint32_t>& px, int w, int h, Style style) {
    static const bool on = GetEnvironmentVariableA("FCI_PROMPTS_DUMP", nullptr, 0) > 0;
    if (!on) return;
    char name[64];
    snprintf(name, sizeof name, "prompts_%s.bmp", hle::prompts::Name(style));
    FILE* f = nullptr;
    if (fopen_s(&f, name, "wb") || !f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih, ih.biWidth = w, ih.biHeight = -h, ih.biPlanes = 1, ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + DWORD(px.size() * 4);
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(px.data(), 4, px.size(), f);
    fclose(f);
}

ComPtr<ID3D11ShaderResourceView> Build(ID3D11Device* device, Style style, int s, const Inputs& in) {
    const auto start = std::chrono::steady_clock::now();
    const int w = int(kPageWidth) * s, h = int(kPageHeight) * s;
    // the game's page at this scale; the replaced glyphs' boxes cleared
    std::vector<uint32_t> base(size_t(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) base[size_t(y) * w + x] = Sample((x + 0.5f) / s, (y + 0.5f) / s);
    Canvas c;
    c.w = w, c.h = h;
    c.px.assign(size_t(w) * h, {0, 0, 0, 0});
    int drawn = 0;
    for (const Cell& cell : kCells) {
        Box b{float(cell.x0 * s), float(cell.y0 * s), float((cell.x1 + 1) * s), float((cell.y1 + 1) * s)};
        bool done = style == Style::kKeyboard ? DrawKeyboard(c, b, float(s), cell.role, in)
                                              : DrawPad(c, b, float(s), cell.role, style, in);
        if (!done) continue;
        drawn++;
        // the game's glyph goes; around the box (the glyph's quad reaches a little further) its faint antialiased
        // rim too, which showed as a thin coloured ring; other glyphs nearby are opaque there and stay
        const int m = 2 * s;
        for (int y = std::max(0, int(cell.y0) * s - m); y < std::min(h, (cell.y1 + 1) * s + m); y++)
            for (int x = std::max(0, int(cell.x0) * s - m); x < std::min(w, (cell.x1 + 1) * s + m); x++) {
                const bool inside = x >= cell.x0 * s && x < (cell.x1 + 1) * s && y >= cell.y0 * s && y < (cell.y1 + 1) * s;
                uint32_t& p = base[size_t(y) * w + x];
                if (inside || (p >> 24) < 64) p = 0;
            }
    }
    // ours over the cleared page
    std::vector<uint32_t> out(base.size());
    for (size_t i = 0; i < out.size(); i++) {
        const auto& p = c.px[i];
        if (p[3] <= 0) {
            out[i] = base[i];
            continue;
        }
        auto ch = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        out[i] = ch(p[3]) << 24 | ch(p[0]) << 16 | ch(p[1]) << 8 | ch(p[2]);
    }
    DumpPage(out, w, h, style);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(w), td.Height = UINT(h), td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{out.data(), UINT(w * 4), 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateTexture2D(&td, &data, &tex)) || FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        rt::Log("prompts: page for %s could not be made", hle::prompts::Name(style));
        return nullptr;
    }
    rt::Log("prompts: %s glyphs drawn (%d of %zu, %dx%d) in %.1f ms", hle::prompts::Name(style), drawn, std::size(kCells),
            w, h, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return srv;
}

}  // namespace

bool IsGlyphPage(const texture::Desc& d) {
    if (d.width != kPageWidth || d.height != kPageHeight || d.color != 0x0E || d.cube || d.depth != 1) return false;
    if (texpack::ContentKey(d) != kPageKey) return false;
    std::lock_guard lock(g_lock);
    if (g_original.empty()) {
        std::vector<texture::Image> images = texture::Decode(d);
        if (images.empty() || images[0].pixels.size() != size_t(kPageWidth) * kPageHeight) return false;
        g_original = std::move(images[0].pixels);
        rt::Log("prompts: the font page with the button glyphs is loaded (button_prompts: %s now)",
                hle::prompts::Name(hle::prompts::Current()));
    }
    return true;
}

ID3D11ShaderResourceView* Page(ID3D11Device* device, const texture::Desc&, float scale) {
    const Style style = hle::prompts::Current();
    // about one texel per pixel where the prompts are drawn (the font's glyphs show at ~0.7 of their texels)
    const int s = std::clamp(int(std::ceil(scale * 0.75f)), 1, 4);
    const uint32_t revision = hle::prompts::Revision();
    std::lock_guard lock(g_lock);
    if (g_original.empty()) return nullptr;
    // every style's page is drawn ahead on a worker (~10 ms each), at the first use of the page and whenever the
    // bindings changed, so switching devices is instant; until a page is ready the older one (or the game's own)
    // is bound
    bool stale = false;
    for (int i = 1; i < 4; i++) {
        const Built& b = g_built[i];
        stale = stale || (!b.busy && (!b.srv || b.revision != revision || b.scale != s));
    }
    if (stale) {
        Inputs in = Gather();
        std::vector<Style> todo;
        for (int i = 1; i < 4; i++) {
            Built& b = g_built[i];
            if (b.busy || (b.srv && b.revision == revision && b.scale == s)) continue;
            b.busy = true;
            // the current style first
            if (Style(i) == style) todo.insert(todo.begin(), Style(i));
            else todo.push_back(Style(i));
        }
        ComPtr<ID3D11Device> keep(device);
        std::thread([keep, todo, s, revision, in = std::move(in)] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (Style st : todo) {
                ComPtr<ID3D11ShaderResourceView> srv = Build(keep.Get(), st, s, in);
                std::lock_guard lock(g_lock);
                Built& b = g_built[int(st)];
                b.srv = std::move(srv), b.revision = revision, b.scale = s, b.busy = false;
            }
        }).detach();
    }
    if (style == Style::kXbox) return nullptr;
    return g_built[int(style)].srv.Get();
}

}  // namespace gpu::prompt_icons
