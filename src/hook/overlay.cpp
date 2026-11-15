// In-process presentation of the 64-bit renderer's output on the game's own window.
// The renderer writes each finished frame into a shared texture and signals a shared fence (handles in TpwShared);
// here we open both with D3D11 and show the texture through a topmost DirectComposition visual on the game window.
// No second window: no z-order fight with the fullscreen game, and input goes to the game as usual.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include "../shared/tpwrtx_shared.h"

extern "C" void logf_(const char *fmt, ...);

static ID3D11Device5 *dev;
static ID3D11DeviceContext4 *ctx;
static ID3D11Texture2D *src;
static ID3D11Fence *fence;
static IDXGISwapChain1 *swap;
static IDCompositionDevice *dc;
static IDCompositionTarget *target;
static IDCompositionVisual *visual;
static bool failed, shown;
static uint32_t lastValue;
static HWND gameWnd;
static RECT lastClient;

// Fit the renderer's 4:3 image into the game window's client area (centred). Re-run when the window is resized.
static void fit_visual(TpwShared *sh)
{
    RECT r;
    if (!GetClientRect(gameWnd, &r) || (r.right == lastClient.right && r.bottom == lastClient.bottom) || !r.right || !r.bottom) return;
    float s = min((float)r.right / sh->outWidth, (float)r.bottom / sh->outHeight);
    D2D_MATRIX_3X2_F m{s, 0, 0, s, (r.right - sh->outWidth * s) / 2, (r.bottom - sh->outHeight * s) / 2};
    visual->SetTransform(m);
    dc->Commit();
    lastClient = r;
    logf_("overlay: fitted to client %ldx%ld (scale %.3f)\n", r.right, r.bottom, s);
}

static HWND game_window()
{
    struct Find { HWND w; LONG area; } f{nullptr, 0};
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        DWORD pid; RECT r;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        GetClientRect(w, &r);
        Find *f = (Find *)l;
        if (r.right * r.bottom > f->area) { f->area = r.right * r.bottom; f->w = w; }
        return TRUE;
    }, (LPARAM)&f);
    return f.w;
}

#define TRY(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { logf_("overlay: %s failed (%08lx)\n", #x, hr_); return false; } } while (0)

static bool init(TpwShared *sh)
{
    HWND hwnd = game_window();
    if (!hwnd) return false;
    // same GPU as the renderer (high-performance adapter), or the shared handles won't open
    IDXGIFactory6 *fac = nullptr; IDXGIAdapter1 *ad = nullptr;
    TRY(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), (void **)&fac));
    TRY(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void **)&ad));
    ID3D11Device *d0; ID3D11DeviceContext *c0;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    TRY(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &d0, nullptr, &c0));
    TRY(d0->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev));
    TRY(c0->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx));
    d0->Release(); c0->Release();
    TRY(dev->OpenSharedResource1((HANDLE)(UINT_PTR)sh->outTexHandle, __uuidof(ID3D11Texture2D), (void **)&src));
    TRY(dev->OpenSharedFence((HANDLE)(UINT_PTR)sh->outFenceHandle, __uuidof(ID3D11Fence), (void **)&fence));
    DXGI_SWAP_CHAIN_DESC1 sd{sh->outWidth, sh->outHeight, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                             DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_IGNORE, 0};
    TRY(fac->CreateSwapChainForComposition(dev, &sd, nullptr, &swap));
    IDXGIDevice *xd;
    TRY(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&xd));
    TRY(DCompositionCreateDevice(xd, __uuidof(IDCompositionDevice), (void **)&dc));
    TRY(dc->CreateTargetForHwnd(hwnd, TRUE, &target));   // topmost: above whatever the game itself presents
    TRY(dc->CreateVisual(&visual));
    TRY(target->SetRoot(visual));
    gameWnd = hwnd;
    fit_visual(sh);
    xd->Release(); ad->Release(); fac->Release();
    logf_("overlay: presenting %ux%u renderer output on game window %p\n", sh->outWidth, sh->outHeight, (void *)hwnd);
    return true;
}

// Called once per game frame (EndScene) on the game's render thread.
extern "C" void overlay_frame(TpwShared *sh)
{
    if (failed || !sh->outTexHandle) return;
    if (!dev && !(init(sh))) { failed = true; return; }
    fit_visual(sh);   // cheap when the size is unchanged
    static uint32_t staleFrames;
    uint32_t v = sh->outFenceValue;
    staleFrames = v == lastValue ? staleFrames + 1 : 0;
    bool want = sh->outVisible != 0 && staleFrames < 120;   // renderer stopped delivering (crashed/closed): show the game again
    if (want != shown) { visual->SetContent(want ? swap : nullptr); dc->Commit(); shown = want; }
    if (!want || v == lastValue) return;
    ctx->Wait(fence, v);   // GPU-side wait: never show a half-rendered frame
    ID3D11Texture2D *bb;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) { ctx->CopyResource(bb, src); bb->Release(); }
    swap->Present(0, 0);
    lastValue = v;
}
