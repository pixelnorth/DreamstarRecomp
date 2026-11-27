// The in-game stats box's hardware figures (Show FPS, runtime/ui/menu.mm fps_box): GPU and CPU load and temperatures,
// system memory. A low-priority thread samples them once a second; everything is loaded at run time (no new import):
// - NVIDIA: nvml.dll (the driver's): GPU load, GPU temperature, memory temperature (a field some cards don't report)
// - any GPU: D3DKMT adapter perf data (Task Manager's GPU temperature, WDDM 2.4+) and the PDH "GPU Engine" counters
//   (Task Manager's GPU %: the busiest engine type over every process, 3D / compute / copy / video)
// - CPU load: GetSystemTimes; CPU temperature: the ACPI thermal zones (PDH "Thermal Zone Information"), the only one
//   readable without administrator rights - many PCs report none (then "--")
// - hotspot: not readable through public APIs on most cards: NaN, shown as "--"
// Values are NaN until known; plat_hw_stat returns PLAT_HW_UNKNOWN for those.
#include <windows.h>
#include <pdh.h>
#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA 0x800007D2L  // (pdhmsg.h)
#endif
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <pthread.h>
#include "../platform.h"

static volatile float g_hw[8] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
static uint8_t g_luid[8];
static volatile int g_have_luid;

void plat_hw_set_gpu_luid(const uint8_t luid[8]) {  // (vulkan/device.c: the adapter the game draws on)
    memcpy(g_luid, luid, 8);
    g_have_luid = 1;
}

// NVML, the few calls used (nvml.h is not shipped: the documented signatures)
typedef struct { unsigned gpu, memory; } NvmlUtil;
typedef struct { unsigned fieldId, scopeId; long long timestamp, latencyUsec; int valueType, nvmlReturn; union { double d; unsigned ui; unsigned long ul; unsigned long long ull; long long sll; } value; } NvmlField;
static int (*nv_init)(void), (*nv_handle)(unsigned, void **), (*nv_temp)(void *, int, unsigned *), (*nv_util)(void *, NvmlUtil *),
    (*nv_fields)(void *, int, NvmlField *), (*nv_pci)(void *, void *);
static void *nv_dev;

static void nvml_open(void) {
    HMODULE m = LoadLibraryA("nvml.dll");
    if (!m) {
        char p[MAX_PATH];
        if (GetEnvironmentVariableA("ProgramW6432", p, sizeof p)) {
            strncat(p, "\\NVIDIA Corporation\\NVSMI\\nvml.dll", sizeof p - strlen(p) - 1);
            m = LoadLibraryA(p);
        }
    }
    if (!m) return;
    nv_init = (int (*)(void))GetProcAddress(m, "nvmlInit_v2");
    nv_handle = (int (*)(unsigned, void **))GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
    nv_temp = (int (*)(void *, int, unsigned *))GetProcAddress(m, "nvmlDeviceGetTemperature");
    nv_util = (int (*)(void *, NvmlUtil *))GetProcAddress(m, "nvmlDeviceGetUtilizationRates");
    nv_fields = (int (*)(void *, int, NvmlField *))GetProcAddress(m, "nvmlDeviceGetFieldValues");
    if (!nv_init || !nv_handle || !nv_temp || nv_init() != 0 || nv_handle(0, &nv_dev) != 0) nv_dev = NULL;
    // ponytail: NVML's first GPU; a PC with two NVIDIA cards may show the other one
}

// D3DKMT (gdi32): the adapter's perf data, Temperature in tenths of a degree
typedef struct { LUID luid; UINT h; } KmtOpen;
typedef struct { UINT h; int type; void *data; UINT size; } KmtQuery;
typedef struct { UINT32 idx; ULONGLONG memf, maxmemf, maxmemfoc, membw, pciebw; ULONG fan, power, temp; UCHAR pso; } KmtPerf;
static LONG(APIENTRY *kmt_open)(KmtOpen *);
static LONG(APIENTRY *kmt_query)(KmtQuery *);
static UINT kmt_h;

