/* tpwrtx_nvngx.dll_forwarder.dll: makes the calls into NVIDIA's DLSS 5 model (nvngx_dlssnr.dll) on the viewer's behalf.
 *
 * The model has no public SDK. It checks the module that calls it and refuses any whose file path lacks "nvngx.dll"
 * (normally the driver's _nvngx.dll calls it), so these four calls have to come from a DLL with such a name.
 * Same approach as the OptiScaler DLSS-NR module (github.com/optiscaler/OptiScaler/pull/1116), written fresh here.
 *
 * Load-bearing detail: never `return f(...)`. That compiles to a tail jump, this module's frame vanishes and the model
 * sees the viewer as its caller. The result goes through a volatile local so the call returns here first. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef int (*PFN_Init)(unsigned long long appId, const wchar_t *dataPath, void *device, int sdkVersion, const void *info);
typedef int (*PFN_Create)(void *cmdList, int feature, void *params, void **handle);
typedef int (*PFN_Eval)(void *cmdList, void *handle, void *params, void *callback);
typedef int (*PFN_Release)(void *handle);

static PFN_Init pInit;
static PFN_Create pCreate;
static PFN_Eval pEval;
static PFN_Release pRelease;

__declspec(dllexport) int fwd_load(const wchar_t *modelPath)
{
    HMODULE m = LoadLibraryExW(modelPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return 0;
    pInit = (PFN_Init)GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext");
    pCreate = (PFN_Create)GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature");
    pEval = (PFN_Eval)GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature");
    pRelease = (PFN_Release)GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature");
    return pInit && pCreate && pEval && pRelease;
}
__declspec(dllexport) int fwd_init(unsigned long long appId, const wchar_t *dataPath, void *device, int sdkVersion, const void *info)
{
    volatile int r = pInit(appId, dataPath, device, sdkVersion, info);
    return r;
}
__declspec(dllexport) int fwd_create(void *cmdList, int feature, void *params, void **handle)
{
    volatile int r = pCreate(cmdList, feature, params, handle);
    return r;
}
__declspec(dllexport) int fwd_evaluate(void *cmdList, void *handle, void *params)
{
    volatile int r = pEval(cmdList, handle, params, NULL);
    return r;
}
__declspec(dllexport) int fwd_release(void *handle)
{
    volatile int r = pRelease(handle);
    return r;
}
