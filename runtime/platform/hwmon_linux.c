// The stats box's hardware figures on Linux (the Windows counterpart: hwmon_win.c), sampled once a second on a
// background thread started by the first plat_hw_stat call:
// - NVIDIA: libnvidia-ml.so.1 (the driver's): GPU load, GPU temperature, memory temperature
// - AMD (amdgpu): /sys/class/drm/card*/device - gpu_busy_percent, hwmon temp1 (edge), temp2 (junction: the hotspot),
//   temp3 (memory)
// - CPU: /proc/stat (load), hwmon k10temp / zenpower / coretemp (temperature); memory in use: /proc/meminfo
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../platform.h"

static volatile float g_hw[8] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};

void plat_hw_set_gpu_luid(const uint8_t luid[8]) { (void)luid; }

static float read_num(const char *path, float div) {
    FILE *f = fopen(path, "r");
    if (!f) return NAN;
    double v;
    int ok = fscanf(f, "%lf", &v) == 1;
    fclose(f);
    return ok ? (float)(v / div) : NAN;
}

static void read_name(const char *dir, char *out, size_t n) {
    char p[512];
    snprintf(p, sizeof p, "%s/name", dir);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

// NVML, the few calls used (the documented signatures)
typedef struct { unsigned gpu, memory; } NvmlUtil;
typedef struct { unsigned fieldId, scopeId; long long timestamp, latencyUsec; int valueType, nvmlReturn; union { double d; unsigned ui; unsigned long ul; unsigned long long ull; long long sll; } value; } NvmlField;
static int (*nv_temp)(void *, int, unsigned *), (*nv_util)(void *, NvmlUtil *), (*nv_fields)(void *, int, NvmlField *);
static void *nv_dev;

static void nvml_open(void) {
    void *m = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m) return;
    int (*init)(void) = (int (*)(void))dlsym(m, "nvmlInit_v2");
    int (*handle)(unsigned, void **) = (int (*)(unsigned, void **))dlsym(m, "nvmlDeviceGetHandleByIndex_v2");
    nv_temp = (int (*)(void *, int, unsigned *))dlsym(m, "nvmlDeviceGetTemperature");
    nv_util = (int (*)(void *, NvmlUtil *))dlsym(m, "nvmlDeviceGetUtilizationRates");
    nv_fields = (int (*)(void *, int, NvmlField *))dlsym(m, "nvmlDeviceGetFieldValues");
    if (!init || !handle || !nv_temp || init() != 0 || handle(0, &nv_dev) != 0) nv_dev = NULL;
}

// amdgpu: the first card with gpu_busy_percent, and its hwmon folder
static char amd_dev[256], amd_hwmon[512];
static void amd_open(void) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
        char p[512];
        snprintf(p, sizeof p, "/sys/class/drm/%s/device/gpu_busy_percent", e->d_name);
        if (access(p, R_OK)) continue;
        snprintf(amd_dev, sizeof amd_dev, "/sys/class/drm/%s/device", e->d_name);
        snprintf(p, sizeof p, "%s/hwmon", amd_dev);
        DIR *h = opendir(p);
        if (h) {
            struct dirent *he;
            while ((he = readdir(h)))
                if (!strncmp(he->d_name, "hwmon", 5)) { snprintf(amd_hwmon, sizeof amd_hwmon, "%s/%s", p, he->d_name); break; }
            closedir(h);
        }
        break;
    }
    closedir(d);
}

// CPU temperature: the hwmon of the CPU's sensor driver, its first input (k10temp Tctl, coretemp Package)
static char cpu_temp_path[512];
static void cpu_temp_open(void) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[300], name[64];
        snprintf(dir, sizeof dir, "/sys/class/hwmon/%s", e->d_name);
        read_name(dir, name, sizeof name);
        if (!strcmp(name, "k10temp") || !strcmp(name, "zenpower") || !strcmp(name, "coretemp")) {
            snprintf(cpu_temp_path, sizeof cpu_temp_path, "%s/temp1_input", dir);
            break;
        }
    }
    closedir(d);
}

static float cpu_load(void) {
    static unsigned long long pidle, ptotal;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return NAN;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return NAN;
    unsigned long long idle = v[3] + v[4], total = 0;
    for (int i = 0; i < 8; i++) total += v[i];
    float r = NAN;
    if (ptotal && total > ptotal) r = 100.0f * (1.0f - (float)(idle - pidle) / (float)(total - ptotal));
    pidle = idle, ptotal = total;
    return r;
}

static float mem_used_gb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return NAN;
    char line[128];
    unsigned long long total = 0, avail = 0, v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %llu", &v) == 1) total = v;
        else if (sscanf(line, "MemAvailable: %llu", &v) == 1) avail = v;
    }
    fclose(f);
    return total ? (float)((total - avail) / 1048576.0) : NAN;
}

static void *hw_thread(void *arg) {
    (void)arg;
    nvml_open();
    if (!nv_dev) amd_open();
    cpu_temp_open();
    for (;;) {
        float v[8] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
        if (nv_dev) {
            unsigned t;
            NvmlUtil u;
            if (nv_temp(nv_dev, 0, &t) == 0) v[PLAT_HW_GPU_TEMP] = (float)t;
            if (nv_util && nv_util(nv_dev, &u) == 0) v[PLAT_HW_GPU_LOAD] = (float)u.gpu;
            NvmlField f = {.fieldId = 82};  // NVML_FI_DEV_MEMORY_TEMP
            if (nv_fields && nv_fields(nv_dev, 1, &f) == 0 && f.nvmlReturn == 0 && f.value.ui > 0 && f.value.ui < 150)
                v[PLAT_HW_GPU_MEM_TEMP] = (float)f.value.ui;
        } else if (amd_dev[0]) {
            char p[600];
            snprintf(p, sizeof p, "%s/gpu_busy_percent", amd_dev);
            v[PLAT_HW_GPU_LOAD] = read_num(p, 1);
            if (amd_hwmon[0]) {
                snprintf(p, sizeof p, "%s/temp1_input", amd_hwmon), v[PLAT_HW_GPU_TEMP] = read_num(p, 1000);
                snprintf(p, sizeof p, "%s/temp2_input", amd_hwmon), v[PLAT_HW_GPU_HOTSPOT] = read_num(p, 1000);
                snprintf(p, sizeof p, "%s/temp3_input", amd_hwmon), v[PLAT_HW_GPU_MEM_TEMP] = read_num(p, 1000);
            }
        }
        v[PLAT_HW_CPU_LOAD] = cpu_load();
        if (cpu_temp_path[0]) v[PLAT_HW_CPU_TEMP] = read_num(cpu_temp_path, 1000);
        v[PLAT_HW_SYS_MEM_GB] = mem_used_gb();
        for (int i = 0; i < 8; i++) g_hw[i] = v[i];
        sleep(1);
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
