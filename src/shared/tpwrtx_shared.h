/* Shared-memory layout between the 32-bit hook (inside TP.EXE) and the 64-bit renderer.
 * Fixed-size, pointer-free, identical on x86 and x64. */
#pragma once
#include <stdint.h>

#define TPWRTX_MAP_NAME     "Local\\TPWRTX_SHARED_V1"
#define TPWRTX_MAGIC        0x58525054u /* 'TPRX' */
#define TPWRTX_VERSION      1u
#define TPWRTX_SLOTS        3u
#define TPWRTX_MAX_DRAWS    8192u
#define TPWRTX_MAX_VERTS    (192u * 1024u)
#define TPWRTX_MAX_INDICES  (384u * 1024u)
#define TPWRTX_MAX_TEX      4096u
#define TPWRTX_TEX_HEAP     (64u * 1024u * 1024u)   /* BGRA8 pixels; ~25 MB used by the game's 256px texture cache.
                                                       ponytail: kept small, the mapping lives in a 32-bit address space */

/* Game's vertex format, FVF 0x1c4 (XYZRHW | DIFFUSE | SPECULAR | TEX1), copied verbatim. */
typedef struct { float x, y, z, rhw; uint32_t diffuse, specular; float u, v; } TpwVertex;

enum { TPW_DRAW_UI = 1u };   /* vertex z < 0.1: engine's UI depth band, already in final screen space */

typedef struct {
    uint32_t firstVertex, numVertices, firstIndex, numIndices; /* indices are relative to firstVertex */
    uint32_t texId;        /* 0 = untextured, else 1-based index into TpwShared.tex */
    uint32_t flags;
    /* Direct3D render states at draw time (D3DRENDERSTATE_* values, numerically equal to D3D12 enums for blend/compare) */
    uint8_t  alphaBlend, srcBlend, dstBlend, alphaTest;
    uint8_t  alphaRef, alphaFunc, zEnable, zWrite;
    uint8_t  zFunc, cull, texBlend, colorKey;
    uint8_t  specular, pad[3];
} TpwDraw;

typedef struct {
    volatile uint32_t seq;   /* seqlock: odd while the hook writes this slot */
    uint32_t frame;
    float    view[16];       /* world->view, row-vector convention (game memory 0x790900) */
    float    camInv[16];     /* view->world, row 3 = eye position (0x790990) */
    float    f, k, cx, cy;   /* z_view = k/rhw, x_view = (sx-cx)*z/f, y_view = -(sy-cy)*z/f */
    uint32_t width, height;
    uint32_t numDraws, numVertices, numIndices, pad;
} TpwFrameHeader;

typedef struct {
    TpwFrameHeader h;
    TpwDraw  draws[TPWRTX_MAX_DRAWS];
    TpwVertex verts[TPWRTX_MAX_VERTS];
    uint16_t indices[TPWRTX_MAX_INDICES];
} TpwFrameSlot;

typedef struct {
    uint32_t width, height;
    uint32_t offset;            /* into texHeap */
    volatile uint32_t version;  /* bumped after the pixels are written */
} TpwTexture;

typedef struct {
    uint32_t magic, version;
    volatile uint32_t latestSlot;     /* last completed slot */
    volatile uint32_t framesWritten;
    volatile uint32_t texCount;       /* entries in use (1-based ids up to texCount) */
    volatile uint32_t texHeapUsed;
    volatile uint32_t texGeneration;  /* bumped when the heap is reset: drop all cached textures */
    uint32_t hookPid;
    /* Renderer output, shown by the hook inside the game window (overlay mode). Handles are NT handles already
     * duplicated into the game process by the renderer; 0 = no output. */
    volatile uint32_t outTexHandle, outFenceHandle;   /* shared R8G8B8A8 texture, shared fence */
    volatile uint32_t outWidth, outHeight;
    volatile uint32_t outFenceValue;                  /* fence value the renderer signals once the latest frame is in the texture */
    volatile uint32_t outVisible;                     /* show it now (ray tracing a park and not toggled off) */
    volatile uint32_t uiOpen;                         /* renderer's settings panel is open: hook releases the game's mouse/keyboard */
    TpwTexture tex[TPWRTX_MAX_TEX];
    TpwFrameSlot slots[TPWRTX_SLOTS];
    uint8_t texHeap[TPWRTX_TEX_HEAP];
} TpwShared;
