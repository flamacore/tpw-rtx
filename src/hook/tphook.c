/* tphook: dinput.dll proxy for SimTheme Park.
 * - Loads as dinput.dll (game imports only DirectInputCreateA) and patches TP.EXE's import of
 *   DDRAW!DirectDrawCreate, so DDrawCompat (ddraw.dll) stays in the chain.
 * - Vtable-hooks IDirect3D3 / IDirect3DDevice3 / IDirectDrawSurface4 / IDirect3DTexture2.
 * - Streams every frame (draws, render states, camera, textures) to shared memory for the 64-bit
 *   renderer (shared/tpwrtx_shared.h), and starts tpwrtx_viewer.exe from the game dir if present.
 * - Debug: tphook.log; create an empty file "tphook_dump" in the game dir to dump the next frame
 *   (tphook_frameN.bin = raw draws, tphook_frameN.mem = TP.EXE globals). */
#define CINTERFACE
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "../shared/tpwrtx_shared.h"

/* Camera globals in TP.EXE (fixed image base 0x400000), found in Phase 2 */
#define GAME_VIEW    ((const float *)0x790900)
#define GAME_CAMINV  ((const float *)0x790990)

static FILE *g_log;
static BOOL g_minimal;               /* tpwrtx.ini [tpwrtx] minimal=1: only the video-memory fix + logging (for bisecting) */
/* Every fixed game address in this file (camera 0x790900, pointer 0x77c488, flags 0x7a1a14, ...) was found in one TP.EXE
 * build: SimTheme Park, SHA-256 d0399acfc1bcbdcbe5836814b19e386486bfa06f057646ae7821bc25e9927ac9 (PE timestamp
 * 0x63732172, image size 0xbc6000). On any other build those addresses mean nothing, so only the build-independent
 * fixes run (timers, video memory) and nothing reads game memory. */
static BOOL g_knownExe;
static BOOL exe_supported(void)
{
    BYTE *b = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == 0x63732172 && nt->OptionalHeader.SizeOfImage == 0xbc6000;
}
void overlay_frame(TpwShared *sh);   /* overlay.cpp */
void logf_(const char *fmt, ...)
{
    va_list a;
    if (!g_log) return;
    va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
}

static void patch(void **slot, void *hook, void **orig)
{
    DWORD old;
    if (*slot == hook) return; /* vtables are shared between objects; patch once */
    VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    *orig = *slot; *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

/* ---- tiny pointer -> int map (open addressing, no deletes; stale keys are harmless) ---- */
#define PMAP_SIZE 16384
typedef struct { const void *k[PMAP_SIZE]; int v[PMAP_SIZE]; } PtrMap;
static int pmap_get(PtrMap *m, const void *k)
{
    unsigned i = ((unsigned)(UINT_PTR)k >> 4) & (PMAP_SIZE - 1);
    for (; m->k[i]; i = (i + 1) & (PMAP_SIZE - 1)) if (m->k[i] == k) return m->v[i];
    return 0;
}
static void pmap_set(PtrMap *m, const void *k, int v)
{
    unsigned i = ((unsigned)(UINT_PTR)k >> 4) & (PMAP_SIZE - 1);
    for (; m->k[i] && m->k[i] != k; i = (i + 1) & (PMAP_SIZE - 1)) ;
    m->k[i] = k; m->v[i] = v;
}

/* ================= shared-memory stream ================= */
static TpwShared *g_sh;
static TpwFrameSlot *g_slot;          /* slot being written during the current scene, or NULL */
static DWORD g_W = 2048, g_H = 1536;  /* render target size, read at CreateDevice */
static DWORD g_rs[256];               /* current render states */
static DWORD g_curTexId, g_frame;

/* Watchdog: once a second, log frames rendered + who has focus, to diagnose the "screen freezes until alt-tab" stall. */
static BOOL CALLBACK find_game_wnd(HWND w, LPARAM out)
{
    DWORD pid; RECT r;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && !GetWindow(w, GW_OWNER) && GetClassLongA(w, GCW_ATOM) && GetWindowRect(w, &r) && r.right - r.left > 600) {
        *(HWND *)out = w; return FALSE;
    }
    return TRUE;
}

/* Game state flags (window procedure FUN_0046b600): bit 3 = app active (WM_ACTIVATEAPP). Logged for diagnosis. */
#define GAME_STATE_FLAGS ((volatile DWORD *)0x7a1a14)
static volatile DWORD g_focusFixes;   /* h_GetFocus corrections (see GetFocus fix below) */

/* Freeze diagnosis: sample where the main (render) thread is while no frames are being drawn.
 * Logs EIP plus stack values that point into TP.EXE code (likely return addresses; the game is built without frame pointers). */
static volatile DWORD g_renderTid;
/* In-park slowdown diagnosis: count exceptions by code (a floating-point trap storm would show up here). */
static volatile LONG g_excFlt, g_excOther;
static volatile DWORD g_excLastCode;
static LONG CALLBACK count_exceptions(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code >= 0xC000008D && code <= 0xC0000093) || code == 0xC00002B4 || code == 0xC00002B5) InterlockedIncrement(&g_excFlt);
    else InterlockedIncrement(&g_excOther);
    g_excLastCode = code;
    return EXCEPTION_CONTINUE_SEARCH;   /* observe only */
}

static void sample_main_thread(void)
{
    HANDLE th = g_renderTid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_renderTid) : NULL;
    CONTEXT c;
    DWORD stack[96], n = 0, i, rets[8], nr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!th) return;
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
    if (SuspendThread(th) != (DWORD)-1) {
        if (GetThreadContext(th, &c) && VirtualQuery((void *)c.Esp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
            n = min(96, ((DWORD)mbi.BaseAddress + mbi.RegionSize - c.Esp) / 4);
            memcpy(stack, (void *)c.Esp, n * 4);
        }
        ResumeThread(th);   /* no allocation or locks taken while suspended */
    }
    CloseHandle(th);
    for (i = 0; i < n && nr < 8; i++) if (stack[i] >= 0x401000 && stack[i] < 0x6e7000) rets[nr++] = stack[i];
    logf_("[sample] main thread eip %08lx fpu cw %04lx sw %04lx | stack code refs:", c.Eip, c.FloatSave.ControlWord & 0xffff, c.FloatSave.StatusWord & 0xffff);
    for (i = 0; i < nr; i++) logf_(" %08lx", rets[i]);
    logf_("\n");
}

/* Movie wait (FUN_0051b300): waits while DAT_00802bbc && DAT_00802bc0 and FUN_0066f540(DAT_00802bb4) says "playing"
 * (player state [0x27] == 3 playing / 4 paused). The player's clock reads its DirectSound buffer (*(player[0] + 0x5c)). */
