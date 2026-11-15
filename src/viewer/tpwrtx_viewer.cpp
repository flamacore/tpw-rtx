// tpwrtx_viewer: 64-bit renderer for the SimTheme Park stream.
// Maps the shared memory written by the hook (shared/tpwrtx_shared.h), rebuilds each frame in world
// space and renders it with Direct3D 12: path-traced lighting (DXR 1.1 inline ray queries, rt.hlsl)
// with a raster fallback, from the game camera or a free orbit camera.
// Usage: tpwrtx_viewer.exe [gamePid] [--orbit yawDeg pitchDeg]
// Keys: T ray tracing, W world cache, C game/orbit camera, O auto-orbit, R reset, U HUD, F12 screenshot.
// Drag = orbit, wheel = zoom. Files in the working dir: "tpwrtx_shot" -> tpwrtx_view.png; "tpwrtx_cmd" with one of
//   orbit <yaw> <pitch> [zoom] | game | auto | rt <0|1> | cache <0|1> | sun <azimuthDeg> <elevationDeg> | exposure <x>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "../shared/tpwrtx_shared.h"
#include "nvsdk_ngx_helpers_dlssd_d3d.h"   // NVIDIA DLSS SDK (include dir + nvsdk_ngx_s.lib on the build line)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
using Microsoft::WRL::ComPtr;

static FILE *g_log;
static void fail(const char *what, HRESULT hr) { fprintf(g_log, "FATAL %s hr=%08lx\n", what, (unsigned long)hr); fflush(g_log); ExitProcess(1); }
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) fail(#x, hr_); } while (0)

static UINT WIN_W = 1280, WIN_H = 960;   // internal render size (tpwrtx.ini "height"; width from the game aspect, 4:3 or "resolution")
static const UINT BUFS = 3;
static const uint32_t CACHE_MAX_TRIS = 400000;
static const uint32_t RT_MAX_TRIS = TPWRTX_MAX_INDICES / 3 + CACHE_MAX_TRIS, MAX_LIGHTS = 64;
static UINT64 align(UINT64 v, UINT64 a) { return (v + a - 1) & ~(a - 1); }
// Upload buffer layout (persistently mapped; one frame in flight)
static const UINT64 OFF_VB = 0;                                                     // raster vertices (frame)
static const UINT64 OFF_IB = align(OFF_VB + TPWRTX_MAX_VERTS * 24ull, 65536);       // raster indices
static const UINT64 OFF_RTV = align(OFF_IB + TPWRTX_MAX_INDICES * 4ull, 65536);     // RT triangle soup vertices
static const UINT64 OFF_RTI = align(OFF_RTV + RT_MAX_TRIS * 3ull * 24, 65536);      // RT indices
static const UINT64 OFF_MAT = align(OFF_RTI + RT_MAX_TRIS * 3ull * 4, 65536);       // RT per-triangle materials
static const UINT64 OFF_LIGHT = align(OFF_MAT + RT_MAX_TRIS * 8ull, 65536);
static const UINT64 OFF_INST = align(OFF_LIGHT + MAX_LIGHTS * 32ull, 65536);
static const UINT64 OFF_CB = align(OFF_INST + 64, 65536);                           // raster CB (256) + RT CB (512)
static const UINT64 OFF_STAGE = align(OFF_CB + 1024, 65536);                        // texture staging
static const UINT64 STAGE_BYTES = 48ull << 20, UPLOAD_BYTES = OFF_STAGE + STAGE_BYTES;

// ---------------------------------------------------------------- math (row-vector, D3D style)
struct M4 { float m[4][4]; };
static M4 mul(const M4 &a, const M4 &b)
{
    M4 r{};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
static M4 from16(const float *f) { M4 r; memcpy(r.m, f, 64); return r; }
static M4 inverse(const M4 &M)
{
    const float *m = &M.m[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    M4 r;
    for (int i = 0; i < 16; i++) (&r.m[0][0])[i] = inv[i] / det;
    return r;
}
static M4 proj_game(float f, float cx, float cy, float W, float H)
{
    const float n = 1.0f, fa = 6000.0f;
    M4 p{};
    p.m[0][0] = f / (W / 2); p.m[1][1] = f / (H / 2);
    p.m[2][0] = (cx - W / 2) / (W / 2); p.m[2][1] = -(cy - H / 2) / (H / 2);
    p.m[2][2] = fa / (fa - n); p.m[2][3] = 1; p.m[3][2] = -n * fa / (fa - n);
    return p;
}
static M4 look_at(const float e[3], const float t[3])
{
    float f[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]}, l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float &c : f) c /= l;
    float r[3] = {f[2], 0, -f[0]}; l = sqrtf(r[0] * r[0] + r[2] * r[2]); r[0] /= l; r[2] /= l;   // cross(up, f), left-handed
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    M4 v{};
    for (int i = 0; i < 3; i++) { v.m[i][0] = r[i]; v.m[i][1] = u[i]; v.m[i][2] = f[i]; }
    v.m[3][0] = -(r[0] * e[0] + r[1] * e[1] + r[2] * e[2]);
    v.m[3][1] = -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]);
    v.m[3][2] = -(f[0] * e[0] + f[1] * e[1] + f[2] * e[2]);
    v.m[3][3] = 1;
    return v;
}
static void xform(const float p[3], const M4 &m, float out[4])
{
    for (int c = 0; c < 4; c++) out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
}

// ---------------------------------------------------------------- settings / input state
static struct {
    bool orbit = false, autoOrbit = false, hud = true, shot = false, rt = true, cache = true, drag = false;
    float yaw = 0, pitch = 0, zoom = 1; int lx, ly;
} g_in;

// User-tunable look. Panel: Ctrl+Alt+S (game keeps running; the hook lends the mouse to the panel). Saved as JSON.
struct Setting { const char *key, *label; float def, lo, hi; bool integer; float v; };
static Setting g_set[] = {
    {"exposure", "Exposure", 0.50f, 0.05f, 2.0f},
    {"contrast", "Contrast", 1.08f, 0.5f, 1.6f},
    {"saturation", "Saturation", 1.15f, 0.0f, 2.0f},
    {"sun", "Sun intensity", 1.0f, 0.0f, 3.0f},
    {"sunAz", "Sun direction (deg)", 215, 0, 360, true},
    {"sunEl", "Sun height (deg)", 50, 5, 90, true},
    {"sunSoft", "Shadow softness", 1.0f, 0.0f, 6.0f},
    {"sky", "Sky / ambient light", 1.0f, 0.0f, 3.0f},
    {"bounce", "Bounce light (GI)", 1.0f, 0.0f, 2.0f},
    {"torch", "Torch / flame light", 1.0f, 0.0f, 3.0f},
    {"tint", "Vertex colour tint", 0.5f, 0.0f, 1.0f},
    {"shade", "Baked game shading", 0.5f, 0.0f, 1.0f},
    {"normal", "Bumps from textures", 1.0f, 0.0f, 4.0f},
    {"spec", "Sun highlights", 0.5f, 0.0f, 3.0f},
    {"gloss", "Glossiness", 1.0f, 0.0f, 3.0f},
    {"denoise", "Denoiser passes", 4, 0, 5, true},
    {"denoiseSmooth", "Denoiser smoothing", 0.6f, 0.05f, 3.0f},
    {"history", "Temporal frames", 32, 1, 128, true},
    {"jitter", "TAA jitter (0 off / 1 on)", 0, 0, 1, true},
    {"dlss", "DLSS Ray Reconstruction", 1, 0, 4, true},   // 0 off (own denoiser), 1 DLAA, 2 Quality, 3 Balanced, 4 Performance
    // DLSS 5 neural rendering (unofficial: needs nvngx_dlssnr.dll in the game folder). Strength/colour/guard are ours and
    // live; style..tone are the model's own and rebuild it (a short pause).
    {"nr", "DLSS 5 neural rendering", 1, 0, 1, true},
    {"nrStrength", "DLSS 5 detail strength", 1.0f, 0.0f, 2.0f},
    {"nrColour", "DLSS 5 colour strength", 1.0f, 0.0f, 1.0f},
    {"nrGuard", "DLSS 5 highlight guard", 2.0f, 1.0f, 8.0f},
    {"nrStyle", "DLSS 5 style", 0, 0, 2, true},
    {"nrPreset", "DLSS 5 model preset", 0, 0, 3, true},
    {"nrIntensity", "DLSS 5 intensity", 1.0f, 0.0f, 2.0f},
    {"nrStructure", "DLSS 5 local structure", 1.0f, 0.0f, 2.0f},
    {"nrTone", "DLSS 5 local tone", 1.0f, 0.0f, 2.0f},
    // Game camera (applied by the hook inside TP.EXE). The game's pitch follows the zoom: closer = lower, farther = top-down.
    {"fov", "Camera FOV (90 = original)", 90, 50, 130, true},
    {"zoomMin", "Closest zoom (70 = original)", 40, 15, 70, true},
    {"zoomMax", "Farthest zoom (130 = original)", 200, 130, 400, true},
    {"wasd", "WASD pan, Q/E rotate", 1, 0, 1, true},
};
static const char *const kNrStyleNames[] = {"Default", "Natural", "Cinematic"};
static const char *const kDlssNames[] = {"off (own denoiser)", "DLAA (native)", "Quality", "Balanced", "Performance"};
enum { ST_EXPOSURE, ST_CONTRAST, ST_SATURATION, ST_SUN, ST_SUNAZ, ST_SUNEL, ST_SUNSOFT, ST_SKY, ST_BOUNCE, ST_TORCH, ST_TINT, ST_SHADE,
       ST_NORMAL, ST_SPEC, ST_GLOSS, ST_DENOISE, ST_DNSMOOTH, ST_HISTORY, ST_JITTER, ST_DLSS,
       ST_NR, ST_NRSTRENGTH, ST_NRCOLOUR, ST_NRGUARD, ST_NRSTYLE, ST_NRPRESET, ST_NRINTENSITY, ST_NRSTRUCTURE, ST_NRTONE,
       ST_FOV, ST_ZOOMMIN, ST_ZOOMMAX, ST_WASD, ST_COUNT };
static_assert(ST_COUNT == sizeof g_set / sizeof g_set[0], "settings table and enum out of sync");
static float S(int i) { return g_set[i].v; }
static const char *SETTINGS_FILE = "tpwrtx_settings.json";

static void settings_load()
{
    for (Setting &s : g_set) s.v = s.def;
    FILE *f = fopen(SETTINGS_FILE, "rb");
    if (!f) return;
    char buf[8192] = {};
    fread(buf, 1, sizeof buf - 1, f); fclose(f);
    for (Setting &s : g_set) {   // ponytail: flat {"key": number} object, the only shape we write
        char k[64]; snprintf(k, sizeof k, "\"%s\"", s.key);
        const char *p = strstr(buf, k);
        float v;
        if (p && (p = strchr(p + strlen(k), ':')) && sscanf(p + 1, "%f", &v) == 1) s.v = std::clamp(v, s.lo, s.hi);
    }
}
static void settings_save()
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "{\n");
    for (int i = 0; i < ST_COUNT; i++) fprintf(f, "  \"%s\": %g%s\n", g_set[i].key, g_set[i].v, i + 1 < ST_COUNT ? "," : "");
    fprintf(f, "}\n");
    fclose(f);
}

static TpwShared *g_shm;
static HWND g_panel, g_lbl[ST_COUNT], g_tb[ST_COUNT];