static float kmt_temp(void) {
    if (!kmt_h) {
        static int tried;
        if (tried++ || !g_have_luid) return NAN;
        HMODULE g = GetModuleHandleA("gdi32.dll");
        if (!g) g = LoadLibraryA("gdi32.dll");
        kmt_open = (LONG(APIENTRY *)(KmtOpen *))GetProcAddress(g, "D3DKMTOpenAdapterFromLuid");
        kmt_query = (LONG(APIENTRY *)(KmtQuery *))GetProcAddress(g, "D3DKMTQueryAdapterInfo");
        KmtOpen o = {0};
        memcpy(&o.luid, g_luid, 8);
        if (!kmt_open || !kmt_query || kmt_open(&o) != 0) return NAN;
        kmt_h = o.h;
    }
    KmtPerf pd = {0};
    KmtQuery q = {kmt_h, 62 /* KMTQAITYPE_ADAPTERPERFDATA */, &pd, sizeof pd};
    if (kmt_query(&q) != 0 || pd.temp == 0) return NAN;
    return pd.temp / 10.0f;
}

// PDH, loaded at run time (pdh.dll)
static PDH_STATUS(WINAPI *pdh_open)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
static PDH_STATUS(WINAPI *pdh_add)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
static PDH_STATUS(WINAPI *pdh_collect)(PDH_HQUERY);
static PDH_STATUS(WINAPI *pdh_array)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
static PDH_HQUERY pq;
static PDH_HCOUNTER pc_gpu, pc_zone;

static void pdh_setup(void) {
    HMODULE m = LoadLibraryA("pdh.dll");
    if (!m) return;
    pdh_open = (void *)GetProcAddress(m, "PdhOpenQueryW");
    pdh_add = (void *)GetProcAddress(m, "PdhAddEnglishCounterW");
    pdh_collect = (void *)GetProcAddress(m, "PdhCollectQueryData");
    pdh_array = (void *)GetProcAddress(m, "PdhGetFormattedCounterArrayW");
    if (!pdh_open || !pdh_add || !pdh_collect || !pdh_array || pdh_open(NULL, 0, &pq) != 0) { pq = NULL; return; }
    if (pdh_add(pq, L"\\GPU Engine(*)\\Utilization Percentage", 0, &pc_gpu) != 0) pc_gpu = NULL;
    if (pdh_add(pq, L"\\Thermal Zone Information(*)\\Temperature", 0, &pc_zone) != 0) pc_zone = NULL;
    pdh_collect(pq);
}

// a counter's instances, formatted as doubles (NULL: none); the caller frees
static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_items(PDH_HCOUNTER c, DWORD *n) {
    DWORD bytes = 0;
    *n = 0;
    if (!c || pdh_array(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, n, NULL) != (PDH_STATUS)PDH_MORE_DATA) return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = malloc(bytes);
    if (it && pdh_array(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, n, it) != 0) free(it), it = NULL;
    return it;
}

static float pdh_gpu_load(void) {
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = pdh_items(pc_gpu, &n);
    if (!it) return NAN;
    wchar_t want[48] = L"";
    if (g_have_luid) {
        LUID l;
        memcpy(&l, g_luid, 8);
        swprintf(want, 48, L"luid_0x%08X_0x%08X", (unsigned)l.HighPart, (unsigned)l.LowPart);
    }
    // per engine type ("engtype_3D"...): the sum over processes; the GPU's load is the busiest type (Task Manager)
    struct { wchar_t t[32]; double v; } ty[16];
    int nt = 0;
    for (DWORD i = 0; i < n; i++) {
        if (it[i].FmtValue.CStatus != 0 || (want[0] && !wcsstr(it[i].szName, want))) continue;
        const wchar_t *e = wcsstr(it[i].szName, L"engtype_");
        if (!e) continue;
        int k = 0;
        while (k < nt && wcscmp(ty[k].t, e)) k++;
        if (k == nt) {
            if (nt == 16) continue;
            wcsncpy(ty[nt].t, e, 31), ty[nt].t[31] = 0, ty[nt].v = 0, nt++;
        }
        ty[k].v += it[i].FmtValue.doubleValue;
    }
    free(it);
    double best = 0;
    for (int k = 0; k < nt; k++) best = fmax(best, ty[k].v);
    return nt ? (float)fmin(best, 100) : NAN;
}