static void log_movie_state(void)
{
    DWORD waiting = *(volatile DWORD *)0x802bbc, enabled = *(volatile DWORD *)0x802bc0, m = *(volatile DWORD *)0x802bb4;
    DWORD state = 0, a = 0, buf = 0, f3e0 = 0, play = 0, write = 0;
    HRESULT hr = 0;
    __try {
        if (m) {
            state = ((DWORD *)m)[0x27]; a = ((DWORD *)m)[0];
            if (a) { buf = *(DWORD *)(a + 0x5c); f3e0 = *(DWORD *)(a + 0x3e0); }
            if (buf) hr = ((LPDIRECTSOUNDBUFFER)buf)->lpVtbl->GetCurrentPosition((LPDIRECTSOUNDBUFFER)buf, &play, &write);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("[movie] bad pointer while reading player state\n"); return; }
    logf_("[movie] wait=%lu enabled=%lu player=%08lx state=%lu f3e0=%lu dsbuf=%08lx pos=%lu/%lu hr=%08lx\n", waiting, enabled, m, state, f3e0, buf, play, write, hr);
}

/* Game resolution: 0 = the game's own (4:3 modes); else tpwrtx.ini [tpwrtx] resolution = WxH | auto (widescreen) */
static DWORD g_resW, g_resH;
static DWORD g_peakDraws, g_peakVerts, g_dropped;   /* per-frame stream usage, logged every 600 frames */
static float g_viewRadius = 460;                     /* see cam_patch */

/* Windowed mode (dxwrapper): the game sizes its window to the whole screen and dxwrapper stretches the picture to fill
 * it. Keep the window at the largest centred client area of the game's aspect (4:3, or the widescreen resolution)
 * that fits the monitor's work area. */
static void keep_window_4x3(void)
{
    HWND w = NULL;
    RECT c, r;
    MONITORINFO mi = {sizeof mi};
    LONG style, ex, ch, cw, fw, fh, aw = g_resW ? (LONG)g_resW : 4, ah = g_resW ? (LONG)g_resH : 3;
    EnumWindows(find_game_wnd, (LPARAM)&w);
    if (!w || IsIconic(w) || !GetClientRect(w, &c) || !c.bottom) return;
    if (labs(c.right * ah - c.bottom * aw) <= c.bottom * aw / ah / 50) return;   /* already the right aspect (2% tolerance) */
    style = GetWindowLongA(w, GWL_STYLE); ex = GetWindowLongA(w, GWL_EXSTYLE);
    if (!(style & WS_CAPTION)) return;                                /* not windowed (e.g. DDrawCompat fullscreen) */
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    r.left = r.top = 0; r.right = r.bottom = 100;
    AdjustWindowRectEx(&r, style, FALSE, ex);                         /* frame size = r minus the 100x100 client */
    fw = r.right - r.left - 100; fh = r.bottom - r.top - 100;
    ch = (mi.rcWork.bottom - mi.rcWork.top) - fh;
    cw = ch * aw / ah;
    if (cw > (mi.rcWork.right - mi.rcWork.left) - fw) { cw = (mi.rcWork.right - mi.rcWork.left) - fw; ch = cw * ah / aw; }
    SetWindowPos(w, NULL, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - cw - fw) / 2, mi.rcWork.top,
                 cw + fw, ch + fh, SWP_NOZORDER | SWP_NOACTIVATE);
    logf_("window: client %ldx%ld -> %ldx%ld (%ld:%ld)\n", c.right, c.bottom, cw, ch, aw, ah);
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    DWORD last = 0, t0 = GetTickCount(), tick = 0, frameAtTick = 0, stillTicks = 0;
    for (;; tick++) {
        HWND game = NULL, fg;
        DWORD fgPid = 0, f;
        char cls[32] = "", fgCls[32] = "";
        Sleep(100);
        fg = GetForegroundWindow();
        GetWindowThreadProcessId(fg, &fgPid);
        stillTicks = g_frame == frameAtTick ? stillTicks + 1 : 0;
        frameAtTick = g_frame;
        if (stillTicks >= 10 && tick % 10 == 5 && fgPid == GetCurrentProcessId()) sample_main_thread();   /* frozen >1 s with focus */
        if (tick % 10) continue;
        if (!g_minimal) keep_window_4x3();
        {
            LONG ef = InterlockedExchange(&g_excFlt, 0), eo = InterlockedExchange(&g_excOther, 0);
            if (ef || eo) logf_("[exc] last second: %ld floating-point, %ld other exceptions (last code %08lx)\n", ef, eo, g_excLastCode);
        }
        if (stillTicks >= 10 && g_knownExe) {
            GUITHREADINFO gi = {sizeof gi};
            log_movie_state();
            if (g_renderTid && GetGUIThreadInfo(g_renderTid, &gi))
                logf_("[focus] game expects %p | main thread focus %p active %p | GetFocus fixes so far %lu\n",
                      (void *)*(volatile HWND *)0x8bd518, (void *)gi.hwndFocus, (void *)gi.hwndActive, g_focusFixes);
        }
        f = g_frame;
        EnumWindows(find_game_wnd, (LPARAM)&game);
        GetClassNameA(fg, fgCls, sizeof fgCls);
        if (game) GetClassNameA(game, cls, sizeof cls);
        logf_("[watch %4lus] %3lu frames/s | flags %02lx | focus: %s (pid %lu%s) | game wnd '%s' visible=%d\n",
              (GetTickCount() - t0) / 1000, f - last, g_knownExe ? *GAME_STATE_FLAGS : 0, fg == game ? "GAME" : fgCls, fgPid,
              fgPid == GetCurrentProcessId() ? ", game process" : "", cls, game ? IsWindowVisible(game) : 0);
        fflush(g_log);
        last = f;
    }
    return 0;
}

static void stream_open(void)
{
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(TpwShared), TPWRTX_MAP_NAME);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    char cmd[64];
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    AddVectoredExceptionHandler(1, count_exceptions);
    if (g_minimal) { logf_("minimal mode: no streaming, viewer or overlay\n"); return; }
    if (!h || !(g_sh = (TpwShared *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0))) {
        logf_("stream: shared memory failed (%lu), streaming off\n", GetLastError()); g_sh = NULL; return;
    }
    g_sh->magic = TPWRTX_MAGIC; g_sh->version = TPWRTX_VERSION; g_sh->hookPid = GetCurrentProcessId();
    logf_("stream: %lu MB shared memory mapped\n", (DWORD)(sizeof(TpwShared) >> 20));
    if (GetFileAttributesA("tpwrtx_viewer.exe") == INVALID_FILE_ATTRIBUTES) return;
    sprintf(cmd, "tpwrtx_viewer.exe %lu", GetCurrentProcessId());
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); logf_("stream: started viewer\n");
    } else logf_("stream: viewer start failed (%lu)\n", GetLastError());
}

static void stream_begin(void)
{
    TpwFrameSlot *s;
    g_renderTid = GetCurrentThreadId();
    if (!g_sh) return;
    s = &g_sh->slots[(g_sh->latestSlot + 1) % TPWRTX_SLOTS];
    s->h.seq++;  /* odd: writing */
    MemoryBarrier();
    s->h.frame = g_frame;
    memcpy(s->h.view, GAME_VIEW, 64); memcpy(s->h.camInv, GAME_CAMINV, 64);
    /* Engine projection (FUN_00578be0 + screen matrix 0x87b0d0): w = z*sin(fov/2), sx = cx + x/z * cot(fov/2) * aspect*(W-1).
     * At the original fov 90 / aspect 0.75 / 2048 wide that is f = 1535.25, k = 1.4142: what tools/calibrate.py measured. */
    {
        float fov = 90, asp = 0.75f, t;
        if (g_knownExe) {
            fov = *(volatile float *)0x87af58; asp = *(volatile float *)0x8bcbcc;
            if (!(fov >= 20 && fov <= 170)) fov = 90;
            if (!(asp > 0.2f && asp < 2)) asp = 0.75f;
        }
        t = (float)tan(fov * 0.5 * 3.14159265 / 180);
        s->h.f = (g_W - 1) * asp / t; s->h.k = (float)sqrt(1 + t * t) / t;
        s->h.cx = 1023.8f * g_W / 2048; s->h.cy = 767.7f * g_H / 1536;
    }
    s->h.width = g_W; s->h.height = g_H;
    s->h.numDraws = s->h.numVertices = s->h.numIndices = 0;
    g_slot = s;
}

static void stream_end(void)
{
    TpwFrameSlot *s = g_slot;
    if (!s) return;
    g_slot = NULL;
    MemoryBarrier();
    s->h.seq++;  /* even: complete */
    g_sh->latestSlot = (DWORD)(s - g_sh->slots);
    g_sh->framesWritten++;
}

static void stream_draw(const TpwVertex *v, DWORD nv, const WORD *idx, DWORD ni)
{
    TpwFrameSlot *s = g_slot;
    TpwDraw *d;
    DWORD i;
    float maxz = 0;
    if (!s) return;
    if (s->h.numDraws >= TPWRTX_MAX_DRAWS || s->h.numVertices + nv > TPWRTX_MAX_VERTS || s->h.numIndices + ni > TPWRTX_MAX_INDICES) { g_dropped++; return; }
    d = &s->draws[s->h.numDraws++];
    if (s->h.numDraws > g_peakDraws) g_peakDraws = s->h.numDraws;
    if (s->h.numVertices + nv > g_peakVerts) g_peakVerts = s->h.numVertices + nv;
    d->firstVertex = s->h.numVertices; d->numVertices = nv; d->firstIndex = s->h.numIndices; d->numIndices = ni;
    memcpy(&s->verts[s->h.numVertices], v, nv * sizeof(TpwVertex));
    if (idx) memcpy(&s->indices[s->h.numIndices], idx, ni * 2);
    else for (i = 0; i < ni; i++) s->indices[s->h.numIndices + i] = (WORD)i;
    s->h.numVertices += nv; s->h.numIndices += ni;
    for (i = 0; i < nv; i++) if (v[i].z > maxz) maxz = v[i].z;
    d->texId = g_curTexId;
    d->flags = maxz < 0.1f ? TPW_DRAW_UI : 0;
    d->alphaBlend = (BYTE)g_rs[D3DRENDERSTATE_ALPHABLENDENABLE]; d->srcBlend = (BYTE)g_rs[D3DRENDERSTATE_SRCBLEND];
    d->dstBlend = (BYTE)g_rs[D3DRENDERSTATE_DESTBLEND];          d->alphaTest = (BYTE)g_rs[D3DRENDERSTATE_ALPHATESTENABLE];
    d->alphaRef = (BYTE)g_rs[D3DRENDERSTATE_ALPHAREF];           d->alphaFunc = (BYTE)g_rs[D3DRENDERSTATE_ALPHAFUNC];
    d->zEnable = (BYTE)g_rs[D3DRENDERSTATE_ZENABLE];             d->zWrite = (BYTE)g_rs[D3DRENDERSTATE_ZWRITEENABLE];
    d->zFunc = (BYTE)g_rs[D3DRENDERSTATE_ZFUNC];                 d->cull = (BYTE)g_rs[D3DRENDERSTATE_CULLMODE];
    d->texBlend = (BYTE)g_rs[D3DRENDERSTATE_TEXTUREMAPBLEND];    d->colorKey = (BYTE)g_rs[D3DRENDERSTATE_COLORKEYENABLE];
    d->specular = (BYTE)g_rs[D3DRENDERSTATE_SPECULARENABLE];
}

/* ---- textures: captured on first bind and whenever the game rewrites them ---- */
typedef struct { LPDIRECT3DTEXTURE2 tex; LPDIRECTDRAWSURFACE4 surf; BOOL dirty; } TexEntry;
static TexEntry g_tex[TPWRTX_MAX_TEX + 1];        /* 1-based, parallel to g_sh->tex */
static PtrMap g_texByTex, g_texBySurf;
static BOOL g_inCapture;
static LPDIRECTDRAWSURFACE4 g_rt;   /* game's render target (for blit logging) */
static DWORD g_texCaptures, g_texFails;