static void label_text(int i)
{
    wchar_t t[96];
    if (i == ST_DLSS) swprintf(t, 96, L"DLSS RR:  %hs", kDlssNames[std::clamp((int)g_set[i].v, 0, 4)]);
    else if (i == ST_NR || i == ST_WASD) swprintf(t, 96, L"%hs:  %hs", g_set[i].label, g_set[i].v > 0.5f ? "on" : "off");
    else if (i == ST_NRSTYLE) swprintf(t, 96, L"%hs:  %hs", g_set[i].label, kNrStyleNames[std::clamp((int)g_set[i].v, 0, 2)]);
    else swprintf(t, 96, g_set[i].integer ? L"%hs:  %.0f" : L"%hs:  %.2f", g_set[i].label, g_set[i].v);
    SetWindowTextW(g_lbl[i], t);
}
static void panel_sync()
{
    for (int i = 0; i < ST_COUNT; i++) {
        const Setting &s = g_set[i];
        SendMessageW(g_tb[i], TBM_SETPOS, TRUE, s.integer ? lroundf(s.v - s.lo) : lroundf((s.v - s.lo) / (s.hi - s.lo) * 1000));
        label_text(i);
    }
}
static HWND game_window(DWORD pid)   // the game's biggest visible top-level window
{
    struct Find { DWORD pid; HWND w; LONG area; } f{pid, nullptr, 0};
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        Find *f = (Find *)l; DWORD p; RECT r;
        GetWindowThreadProcessId(w, &p);
        if (p != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        GetClientRect(w, &r);
        if (r.right * r.bottom > f->area) { f->area = r.right * r.bottom; f->w = w; }
        return TRUE;
    }, (LPARAM)&f);
    return f.w;
}
// uiOpen is a heartbeat (tick count), refreshed by a timer that also runs inside modal loops (dragging the panel):
// if this process dies with the panel open, the hook takes the mouse back after 2 s instead of leaving the game deaf.
static void heartbeat() { if (g_shm) g_shm->uiOpen = g_panel && IsWindowVisible(g_panel) ? (GetTickCount() | 1) : 0; }
static void panel_show(bool on)
{
    if (!g_panel) return;
    if (on) {
        HWND gw = g_shm ? game_window(g_shm->hookPid) : nullptr;
        RECT gr{100, 100, 0, 0};
        if (gw) GetWindowRect(gw, &gr);
        SetWindowPos(g_panel, HWND_TOPMOST, gr.left + 40, gr.top + 80, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RECT pr; GetWindowRect(g_panel, &pr);
        SetCursorPos((pr.left + pr.right) / 2, pr.top + (pr.bottom - pr.top) / 3);   // the game hides the cursor over its own window
    } else { ShowWindow(g_panel, SW_HIDE); settings_save(); }
    heartbeat();
}
static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // never take focus from the game
    case WM_TIMER: heartbeat(); return 0;
    case WM_HSCROLL:
        for (int i = 0; i < ST_COUNT; i++) if ((HWND)lp == g_tb[i]) {
            Setting &s = g_set[i];
            int p = (int)SendMessageW(g_tb[i], TBM_GETPOS, 0, 0);
            s.v = s.integer ? s.lo + p : s.lo + (s.hi - s.lo) * p / 1000.0f;
            label_text(i);
            if (LOWORD(wp) == TB_ENDTRACK) settings_save();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 1) { for (Setting &s : g_set) s.v = s.def; panel_sync(); settings_save(); }
        if (LOWORD(wp) == 2) panel_show(false);
        return 0;
    case WM_CLOSE: panel_show(false); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void panel_create(HINSTANCE inst)
{
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_BAR_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc{0, panel_proc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), (HBRUSH)(COLOR_BTNFACE + 1), nullptr, L"tpwrtx_panel"};
    RegisterClassW(&wc);
    UINT dpi = GetDpiForSystem();
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    int pad = px(10), rowH = px(30), lw = px(210), tw = px(230), bw = px(100), bh = px(26);
    int rows = (ST_COUNT + 1) / 2, colW = pad * 2 + lw + tw;   // two columns: one would not fit a 1440p screen at 150%
    int cw = pad + colW * 2, ch = pad * 2 + rowH * rows + bh + px(6);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"tpwrtx_panel", L"TPW RTX settings  (Ctrl+Alt+S)", style,
                              100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (int i = 0; i < ST_COUNT; i++) {
        int y = pad + (i % rows) * rowH, x = pad + (i / rows) * colW;
        g_lbl[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, x, y + px(6), lw, rowH - px(6), g_panel, nullptr, inst, nullptr);
        g_tb[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, x + pad + lw, y, tw, rowH, g_panel, nullptr, inst, nullptr);
        SendMessageW(g_tb[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, g_set[i].integer ? (int)(g_set[i].hi - g_set[i].lo) : 1000));
        SendMessageW(g_lbl[i], WM_SETFONT, (WPARAM)font, TRUE);
    }
    int by = pad + rowH * rows + px(4);
    HWND b1 = CreateWindowExW(0, L"BUTTON", L"Defaults", WS_CHILD | WS_VISIBLE, pad, by, bw, bh, g_panel, (HMENU)1, inst, nullptr);
    HWND b2 = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE, cw - pad - bw, by, bw, bh, g_panel, (HMENU)2, inst, nullptr);
    SendMessageW(b1, WM_SETFONT, (WPARAM)font, TRUE); SendMessageW(b2, WM_SETFONT, (WPARAM)font, TRUE);
    SetTimer(g_panel, 1, 250, nullptr);
    panel_sync();
}

// Overlay mode: each frame goes into a shared texture that the hook shows on the game's own window (hook/overlay.cpp);
// this process keeps only a hidden window for the global hotkeys. Shown only while ray tracing a park.
static bool g_overlay, g_overlayOn = true;
static ComPtr<ID3D12Resource> outTex;
static ComPtr<ID3D12Fence> outFence;
static uint32_t outFenceVal;

static M4 orbit_view(const float *camInv, float t)
{
    const float *E = camInv + 12, *Fw = camInv + 8;
    float d = Fw[1] < -0.05f ? E[1] / -Fw[1] : 150.0f;          // ray from the eye to the ground plane y = 0
    float P[3] = {E[0] + Fw[0] * d, E[1] + Fw[1] * d, E[2] + Fw[2] * d};
    float o[3] = {E[0] - P[0], E[1] - P[1], E[2] - P[2]};
    float len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) * g_in.zoom;
    float elev = atan2f(o[1], sqrtf(o[0] * o[0] + o[2] * o[2])), az = atan2f(o[0], o[2]);
    float yaw = g_in.yaw + (g_in.autoOrbit ? 0.7f * sinf(t * 0.6f) : 0);
    elev = std::fmin(std::fmax(elev + g_in.pitch, 0.08f), 1.55f);
    az += yaw;
    float eye[3] = {P[0] + len * cosf(elev) * sinf(az), P[1] + len * sinf(elev), P[2] + len * cosf(elev) * cosf(az)};
    return look_at(eye, P);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_HOTKEY:   // Ctrl+Alt+R / T / C / O / S
        if (wp == 5) panel_show(!IsWindowVisible(g_panel));
        if (wp == 1) g_overlayOn = !g_overlayOn;
        if (wp == 2) g_in.rt = !g_in.rt;
        if (wp == 3) g_in.orbit = !g_in.orbit;
        if (wp == 4) { g_in.autoOrbit = !g_in.autoOrbit; g_in.orbit = true; }
        return 0;
    case WM_KEYDOWN:
        if (wp == 'C') g_in.orbit = !g_in.orbit;
        if (wp == 'O') { g_in.autoOrbit = !g_in.autoOrbit; g_in.orbit = true; }
        if (wp == 'R') { g_in.yaw = g_in.pitch = 0; g_in.zoom = 1; g_in.autoOrbit = false; }
        if (wp == 'U') g_in.hud = !g_in.hud;
        if (wp == 'T') g_in.rt = !g_in.rt;
        if (wp == 'W') g_in.cache = !g_in.cache;
        if (wp == VK_F12) g_in.shot = true;
        return 0;
    case WM_LBUTTONDOWN: g_in.drag = true; g_in.lx = (short)LOWORD(lp); g_in.ly = (short)HIWORD(lp); SetCapture(h); return 0;
    case WM_LBUTTONUP: g_in.drag = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:
        if (g_in.drag) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            g_in.yaw += (x - g_in.lx) * 0.006f; g_in.pitch += (y - g_in.ly) * 0.006f; g_in.lx = x; g_in.ly = y; g_in.orbit = true;
        }
        return 0;
    case WM_MOUSEWHEEL: g_in.zoom *= powf(0.9f, GET_WHEEL_DELTA_WPARAM(wp) / 120.0f); g_in.orbit = true; return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static bool g_dumpDraws;   // log the blended world draws of the next frame (to find light-emitting sprites)
static uint32_t g_dumpTex; // write texture <id> to tpwrtx_tex<id>.png

static void read_cmd_file()
{
    FILE *cf = fopen("tpwrtx_cmd", "r");
    if (!cf) return;
    char cmd[16] = {}; float a = 0, b = 0, c = 1;
    int n = fscanf(cf, "%15s %f %f %f", cmd, &a, &b, &c);
    fclose(cf); DeleteFileA("tpwrtx_cmd");
    if (n < 1) return;
    if (!strcmp(cmd, "orbit")) { g_in.orbit = true; g_in.autoOrbit = false; g_in.yaw = a * 0.01745f; g_in.pitch = b * 0.01745f; g_in.zoom = n >= 4 ? c : 1; }
    if (!strcmp(cmd, "game")) { g_in.orbit = g_in.autoOrbit = false; g_in.yaw = g_in.pitch = 0; g_in.zoom = 1; }
    if (!strcmp(cmd, "auto")) g_in.orbit = g_in.autoOrbit = true;
    if (!strcmp(cmd, "rt")) g_in.rt = a != 0;
    if (!strcmp(cmd, "cache")) g_in.cache = a != 0;
    if (!strcmp(cmd, "sun")) { g_set[ST_SUNAZ].v = a; g_set[ST_SUNEL].v = b; }
    if (!strcmp(cmd, "exposure")) g_set[ST_EXPOSURE].v = a;
    if (!strcmp(cmd, "dumpdraws")) g_dumpDraws = true;
    if (!strcmp(cmd, "dumptex")) g_dumpTex = (uint32_t)a;
}

static BOOL CALLBACK pick_monitor(HMONITOR m, HDC, LPRECT, LPARAM out)
{
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoA(m, &mi);
    if (!(mi.dwFlags & MONITORINFOF_PRIMARY)) { *(RECT *)out = mi.rcWork; return FALSE; }   // prefer a secondary monitor
    return TRUE;
}