static float pdh_cpu_temp(void) {
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = pdh_items(pc_zone, &n);
    if (!it) return NAN;
    double best = NAN;
    for (DWORD i = 0; i < n; i++) {
        double c = it[i].FmtValue.doubleValue - 273.15;  // (Kelvin)
        if (it[i].FmtValue.CStatus == 0 && c > 5 && c < 120 && !(c <= best)) best = c;
    }
    free(it);
    return (float)best;
}

static float cpu_load(void) {
    static ULONGLONG li, lk, lu;
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return NAN;
    ULONGLONG i = ((ULONGLONG)fi.dwHighDateTime << 32) | fi.dwLowDateTime, k = ((ULONGLONG)fk.dwHighDateTime << 32) | fk.dwLowDateTime,
              u = ((ULONGLONG)fu.dwHighDateTime << 32) | fu.dwLowDateTime;
    ULONGLONG di = i - li, dt = (k - lk) + (u - lu);  // (kernel time includes idle)
    int first = !li;
    li = i, lk = k, lu = u;
    return first || !dt ? NAN : (float)(100.0 * (double)(dt - di) / (double)dt);
}

static void *hw_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    nvml_open();
    pdh_setup();
    for (;;) {
        float v[8] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
        if (pq) pdh_collect(pq);
        if (nv_dev) {
            unsigned t;
            NvmlUtil u;
            if (nv_temp(nv_dev, 0, &t) == 0) v[PLAT_HW_GPU_TEMP] = (float)t;
            if (nv_util && nv_util(nv_dev, &u) == 0) v[PLAT_HW_GPU_LOAD] = (float)u.gpu;
            NvmlField f = {.fieldId = 82};  // NVML_FI_DEV_MEMORY_TEMP
            if (nv_fields && nv_fields(nv_dev, 1, &f) == 0 && f.nvmlReturn == 0 && f.value.ui > 0 && f.value.ui < 150)
                v[PLAT_HW_GPU_MEM_TEMP] = (float)f.value.ui;
        }
        if (isnan(v[PLAT_HW_GPU_TEMP])) v[PLAT_HW_GPU_TEMP] = kmt_temp();
        if (isnan(v[PLAT_HW_GPU_LOAD])) v[PLAT_HW_GPU_LOAD] = pdh_gpu_load();
        v[PLAT_HW_CPU_LOAD] = cpu_load();
        v[PLAT_HW_CPU_TEMP] = pdh_cpu_temp();
        MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
        if (GlobalMemoryStatusEx(&ms)) v[PLAT_HW_SYS_MEM_GB] = (float)((ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0);
        for (int i = 0; i < 8; i++) g_hw[i] = v[i];
        Sleep(1000);
    }
    return NULL;
}

float plat_hw_stat(int which) {  // (starts the sampler on first call: only while the stats box is shown)
    static int started;
    if (!started) {
        started = 1;
        pthread_t t;
        if (!pthread_create(&t, NULL, hw_thread, NULL)) pthread_detach(t);
    }
    float v = which >= 0 && which < 8 ? g_hw[which] : NAN;
    uint32_t b;
    memcpy(&b, &v, 4);  // (NaN by its bits: the build's fast-math folds isnan away)
    return (b & 0x7f800000u) == 0x7f800000u ? PLAT_HW_UNKNOWN : v;
}