static void mark_dirty_surf(const void *surf)
{
    int id;
    if (g_inCapture || !(id = pmap_get(&g_texBySurf, surf))) return;
    g_tex[id].dirty = TRUE;
}

static DWORD expand(DWORD v, DWORD mask)
{
    DWORD shift = 0, bits = 0, c;
    if (!mask) return 255;
    while (!(mask & 1)) { mask >>= 1; shift++; }
    while (mask & (1u << bits)) bits++;
    c = (v >> shift) & mask;
    return bits >= 8 ? c >> (bits - 8) : (c * 255 + mask / 2) / mask;
}

static void capture(int id)
{
    TexEntry *e = &g_tex[id];
    TpwTexture *t = &g_sh->tex[id - 1];
    DDSURFACEDESC2 sd;
    DWORD x, y, bpp, need, *dst, rgbMask;
    const DDPIXELFORMAT *pf = &sd.ddpfPixelFormat;
    DDCOLORKEY ck;
    BOOL keyed;
    if (!e->surf) { e->dirty = FALSE; return; }   /* generated textures (pointer arrow) have no game surface */
    keyed = SUCCEEDED(e->surf->lpVtbl->GetColorKey(e->surf, DDCKEY_SRCBLT, &ck));   /* colour-keyed pixels -> transparent */
    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    g_inCapture = TRUE;
    if (FAILED(e->surf->lpVtbl->Lock(e->surf, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_NOSYSLOCK, NULL))) {
        g_inCapture = FALSE; g_texFails++; return;
    }
    need = sd.dwWidth * sd.dwHeight * 4;
    if (t->width != sd.dwWidth || t->height != sd.dwHeight || !t->width) {   /* (re)allocate */
        if (g_sh->texHeapUsed + need > TPWRTX_TEX_HEAP) {   /* heap full: start over, everything recaptures on next bind */
            int i;
            g_sh->texHeapUsed = 0; g_sh->texGeneration++;
            for (i = 1; i <= (int)g_sh->texCount; i++) { g_sh->tex[i - 1].width = 0; g_tex[i].dirty = TRUE; }
            logf_("stream: texture heap reset\n");
        }
        t->offset = g_sh->texHeapUsed; g_sh->texHeapUsed += need;
    }
    dst = (DWORD *)(g_sh->texHeap + t->offset);
    bpp = pf->dwRGBBitCount;
    rgbMask = pf->dwRBitMask | pf->dwGBitMask | pf->dwBBitMask;
    for (y = 0; y < sd.dwHeight; y++) {
        const BYTE *row = (const BYTE *)sd.lpSurface + y * sd.lPitch;
        for (x = 0; x < sd.dwWidth; x++) {
            DWORD v = bpp == 32 ? ((const DWORD *)row)[x] : bpp == 16 ? ((const WORD *)row)[x] : bpp == 24 ? (row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16) : 0;
            DWORD a = (pf->dwFlags & DDPF_ALPHAPIXELS) ? expand(v, pf->dwRGBAlphaBitMask) : 255;
            dst[y * sd.dwWidth + x] = (pf->dwFlags & DDPF_RGB)
                ? (a << 24 | expand(v, pf->dwRBitMask) << 16 | expand(v, pf->dwGBitMask) << 8 | expand(v, pf->dwBBitMask))
                : 0xffff00ff;   /* ponytail: palettized/other formats unseen so far -> magenta */
            if (keyed && (v & rgbMask) == (ck.dwColorSpaceLowValue & rgbMask)) dst[y * sd.dwWidth + x] = 0;
        }
    }
    e->surf->lpVtbl->Unlock(e->surf, NULL);
    g_inCapture = FALSE;
    t->width = sd.dwWidth; t->height = sd.dwHeight;
    MemoryBarrier();
    t->version++;
    e->dirty = FALSE;
    g_texCaptures++;
}

/* ---- per-frame stats and debug frame dump ---- */
static FILE *g_bin;
static DWORD g_frameDraws;
static void bin_u32(DWORD v) { fwrite(&v, 4, 1, g_bin); }

static void dump_globals(const char *name)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    FILE *f = fopen(name, "wb");
    WORD i;
    if (!f) return;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        DWORD va = (DWORD)(UINT_PTR)base + s->VirtualAddress, size = s->Misc.VirtualSize;
        if (!(s->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        fwrite(&va, 4, 1, f); fwrite(&size, 4, 1, f); fwrite((void *)(UINT_PTR)va, 1, size, f);
    }
    fclose(f);
}

static void record_draw(D3DPRIMITIVETYPE pt, DWORD fvf, const void *v, DWORD nv, const WORD *idx, DWORD ni)
{
    g_frameDraws++;
    if (fvf != 0x1c4 || pt != D3DPT_TRIANGLELIST) return;   /* the only format the game uses (Phase 1) */
    stream_draw((const TpwVertex *)v, nv, idx, idx ? ni : nv);
    if (!g_bin) return;
    /* 'D', prim, fvf, nverts, stride, nidx, tex, flags, verts, idx (same format tools/analyze_frame.py reads) */
    bin_u32('D'); bin_u32(pt); bin_u32(fvf); bin_u32(nv); bin_u32(32); bin_u32(idx ? ni : 0); bin_u32(g_curTexId); bin_u32(0);
    fwrite(v, 32, nv, g_bin);
    if (idx) { fwrite(idx, 2, ni, g_bin); if (ni & 1) { WORD z = 0; fwrite(&z, 2, 1, g_bin); } }
}

/* ================= IDirect3DDevice3 hooks ================= */
#define ORIG(name) static HRESULT (WINAPI *o_##name)
ORIG(BeginScene)(LPDIRECT3DDEVICE3);
ORIG(EndScene)(LPDIRECT3DDEVICE3);
ORIG(SetTexture)(LPDIRECT3DDEVICE3, DWORD, LPDIRECT3DTEXTURE2);
ORIG(SetRenderState)(LPDIRECT3DDEVICE3, D3DRENDERSTATETYPE, DWORD);
ORIG(DrawPrimitive)(LPDIRECT3DDEVICE3, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD);
ORIG(DrawIndexedPrimitive)(LPDIRECT3DDEVICE3, D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, LPWORD, DWORD, DWORD);
ORIG(Load)(LPDIRECT3DTEXTURE2, LPDIRECT3DTEXTURE2);
ORIG(Unlock)(LPDIRECTDRAWSURFACE4, LPRECT);
ORIG(Blt)(LPDIRECTDRAWSURFACE4, LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDBLTFX);
ORIG(BltFast)(LPDIRECTDRAWSURFACE4, DWORD, DWORD, LPDIRECTDRAWSURFACE4, LPRECT, DWORD);

static HRESULT WINAPI h_BeginScene(LPDIRECT3DDEVICE3 d)
{
    if (GetFileAttributesA("tphook_dump") != INVALID_FILE_ATTRIBUTES) {
        char name[64];
        DeleteFileA("tphook_dump");
        sprintf(name, "tphook_frame%lu.mem", g_frame);
        dump_globals(name);   /* taken before the frame's draws = the state this frame is rendered from */
        sprintf(name, "tphook_frame%lu.bin", g_frame);
        g_bin = fopen(name, "wb");
        bin_u32('TPHF'); bin_u32(1);
        logf_("==== dump frame %lu\n", g_frame);
    }
    stream_begin();
    return o_BeginScene(d);
}

/* Cursor over the overlay: the game hides the Windows cursor and draws its own pointer into its frame, which the
 * overlay covers. While the overlay shows, keep the Windows cursor visible (it draws above everything) and ignore the
 * game's ShowCursor calls; hand control back when the overlay hides. Runs on the game thread (cursor state is per-thread).
 * ponytail: plain arrow, not the game's contextual pointer. */
typedef int (WINAPI *SC_t)(BOOL);
static SC_t o_ShowCursor;
static BOOL g_cursorForced;
static int WINAPI h_ShowCursor(BOOL show)
{
    if (g_cursorForced) return show ? 0 : -1;   /* report what the game wanted so its show/hide loops terminate */
    return o_ShowCursor(show);
}
/* The game's window class has no cursor, so Windows clears the shape on every mouse move (WM_SETCURSOR): give the
 * class an arrow while forced, and turn the game's own SetCursor(NULL) into the arrow. */
typedef HCURSOR (WINAPI *SETC_t)(HCURSOR);
static SETC_t o_SetCursor;
static HCURSOR WINAPI h_SetCursor(HCURSOR c)
{
    if (g_cursorForced && !c) c = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    return o_SetCursor(c);
}
static BOOL CALLBACK set_class_cursor(HWND w, LPARAM cur)
{
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) SetClassLongPtrA(w, GCLP_HCURSOR, cur);
    return TRUE;
}
static void sync_cursor(void)
{
    BOOL want = g_sh && g_sh->outVisible;
    HCURSOR arrow = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    if (!o_ShowCursor) return;
    if (want && !g_cursorForced) {
        g_cursorForced = TRUE; while (o_ShowCursor(TRUE) < 0) ;
        EnumWindows(set_class_cursor, (LPARAM)arrow);
    } else if (!want && g_cursorForced) {
        g_cursorForced = FALSE; while (o_ShowCursor(FALSE) >= 0) ;
        EnumWindows(set_class_cursor, 0);
    }
    if (g_cursorForced) (o_SetCursor ? o_SetCursor : SetCursor)(arrow);
}