// ---------------------------------------------------------------- PNG (stored deflate; screenshots only)
static void write_png(const char *path, UINT w, UINT h, const uint8_t *rgba, UINT pitch)
{
    static uint32_t crct[256];
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crct[n] = c; }
    std::vector<uint8_t> raw;
    for (UINT y = 0; y < h; y++) {
        raw.push_back(0);
        for (UINT x = 0; x < w; x++) { const uint8_t *p = rgba + y * pitch + x * 4; raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); i += 65535) {
        uint32_t n = (uint32_t)std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size()); z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t ad = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((ad >> s) & 255);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    auto be = [&](uint32_t v) { uint8_t o[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; fwrite(o, 1, 4, f); };
    auto chunk = [&](const char *t, const uint8_t *d, size_t n) {
        be((uint32_t)n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
        uint32_t c = 0xffffffffu;
        for (int i = 0; i < 4; i++) c = crct[(c ^ (uint8_t)t[i]) & 255] ^ (c >> 8);
        for (size_t i = 0; i < n; i++) c = crct[(c ^ d[i]) & 255] ^ (c >> 8);
        be(c ^ 0xffffffffu);
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ih, 13); chunk("IDAT", z.data(), z.size()); chunk("IEND", nullptr, 0);
    fclose(f);
}

// ---------------------------------------------------------------- raster + composite shaders
static const char *kShader = R"(
cbuffer Frame : register(b0) { row_major float4x4 viewProj; };
cbuffer Draw  : register(b1) { uint alphaTest; float alphaRef; uint texMode; uint isUI; };
Texture2D tex0 : register(t0);
SamplerState s0 : register(s0);   // world: anisotropic + mips
SamplerState s1 : register(s1);   // UI: linear, top mip only (mips bleed neighbouring glyphs in font atlases)
struct VSIn { float3 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
struct PSIn { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };
PSIn VSMain(VSIn i) {
    PSIn o;
    o.pos = isUI ? float4(i.pos, 1) : mul(float4(i.pos, 1), viewProj);
    o.col = i.col; o.uv = i.uv;
    return o;
}
float4 PSMain(PSIn i) : SV_Target {
    float4 t = isUI ? tex0.SampleLevel(s1, i.uv, 0) : tex0.Sample(s0, i.uv);
    float4 c = (texMode & 1) ? t : t * i.col;   // D3DTBLEND_DECAL/COPY use the texture alone
    if (alphaTest && c.a < alphaRef) discard;
    if ((texMode & 2) && t.a < 0.5) discard;    // colour key: keyed texels were captured as transparent black
                                                // (multiply-blended shadow decals turned them into black boxes)
    return c;
}
)";
// Composite: remodulate the denoised lighting with the G-buffer albedo, add the sun highlight, tone map + grade.
static const char *kComposite = R"(
Texture2D<float4> irr : register(t0);      // rgb diffuse irradiance, a = sun specular (denoised)
Texture2D<float> rtDepth : register(t1);
Texture2D<uint4> gbuf : register(t2);      // y = albedo (RGB8, sqrt-encoded)
cbuffer K : register(b0) { float exposure, contrast, saturation, direct; float3 sunColor; float pad; float2 depthScale; float2 pad2; };
float4 VSFull(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
void PSComposite(float4 pos : SV_Position, out float4 c : SV_Target, out float d : SV_Depth) {
    int3 p = int3(pos.xy, 0);
    float4 v = irr.Load(p);
    float3 col = v.rgb;   // direct: DLSS-RR output, already the final HDR image
    if (direct < 0.5) {   // own denoiser: remodulate lighting with albedo, add the sun highlight
        uint a = gbuf.Load(p).y;
        float3 alb = float3(a & 255, (a >> 8) & 255, (a >> 16) & 255) / 255.0;
        col = alb * alb * v.rgb + sunColor * v.a;
    }
    d = rtDepth.Load(int3(pos.xy * depthScale, 0));   // depth stays at render size when DLSS upscales
    float3 t = pow(aces(col * exposure), 1 / 2.2);
    t = lerp(dot(t, float3(0.2126, 0.7152, 0.0722)).xxx, t, saturation);
    c = float4(saturate((t - 0.5) * contrast + 0.5), 1);
}
)";
// Denoiser: edge-avoiding a-trous wavelet (3x3 B-spline taps at growing strides) on the demodulated lighting.
// Edge stops: normal, view depth, luminance; the luminance stop relaxes where the temporal history is short (noisier).
static const char *kAtrous = R"(
RWTexture2D<float4> src : register(u0);
RWTexture2D<float4> dst : register(u1);
RWTexture2D<uint4> gbuf : register(u2);
cbuffer K : register(b0) { uint stride; float sigma; uint2 dim; };
float3 unpackNormal(uint p) {
    float2 o = float2(p & 0xffff, p >> 16) / 65535 * 2 - 1;
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0 ? -t : t;
    return normalize(n);
}
float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
[numthreads(8, 8, 1)]
void CSAtrous(uint2 px : SV_DispatchThreadID) {
    if (any(px >= dim)) return;
    uint4 g0 = gbuf[px];
    float4 c0 = src[px];
    float z0 = asfloat(g0.z);
    if (z0 > 1e8) { dst[px] = c0; return; }   // sky
    float3 n0 = unpackNormal(g0.x);
    float l0 = lum(c0.rgb), sl = sigma * (l0 + 0.02) * (1 + 16 / max(asfloat(g0.w), 1));
    float4 sum = c0 * 0.25; float wsum = 0.25;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        if (x == 0 && y == 0) continue;
        int2 q = int2(px) + int2(x, y) * int(stride);
        if (any(q < 0) || any(q >= int2(dim))) continue;
        uint4 g = gbuf[q];
        float4 c = src[q];
        float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25)
                * pow(saturate(dot(n0, unpackNormal(g.x))), 16)
                * exp(-abs(asfloat(g.z) - z0) / (0.015 * z0 * stride + 1e-3))
                * exp(-abs(lum(c.rgb) - l0) / sl);
        sum += c * w; wsum += w;
    }
    dst[px] = sum / wsum;
}
)";

static ComPtr<ID3DBlob> compile(const char *src, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(src, strlen(src), "tpwrtx", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        { fprintf(g_log, "shader: %s\n", err ? (const char *)err->GetBufferPointer() : "?"); fail("D3DCompile", E_FAIL); }
    return code;
}

// ---------------------------------------------------------------- renderer state
struct GpuVertex { float x, y, z; uint32_t color; float u, v; };
struct TexCache { ComPtr<ID3D12Resource> res; uint32_t version = 0, w = 0, h = 0; bool srv = false, emissive = false, translucent = false; float avg[3] = {}; };
struct Light { float pos[3], radius, color[3], pad; };
struct RTConsts {   // mirrors cbuffer C in rt.hlsl
    float invViewProj[16], viewProj[16], prevViewProj[16];
    float camPos[3]; uint32_t frame;
    float sunDir[3]; uint32_t numLights;
    float sunColor[3]; float pixelSpread;
    float skyZenith[3]; uint32_t opaqueTris;
    float skyHorizon[3]; uint32_t alphaGeom;
    uint32_t dim[2]; float jitter[2];
    uint32_t resetHistory; float normalStr, specStr, gloss;
    float tintMix, shadeMix, bounce, historyMax;
    float sunSoft; uint32_t rrMode; float pad1[2];
};

static ComPtr<ID3D12Device5> dev;
static ComPtr<ID3D12CommandQueue> queue;
static ComPtr<IDXGISwapChain3> swap;
static ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap, srvHeap;
static ComPtr<ID3D12Resource> backbuf[BUFS], depth, upload, readback, whiteTex;
static ComPtr<ID3D12CommandAllocator> alloc;
static ComPtr<ID3D12GraphicsCommandList4> cl;
static ComPtr<ID3D12Fence> fence;
static ComPtr<ID3D12RootSignature> rootSig, rtRootSig, compRootSig, atrousRootSig;
static ComPtr<ID3D12PipelineState> rtPso, compPso, atrousPso;
static ComPtr<ID3DBlob> vsCode, psCode;
static std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> psos;
static std::vector<TexCache> texCache(TPWRTX_MAX_TEX + 1);
static ComPtr<ID3D12Resource> accum[2], gbuf[2], dn[2], rtDepthTex, blas, tlas, scratch;
static ComPtr<ID3D12Resource> rrColor, rrAlbedo, rrSpecAlb, rrNormRough, rrMotion, rrOut;
static UINT64 blasSize, tlasSize, scratchSize;
static uint8_t *upPtr;
static UINT64 fenceVal;
static HANDLE fenceEvt;
static UINT rtvInc, srvInc;
static uint32_t texGen = ~0u;
static bool rtAvailable;
static const UINT HEAP_RT = TPWRTX_MAX_TEX + 1;   // descriptor heap: [0] white, [1..MAX_TEX] game textures, then RT views
static const UINT RT_UAVS = 10;                               // RT UAV table per ping-pong side (u0..u9)
static const UINT HEAP_DYN = HEAP_RT + 2 * RT_UAVS, HEAP_DYN_N = 48;   // per-frame tables (denoise, composite); safe: one frame in flight
static UINT dynUsed;

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc(UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * srvInc; return h; }
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc(UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)i * srvInc; return h; }

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &br);
}
static void uav_barrier(ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; br.UAV.pResource = r;
    cl->ResourceBarrier(1, &br);
}

static ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st, D3D12_RESOURCE_FLAGS fl = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, fl};
    ComPtr<ID3D12Resource> r;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static ComPtr<ID3D12Resource> texture2d(UINT w, UINT h, UINT16 mips, DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM,
                                        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAGS fl = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, mips, fmt, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, fl};
    ComPtr<ID3D12Resource> r;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static void make_srv(ID3D12Resource *r, UINT slot, DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = (UINT)-1;
    dev->CreateShaderResourceView(r, &sd, cpu_desc(slot));
}
static void make_uav(ID3D12Resource *r, UINT slot, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = fmt; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(r, nullptr, &ud, cpu_desc(slot));
}

// D3DBLEND 1..11 match D3D12_BLEND numerically; 12/13 are the D3D6 "both" shorthands.
static D3D12_BLEND blend_of(uint8_t b) { return b >= 1 && b <= 11 ? (D3D12_BLEND)b : D3D12_BLEND_ONE; }

static ID3D12PipelineState *pso_for(const TpwDraw &d, bool ui)
{
    bool blend = d.alphaBlend != 0;
    uint8_t src = d.srcBlend, dst = d.dstBlend;
    if (src == 12) { src = 5; dst = 6; } else if (src == 13) { src = 6; dst = 5; }
    uint32_t key = (ui ? 1u : 0u) | (blend ? 2u : 0u) | (d.zEnable ? 4u : 0u) | (d.zWrite ? 8u : 0u) | (d.zFunc & 15u) << 4 | (blend ? (uint32_t)src << 8 | (uint32_t)dst << 16 : 0u);
    auto it = psos.find(key);
    if (it != psos.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig.Get();
    pd.VS = {vsCode->GetBufferPointer(), vsCode->GetBufferSize()};
    pd.PS = {psCode->GetBufferPointer(), psCode->GetBufferSize()};
    pd.InputLayout = {il, 3};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // winding is screen-space; orbit views see back sides
    pd.RasterizerState.DepthClipEnable = TRUE;
    auto &rt = pd.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rt.BlendEnable = blend; rt.SrcBlend = blend_of(src); rt.DestBlend = blend_of(dst); rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.DepthStencilState.DepthEnable = d.zEnable;   // UI too: its z (0..0.1) is always in front of the world, and 3D UI models self-occlude
    pd.DepthStencilState.DepthWriteMask = d.zWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = d.zFunc >= 1 && d.zFunc <= 8 ? (D3D12_COMPARISON_FUNC)d.zFunc : D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> p;
    CHECK(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p)));
    psos[key] = p;
    return p.Get();
}

static void submit_and_wait()
{
    CHECK(cl->Close());
    ID3D12CommandList *lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++fenceVal);
    fence->SetEventOnCompletion(fenceVal, fenceEvt); WaitForSingleObject(fenceEvt, INFINITE);
}

static ComPtr<ID3D12RootSignature> make_root(const D3D12_ROOT_SIGNATURE_DESC &rsd)
{
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        { fprintf(g_log, "root sig: %s\n", err ? (const char *)err->GetBufferPointer() : "?"); fail("D3D12SerializeRootSignature", E_FAIL); }
    ComPtr<ID3D12RootSignature> r;
    CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&r)));
    return r;
}

static void nr_gpu_init();
static void init_rt()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
    FILE *f = fopen("rt.cso", "rb");
    fprintf(g_log, "raytracing tier %d, shader model 0x%x, rt.cso %s\n", o5.RaytracingTier, sm.HighestShaderModel, f ? "found" : "missing");
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 || sm.HighestShaderModel < D3D_SHADER_MODEL_6_5 || !f) { if (f) fclose(f); return; }
    std::vector<uint8_t> cso;
    fseek(f, 0, SEEK_END); cso.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(cso.data(), 1, cso.size(), f); fclose(f);

    // RT root: b0 CBV, t0 TLAS, t1 verts, t2 indices, t3 materials, t4 lights, textures (t0 space1, unbounded), u0..u4
    D3D12_DESCRIPTOR_RANGE texRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0};
    D3D12_DESCRIPTOR_RANGE uavRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, RT_UAVS, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[8]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0;
    for (int i = 1; i <= 5; i++) { rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[i].Descriptor.ShaderRegister = i - 1; }
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[6].DescriptorTable = {1, &texRange};
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[7].DescriptorTable = {1, &uavRange};
    D3D12_STATIC_SAMPLER_DESC ss{D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                 0, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0, D3D12_FLOAT32_MAX, 0, 0, D3D12_SHADER_VISIBILITY_ALL};
    rtRootSig = make_root({8, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = rtRootSig.Get(); cd.CS = {cso.data(), cso.size()};
    CHECK(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&rtPso)));

    // denoiser: 4 constants + UAV table (src, dst, gbuf)
    D3D12_DESCRIPTOR_RANGE dnRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
    D3D12_ROOT_PARAMETER ap[2]{};
    ap[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; ap[0].Constants = {0, 0, 4};
    ap[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; ap[1].DescriptorTable = {1, &dnRange};
    atrousRootSig = make_root({2, ap, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    auto acs = compile(kAtrous, "CSAtrous", "cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC ad{};
    ad.pRootSignature = atrousRootSig.Get(); ad.CS = {acs->GetBufferPointer(), acs->GetBufferSize()};
    CHECK(dev->CreateComputePipelineState(&ad, IID_PPV_ARGS(&atrousPso)));

    // composite: lighting + depth + gbuf SRVs, 8 constants; writes colour and depth
    D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    D3D12_ROOT_PARAMETER cp[2]{};
    cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; cp[0].DescriptorTable = {1, &srvRange};
    cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; cp[1].Constants = {0, 0, 12};
    compRootSig = make_root({2, cp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    auto vs = compile(kComposite, "VSFull", "vs_5_1"), ps = compile(kComposite, "PSComposite", "ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = compRootSig.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.SampleMask = UINT_MAX; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    CHECK(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&compPso)));

    // RT targets and their views: two UAV tables (ping-pong) and two composite SRV tables
    const auto UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; const auto UF = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (int i = 0; i < 2; i++) {
        accum[i] = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, UA, UF);
        gbuf[i] = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R32G32B32A32_UINT, UA, UF);
        dn[i] = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, UA, UF);
    }
    rtDepthTex = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R32_FLOAT, UA, UF);
    // DLSS-RR guides at full size (the render resolution is a sub-rectangle of them) + its output
    rrColor = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, UA, UF);
    rrAlbedo = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, UA, UF);
    rrSpecAlb = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, UA, UF);
    rrNormRough = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, UA, UF);
    rrMotion = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R16G16_FLOAT, UA, UF);
    rrOut = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, UA, UF);
    for (int p = 0; p < 2; p++) {
        UINT b = HEAP_RT + p * RT_UAVS;
        make_uav(accum[p].Get(), b + 0, DXGI_FORMAT_R16G16B16A16_FLOAT); make_uav(accum[1 - p].Get(), b + 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
        make_uav(gbuf[p].Get(), b + 2, DXGI_FORMAT_R32G32B32A32_UINT);  make_uav(gbuf[1 - p].Get(), b + 3, DXGI_FORMAT_R32G32B32A32_UINT);
        make_uav(rtDepthTex.Get(), b + 4, DXGI_FORMAT_R32_FLOAT);
        make_uav(rrColor.Get(), b + 5, DXGI_FORMAT_R16G16B16A16_FLOAT); make_uav(rrAlbedo.Get(), b + 6, DXGI_FORMAT_R8G8B8A8_UNORM);
        make_uav(rrSpecAlb.Get(), b + 7, DXGI_FORMAT_R8G8B8A8_UNORM);   make_uav(rrNormRough.Get(), b + 8, DXGI_FORMAT_R16G16B16A16_FLOAT);
        make_uav(rrMotion.Get(), b + 9, DXGI_FORMAT_R16G16_FLOAT);
    }
    nr_gpu_init();
    rtAvailable = true;
}