/* Pointer over the overlay. The game's UI library keeps the pointer position in 2048x1536 space
 * (FUN_006588ef: DAT_00faa540 = x, DAT_00faa544 = y; DAT_0077c488 = pointer system up).
 * ponytail: drawn with a generated gold arrow, not the game's own (contextual) pointer art. */
#define PTR_ACTIVE (*(volatile int *)0x77c488)
#define PTR_X      (*(volatile short *)0xfaa540)
#define PTR_Y      (*(volatile short *)0xfaa544)
enum { ARROW = 32 };
static int g_arrowId;

static void make_arrow(void)
{
    static const float poly[][2] = {{0, 0}, {0, 25}, {6.5f, 19.5f}, {11, 30}, {15.5f, 28}, {11, 18.5f}, {19, 18.5f}};
    TpwTexture *t;
    DWORD *px, x, y;
    int i, j, n = sizeof poly / sizeof poly[0];
    if (!g_arrowId) {
        if (g_sh->texCount >= TPWRTX_MAX_TEX) return;
        g_arrowId = ++g_sh->texCount;
        g_tex[g_arrowId].tex = NULL; g_tex[g_arrowId].surf = NULL; g_tex[g_arrowId].dirty = FALSE;
    }
    t = &g_sh->tex[g_arrowId - 1];
    if (g_sh->texHeapUsed + ARROW * ARROW * 4 > TPWRTX_TEX_HEAP) return;
    t->offset = g_sh->texHeapUsed; g_sh->texHeapUsed += ARROW * ARROW * 4;
    px = (DWORD *)(g_sh->texHeap + t->offset);
    for (y = 0; y < ARROW; y++) for (x = 0; x < ARROW; x++) {
        float fx = x + 0.5f, fy = y + 0.5f, dmin = 1e9f;
        int inside = 0;
        for (i = 0, j = n - 1; i < n; j = i++) {   /* point in polygon + distance to the outline */
            float ax = poly[j][0], ay = poly[j][1], bx = poly[i][0], by = poly[i][1], ex = bx - ax, ey = by - ay;
            float u = ((fx - ax) * ex + (fy - ay) * ey) / (ex * ex + ey * ey), dx, dy;
            if ((ay > fy) != (by > fy) && fx < ax + (fy - ay) * ex / ey) inside = !inside;
            u = u < 0 ? 0 : u > 1 ? 1 : u; dx = ax + u * ex - fx; dy = ay + u * ey - fy;
            if (dx * dx + dy * dy < dmin) dmin = dx * dx + dy * dy;
        }
        px[y * ARROW + x] = inside ? (dmin < 2.25f ? 0xff201000u : 0xffffd23cu) : (dmin < 1.0f ? 0xff201000u : 0);
    }
    t->width = ARROW; t->height = ARROW;
    MemoryBarrier();
    t->version++;
}

static void di_reacquire(void);
static void cam_frame(void);

static void stream_cursor(void)
{
    TpwFrameSlot *s = g_slot;
    TpwDraw *d;
    TpwVertex *v;
    WORD *ix;
    float x0, y0, sz;
    int i, id;
    static const WORD quad[6] = {0, 1, 2, 2, 1, 3};
    if (!s || !PTR_ACTIVE || s->h.numDraws >= TPWRTX_MAX_DRAWS || s->h.numVertices + 4 > TPWRTX_MAX_VERTS || s->h.numIndices + 6 > TPWRTX_MAX_INDICES) return;
    if (!g_arrowId || !g_sh->tex[g_arrowId - 1].width) make_arrow();   /* again after a texture-heap reset */
    if (!(id = g_arrowId) || !g_sh->tex[id - 1].width) return;
    x0 = PTR_X * (float)g_W / 2048; y0 = PTR_Y * (float)g_H / 1536; sz = ARROW * (float)g_H / 1536;
    v = &s->verts[s->h.numVertices];
    for (i = 0; i < 4; i++) {
        v[i].x = x0 + (i & 1 ? sz : 0); v[i].y = y0 + (i & 2 ? sz : 0);
        v[i].z = 0.0001f; v[i].rhw = 1; v[i].diffuse = 0xffffffff; v[i].specular = 0;
        v[i].u = (float)(i & 1); v[i].v = (float)((i & 2) >> 1);
    }
    ix = &s->indices[s->h.numIndices];
    memcpy(ix, quad, sizeof quad);
    d = &s->draws[s->h.numDraws++];
    memset(d, 0, sizeof *d);
    d->firstVertex = s->h.numVertices; d->numVertices = 4; d->firstIndex = s->h.numIndices; d->numIndices = 6;
    d->texId = id; d->flags = TPW_DRAW_UI;
    d->alphaTest = 1; d->alphaRef = 1; d->alphaFunc = D3DCMP_GREATEREQUAL; d->texBlend = D3DTBLEND_DECAL;
    d->srcBlend = D3DBLEND_ONE; d->dstBlend = D3DBLEND_ZERO; d->zFunc = D3DCMP_ALWAYS;
    s->h.numVertices += 4; s->h.numIndices += 6;
}

static HRESULT WINAPI h_EndScene(LPDIRECT3DDEVICE3 d)
{
    di_reacquire();
    cam_frame();
    stream_cursor();
    stream_end();
    /* show the renderer's latest frame on the game window (overlay mode). The game's own pointer is streamed by
     * stream_cursor(); sync_cursor() (forced Windows arrow) is off: DirectInput parks that cursor mid-window. */
    if (g_sh) overlay_frame(g_sh);
    if (g_bin) { fclose(g_bin); g_bin = NULL; }
    if (g_frame % 600 == 0) {
        logf_("frame %lu: %lu draws (peak %lu draws / %lu vertices since last line, %lu dropped); zoom %.0f, draw area %.0f; "
              "textures %lu captured, %lu lock failures, %lu ids, heap %lu KB\n", g_frame, g_frameDraws, g_peakDraws, g_peakVerts,
              g_dropped, g_knownExe ? *(volatile float *)0x7909ec : 0, g_viewRadius,
              g_texCaptures, g_texFails, g_sh ? g_sh->texCount : 0, g_sh ? g_sh->texHeapUsed >> 10 : 0);
        fflush(g_log);
        g_peakDraws = g_peakVerts = g_dropped = 0;
    }
    g_frame++; g_frameDraws = 0;
    return o_EndScene(d);
}

static HRESULT WINAPI h_Load(LPDIRECT3DTEXTURE2 dst, LPDIRECT3DTEXTURE2 src)
{
    int id = pmap_get(&g_texByTex, dst);
    if (id) g_tex[id].dirty = TRUE;
    return o_Load(dst, src);
}
/* Game pointer: the game draws its mouse pointer as a small 2D blit (its "asynchronous pointer" thread), which the
 * overlay covers. Remember the latest small screen blit and stream it as a HUD quad (stream_cursor). */
static DWORD g_blitLogs;
static void note_blit(const char *kind, LPDIRECTDRAWSURFACE4 dst, LPDIRECTDRAWSURFACE4 src, const RECT *sr, LONG x, LONG y, LONG dw, LONG dh)
{
    RECT r;
    DDSURFACEDESC2 sd;
    if (!src || pmap_get(&g_texBySurf, dst) || g_inCapture) return;   /* texture uploads are not screen blits */
    if (sr) r = *sr;
    else {
        memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
        if (FAILED(src->lpVtbl->GetSurfaceDesc(src, &sd))) return;
        r.left = r.top = 0; r.right = sd.dwWidth; r.bottom = sd.dwHeight;
    }
    if (dw <= 0) { dw = r.right - r.left; dh = r.bottom - r.top; }
    if (g_blitLogs < 40) {
        g_blitLogs++;
        logf_("blit %s dst=%p%s src=%p src rect %ld,%ld %ldx%ld -> %ld,%ld %ldx%ld (thread %lu, frame %lu)\n", kind, dst, dst == g_rt ? " (render target)" : "",
              src, r.left, r.top, r.right - r.left, r.bottom - r.top, x, y, dw, dh, GetCurrentThreadId(), g_frame);
    }
}

static HRESULT WINAPI h_Unlock(LPDIRECTDRAWSURFACE4 s, LPRECT r) { mark_dirty_surf(s); return o_Unlock(s, r); }
static HRESULT WINAPI h_Blt(LPDIRECTDRAWSURFACE4 s, LPRECT r, LPDIRECTDRAWSURFACE4 src, LPRECT sr, DWORD fl, LPDDBLTFX fx)
{
    mark_dirty_surf(s);
    note_blit("Blt", s, src, sr, r ? r->left : 0, r ? r->top : 0, r ? r->right - r->left : 0, r ? r->bottom - r->top : 0);
    return o_Blt(s, r, src, sr, fl, fx);
}
static HRESULT WINAPI h_BltFast(LPDIRECTDRAWSURFACE4 s, DWORD x, DWORD y, LPDIRECTDRAWSURFACE4 src, LPRECT sr, DWORD t)
{
    mark_dirty_surf(s);
    note_blit("BltFast", s, src, sr, (LONG)x, (LONG)y, 0, 0);
    return o_BltFast(s, x, y, src, sr, t);
}

#define HOOK(vt, name) patch((void **)&(vt)->name, (void *)h_##name, (void **)&o_##name)

/* Texture -> surface. DDrawCompat answers QI(texture -> IDirectDrawSurface4); dxwrapper doesn't (E_NOINTERFACE), so
 * also remember the surface each texture was obtained from (the game QIs surface -> IDirect3DTexture2). */
static PtrMap g_surfOfTex;
ORIG(SurfQI)(LPDIRECTDRAWSURFACE4, REFIID, LPVOID *);
static HRESULT WINAPI h_SurfQI(LPDIRECTDRAWSURFACE4 s, REFIID riid, LPVOID *out)
{
    HRESULT hr = o_SurfQI(s, riid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(riid, &IID_IDirect3DTexture2)) pmap_set(&g_surfOfTex, *out, (int)(UINT_PTR)s);
    return hr;
}

static LPDIRECTDRAWSURFACE4 tex_surface(LPDIRECT3DTEXTURE2 t)
{
    LPDIRECTDRAWSURFACE4 s = NULL;
    if (SUCCEEDED(t->lpVtbl->QueryInterface(t, &IID_IDirectDrawSurface4, (void **)&s))) {
        s->lpVtbl->Release(s);   /* the game keeps the object alive; we only key on the pointer */
        return s;
    }
    return (LPDIRECTDRAWSURFACE4)(UINT_PTR)pmap_get(&g_surfOfTex, t);
}

static HRESULT WINAPI h_SetTexture(LPDIRECT3DDEVICE3 d, DWORD stage, LPDIRECT3DTEXTURE2 t)
{
    if (stage == 0 && g_sh) {
        int id = t ? pmap_get(&g_texByTex, t) : 0;
        LPDIRECTDRAWSURFACE4 s = t ? tex_surface(t) : NULL;   /* every bind: textures get freed and reallocated while loading */
        if (!s) id = 0;
        else if (!id && g_sh->texCount < TPWRTX_MAX_TEX) {
            id = ++g_sh->texCount;
            g_tex[id].tex = t; g_tex[id].surf = s; g_tex[id].dirty = TRUE;
            pmap_set(&g_texByTex, t, id); pmap_set(&g_texBySurf, s, id);
            HOOK((IDirect3DTexture2Vtbl *)t->lpVtbl, Load);
            if (((IDirectDrawSurface4Vtbl *)s->lpVtbl)->Unlock != h_Unlock) logf_("WARNING: texture surface vtable differs from render target's\n");
        } else if (id && s != g_tex[id].surf) { g_tex[id].surf = s; g_tex[id].dirty = TRUE; pmap_set(&g_texBySurf, s, id); }
        if (id && g_tex[id].dirty) capture(id);
        g_curTexId = id;
    } else if (stage == 0) g_curTexId = 0;
    return o_SetTexture(d, stage, t);
}

static HRESULT WINAPI h_SetRenderState(LPDIRECT3DDEVICE3 d, D3DRENDERSTATETYPE s, DWORD v)
{
    if ((DWORD)s < 256) g_rs[s] = v;
    return o_SetRenderState(d, s, v);
}

static HRESULT WINAPI h_DrawPrimitive(LPDIRECT3DDEVICE3 d, D3DPRIMITIVETYPE pt, DWORD fvf, LPVOID v, DWORD n, DWORD fl)
{
    record_draw(pt, fvf, v, n, NULL, 0);
    return o_DrawPrimitive(d, pt, fvf, v, n, fl);
}

static HRESULT WINAPI h_DrawIndexedPrimitive(LPDIRECT3DDEVICE3 d, D3DPRIMITIVETYPE pt, DWORD fvf, LPVOID v, DWORD n, LPWORD idx, DWORD ni, DWORD fl)
{
    record_draw(pt, fvf, v, n, idx, ni);
    return o_DrawIndexedPrimitive(d, pt, fvf, v, n, idx, ni, fl);
}

static void hook_device(LPDIRECT3DDEVICE3 dev, LPDIRECTDRAWSURFACE4 rt)
{
    IDirect3DDevice3Vtbl *vt = (IDirect3DDevice3Vtbl *)dev->lpVtbl;
    IDirectDrawSurface4Vtbl *sv = (IDirectDrawSurface4Vtbl *)rt->lpVtbl;
    DDSURFACEDESC2 sd;
    /* D3D defaults until the game sets them */
    g_rs[D3DRENDERSTATE_ZENABLE] = 1; g_rs[D3DRENDERSTATE_ZWRITEENABLE] = 1; g_rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    g_rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE; g_rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    g_rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS; g_rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    g_rs[D3DRENDERSTATE_TEXTUREMAPBLEND] = D3DTBLEND_MODULATE;
    memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    if (SUCCEEDED(rt->lpVtbl->GetSurfaceDesc(rt, &sd))) { g_W = sd.dwWidth; g_H = sd.dwHeight; }
    g_rt = rt;
    HOOK(vt, BeginScene); HOOK(vt, EndScene); HOOK(vt, SetTexture); HOOK(vt, SetRenderState);
    HOOK(vt, DrawPrimitive); HOOK(vt, DrawIndexedPrimitive);
    HOOK(sv, Unlock); HOOK(sv, Blt); HOOK(sv, BltFast);
    patch((void **)&sv->QueryInterface, (void *)h_SurfQI, (void **)&o_SurfQI);
    logf_("hooked IDirect3DDevice3 %p, render target %lux%lu\n", dev, g_W, g_H); fflush(g_log);
}

/* ================= IDirect3D3 / DirectDraw hooks ================= */
ORIG(CreateDevice)(LPDIRECT3D3, REFCLSID, LPDIRECTDRAWSURFACE4, LPDIRECT3DDEVICE3 *, LPUNKNOWN);

static HRESULT WINAPI h_CreateDevice(LPDIRECT3D3 d3d, REFCLSID cls, LPDIRECTDRAWSURFACE4 rt, LPDIRECT3DDEVICE3 *out, LPUNKNOWN u)
{
    HRESULT hr = o_CreateDevice(d3d, cls, rt, out, u);
    if (SUCCEEDED(hr) && out && *out) hook_device(*out, rt);
    return hr;
}

/* Video memory query fix (dxwrapper): the game calls GetAvailableVidMem with empty caps; dxwrapper rejects that
 * ("Unknown memory type"), the game decides it's out of VRAM, its texture cache fails and it drops to the software
 * renderer. Report a sane amount instead. ponytail: fixed 256 MB, plenty for a 1999 texture cache. */
typedef HRESULT (WINAPI *GAVM_t)(void *, void *, LPDWORD, LPDWORD);
static GAVM_t o_GAVM2, o_GAVM4;
static HRESULT vidmem_fix(HRESULT hr, LPDWORD total, LPDWORD freeMem)
{
    static BOOL logged;
    if (SUCCEEDED(hr) && (!total || *total) && (!freeMem || *freeMem)) return hr;
    if (total) *total = 256u << 20;
    if (freeMem) *freeMem = 256u << 20;
    if (!logged) { logged = TRUE; logf_("vidmem: GetAvailableVidMem failed/zero (hr %08lx), reporting 256 MB\n", hr); }
    return DD_OK;
}
static HRESULT WINAPI h_GAVM2(void *dd, void *caps, LPDWORD t, LPDWORD f) { return vidmem_fix(o_GAVM2(dd, caps, t, f), t, f); }
static HRESULT WINAPI h_GAVM4(void *dd, void *caps, LPDWORD t, LPDWORD f) { return vidmem_fix(o_GAVM4(dd, caps, t, f), t, f); }

typedef HRESULT (WINAPI *QI_t)(IUnknown *, REFIID, void **);
static struct { void *vtbl; QI_t orig; } g_qi[8];
static int g_nqi;
static void hook_qi(void *obj);

static HRESULT WINAPI h_QI(IUnknown *self, REFIID riid, void **out)
{
    QI_t orig = NULL;
    HRESULT hr;
    int i;
    for (i = 0; i < g_nqi; i++) if (g_qi[i].vtbl == *(void **)self) orig = g_qi[i].orig;
    hr = orig(self, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (IsEqualGUID(riid, &IID_IDirect3D3)) HOOK((IDirect3D3Vtbl *)((LPDIRECT3D3)*out)->lpVtbl, CreateDevice);
        else if (IsEqualGUID(riid, &IID_IDirectDraw2)) {
            hook_qi(*out);
            patch((void **)&((IDirectDraw2Vtbl *)((LPDIRECTDRAW2)*out)->lpVtbl)->GetAvailableVidMem, (void *)h_GAVM2, (void **)&o_GAVM2);
        } else if (IsEqualGUID(riid, &IID_IDirectDraw4)) {
            hook_qi(*out);
            patch((void **)&((IDirectDraw4Vtbl *)((LPDIRECTDRAW4)*out)->lpVtbl)->GetAvailableVidMem, (void *)h_GAVM4, (void **)&o_GAVM4);
        }
    }
    return hr;
}

static void hook_qi(void *obj)
{
    void **vt = *(void ***)obj;
    if (vt[0] == (void *)h_QI || g_nqi == 8) return;
    g_qi[g_nqi].vtbl = vt;
    patch(&vt[0], (void *)h_QI, (void **)&g_qi[g_nqi].orig);
    g_nqi++;
}

typedef HRESULT (WINAPI *DDC_t)(GUID *, LPDIRECTDRAW *, IUnknown *);
static DDC_t o_DirectDrawCreate;

static HRESULT WINAPI h_DirectDrawCreate(GUID *g, LPDIRECTDRAW *out, IUnknown *u)
{
    HRESULT hr = o_DirectDrawCreate(g, out, u);
    if (!g_sh) stream_open();   /* first DirectDraw use; not in DllMain (no CreateProcess under the loader lock) */
    if (SUCCEEDED(hr) && out && *out) hook_qi(*out);
    return hr;
}