// ---------------------------------------------------------------- DLSS Ray Reconstruction (NVIDIA NGX)
// Replaces our temporal accumulation + denoiser + (no) anti-aliasing with one network: noisy per-frame radiance and
// guides in, clean anti-aliased (optionally upscaled) HDR image out. nvngx_dlssd.dll must sit next to this exe.
static bool rrAvail;
static NVSDK_NGX_Parameter *ngxCaps, *ngxParams;
static NVSDK_NGX_Handle *rrHandle;
static int rrModeCur = -1;
static UINT rrW, rrH;   // render (input) size for the current mode

static void ngx_init()
{
    // ponytail: project id is any GUID-shaped string for custom engines (no NVIDIA app id needed)
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("7c1f3e52-9a4d-4b8e-b2d6-5e0f9a3c71d4", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", L".", dev.Get());
    if (NVSDK_NGX_FAILED(r)) { fprintf(g_log, "DLSS: NGX init failed (%08x), using own denoiser\n", (unsigned)r); return; }
    int avail = 0, needDriver = 0, initRes = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&ngxCaps))) {
        NVSDK_NGX_Parameter_GetI(ngxCaps, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &avail);
        NVSDK_NGX_Parameter_GetI(ngxCaps, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needDriver);
        NVSDK_NGX_Parameter_GetI(ngxCaps, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &initRes);
    }
    NVSDK_NGX_D3D12_AllocateParameters(&ngxParams);
    rrAvail = avail && ngxParams;
    fprintf(g_log, "DLSS Ray Reconstruction: %s (needs newer driver %d, init result %08x)\n", rrAvail ? "available" : "NOT available", needDriver, (unsigned)initRes);
}

// (Re)create the RR feature when the chosen mode changes; records into the open command list. Returns true if RR runs.
static bool rr_update(int mode)
{
    if (!rrAvail) mode = 0;
    if (mode == rrModeCur) return rrHandle != nullptr;
    if (rrHandle) { NVSDK_NGX_D3D12_ReleaseFeature(rrHandle); rrHandle = nullptr; }   // GPU is idle: one frame in flight
    rrModeCur = mode; rrW = WIN_W; rrH = WIN_H;
    if (!mode) return false;
    static const NVSDK_NGX_PerfQuality_Value pq[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_DLAA,
        NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf};
    if (mode > 1) {
        unsigned ow = 0, oh = 0, mxw, mxh, mnw, mnh; float sharp;
        if (NVSDK_NGX_SUCCEED(NGX_DLSSD_GET_OPTIMAL_SETTINGS(ngxCaps, WIN_W, WIN_H, pq[mode], &ow, &oh, &mxw, &mxh, &mnw, &mnh, &sharp)) && ow && oh)
            { rrW = std::min(ow, WIN_W); rrH = std::min(oh, WIN_H); }
    }
    NVSDK_NGX_DLSSD_Create_Params cp{};
    cp.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;   // roughness in normals.w
    cp.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;              // our NDC depth (near 1, far 6000, not inverted)
    cp.InWidth = rrW; cp.InHeight = rrH; cp.InTargetWidth = WIN_W; cp.InTargetHeight = WIN_H;
    cp.InPerfQualityValue = pq[mode];
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSD_EXT(cl.Get(), 1, 1, &rrHandle, ngxParams, &cp);
    fprintf(g_log, "DLSS RR %s: render %ux%u -> %ux%u: %s (%08x)\n", kDlssNames[mode], rrW, rrH, WIN_W, WIN_H,
            NVSDK_NGX_SUCCEED(r) ? "created" : "FAILED", (unsigned)r);
    fflush(g_log);
    if (NVSDK_NGX_FAILED(r)) { rrHandle = nullptr; rrW = WIN_W; rrH = WIN_H; return false; }
    return true;
}

static float halton_seq(uint32_t i, uint32_t b) { float f = 1, r = 0; for (; i; i /= b) { f /= b; r += f * (i % b); } return r; }

// ---------------------------------------------------------------- DLSS 5 neural rendering (unofficial)
// NVIDIA ships no public integration for DLSS 5. Driven the way the OptiScaler DLSS-NR module does it: the model
// (nvngx_dlssnr.dll, supplied by the user) is NGX feature 18 with "DLSSNR.*" parameters on the NGX capability block;
// tuning is read once at creation; calls go through tpwrtx_nvngx.dll_forwarder.dll because the model only accepts
// callers named like nvngx.dll. It runs on the finished, display-referred frame (what it was trained on), before the
// UI, water and smoke are drawn, and its picture is blended back by luminance/colour strength with a highlight guard.
typedef int (*PFN_FwdLoad)(const wchar_t *);
typedef int (*PFN_FwdInit)(unsigned long long, const wchar_t *, void *, int, const void *);
typedef int (*PFN_FwdCreate)(void *, int, void *, void **);
typedef int (*PFN_FwdEval)(void *, void *, void *);
typedef int (*PFN_FwdRelease)(void *);
static struct {
    bool tried, ok;
    PFN_FwdCreate create; PFN_FwdEval eval; PFN_FwdRelease release;
    void *feature;
    float built[5];   // style, preset, intensity, structure, tone the feature was created with
    bool fresh;       // first evaluate after creation: reset the model's history
} nr;
static ComPtr<ID3D12Resource> nrIn, nrOut;
static ComPtr<ID3D12RootSignature> nrBlendRoot;
static ComPtr<ID3D12PipelineState> nrBlendPso;

static const char *kNrBlend = R"(
Texture2D<float4> orig : register(t0);   // the frame as we composed it (display-referred)
Texture2D<float4> model : register(t1);  // the model's picture of it
cbuffer K : register(b0) { float strength, colour, guard, pad; };
float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float4 VSFull(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 PSBlend(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    float3 o = pow(saturate(orig.Load(p).rgb), 2.2), m = pow(saturate(model.Load(p).rgb), 2.2);   // in linear light
    float r = min(lum(m) / max(lum(o), 1e-4), guard);   // brightness ratio, capped: no blown-out highlights
    float3 lumOnly = o * r;                             // the model's light with our colour
    float3 capped = m * min(1, guard * max(lum(o), 1e-4) / max(lum(m), 1e-4));
    float3 target = lerp(lumOnly, capped, colour);
    return float4(pow(saturate(lerp(o, target, strength)), 1 / 2.2), 1);
}
)";