/* Redirect module `mod`'s import of dll!fn (matched by resolved address, so ordinal imports work too). */
static BOOL patch_import(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    void *target = GetProcAddress(GetModuleHandleA(dll), fn);
    BOOL done = FALSE;
    if (!base || !target) return FALSE;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        if (lstrcmpiA((char *)base + imp->Name, dll)) continue;
        for (; t->u1.Function; t++)
            if ((void *)t->u1.Function == target) { patch((void **)&t->u1.Function, hook, orig); done = TRUE; }
    }
    return done;
}

/* ---- DirectSound global focus (movie freeze fix) ----
 * The game's movie player clocks video frames off the movie's DirectSound buffer play position (FUN_00672730).
 * Buffers without DSBCAPS_GLOBALFOCUS go silent/stall when the window DirectSound was bound to isn't the focused
 * one, and the game has two top-level windows; the clip then never ends and the game waits for it (FUN_0051b300).
 * Giving every secondary buffer global focus keeps the clock running. */
typedef HRESULT (WINAPI *DSC_t)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
static DSC_t o_DirectSoundCreate;
ORIG(CreateSoundBuffer)(LPDIRECTSOUND, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);
static DWORD g_dsGlobal;

static HRESULT WINAPI h_CreateSoundBuffer(LPDIRECTSOUND ds, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN u)
{
    DSBUFFERDESC copy;
    if (d && d->dwSize <= sizeof copy && !(d->dwFlags & DSBCAPS_PRIMARYBUFFER)) {
        memset(&copy, 0, sizeof copy);
        memcpy(&copy, d, d->dwSize);   /* DX5/6 callers may pass the shorter DSBUFFERDESC1 */
        copy.dwFlags |= DSBCAPS_GLOBALFOCUS;
        d = &copy;
        if (g_dsGlobal++ < 3) logf_("dsound: buffer created with DSBCAPS_GLOBALFOCUS (flags %08lx)\n", copy.dwFlags);
    }
    return o_CreateSoundBuffer(ds, d, out, u);
}

static HRESULT WINAPI h_DirectSoundCreate(LPCGUID g, LPDIRECTSOUND *out, LPUNKNOWN u)
{
    HRESULT hr = o_DirectSoundCreate(g, out, u);
    if (SUCCEEDED(hr) && out && *out) HOOK((IDirectSoundVtbl *)(*out)->lpVtbl, CreateSoundBuffer);   /* vtable shared by all users */
    return hr;
}

/* ---- GetFocus fix (loading-screen freeze) ----
 * The loading screen (FUN_00583730/FUN_00583790, called from FUN_00587c80) only presents and advances when
 * GetFocus() == the game window in DAT_008bd518. The game has two top-level windows and keyboard focus can land on
 * the other one, so loading stalls until an alt-tab moves focus. While a game window is in the foreground, report
 * the expected window; otherwise (really in another app) behave normally so the game still pauses. */
#define GAME_MAIN_HWND (*(volatile HWND *)0x8bd518)
typedef HWND (WINAPI *GF_t)(void);
static GF_t o_GetFocus;

static HWND WINAPI h_GetFocus(void)
{
    HWND f = o_GetFocus(), want = GAME_MAIN_HWND;
    DWORD pid = 0;
    if (want && f != want) {
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) { g_focusFixes++; return want; }
    }
    return f;
}

/* ---- Uptime-independent timers ----
 * Windows' timers count from boot: after ~12 days timeGetTime is ~1e9 ms, where a 32-bit float only resolves 128 ms
 * steps. 1999 code that keeps times in floats then advances in big jumps or not at all. Give TP.EXE clocks that
 * start at 10 s when the game launches (same idea as dxwrapper's FixPerfCounterUptime, for all three timers). */
typedef DWORD (WINAPI *TIME_t)(void);
typedef BOOL (WINAPI *QPC_t)(LARGE_INTEGER *);
static TIME_t o_timeGetTime, o_GetTickCount;
static QPC_t o_QPC;
static DWORD g_tgtBase, g_gtcBase;
static LONGLONG g_qpcBase;
static DWORD WINAPI h_timeGetTime(void) { return o_timeGetTime() - g_tgtBase; }
static DWORD WINAPI h_GetTickCount(void) { return o_GetTickCount() - g_gtcBase; }
static BOOL WINAPI h_QPC(LARGE_INTEGER *c)
{
    BOOL r = o_QPC(c);
    if (r && c) c->QuadPart -= g_qpcBase;
    return r;
}

static void patch_timers(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    LARGE_INTEGER f, c;
    BOOL a, b, q;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    g_tgtBase = timeGetTime() - 10000; g_gtcBase = GetTickCount() - 10000; g_qpcBase = c.QuadPart - 10 * f.QuadPart;
    a = patch_import(exe, "WINMM.dll", "timeGetTime", (void *)h_timeGetTime, (void **)&o_timeGetTime);
    b = patch_import(exe, "KERNEL32.dll", "GetTickCount", (void *)h_GetTickCount, (void **)&o_GetTickCount);
    q = patch_import(exe, "KERNEL32.dll", "QueryPerformanceCounter", (void *)h_QPC, (void **)&o_QPC);
    logf_("timers rebased to process start: timeGetTime %d, GetTickCount %d, QueryPerformanceCounter %d (was %lu ms since boot)\n",
          a, b, q, timeGetTime());
}

static void patch_iat(void)
{
    BOOL a, b;
    patch_timers();
    patch_import(GetModuleHandleA(NULL), "DDRAW.dll", "DirectDrawCreate", (void *)h_DirectDrawCreate, (void **)&o_DirectDrawCreate);
    logf_(o_DirectDrawCreate ? "patched DDRAW!DirectDrawCreate import\n" : "WARNING: DirectDrawCreate import not found\n");
    if (g_minimal) return;   /* DirectDraw hook only: it carries the video-memory fix */
    a = patch_import(GetModuleHandleA(NULL), "USER32.dll", "GetFocus", (void *)h_GetFocus, (void **)&o_GetFocus);
    b = patch_import(GetModuleHandleA(NULL), "USER32.dll", "ShowCursor", (void *)h_ShowCursor, (void **)&o_ShowCursor);
    patch_import(GetModuleHandleA(NULL), "USER32.dll", "SetCursor", (void *)h_SetCursor, (void **)&o_SetCursor);
    logf_("patched USER32!GetFocus import: %d, ShowCursor: %d\n", a, b);
    a = patch_import(GetModuleHandleA(NULL), "DSOUND.dll", "DirectSoundCreate", (void *)h_DirectSoundCreate, (void **)&o_DirectSoundCreate);
    b = patch_import(GetModuleHandleA("QMixer.dll"), "DSOUND.dll", "DirectSoundCreate", (void *)h_DirectSoundCreate, (void **)&o_DirectSoundCreate);
    logf_("patched DSOUND!DirectSoundCreate import: TP.EXE %d, QMixer.dll %d\n", a, b);
}

/* ================= dinput.dll proxy ================= */
/* Alt-tab fix: after focus returns, the game's mouse/keyboard reads fail with DIERR_INPUTLOST / DIERR_NOTACQUIRED and
 * it never re-acquires (windowed mode), so the pointer freezes. Re-acquire and retry while the game has focus.
 * IDirectInputDevice vtable slots: 7 Acquire, 9 GetDeviceState, 10 GetDeviceData (same in Device2). */
#define DIERR_INPUTLOST_   ((HRESULT)0x8007001EL)
#define DIERR_NOTACQUIRED_ ((HRESULT)0x8007000CL)
typedef HRESULT (WINAPI *DIACQ_t)(void *);
typedef HRESULT (WINAPI *DIGDS_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *DIGDD_t)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (WINAPI *DIQI_t)(void *, REFIID, void **);
typedef ULONG (WINAPI *DIREL_t)(void *);
static struct { void **vt; DIACQ_t acq, unacq; DIGDS_t gds; DIGDD_t gdd; DIQI_t qi; DIREL_t rel; } g_did[8];
static int g_ndid;
static DWORD g_reacquires;

static int did_find(void *dev) { int i; for (i = 0; i < g_ndid; i++) if (g_did[i].vt == *(void ***)dev) return i; return 0; }
static BOOL game_has_focus(void) { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
/* Renderer's settings panel is open: the mouse belongs to it. uiOpen is a heartbeat (renderer tick count): a dead
 * renderer can't leave the game without input for more than 2 s. */
static BOOL ui_open(void) { DWORD t = g_sh ? g_sh->uiOpen : 0; return t && GetTickCount() - t < 2000; }
static BOOL lost(HRESULT hr) { return hr == DIERR_INPUTLOST_ || hr == DIERR_NOTACQUIRED_; }

static HRESULT WINAPI h_DIGetDeviceState(void *dev, DWORD cb, void *data)
{
    int i = did_find(dev);
    HRESULT hr = g_did[i].gds(dev, cb, data);
    if (lost(hr) && !ui_open() && game_has_focus() && SUCCEEDED(g_did[i].acq(dev))) { hr = g_did[i].gds(dev, cb, data); if (g_reacquires++ < 5) logf_("dinput: re-acquired device after input loss (hr now %08lx)\n", hr); }
    return hr;
}
static HRESULT WINAPI h_DIGetDeviceData(void *dev, DWORD cbObj, void *rg, DWORD *n, DWORD fl)
{
    int i = did_find(dev);
    DWORD want = n ? *n : 0;
    HRESULT hr = g_did[i].gdd(dev, cbObj, rg, n, fl);
    if (lost(hr) && !ui_open() && game_has_focus() && SUCCEEDED(g_did[i].acq(dev))) { if (n) *n = want; hr = g_did[i].gdd(dev, cbObj, rg, n, fl); if (g_reacquires++ < 5) logf_("dinput: re-acquired device after input loss (hr now %08lx)\n", hr); }
    return hr;
}
/* The game acquires on WM_ACTIVATEAPP(TRUE), often before Windows has made it foreground, so that fails and the game
 * stops polling for good. Report success instead; the next read re-acquires for real once focus is back (above). */
/* The mouse is NOT polled: FUN_0063e8c0 sets up DirectInput event notification (SetEventNotification + 32-entry buffer)
 * and the game reads only after the event fires. When Windows takes the device away on alt-tab (the game never sees
 * WM_ACTIVATEAPP in windowed mode: flag 0x7a1a14 bit 3 stays set), no event ever fires again, so no read happens and
 * the read hooks above never get a chance. So: remember devices the game acquired and hasn't unacquired, and
 * re-acquire them from EndScene while the game has focus. */
static void *g_want[8];
static CRITICAL_SECTION g_diLock;   /* re-entrant on purpose: dinput may Release through the vtable inside Acquire */
static void want_set(void *dev, BOOL on)
{
    int i, slot = -1;
    EnterCriticalSection(&g_diLock);
    for (i = 0; i < 8; i++) { if (g_want[i] == dev) break; if (!g_want[i] && slot < 0) slot = i; }
    if (i < 8) { if (!on) g_want[i] = NULL; }
    else if (on && slot >= 0) g_want[slot] = dev;
    LeaveCriticalSection(&g_diLock);
}
static ULONG WINAPI h_DIRelease(void *dev)
{
    ULONG r;
    EnterCriticalSection(&g_diLock);
    r = g_did[did_find(dev)].rel(dev);
    if (!r) ZeroMemory(g_want, sizeof g_want);   /* ponytail: forget all on any device death (aliases could dangle); the game's next Acquire re-adds */
    LeaveCriticalSection(&g_diLock);
    return r;
}
static HRESULT WINAPI h_DIUnacquire(void *dev)
{
    static DWORD n;
    want_set(dev, FALSE);
    if (n++ < 10) logf_("dinput: game unacquired device %p\n", dev);
    return g_did[did_find(dev)].unacq(dev);
}
static void di_reacquire(void)
{
    static DWORD last, n;
    static BOOL released;
    int i;
    if (ui_open()) {   /* let go once (system cursor moves freely, panel gets clicks); stays wanted for afterwards */
        if (!released) {
            EnterCriticalSection(&g_diLock);
            for (i = 0; i < 8; i++) if (g_want[i]) g_did[did_find(g_want[i])].unacq(g_want[i]);
            LeaveCriticalSection(&g_diLock);
            released = TRUE;
            logf_("dinput: settings panel open, released game input\n");
        }
        return;
    }
    if (released) { released = FALSE; last = 0; }
    if (GetTickCount() - last < 250 || !game_has_focus()) return;
    last = GetTickCount();
    EnterCriticalSection(&g_diLock);
    for (i = 0; i < 8; i++)   /* DI_OK = it was not acquired (already acquired gives S_FALSE) */
        if (g_want[i] && g_did[did_find(g_want[i])].acq(g_want[i]) == 0 && n++ < 10)
            logf_("dinput: re-acquired device %p after the game got focus back\n", g_want[i]);
    LeaveCriticalSection(&g_diLock);
}
static HRESULT WINAPI h_DIAcquire(void *dev)
{
    HRESULT hr;
    static DWORD n;
    want_set(dev, TRUE);
    if (ui_open()) return 0;   /* acquired for real when the panel closes */
    hr = g_did[did_find(dev)].acq(dev);
    if (FAILED(hr)) { if (n++ < 5) logf_("dinput: game's Acquire failed (%08lx), reporting success; will re-acquire\n", hr); return 0; }
    return hr;
}
static void hook_didevice(void *dev);
static HRESULT WINAPI h_DIDevQI(void *dev, REFIID riid, void **out)
{
    HRESULT hr = g_did[did_find(dev)].qi(dev, riid, out);
    if (SUCCEEDED(hr) && out && *out) hook_didevice(*out);   /* Device2 may use another vtable */
    return hr;
}
static void hook_didevice(void *dev)
{
    void **vt = *(void ***)dev, *o;
    int i;
    for (i = 0; i < g_ndid; i++) if (g_did[i].vt == vt) return;
    if (g_ndid == 8) return;
    i = g_ndid++;   /* entry 0 doubles as the fallback in did_find, so fill it before anything can call through */
    g_did[i].vt = vt;
    patch(&vt[2], (void *)h_DIRelease, &o); g_did[i].rel = (DIREL_t)o;
    patch(&vt[7], (void *)h_DIAcquire, &o); g_did[i].acq = (DIACQ_t)o;
    patch(&vt[8], (void *)h_DIUnacquire, &o); g_did[i].unacq = (DIACQ_t)o;
    patch(&vt[9], (void *)h_DIGetDeviceState, &o); g_did[i].gds = (DIGDS_t)o;
    patch(&vt[10], (void *)h_DIGetDeviceData, &o); g_did[i].gdd = (DIGDD_t)o;
    patch(&vt[0], (void *)h_DIDevQI, &o); g_did[i].qi = (DIQI_t)o;
    logf_("dinput: hooked device vtable %p (re-acquire on input lost)\n", vt);
}

/* ================= camera & screen options (supported TP.EXE only) =================
 * Camera: FUN_0042b1c0 every frame, FUN_0042a990 on park load. Zoom = camera distance 0x7909ec, clamped to 70..130 at
 * 0x42c66b..0x42c6a1; the pitch follows zoom (45 deg at 70, 65 deg at 130, extrapolated beyond and capped at 84 deg).
 * Projection: FUN_00578be0(this 0x87b110, near, far, fov in degrees) + frustum FUN_0056b790(this [0x879d54], fov), called
 * by the camera init with fov = [0x74c9b8] (pi/2) * 57.29 = 90. Mouse picking (FUN_0045bf90) keeps its own copy of
 * that frustum: half-diagonal angle [0x74c9b8] * 0.885 * 0.5 and the aspect as a double at [0x6fe640].
 * Aspect 0x8bcbcc (H/W, 0.75) also feeds the screen matrix 0x87b0d0[0] = 2 * aspect.
 * Keys: FUN_0040ca20(group, action) = is a key bound to action down. Camera group [0x787c14] (+6 = enabled; the game
 * clears it while you type), pan actions 0xd..0x10 = numpad 8/2/4/6, rotate 6/7 = left/right arrow. */
static float g_zoomMin = 70, g_zoomMax = 130, g_fovRad = 1.5707964f, g_pickBase = 1.5707964f;
static double g_pickAspect = 0.75;
static float g_fovDeg = 90, g_fovApplied, g_aspect = 0.75f;
static BOOL g_wasd, g_camPatched;
static WORD g_scSaved[32];
/* FUN_0056ea90 only draws objects and terrain inside a square around the eye: side [0x8bd4c0] (view distance setting)
 * in first-person views (0x8bcbc8 & 0x2000), else a fixed 460 (0x56eaae), just enough for zoom 130 at 4:3. The patch
 * always takes the first branch and reads g_viewRadius instead, which cam_frame grows with zoom, FOV and screen width. */

static BOOL poke(DWORD addr, DWORD expect, DWORD value)   /* expect 0 = don't check */
{
    DWORD old, *p = (DWORD *)addr;
    if (expect && *p != expect) { logf_("camera: unexpected code at %08lx (%08lx), not patched\n", addr, *p); return FALSE; }
    VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old);
    *p = value;
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    return TRUE;
}
static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
static BOOL poke2(DWORD addr, WORD expect, WORD value)
{
    DWORD old;
    WORD *p = (WORD *)addr;
    if (*p != expect) { logf_("camera: unexpected code at %08lx (%04x), not patched\n", addr, *p); return FALSE; }
    VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old);
    *p = value;
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    return TRUE;
}

typedef DWORD (__fastcall *KEYTEST_t)(void *grp, void *edx, short action);       /* __thiscall, one stack argument */
typedef void (__fastcall *PROJ_t)(void *m, void *edx, float zn, float zf, float fov);
typedef void (__fastcall *FRUST_t)(void *f, void *edx, float fov);