static void nr_gpu_init()
{
    nrIn = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    nrOut = texture2d(WIN_W, WIN_H, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = {1, &srvRange};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = {0, 0, 4};
    nrBlendRoot = make_root({2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    auto vs = compile(kNrBlend, "VSFull", "vs_5_1"), ps = compile(kNrBlend, "PSBlend", "ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = nrBlendRoot.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;   // keep the composite's depth for the raster passes on top
    pd.SampleMask = UINT_MAX; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    CHECK(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&nrBlendPso)));
}

// Load the forwarder + model and initialise it, once. False (and a log line) if anything is missing or refuses.
static bool nr_load()
{
    if (nr.tried) return nr.ok;
    nr.tried = true;
    wchar_t model[MAX_PATH];
    if (!ngxCaps || !GetFullPathNameW(L"nvngx_dlssnr.dll", MAX_PATH, model, nullptr) || GetFileAttributesW(model) == INVALID_FILE_ATTRIBUTES) {
        fprintf(g_log, "DLSS 5: off (nvngx_dlssnr.dll is not in the game folder%s)\n", ngxCaps ? "" : ", or NGX is unavailable");
        fflush(g_log);
        return false;
    }
    HMODULE fwd = LoadLibraryW(L"tpwrtx_nvngx.dll_forwarder.dll");
    PFN_FwdLoad load = fwd ? (PFN_FwdLoad)GetProcAddress(fwd, "fwd_load") : nullptr;
    PFN_FwdInit init = fwd ? (PFN_FwdInit)GetProcAddress(fwd, "fwd_init") : nullptr;
    nr.create = fwd ? (PFN_FwdCreate)GetProcAddress(fwd, "fwd_create") : nullptr;
    nr.eval = fwd ? (PFN_FwdEval)GetProcAddress(fwd, "fwd_evaluate") : nullptr;
    nr.release = fwd ? (PFN_FwdRelease)GetProcAddress(fwd, "fwd_release") : nullptr;
    if (!load || !init || !nr.create || !nr.eval || !nr.release) { fprintf(g_log, "DLSS 5: forwarder DLL missing or incomplete\n"); fflush(g_log); return false; }
    if (!load(model)) { fprintf(g_log, "DLSS 5: nvngx_dlssnr.dll would not load or lacks the NGX entry points (%lu)\n", GetLastError()); fflush(g_log); return false; }
    // SDK version 0x15 and this generic app id are what the working community integration passes
    int r = init(0x24480451ull, L".", dev.Get(), 0x15, ngxCaps);
    fprintf(g_log, "DLSS 5: model init %s (%08x)\n", r == 1 ? "ok" : "FAILED", (unsigned)r);
    fflush(g_log);
    nr.ok = r == 1;
    return nr.ok;
}

// Every DLSSNR value the model reads. The capability block is shared, so everything is written again each time.
static void nr_params(bool atCreate)
{
    NVSDK_NGX_Parameter *P = ngxCaps;
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.Enabled", 1);
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.Width", (int)WIN_W);
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.Height", (int)WIN_H);
    if (atCreate) {
        NVSDK_NGX_Parameter_SetI(P, "CreationNodeMask", 1);
        NVSDK_NGX_Parameter_SetI(P, "VisibilityNodeMask", 1);
        NVSDK_NGX_Parameter_SetI(P, "DLSSNR.Hint.Render.Preset", (int)S(ST_NRPRESET));
        NVSDK_NGX_Parameter_SetI(P, "DLSSNR.UICorrection", 1);
    }
    NVSDK_NGX_Parameter_SetF(P, "DLSSNR.Intensity", S(ST_NRINTENSITY));
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.Style", (int)S(ST_NRSTYLE));
    NVSDK_NGX_Parameter_SetF(P, "DLSSNR.LocalStructureStrength", S(ST_NRSTRUCTURE));
    NVSDK_NGX_Parameter_SetF(P, "DLSSNR.LocalToneStrength", S(ST_NRTONE));
    NVSDK_NGX_Parameter_SetF(P, "DLSSNR.SkinStructureStrength", -1.0f);   // model default
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.UseAutoMask", 1);
    for (const char *k : {"DLSSNR.UI", "DLSSNR.UIAlpha", "DLSSNR.Backbuffer"}) NVSDK_NGX_Parameter_SetVoidPointer(P, k, nullptr);   // no stale pointers
}

// Create (or rebuild after a tuning change) on this command list. True only if a feature from an earlier frame is
// ready: creating and evaluating in the same command list is what hung GPUs in the community integration.
static bool nr_ready()
{
    float want[5] = {S(ST_NRSTYLE), S(ST_NRPRESET), S(ST_NRINTENSITY), S(ST_NRSTRUCTURE), S(ST_NRTONE)};
    if (nr.feature && !memcmp(want, nr.built, sizeof want)) return true;
    if (nr.feature) { nr.release(nr.feature); nr.feature = nullptr; }   // GPU is idle: one frame in flight
    static int fails;
    if (fails >= 3) return false;
    nr_params(true);
    int r = nr.create(cl.Get(), 18, ngxCaps, &nr.feature);
    if (r != 1 || !nr.feature) {
        nr.feature = nullptr; fails++;
        fprintf(g_log, "DLSS 5: create FAILED (%08x)%s\n", (unsigned)r, fails >= 3 ? ", giving up for this session" : "");
    } else {
        memcpy(nr.built, want, sizeof want); nr.fresh = true;
        fprintf(g_log, "DLSS 5: model created at %ux%u (style %s, preset %d, intensity %.2f)\n", WIN_W, WIN_H,
                kNrStyleNames[std::clamp((int)want[0], 0, 2)], (int)want[1], want[2]);
    }
    fflush(g_log);
    return false;
}

// The pass: backbuffer (composited frame) -> model -> blended back into the backbuffer. Leaves the backbuffer as the
// render target again. depth/motion are the RT guides at render size (rw x rh).
static void nr_pass(ID3D12Resource *backbuf, UINT rw, UINT rh, bool reset)
{
    const auto SR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(backbuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(nrIn.Get(), backbuf);
    transition(nrIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, SR);
    transition(rtDepthTex.Get(), UA, SR); transition(rrMotion.Get(), UA, SR);
    NVSDK_NGX_Parameter *P = ngxCaps;
    nr_params(false);
    NVSDK_NGX_Parameter_SetVoidPointer(P, "DLSSNR.Color", nrIn.Get());
    NVSDK_NGX_Parameter_SetVoidPointer(P, "DLSSNR.Depth", rtDepthTex.Get());
    NVSDK_NGX_Parameter_SetVoidPointer(P, "DLSSNR.MVec", rrMotion.Get());
    NVSDK_NGX_Parameter_SetVoidPointer(P, "DLSSNR.Output", nrOut.Get());
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.DepthInverted", 0);
    NVSDK_NGX_Parameter_SetI(P, "DLSSNR.Reset", (reset || nr.fresh) ? 1 : 0);
    struct { const char *k; UINT w, h; } sub[] = {{"Color", WIN_W, WIN_H}, {"Output", WIN_W, WIN_H}, {"Depth", rw, rh}, {"MVec", rw, rh}};
    char name[64];
    for (auto &s : sub) {
        snprintf(name, sizeof name, "DLSSNR.%sSubrectBaseX", s.k); NVSDK_NGX_Parameter_SetI(P, name, 0);
        snprintf(name, sizeof name, "DLSSNR.%sSubrectBaseY", s.k); NVSDK_NGX_Parameter_SetI(P, name, 0);
        snprintf(name, sizeof name, "DLSSNR.%sSubrectWidth", s.k); NVSDK_NGX_Parameter_SetI(P, name, (int)s.w);
        snprintf(name, sizeof name, "DLSSNR.%sSubrectHeight", s.k); NVSDK_NGX_Parameter_SetI(P, name, (int)s.h);
    }
    NVSDK_NGX_Parameter_SetF(P, "DLSSNR.MVecScaleX", 1.0f);   // our vectors are already in render pixels
    NVSDK_NGX_Parameter_SetF(P, "DLSSNR.MVecScaleY", 1.0f);
    int r = nr.eval(cl.Get(), nr.feature, P);
    static int evalFails;
    if (r != 1 && evalFails++ < 5) { fprintf(g_log, "DLSS 5: evaluate failed (%08x)\n", (unsigned)r); fflush(g_log); }
    nr.fresh = false;
    transition(rtDepthTex.Get(), SR, UA); transition(rrMotion.Get(), SR, UA);
    uav_barrier(nrOut.Get());
    transition(nrOut.Get(), UA, SR);
    transition(backbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // blend the model's picture back (NGX may have changed heaps/root state: set everything again)
    UINT tbl = HEAP_DYN + dynUsed; dynUsed += 2;
    make_srv(nrIn.Get(), tbl, DXGI_FORMAT_R8G8B8A8_UNORM); make_srv(nrOut.Get(), tbl + 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(nrBlendRoot.Get());
    cl->SetPipelineState(nrBlendPso.Get());
    cl->SetGraphicsRootDescriptorTable(0, gpu_desc(tbl));
    float kc[4] = {r == 1 ? S(ST_NRSTRENGTH) : 0.0f, S(ST_NRCOLOUR), S(ST_NRGUARD), 0};
    cl->SetGraphicsRoot32BitConstants(1, 4, kc, 0);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
    transition(nrOut.Get(), SR, UA);
    transition(nrIn.Get(), SR, D3D12_RESOURCE_STATE_COPY_DEST);
}

static void init_d3d(HWND hwnd)
{
    ComPtr<IDXGIFactory6> fac;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
    ComPtr<IDXGIAdapter1> ad;
    CHECK(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)));
    DXGI_ADAPTER_DESC1 add; ad->GetDesc1(&add);
    fprintf(g_log, "adapter: %ls\n", add.Description);
    CHECK(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    DXGI_SWAP_CHAIN_DESC1 sd{WIN_W, WIN_H, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, BUFS, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(fac->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
    CHECK(sc1.As(&swap));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BUFS};
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < BUFS; i++) {
        CHECK(swap->GetBuffer(i, IID_PPV_ARGS(&backbuf[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * rtvInc;
        dev->CreateRenderTargetView(backbuf[i].Get(), nullptr, h);
    }
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC dd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, WIN_W, WIN_H, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_D32_FLOAT}; cv.DepthStencil.Depth = 1;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depth)));
    dev->CreateDepthStencilView(depth.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HEAP_DYN + HEAP_DYN_N, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)));
    srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // raster root: b0 frame CBV, b1 4 draw constants, t0 table; static anisotropic wrap sampler
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = {1, 0, 4};
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable = {1, &range};
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss[2] = {
        {D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
         0, 16, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0, D3D12_FLOAT32_MAX, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL},
        {D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
         0, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0, 0, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL}};
    rootSig = make_root({3, rp, 2, ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
    vsCode = compile(kShader, "VSMain", "vs_5_1"); psCode = compile(kShader, "PSMain", "ps_5_1");

    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)));
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    fenceEvt = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    upload = buffer(UPLOAD_BYTES, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    CHECK(upload->Map(0, nullptr, (void **)&upPtr));
    readback = buffer((UINT64)WIN_W * WIN_H * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    // 1x1 white texture for untextured draws (descriptor slot 0)
    whiteTex = texture2d(1, 1, 1);
    memset(upPtr + OFF_STAGE, 0xff, 4);
    D3D12_TEXTURE_COPY_LOCATION dl{whiteTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, sl{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = {OFF_STAGE, {DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 256}};
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    transition(whiteTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    make_srv(whiteTex.Get(), 0);
    init_rt();
    submit_and_wait();
    if (rtAvailable) {   // create the DLSS feature up front so a failure shows in the log right away
        ngx_init();
        CHECK(alloc->Reset()); CHECK(cl->Reset(alloc.Get(), nullptr));
        rr_update((int)S(ST_DLSS));
        submit_and_wait();
    }
}

// ---------------------------------------------------------------- textures (with CPU-built mip chains)
static const D3D12_RESOURCE_STATES TEX_READ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

static UINT64 upload_texture(const TpwShared *sh, uint32_t id, UINT64 stageUsed)
{
    const TpwTexture &st = sh->tex[id - 1];
    TexCache &tc = texCache[id];
    uint32_t ver = st.version, w = st.width, h = st.height;
    if (!w || ver == tc.version) return stageUsed;
    UINT16 mips = 1;
    while ((w >> mips) || (h >> mips)) mips++;
    bool fresh = !tc.res || tc.w != w || tc.h != h;
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, mips, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 rowBytes[16], total;
    UINT64 base = align(OFF_STAGE + stageUsed, 512);
    dev->GetCopyableFootprints(&rd, 0, mips, base, fp, rows, rowBytes, &total);
    if (base - OFF_STAGE + total > STAGE_BYTES) return stageUsed;   // staging full: rest next frame
    if (fresh) { tc.res = texture2d(w, h, mips); tc.w = w; tc.h = h; tc.srv = false; }
    else transition(tc.res.Get(), TEX_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    // level 0 from shared memory, then 2x2 box filter downwards (straight in the staging buffer)
    const uint8_t *src = sh->texHeap + st.offset;
    for (UINT y = 0; y < h; y++) memcpy(upPtr + fp[0].Offset + y * fp[0].Footprint.RowPitch, src + (UINT64)y * w * 4, w * 4);
    // ponytail: flame detection by colour (bright, warm, mostly-visible pixels); a list of known textures if this misfires
    double acc[3] = {}; UINT64 n = 0, nz = 0, mid = 0;
    for (UINT64 i = 0; i < (UINT64)w * h * 4; i += 4) {
        uint8_t a = src[i + 3];
        if (a > 128) { acc[0] += src[i + 2]; acc[1] += src[i + 1]; acc[2] += src[i]; n++; }
        nz += a > 8; mid += a > 32 && a < 224;
    }
    // soft alpha over much of the texture = see-through (water, smoke, glass), not a cut-out with antialiased edges
    tc.translucent = mid * 4 > nz;
    for (int c = 0; c < 3; c++) tc.avg[c] = n ? (float)(acc[c] / n / 255) : 0;
    tc.emissive = n > (UINT64)w * h / 8 && tc.avg[0] > 0.7f && tc.avg[1] > 0.2f && tc.avg[1] < 0.8f && tc.avg[2] < 0.35f && tc.avg[0] - tc.avg[2] > 0.45f;
    for (UINT m = 1; m < mips; m++) {
        UINT pw = fp[m - 1].Footprint.Width, ph = fp[m - 1].Footprint.Height, cw = fp[m].Footprint.Width, chh = fp[m].Footprint.Height;
        for (UINT y = 0; y < chh; y++) for (UINT x = 0; x < cw; x++) {
            UINT sum[4] = {};
            for (UINT dy = 0; dy < 2; dy++) for (UINT dx = 0; dx < 2; dx++) {
                const uint8_t *p = upPtr + fp[m - 1].Offset + std::min(y * 2 + dy, ph - 1) * fp[m - 1].Footprint.RowPitch + std::min(x * 2 + dx, pw - 1) * 4;
                for (int c = 0; c < 4; c++) sum[c] += p[c];
            }
            uint8_t *d = upPtr + fp[m].Offset + y * fp[m].Footprint.RowPitch + x * 4;
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)((sum[c] + 2) / 4);
        }
    }
    for (UINT m = 0; m < mips; m++) {
        D3D12_TEXTURE_COPY_LOCATION dl{tc.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, sl{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dl.SubresourceIndex = m; sl.PlacedFootprint = fp[m];
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    transition(tc.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, TEX_READ);
    if (!tc.srv) { make_srv(tc.res.Get(), id); tc.srv = true; }
    tc.version = ver;
    return base - OFF_STAGE + total;
}

// ---------------------------------------------------------------- world cache: geometry that scrolled off-screen
struct CTri { GpuVertex v[3]; uint32_t mat[2]; };
static std::vector<CTri> cacheTris, cacheNext;

// 0 = fully off-screen (keep), 1 = fully on-screen (should have been drawn), 2 = straddles the edge
static int screen_class(const CTri &t, const M4 &vp)
{
    int out[3] = {}, inside = 0;
    for (int i = 0; i < 3; i++) {
        float p[3] = {t.v[i].x, t.v[i].y, t.v[i].z}, c[4];
        xform(p, vp, c);
        if (c[3] <= 0.5f) { out[i] = 16; continue; }
        float x = c[0] / c[3], y = c[1] / c[3];
        out[i] = (x < -1) | (x > 1) << 1 | (y < -1) << 2 | (y > 1) << 3;
        if (fabsf(x) < 0.96f && fabsf(y) < 0.96f) inside++;
    }
    if (out[0] & out[1] & out[2]) return 0;
    return inside == 3 ? 1 : 2;
}

static void update_cache(std::vector<CTri> &current, const M4 &gameVP, bool keepOld)
{
    cacheNext.assign(current.begin(), current.end());
    // Keep only what is fully off-screen now. Anything (partly) in view is either in this frame's draws already or
    // has moved/vanished: keeping it left trails of walking visitors and other copies along the screen edges.
    if (keepOld)
        for (const CTri &t : cacheTris) {
            if (cacheNext.size() >= CACHE_MAX_TRIS) break;
            if (screen_class(t, gameVP) == 0) cacheNext.push_back(t);
        }
    cacheTris.swap(cacheNext);
}

// ---------------------------------------------------------------- frame copy from shared memory (seqlock)
struct Frame { TpwFrameHeader hdr; std::vector<TpwDraw> draws; std::vector<TpwVertex> verts; std::vector<uint16_t> idx; };
static bool read_frame(const TpwShared *sh, Frame &f)
{
    for (int tries = 0; tries < 8; tries++) {
        const TpwFrameSlot &s = sh->slots[sh->latestSlot % TPWRTX_SLOTS];
        uint32_t q = s.h.seq;
        if (q & 1) continue;
        MemoryBarrier();
        memcpy(&f.hdr, (const void *)&s.h, sizeof f.hdr);
        uint32_t nd = std::min(f.hdr.numDraws, TPWRTX_MAX_DRAWS), nv = std::min(f.hdr.numVertices, TPWRTX_MAX_VERTS), ni = std::min(f.hdr.numIndices, TPWRTX_MAX_INDICES);
        f.draws.assign(s.draws, s.draws + nd); f.verts.assign(s.verts, s.verts + nv); f.idx.assign(s.indices, s.indices + ni);
        MemoryBarrier();
        if (s.h.seq == q) return true;
    }
    return false;
}

static bool is_additive(const TpwDraw &d) { return d.alphaBlend && d.dstBlend == 2; }   // D3DBLEND_ONE destination: glows / flames

// Build BLAS + TLAS over the triangle soup in the upload buffer (opaque first, then alpha-tested).
static bool build_accel(uint32_t opaqueTris, uint32_t alphaTris, uint32_t &alphaGeom)
{
    D3D12_GPU_VIRTUAL_ADDRESS gpu = upload->GetGPUVirtualAddress();
    D3D12_RAYTRACING_GEOMETRY_DESC g[2]{};
    UINT ng = 0;
    uint32_t counts[2] = {opaqueTris, alphaTris};
    alphaGeom = opaqueTris ? 1 : 0;
    for (int i = 0; i < 2; i++) {
        if (!counts[i]) continue;
        D3D12_RAYTRACING_GEOMETRY_DESC &d = g[ng++];
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = i == 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT; d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.IndexCount = counts[i] * 3; d.Triangles.VertexCount = (opaqueTris + alphaTris) * 3;
        d.Triangles.IndexBuffer = gpu + OFF_RTI + (i == 0 ? 0 : (UINT64)opaqueTris * 12);
        d.Triangles.VertexBuffer = {gpu + OFF_RTV, sizeof(GpuVertex)};
    }
    if (!ng) return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    bi.NumDescs = ng; bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bi.pGeometryDescs = g;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pb{}, pt{};
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&bi, &pb);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; ti.NumDescs = 1; ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.InstanceDescs = gpu + OFF_INST;
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &pt);
    const auto AS = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE; const auto UF = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (pb.ResultDataMaxSizeInBytes > blasSize) { blasSize = align(pb.ResultDataMaxSizeInBytes * 3 / 2, 65536); blas = buffer(blasSize, D3D12_HEAP_TYPE_DEFAULT, AS, UF); }
    if (pt.ResultDataMaxSizeInBytes > tlasSize) { tlasSize = align(pt.ResultDataMaxSizeInBytes, 65536); tlas = buffer(tlasSize, D3D12_HEAP_TYPE_DEFAULT, AS, UF); }
    UINT64 sc = std::max(pb.ScratchDataSizeInBytes, pt.ScratchDataSizeInBytes);
    if (sc > scratchSize) { scratchSize = align(sc * 3 / 2, 65536); scratch = buffer(scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, UF); }
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1;
    inst.InstanceMask = 0xff; inst.AccelerationStructure = blas->GetGPUVirtualAddress();
    memcpy(upPtr + OFF_INST, &inst, sizeof inst);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = bi; bd.DestAccelerationStructureData = blas->GetGPUVirtualAddress(); bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    uav_barrier(blas.Get()); uav_barrier(scratch.Get());
    bd.Inputs = ti; bd.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    uav_barrier(tlas.Get());
    return true;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    g_log = fopen("tpwrtx_viewer.log", "w");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DWORD pid = 0;
    for (int i = 1; i < __argc; i++) {
        if (!strcmp(__argv[i], "--orbit") && i + 2 < __argc) {
            g_in.orbit = true; g_in.yaw = (float)atof(__argv[i + 1]) * 0.01745f; g_in.pitch = (float)atof(__argv[i + 2]) * 0.01745f; i += 2;
        } else pid = strtoul(__argv[i], nullptr, 10);
    }
    HANDLE game = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (FILE *ini = fopen("tpwrtx.ini", "r")) {   // overlay = 1, height = 1440 | auto, resolution = 2560x1440 | auto
        char line[128];
        UINT aw = 4, ah = 3, h = WIN_H;
        bool hAuto = false;
        while (fgets(line, sizeof line, ini)) {
            int v; UINT rw, rh;
            const char *p = line + strspn(line, " \t");
            if (*p == ';' || *p == '#') continue;
            if (sscanf(p, "overlay = %d", &v) == 1) g_overlay = v != 0;
            if (sscanf(p, "height = %d", &v) == 1 && v >= 480 && v <= 2160) h = (UINT)v;
            else if (!strncmp(p, "height", 6) && strstr(p, "auto")) hAuto = true;   // the game window's client size (the hook fits it to the work area)
            // widescreen: same aspect as the game's resolution (the hook patches the game to it)
            if (sscanf(p, "resolution = %ux%u", &rw, &rh) == 2 && rh >= 480 && rw >= rh) { aw = rw; ah = rh; }
            else if (!strncmp(p, "resolution", 10) && strstr(p, "auto")) { aw = GetSystemMetrics(SM_CXSCREEN); ah = GetSystemMetrics(SM_CYSCREEN); }
        }
        fclose(ini);
        if (hAuto) {
            RECT wa, fr{0, 0, 100, 100};
            SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
            AdjustWindowRect(&fr, WS_OVERLAPPEDWINDOW, FALSE);
            int ww = (int)(wa.right - wa.left) - (int)(fr.right - fr.left - 100), wh = (int)(wa.bottom - wa.top) - (int)(fr.bottom - fr.top - 100);
            h = (UINT)std::min(wh, (int)((INT64)ww * ah / aw));
        }
        WIN_H = std::clamp(h, 480u, 2160u) & ~7u;
        WIN_W = (UINT)((UINT64)WIN_H * aw / ah) & ~7u;
    }
    fprintf(g_log, "render %ux%u, %s\n", WIN_W, WIN_H, g_overlay ? "overlay over the game" : "window");

    WNDCLASSA wc{CS_OWNDC, wndproc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, "tpwrtx"};
    RegisterClassA(&wc);
    RECT mon; SystemParametersInfoA(SPI_GETWORKAREA, 0, &mon, 0);
    EnumDisplayMonitors(nullptr, nullptr, pick_monitor, (LPARAM)&mon);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, (LONG)WIN_W, (LONG)WIN_H}; AdjustWindowRect(&r, style, FALSE);
    // Never activatable and not in alt-tab: the game pauses whenever it loses focus. Mouse drag/wheel still reach it;
    // keys are global hotkeys instead (Ctrl+Alt+R overlay, T ray tracing, C camera, O auto-orbit).
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "tpwrtx", "TPW RTX viewer", style, mon.left + 20, mon.top + 20,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
    RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T');
    RegisterHotKey(hwnd, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    RegisterHotKey(hwnd, 4, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
    RegisterHotKey(hwnd, 5, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    settings_load();
    panel_create(inst);
    if (!g_overlay) ShowWindow(hwnd, SW_SHOWNOACTIVATE);   // overlay mode: stays hidden, the hook shows the image in the game window
    init_d3d(hwnd);
    fflush(g_log);

    HANDLE map = nullptr;
    TpwShared *sh = nullptr;
    Frame fr;
    std::vector<uint32_t> idxOut;
    std::vector<CTri> curTris;
    std::vector<Light> lights;
    M4 prevVP{};
    bool havePrev = false, prevRt = false;
    uint32_t rtFrame = 0;
    DWORD t0 = GetTickCount(), lastTitle = 0, frames = 0;
    uint32_t lastGameFrame = 0;
    size_t lastRtTris = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return 0; TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0) return 0;   // game exited
        if (!sh) {
            if (!map) map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, TPWRTX_MAP_NAME);
            if (map) sh = (TpwShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (!sh || sh->magic != TPWRTX_MAGIC) { sh = nullptr; Sleep(100); continue; }
            fprintf(g_log, "mapped shared memory (hook pid %u)\n", sh->hookPid); fflush(g_log);
            g_shm = sh;
            if (g_overlay) {   // shared output texture + fence, handles duplicated into the game process for the hook
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, WIN_W, WIN_H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
                CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outTex)));
                CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&outFence)));
                HANDLE ht, hf, gp = OpenProcess(PROCESS_DUP_HANDLE, FALSE, sh->hookPid), gt = nullptr, gf = nullptr;
                CHECK(dev->CreateSharedHandle(outTex.Get(), nullptr, GENERIC_ALL, nullptr, &ht));
                CHECK(dev->CreateSharedHandle(outFence.Get(), nullptr, GENERIC_ALL, nullptr, &hf));
                if (gp && DuplicateHandle(GetCurrentProcess(), ht, gp, &gt, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
                    DuplicateHandle(GetCurrentProcess(), hf, gp, &gf, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    sh->outWidth = WIN_W; sh->outHeight = WIN_H; sh->outFenceValue = 0; sh->outVisible = 0;
                    sh->outFenceHandle = (uint32_t)(UINT_PTR)gf;
                    MemoryBarrier();
                    sh->outTexHandle = (uint32_t)(UINT_PTR)gt;   // last: the hook starts once this is set
                    fprintf(g_log, "overlay output shared with the game process\n");
                } else fprintf(g_log, "overlay: handle duplication into the game failed (%lu)\n", GetLastError());
                fflush(g_log);
            }
        }
        bool have = read_frame(sh, fr) && fr.hdr.width;
        sh->camZoomMin = S(ST_ZOOMMIN); sh->camZoomMax = S(ST_ZOOMMAX); sh->camWasd = S(ST_WASD) > 0.5f;
        sh->camFov = S(ST_FOV);   // last: the hook starts applying camera options once this is set
        if (GetFileAttributesA("tpwrtx_shot") != INVALID_FILE_ATTRIBUTES) { DeleteFileA("tpwrtx_shot"); g_in.shot = true; }
        read_cmd_file();
        if (g_dumpTex && g_dumpTex <= sh->texCount && sh->tex[g_dumpTex - 1].width) {
            const TpwTexture &tt = sh->tex[g_dumpTex - 1];
            std::vector<uint8_t> px(tt.width * tt.height * 4);
            const uint8_t *s = sh->texHeap + tt.offset;
            for (size_t i = 0; i < px.size(); i += 4) { px[i] = s[i + 2]; px[i + 1] = s[i + 1]; px[i + 2] = s[i]; px[i + 3] = s[i + 3]; }
            char name[64]; snprintf(name, sizeof name, "tpwrtx_tex%u.png", g_dumpTex);
            write_png(name, tt.width, tt.height, px.data(), tt.width * 4);
            g_dumpTex = 0;
        }
        float t = (GetTickCount() - t0) / 1000.0f;

        CHECK(alloc->Reset());
        CHECK(cl->Reset(alloc.Get(), nullptr));
        D3D12_GPU_VIRTUAL_ADDRESS gpu = upload->GetGPUVirtualAddress();
        dynUsed = 0;
        ID3D12Resource *lit = nullptr, *litGbuf = nullptr;   // this frame's (denoised) lighting and its G-buffer
        float compSun[3] = {}, depthScale[2] = {1, 1};
        UINT frameRw = WIN_W, frameRh = WIN_H;   // this frame's render size (guides for the DLSS 5 pass)
        bool frameReset = false;
        int wantRr = (int)S(ST_DLSS);
        bool rrFresh = rtAvailable && wantRr != rrModeCur;   // new feature: reset its history
        bool rrOn = rtAvailable && rr_update(wantRr);

        if (have && sh->texGeneration != texGen) { for (auto &tc : texCache) tc = TexCache{}; texGen = sh->texGeneration; cacheTris.clear(); }
        UINT64 stageUsed = 0;
        if (have) for (const TpwDraw &d : fr.draws)
            if (d.texId && d.texId <= TPWRTX_MAX_TEX) stageUsed = upload_texture(sh, d.texId, stageUsed);

        // vertices: world band -> world space (solved projection), UI band -> NDC
        struct DrawCmd { const TpwDraw *d; UINT first, count; bool inRt; };
        std::vector<DrawCmd> world, ui;
        idxOut.clear(); curTris.clear(); lights.clear();
        M4 vp{}, gameVP{}, viewM{}, projM{};
        bool camOk = false;
        const TpwFrameHeader &h = fr.hdr;
        if (have) {
            // Outside a park (menus) the camera globals are unset: rebuild in view space instead (camera = identity).
            static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float l0 = h.camInv[0] * h.camInv[0] + h.camInv[1] * h.camInv[1] + h.camInv[2] * h.camInv[2];
            camOk = fabsf(h.camInv[15] - 1) < 1e-3f && fabsf(l0 - 1) < 1e-2f;
            const float *M = camOk ? h.camInv : kIdent, *V = camOk ? h.view : kIdent;
            GpuVertex *out = (GpuVertex *)(upPtr + OFF_VB);
            for (size_t i = 0; i < fr.verts.size(); i++) {
                const TpwVertex &v = fr.verts[i];
                GpuVertex &o = out[i];
                o.color = v.diffuse; o.u = v.u; o.v = v.v;
                float z = v.rhw > 0 ? h.k / v.rhw : 0, x = (v.x - h.cx) * z / h.f, y = -(v.y - h.cy) * z / h.f;
                o.x = x * M[0] + y * M[4] + z * M[8] + M[12];
                o.y = x * M[1] + y * M[5] + z * M[9] + M[13];
                o.z = x * M[2] + y * M[6] + z * M[10] + M[14];
            }
            for (const TpwDraw &d : fr.draws) {
                bool isUI = (d.flags & TPW_DRAW_UI) != 0;
                if (d.firstVertex + d.numVertices > fr.verts.size() || d.firstIndex + d.numIndices > fr.idx.size()) continue;
                if (isUI) for (UINT i = 0; i < d.numVertices; i++) {   // pre-transformed screen coords -> NDC
                    const TpwVertex &v = fr.verts[d.firstVertex + i];
                    GpuVertex &o = out[d.firstVertex + i];
                    o.x = v.x / h.width * 2 - 1; o.y = 1 - v.y / h.height * 2; o.z = v.z;
                }
                // The game draws foliage, fences and visitor sprites blended *and* alpha-tested (blend only softens edges):
                // those go into the ray tracing as cut-outs. Truly translucent draws (water, glass) and flames stay rasterized on top.
                const TexCache *tcd = d.texId && d.texId <= TPWRTX_MAX_TEX ? &texCache[d.texId] : nullptr;
                // only standard alpha blending counts as a cut-out; other blends (e.g. the multiply-blended visitor
                // shadows) keep their own blend on top, otherwise they turn into white cards in the ray tracer
                // ... and only if it is not actually see-through: soft texture alpha or faded vertex alpha (smoke puffs,
                // balloons, fountain water) stay blended on top, otherwise they turn solid
                uint32_t minA = 255;
                for (UINT i = 0; i < d.numVertices; i++) minA = std::min(minA, fr.verts[d.firstVertex + i].diffuse >> 24);
                bool seeThrough = (tcd && tcd->translucent) || minA < 230;
                bool cutout = d.alphaBlend && d.alphaTest && d.alphaRef >= 8 && d.srcBlend == 5 && d.dstBlend == 6 && !seeThrough;
                bool emissive = tcd && tcd->emissive;
                bool rtGeom = !isUI && !emissive && (!d.alphaBlend || cutout);
                uint32_t mat0 = (tcd && tcd->srv ? d.texId : 0) | ((d.alphaTest || d.colorKey || cutout) ? 0x10000u : 0u);
                UINT first = (UINT)idxOut.size();
                float lc[3] = {}, lp[3] = {}; uint32_t ln = 0;
                for (UINT i = 0; i + 2 < d.numIndices; i += 3) {
                    uint32_t a = d.firstVertex + fr.idx[d.firstIndex + i], b = d.firstVertex + fr.idx[d.firstIndex + i + 1], c = d.firstVertex + fr.idx[d.firstIndex + i + 2];
                    if (a >= fr.verts.size() || b >= fr.verts.size() || c >= fr.verts.size()) continue;
                    if (!isUI && (fr.verts[a].rhw <= 0 || fr.verts[b].rhw <= 0 || fr.verts[c].rhw <= 0)) continue;   // behind the eye
                    idxOut.push_back(a); idxOut.push_back(b); idxOut.push_back(c);
                    if (rtGeom) curTris.push_back({{out[a], out[b], out[c]}, {mat0, std::max<uint32_t>(d.alphaRef, 1)}});
                    if (!isUI && (emissive || is_additive(d))) for (uint32_t k : {a, b, c}) {
                        lp[0] += out[k].x; lp[1] += out[k].y; lp[2] += out[k].z;
                        if (emissive) for (int ch = 0; ch < 3; ch++) lc[ch] += tcd->avg[ch];
                        else { lc[0] += ((out[k].color >> 16) & 255) / 255.0f; lc[1] += ((out[k].color >> 8) & 255) / 255.0f; lc[2] += (out[k].color & 255) / 255.0f; }
                        ln++;
                    }
                }
                if (ln && camOk && lights.size() < MAX_LIGHTS) {   // flame / glow sprite -> point light (merged when close)
                    float tk = S(ST_TORCH);
                    Light L{{lp[0] / ln, lp[1] / ln + 0.5f, lp[2] / ln}, 25.0f, {lc[0] / ln * 3.0f * tk, lc[1] / ln * 2.4f * tk, lc[2] / ln * 1.6f * tk}, 0};
                    bool merged = false;
                    for (Light &o : lights) {
                        float dx = o.pos[0] - L.pos[0], dy = o.pos[1] - L.pos[1], dz = o.pos[2] - L.pos[2];
                        if (dx * dx + dy * dy + dz * dz < 16) { merged = true; break; }
                    }
                    if (!merged) lights.push_back(L);
                }
                if (g_dumpDraws && !isUI && (d.alphaBlend || d.alphaTest) && d.numVertices) {
                    const GpuVertex &v0 = out[d.firstVertex];
                    fprintf(g_log, "draw blend=%d src=%d dst=%d atest=%d ref=%d zw=%d tex=%u %ux%u nv=%u col=%08x pos=(%.1f %.1f %.1f)\n", d.alphaBlend, d.srcBlend, d.dstBlend,
                            d.alphaTest, d.alphaRef, d.zWrite, d.texId, d.texId ? sh->tex[d.texId - 1].width : 0, d.texId ? sh->tex[d.texId - 1].height : 0,
                            d.numVertices, v0.color, v0.x, v0.y, v0.z);
                }
                (isUI ? ui : world).push_back({&d, first, (UINT)idxOut.size() - first, rtGeom});
            }
            if (g_dumpDraws) { g_dumpDraws = false; fflush(g_log); }
            memcpy(upPtr + OFF_IB, idxOut.data(), idxOut.size() * 4);
            gameVP = mul(from16(V), proj_game(h.f, h.cx, h.cy, (float)h.width, (float)h.height));
            viewM = g_in.orbit ? orbit_view(M, t) : from16(V);
            projM = proj_game(h.f, h.cx, h.cy, (float)h.width, (float)h.height);
            vp = mul(viewM, projM);
            memcpy(upPtr + OFF_CB, vp.m, 64);
        }

        // ---- ray-traced lighting
        bool doRt = have && rtAvailable && g_in.rt && camOk;
        if (doRt) {
            update_cache(curTris, gameVP, g_in.cache);
            // triangle soup: opaque first, then alpha-tested
            GpuVertex *rv = (GpuVertex *)(upPtr + OFF_RTV);
            uint32_t *ri = (uint32_t *)(upPtr + OFF_RTI), *rm = (uint32_t *)(upPtr + OFF_MAT), nTri = 0, opaque = 0;
            for (int pass = 0; pass < 2; pass++)
                for (const CTri &c : cacheTris) {
                    if (((c.mat[0] & 0x10000) != 0) != (pass == 1) || nTri >= RT_MAX_TRIS) continue;
                    memcpy(rv + nTri * 3, c.v, sizeof c.v);
                    ri[nTri * 3] = nTri * 3; ri[nTri * 3 + 1] = nTri * 3 + 1; ri[nTri * 3 + 2] = nTri * 3 + 2;
                    rm[nTri * 2] = c.mat[0]; rm[nTri * 2 + 1] = c.mat[1];
                    nTri++; if (pass == 0) opaque++;
                }
            lastRtTris = nTri;
            memcpy(upPtr + OFF_LIGHT, lights.data(), lights.size() * sizeof(Light));
            uint32_t alphaGeom = 1;
            if (build_accel(opaque, nTri - opaque, alphaGeom)) {
                RTConsts k{};
                M4 inv = inverse(vp);
                memcpy(k.invViewProj, inv.m, 64); memcpy(k.viewProj, vp.m, 64); memcpy(k.prevViewProj, (havePrev ? prevVP : vp).m, 64);
                float camW[4], o0[3] = {0, 0, 0};
                M4 vinv = inverse(g_in.orbit ? orbit_view(h.camInv, t) : from16(h.view));
                xform(o0, vinv, camW);
                memcpy(k.camPos, camW, 12);
                k.frame = rtFrame++;
                float az = S(ST_SUNAZ) * 0.01745f, el = S(ST_SUNEL) * 0.01745f, sun = S(ST_SUN), sky = S(ST_SKY);
                k.sunDir[0] = cosf(el) * sinf(az); k.sunDir[1] = sinf(el); k.sunDir[2] = cosf(el) * cosf(az);
                k.numLights = (uint32_t)lights.size();
                k.sunColor[0] = 1.9f * sun; k.sunColor[1] = 1.8f * sun; k.sunColor[2] = 1.6f * sun;
                UINT rw = rrOn ? rrW : WIN_W, rh = rrOn ? rrH : WIN_H;   // render size (DLSS may upscale from less)
                k.pixelSpread = 2 * atanf(h.height / 2.0f / h.f) / rh;
                k.skyZenith[0] = 0.25f * sky; k.skyZenith[1] = 0.38f * sky; k.skyZenith[2] = 0.62f * sky;
                k.skyHorizon[0] = 0.50f * sky; k.skyHorizon[1] = 0.55f * sky; k.skyHorizon[2] = 0.62f * sky;
                k.opaqueTris = opaque; k.alphaGeom = alphaGeom;
                k.dim[0] = rw; k.dim[1] = rh;
                if (rrOn || S(ST_JITTER) > 0.5f) {   // RR needs sub-pixel jitter (the guide recommends >= 32 phases)
                    uint32_t hi = (k.frame & (rrOn ? 63 : 15)) + 1;
                    k.jitter[0] = halton_seq(hi, 2) - 0.5f; k.jitter[1] = halton_seq(hi, 3) - 0.5f;
                }
                k.rrMode = rrOn ? 1u : 0u;
                frameRw = rw; frameRh = rh;
                k.resetHistory = (!havePrev || !prevRt || rrFresh) ? 1u : 0u;
                k.normalStr = S(ST_NORMAL); k.specStr = S(ST_SPEC); k.gloss = S(ST_GLOSS);
                k.tintMix = S(ST_TINT); k.shadeMix = S(ST_SHADE); k.bounce = S(ST_BOUNCE); k.historyMax = S(ST_HISTORY);
                k.sunSoft = S(ST_SUNSOFT);
                frameReset = k.resetHistory != 0;
                memcpy(compSun, k.sunColor, 12);
                memcpy(upPtr + OFF_CB + 256, &k, sizeof k);
                UINT p = k.frame & 1;
                ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
                cl->SetDescriptorHeaps(1, heaps);
                cl->SetComputeRootSignature(rtRootSig.Get());
                cl->SetPipelineState(rtPso.Get());
                cl->SetComputeRootConstantBufferView(0, gpu + OFF_CB + 256);
                cl->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress());
                cl->SetComputeRootShaderResourceView(2, gpu + OFF_RTV);
                cl->SetComputeRootShaderResourceView(3, gpu + OFF_RTI);
                cl->SetComputeRootShaderResourceView(4, gpu + OFF_MAT);
                cl->SetComputeRootShaderResourceView(5, gpu + OFF_LIGHT);
                cl->SetComputeRootDescriptorTable(6, gpu_desc(0));
                cl->SetComputeRootDescriptorTable(7, gpu_desc(HEAP_RT + p * RT_UAVS));
                cl->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
                uav_barrier(accum[p].Get()); uav_barrier(gbuf[p].Get());
                lit = accum[p].Get(); litGbuf = gbuf[p].Get();
                depthScale[0] = (float)rw / WIN_W; depthScale[1] = (float)rh / WIN_H;
                if (rrOn) {   // DLSS Ray Reconstruction: guides -> clean HDR image at output size
                    ID3D12Resource *ins[] = {rrColor.Get(), rrAlbedo.Get(), rrSpecAlb.Get(), rrNormRough.Get(), rrMotion.Get(), rtDepthTex.Get()};
                    for (ID3D12Resource *r : ins) transition(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    static DWORD lastEval;
                    DWORD now = GetTickCount();
                    NVSDK_NGX_D3D12_DLSSD_Eval_Params ep{};
                    ep.pInColor = rrColor.Get(); ep.pInOutput = rrOut.Get(); ep.pInDepth = rtDepthTex.Get(); ep.pInMotionVectors = rrMotion.Get();
                    ep.pInDiffuseAlbedo = rrAlbedo.Get(); ep.pInSpecularAlbedo = rrSpecAlb.Get();
                    ep.pInNormals = rrNormRough.Get(); ep.pInRoughness = rrNormRough.Get();
                    // NGX wants the projection's pixel offset; ours moves the sample instead, hence the sign flip
                    ep.InJitterOffsetX = -k.jitter[0]; ep.InJitterOffsetY = -k.jitter[1];
                    ep.InRenderSubrectDimensions = {rw, rh};
                    ep.InReset = (k.resetHistory || rrFresh) ? 1 : 0;
                    ep.InMVScaleX = ep.InMVScaleY = 1;
                    ep.pInWorldToViewMatrix = &viewM.m[0][0]; ep.pInViewToClipMatrix = &projM.m[0][0];
                    ep.InFrameTimeDeltaInMsec = lastEval ? (float)std::min<DWORD>(now - lastEval, 200) : 16.0f;
                    lastEval = now;
                    NVSDK_NGX_Result er = NGX_D3D12_EVALUATE_DLSSD_EXT(cl.Get(), rrHandle, ngxParams, &ep);
                    static int evalFails;
                    if (NVSDK_NGX_FAILED(er) && evalFails++ < 5) { fprintf(g_log, "DLSS RR evaluate failed (%08x)\n", (unsigned)er); fflush(g_log); }
                    rrFresh = false;
                    for (ID3D12Resource *r : ins) transition(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    uav_barrier(rrOut.Get());
                    lit = rrOut.Get();   // output size; depth stays at render size (depthScale)
                }
                // own denoiser on the lighting (never fed back into the history, which stays per-pixel)
                cl->SetComputeRootSignature(atrousRootSig.Get());
                cl->SetPipelineState(atrousPso.Get());
                for (int i = 0; i < (rrOn ? 0 : (int)S(ST_DENOISE)); i++) {
                    ID3D12Resource *out = dn[i & 1].Get();
                    UINT tbl = HEAP_DYN + dynUsed; dynUsed += 3;
                    make_uav(lit, tbl, DXGI_FORMAT_R16G16B16A16_FLOAT); make_uav(out, tbl + 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
                    make_uav(litGbuf, tbl + 2, DXGI_FORMAT_R32G32B32A32_UINT);
                    float sg = S(ST_DNSMOOTH);
                    uint32_t ac[4] = {1u << i, 0, WIN_W, WIN_H};
                    memcpy(&ac[1], &sg, 4);
                    cl->SetComputeRoot32BitConstants(0, 4, ac, 0);
                    cl->SetComputeRootDescriptorTable(1, gpu_desc(tbl));
                    cl->Dispatch((WIN_W + 7) / 8, (WIN_H + 7) / 8, 1);
                    uav_barrier(out);
                    lit = out;
                }
                // composite reads the denoised lighting, depth and G-buffer
                for (ID3D12Resource *r : {lit, rtDepthTex.Get(), litGbuf}) transition(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                prevVP = vp; havePrev = true;
            } else doRt = false;
        } else if (have && !camOk) cacheTris.clear();   // left the park (menus/loading): forget its geometry
        prevRt = doRt;

        UINT bi = swap->GetCurrentBackBufferIndex();
        transition(backbuf[bi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += bi * rtvInc;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {0.10f, 0.11f, 0.14f, 1};
        cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
        cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        D3D12_VIEWPORT vpt{0, 0, (float)WIN_W, (float)WIN_H, 0, 1};
        D3D12_RECT sc{0, 0, (LONG)WIN_W, (LONG)WIN_H};
        cl->RSSetViewports(1, &vpt); cl->RSSetScissorRects(1, &sc);
        ID3D12DescriptorHeap *heaps[] = {srvHeap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (doRt) {
            UINT tbl = HEAP_DYN + dynUsed; dynUsed += 3;
            make_srv(lit, tbl, DXGI_FORMAT_R16G16B16A16_FLOAT); make_srv(rtDepthTex.Get(), tbl + 1, DXGI_FORMAT_R32_FLOAT);
            make_srv(litGbuf, tbl + 2, DXGI_FORMAT_R32G32B32A32_UINT);
            cl->SetGraphicsRootSignature(compRootSig.Get());
            cl->SetPipelineState(compPso.Get());
            cl->SetGraphicsRootDescriptorTable(0, gpu_desc(tbl));
            float kc[12] = {S(ST_EXPOSURE), S(ST_CONTRAST), S(ST_SATURATION), rrOn ? 1.0f : 0.0f, compSun[0], compSun[1], compSun[2], 0,
                            depthScale[0], depthScale[1], 0, 0};
            cl->SetGraphicsRoot32BitConstants(1, 12, kc, 0);
            cl->DrawInstanced(3, 1, 0, 0);
            for (ID3D12Resource *r : {lit, rtDepthTex.Get(), litGbuf}) transition(r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // DLSS 5 on the finished frame, before water, smoke and the UI are drawn on top (the model never sees them)
            if (S(ST_NR) > 0.5f && nr_load() && nr_ready()) {
                nr_pass(backbuf[bi].Get(), frameRw, frameRh, frameReset);
                cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                cl->RSSetViewports(1, &vpt); cl->RSSetScissorRects(1, &sc);
            }
        }
        cl->SetGraphicsRootSignature(rootSig.Get());
        cl->SetGraphicsRootConstantBufferView(0, gpu + OFF_CB);
        D3D12_VERTEX_BUFFER_VIEW vbv{gpu + OFF_VB, (UINT)(fr.verts.size() * sizeof(GpuVertex)), sizeof(GpuVertex)};
        D3D12_INDEX_BUFFER_VIEW ibv{gpu + OFF_IB, (UINT)(idxOut.size() * 4), DXGI_FORMAT_R32_UINT};
        if (have) { cl->IASetVertexBuffers(0, 1, &vbv); cl->IASetIndexBuffer(&ibv); }
        auto run = [&](std::vector<DrawCmd> &cmds, bool isUI, bool skipRtGeom) {
            for (DrawCmd &c : cmds) {
                if (!c.count || (skipRtGeom && c.inRt)) continue;
                const TpwDraw &d = *c.d;
                uint32_t tex = d.texId && d.texId <= TPWRTX_MAX_TEX && texCache[d.texId].srv ? d.texId : 0;
                float ref = d.alphaRef / 255.0f;
                uint32_t k[4] = {d.alphaTest ? 1u : 0u, 0, ((d.texBlend == 1 || d.texBlend == 7) ? 1u : 0u) | (d.colorKey && tex ? 2u : 0u), isUI ? 1u : 0u};
                memcpy(&k[1], &ref, 4);
                cl->SetPipelineState(pso_for(d, isUI));
                cl->SetGraphicsRoot32BitConstants(1, 4, k, 0);
                cl->SetGraphicsRootDescriptorTable(2, gpu_desc(tex));
                cl->DrawIndexedInstanced(c.count, 1, c.first, 0, 0);
            }
        };
        run(world, false, doRt);   // with RT on, only what the ray tracer doesn't cover (water, glass, flames) is rasterized on top
        if (g_in.hud && !g_in.orbit) run(ui, true, false);

        bool shot = g_in.shot, toGame = outTex && sh && sh->outTexHandle;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{0, {DXGI_FORMAT_R8G8B8A8_UNORM, WIN_W, WIN_H, 1, WIN_W * 4}};
        if (shot || toGame) {
            transition(backbuf[bi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (shot) {
                D3D12_TEXTURE_COPY_LOCATION src{backbuf[bi].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = fp;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            if (toGame) cl->CopyResource(outTex.Get(), backbuf[bi].Get());   // simultaneous-access texture: no transitions needed
            transition(backbuf[bi].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        } else transition(backbuf[bi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        CHECK(cl->Close());
        ID3D12CommandList *lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (toGame) {
            queue->Signal(outFence.Get(), ++outFenceVal);
            sh->outVisible = g_overlayOn && doRt;
            MemoryBarrier();
            sh->outFenceValue = outFenceVal;
        }
        HRESULT pr = swap->Present(1, 0);
        if (FAILED(pr)) fail("Present", pr == DXGI_ERROR_DEVICE_REMOVED ? dev->GetDeviceRemovedReason() : pr);
        queue->Signal(fence.Get(), ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEvt); WaitForSingleObject(fenceEvt, INFINITE);   // ponytail: one frame in flight; plenty for this load
        if (shot) {
            uint8_t *p; D3D12_RANGE rr{0, (SIZE_T)WIN_W * WIN_H * 4};
            if (SUCCEEDED(readback->Map(0, &rr, (void **)&p))) { write_png("tpwrtx_view.png", WIN_W, WIN_H, p, WIN_W * 4); D3D12_RANGE none{0, 0}; readback->Unmap(0, &none); }
            g_in.shot = false;
            fprintf(g_log, "screenshot at game frame %u (%s camera, %s, %zu RT tris, %zu lights)\n", fr.hdr.frame, g_in.orbit ? "orbit" : "game",
                    doRt ? "ray traced" : "raster", lastRtTris, lights.size());
            fflush(g_log);
        }
        frames++;
        if (GetTickCount() - lastTitle > 500) {
            char title[256];
            float secs = (GetTickCount() - lastTitle) / 1000.0f;
            uint32_t gf = sh ? sh->framesWritten : 0;
            snprintf(title, sizeof title, "TPW RTX viewer | %.0f fps | game %.0f fps | %zu draws | %s %zu tris, %zu lights | %s camera [T/W/C/O/R/U/F12]",
                     frames / secs, (gf - lastGameFrame) / secs, fr.draws.size(), doRt ? "RT" : "raster", lastRtTris, lights.size(), g_in.orbit ? "orbit" : "game");
            SetWindowTextA(hwnd, title);
            lastTitle = GetTickCount(); frames = 0; lastGameFrame = gf;
        }
    }
}