static DWORD __fastcall h_KeyTest(void *grp, void *edx, short action)
{
    DWORD r = ((KEYTEST_t)0x40ca20)(grp, edx, action);
    int vk;
    if ((WORD)r || !g_wasd || !*(short *)((BYTE *)grp + 6)) return r;   /* bound key down / WASD off / keys disabled (typing) */
    switch (action) {
    case 0xd: vk = 'W'; break;
    case 0xe: vk = 'S'; break;
    case 0xf: vk = 'A'; break;
    case 0x10: vk = 'D'; break;
    case 6: vk = 'Q'; break;
    case 7: vk = 'E'; break;
    default: return r;
    }
    if (((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) || !game_has_focus() || ui_open()) return r;
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : r;
}

/* WASD/Q/E would also fire the game's own one-letter shortcuts (A = all items, S = all staff): unbind those while on. */
static void shortcuts_wasd(BOOL on)
{
    BYTE *grp = *(BYTE **)0x78721c, *e;
    int i, n;
    if (!grp || !(e = *(BYTE **)grp)) return;
    n = *(short *)(grp + 4);
    for (i = 0; i < n && i < 32; i++, e += 20) {
        WORD *key = (WORD *)(e + 2);
        if (on && !g_scSaved[i] && !*(WORD *)(e + 4) && *key < 128 && *key && strchr("WASDQE", *key)) {
            g_scSaved[i] = *key; *key = 0;
            logf_("camera: WASD on, game shortcut '%c' unbound\n", g_scSaved[i]);
        } else if (!on && g_scSaved[i]) { *key = g_scSaved[i]; g_scSaved[i] = 0; }
    }
}

static void cam_set_fov(float fovDeg)
{
    double t = tan(fovDeg * 0.5 * 3.14159265358979 / 180);
    g_fovRad = (float)(fovDeg * 3.14159265358979 / 180);
    g_pickBase = (float)(atan(t * 0.5 * sqrt(1 + 1 / ((double)g_aspect * g_aspect))) / (0.885 * 0.5));   /* 1.5700 at 90, 4:3 */
    g_pickAspect = g_aspect;
}

static void cam_patch(void)
{
    static const DWORD keySites[] = {0x42b334, 0x42b364, 0x42b620, 0x42b632, 0x42b644, 0x42b656, 0x42b9a3, 0x42b9bd, 0x42b9d7, 0x42b9f1};
    char res[32] = "";
    BOOL ok = TRUE;
    BYTE *p;
    int i;
    GetPrivateProfileStringA("tpwrtx", "resolution", "", res, sizeof res, ".\\tpwrtx.ini");
    if (!_stricmp(res, "auto")) { g_resW = GetSystemMetrics(SM_CXSCREEN); g_resH = GetSystemMetrics(SM_CYSCREEN); }
    else if (sscanf(res, "%lux%lu", &g_resW, &g_resH) != 2) g_resW = g_resH = 0;
    if (g_resW < 640 || g_resH < 480 || g_resW > 7680 || g_resH > 4320 || g_resW < g_resH) g_resW = g_resH = 0;
    if (g_resW) {   /* resolution switch FUN_00449ec0: mov [0x7a1968], W / mov [0x7a1930], H; replace every mode from 1024 up */
        int n = 0;
        g_aspect = (float)g_resH / g_resW;
        for (p = (BYTE *)0x449ec0; p < (BYTE *)0x44a160; p++)
            if (p[0] == 0xC7 && p[1] == 0x05 && *(DWORD *)(p + 2) == 0x7a1968 && *(DWORD *)(p + 6) >= 1024 &&
                p[10] == 0xC7 && p[11] == 0x05 && *(DWORD *)(p + 12) == 0x7a1930) {
                poke((DWORD)(p + 6), 0, g_resW); poke((DWORD)(p + 16), 0, g_resH); n++;
            }
        logf_("screen: resolution %lux%lu (aspect %.4f), %d game modes replaced\n", g_resW, g_resH, g_aspect, n);
    }
    cam_set_fov(90);
    ok &= poke(0x42c66d, 0x6fde08, (DWORD)&g_zoomMin) & poke(0x42c67e, 0x428c0000, fbits(70)) &
          poke(0x42c68c, 0x6fde0c, (DWORD)&g_zoomMax) & poke(0x42c69d, 0x43020000, fbits(130));
    ok &= poke(0x42aa31, 0x74c9b8, (DWORD)&g_fovRad) & poke(0x45bf9b, 0x74c9b8, (DWORD)&g_pickBase) &
          poke(0x45c09c, 0x6fe640, (DWORD)&g_pickAspect);
    /* 0x56eaa1: JZ +0x0b (74 0b) -> NOP NOP; 0x56eaa3: MOV EAX, [0x8bd4c0] -> MOV EAX, [&g_viewRadius] */
    ok &= poke(0x56eaa4, 0x8bd4c0, (DWORD)&g_viewRadius) && poke2(0x56eaa1, 0x0b74, 0x9090);
    for (i = 0; i < (int)(sizeof keySites / sizeof keySites[0]); i++) {
        p = (BYTE *)keySites[i];
        if (p[0] == 0xE8 && (DWORD)(p + 5) + *(DWORD *)(p + 1) == 0x40ca20) poke((DWORD)(p + 1), 0, (DWORD)h_KeyTest - (DWORD)(p + 5));
        else { logf_("camera: unexpected key call at %08lx\n", (DWORD)p); ok = FALSE; }
    }
    g_camPatched = ok;
    logf_("camera: zoom/FOV/picking/WASD patches %s\n", ok ? "applied" : "INCOMPLETE");
}

/* Every frame (game thread): follow the settings panel and keep the game on our FOV / aspect. */
static void cam_frame(void)
{
    TpwShared *sh = g_sh;
    float fov, zmin, zmax, cur;
    void *frustum;
    if (!g_camPatched) return;
    {   /* draw area for the next frame: the original 460 covers zoom 130, 4:3, fov 90; scale up from there */
        float z = *(volatile float *)0x7909ec, t = (float)tan(g_fovDeg * 0.5 * 3.14159265 / 180), r = 460;
        if (z > 130) r *= z / 130;
        if (g_aspect < 0.75f) r *= 0.75f / g_aspect;
        if (t > 1) r *= t;
        if (r > 2600) r = 2600;   /* ponytail: about the whole 128-tile map from anywhere on it */
        g_viewRadius = (*(volatile DWORD *)0x8bcbc8 & 0x2000) ? *(volatile float *)0x8bd4c0 : r;
    }
    if (g_resW) {
        volatile float *a = (volatile float *)0x8bcbcc;
        if (*a != g_aspect && *a > 0) { logf_("screen: aspect %.4f -> %.4f\n", *a, g_aspect); *a = g_aspect; *(volatile float *)0x87b0d0 = 2 * g_aspect; }
    }
    if (!sh || !(sh->camFov > 0)) return;   /* renderer not up yet */
    g_wasd = sh->camWasd != 0;
    shortcuts_wasd(g_wasd);
    zmin = sh->camZoomMin; zmax = sh->camZoomMax;
    if (zmin >= 5 && zmax <= 1000 && zmin < zmax && (zmin != g_zoomMin || zmax != g_zoomMax)) {
        g_zoomMin = zmin; g_zoomMax = zmax;
        poke(0x42c67e, 0, fbits(zmin)); poke(0x42c69d, 0, fbits(zmax));
        logf_("camera: zoom range %.0f..%.0f\n", zmin, zmax);
    }
    fov = sh->camFov;
    if (fov >= 30 && fov <= 150 && fov != g_fovDeg) { g_fovDeg = fov; cam_set_fov(fov); logf_("camera: FOV %.0f\n", fov); }
    /* the game sets 90 on park load (100 in the front-end menus: left alone) */
    cur = *(volatile float *)0x87af58;
    frustum = *(void **)0x879d54;
    if (frustum && cur != g_fovDeg && (cur == 90 || cur == g_fovApplied)) {
        ((PROJ_t)0x578be0)((void *)0x87b110, NULL, *(float *)0x87b1dc, *(float *)0x768acc, g_fovDeg);
        ((FRUST_t)0x56b790)(frustum, NULL, g_fovDeg);
        g_fovApplied = g_fovDeg;
    }
}

typedef HRESULT (WINAPI *DICD_t)(void *, REFGUID, void **, IUnknown *);
static DICD_t o_DICreateDevice;
static HRESULT WINAPI h_DICreateDevice(void *di, REFGUID g, void **out, IUnknown *u)
{
    HRESULT hr = o_DICreateDevice(di, g, out, u);
    if (SUCCEEDED(hr) && out && *out) hook_didevice(*out);
    return hr;
}

typedef HRESULT (WINAPI *DIC_t)(HINSTANCE, DWORD, void **, IUnknown *);
HRESULT WINAPI Proxy_DirectInputCreateA(HINSTANCE h, DWORD ver, void **out, IUnknown *u)
{
    static DIC_t real;
    HRESULT hr;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\dinput.dll");
        real = (DIC_t)GetProcAddress(LoadLibraryA(path), "DirectInputCreateA");
    }
    hr = real ? real(h, ver, out, u) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) patch(&(*(void ***)*out)[3], (void *)h_DICreateDevice, (void **)&o_DICreateDevice);   /* IDirectInput::CreateDevice */
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_diLock);
        g_log = fopen("tphook.log", "w");
        g_knownExe = exe_supported();
        g_minimal = GetPrivateProfileIntA("tpwrtx", "minimal", 0, ".\\tpwrtx.ini") != 0 || !g_knownExe;
        if (!g_knownExe) logf_("tphook: this TP.EXE is not the supported build -> timer + video-memory fixes only, no ray tracing\n");
        logf_("tphook loaded%s\n", g_minimal ? " (minimal: video-memory fix + logging only)" : "");
        /* Windowed (dxwrapper): without this, 150% display scaling blows the 2048x1536 window up past the screen height
         * and it gets squeezed to 16:9. Must happen before the game creates its window, hence DllMain. */
        if (!g_minimal) SetProcessDPIAware();
        if (!g_minimal) cam_patch();
        patch_iat();
        fflush(g_log);
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        logf_("detach at frame %lu\n", g_frame);
        fclose(g_log);
    }
    return TRUE;
}
