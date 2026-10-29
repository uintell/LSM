#define _GNU_SOURCE
#include "metrics.h"
#include <ctype.h>
#include <stddef.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static Metrics metrics;
static Process old_processes[MAX_PROCESSES];
static int old_count;
static int read_text(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (size)
            *out = 0;
        return 0;
    }
    int ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (ok)
        out[strcspn(out, "\r\n")] = 0;
    else if (size)
        *out = 0;
    return ok;
}
static double number(const char *path) {
    char s[128], *end;
    if (!read_text(path, s, sizeof s))
        return -1;
    errno = 0;
    double n = strtod(s, &end);
    return end == s || errno || !isfinite(n) ? -1 : n;
}
static int join_path(char *out, size_t size, const char *root, const char *name) {
    size_t a = strlen(root), b = strlen(name);
    if (a + b + 2 > size) {
        errno = ENAMETOOLONG;
        return 0;
    }
    memcpy(out, root, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return 1;
}
static double field(const char *root, const char *name) {
    char path[PATH_MAX];
    return join_path(path, sizeof path, root, name) ? number(path) : -1;
}
static void field_text(const char *root, const char *name, char *out, size_t size) {
    char path[PATH_MAX];
    if (join_path(path, sizeof path, root, name))
        read_text(path, out, size);
    else if (size)
        *out = 0;
}
static int numeric(const char *s) {
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}
static int parse_stat(char *line, Process *p) {
    char *begin = strchr(line, '('), *end = strrchr(line, ')');
    if (!begin || !end || end <= begin || end[1] != ' ')
        return 0;
    size_t n = (size_t)(end - begin - 1);
    if (n >= sizeof p->name)
        n = sizeof p->name - 1;
    memcpy(p->name, begin + 1, n);
    p->name[n] = 0;
    char *save, *tok = strtok_r(end + 2, " ", &save);
    int field_number = 3;
    while (tok) {
        switch (field_number) {
        case 3:
            p->state = *tok;
            break;
        case 4:
            p->ppid = atoi(tok);
            break;
        case 14:
        case 15:
            p->ticks += strtoull(tok, NULL, 10);
            break;
        case 18:
            p->priority = atoi(tok);
            break;
        case 19:
            p->nice = atoi(tok);
            break;
        case 20:
            p->threads = atoi(tok);
            break;
        case 22:
            p->start = strtoull(tok, NULL, 10);
            break;
        case 24: {
            long long rss = strtoll(tok, NULL, 10);
            p->rss_kib = rss > 0 ? (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE) / 1024 : 0;
            break;
        }
        }
        tok = strtok_r(NULL, " ", &save);
        field_number++;
    }
    return field_number > 24;
}
int process_read(int pid, Process *p) {
    char path[128], line[4096];
    memset(p, 0, sizeof *p);
    p->read_rate = p->write_rate = -1;
    p->pid = pid;
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!read_text(path, line, sizeof line) || !parse_stat(line, p))
        return 0;
    snprintf(path, sizeof path, "/proc/%d", pid);
    struct stat st;
    if (stat(path, &st))
        return 0;
    p->uid = st.st_uid;
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(p->command, 1, sizeof p->command - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p->command[i] < 32)
                p->command[i] = ' ';
        p->command[n] = 0;
    }
    if (!p->command[0])
        snprintf(p->command, sizeof p->command, "[%s]", p->name);
    /* A second stat read makes sure cmdline and ownership belong to this identity. */
    Process after = {0};
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!read_text(path, line, sizeof line) || !parse_stat(line, &after) || after.start != p->start)
        return 0;
    for (char *s = p->name; *s; s++)
        if ((unsigned char)*s < 32 || (unsigned char)*s > 126)
            *s = '?';
    snprintf(path, sizeof path, "/proc/%d/io", pid);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            unsigned long long n;
            if (sscanf(line, "read_bytes: %llu", &n) == 1) {
                p->read_bytes = n;
                p->io_available |= 1;
            }
            if (sscanf(line, "write_bytes: %llu", &n) == 1) {
                p->write_bytes = n;
                p->io_available |= 2;
            }
        }
        fclose(f);
    }
    return 1;
}
int process_handle(const Process *p) {
    if (p->pid <= 1 || p->pid == getpid()) {
        errno = EPERM;
        return -1;
    }
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, p->pid, 0);
    if (fd < 0)
        return -1;
    Process live;
    if (!process_read(p->pid, &live) || live.start != p->start) {
        close(fd);
        errno = ESRCH;
        return -1;
    }
    return fd;
#else
    (void)p;
    errno = ENOSYS;
    return -1;
#endif
}
int process_signal(int handle, int sig) {
    if (handle < 0 || (sig != SIGTERM && sig != SIGKILL)) {
        errno = EINVAL;
        return -1;
    }
#ifdef SYS_pidfd_send_signal
    return (int)syscall(SYS_pidfd_send_signal, handle, sig, NULL, 0);
#else
    errno = ENOSYS;
    return -1;
#endif
}
static int process_compare(const void *a, const void *b) {
    const Process *p = a, *q = b;
    return (p->pid > q->pid) - (p->pid < q->pid);
}
static const Process *find_old(int pid) {
    Process key = {.pid = pid};
    return bsearch(&key, old_processes, (size_t)old_count, sizeof key, process_compare);
}

/* NVML is optional: the executable has no NVIDIA link-time dependency. */
typedef void *NvDevice;
typedef struct {
    unsigned gpu, memory;
} NvUtil;
typedef struct {
    unsigned long long total, free, used;
} NvMemory;
static void *nv_library;
static int (*nv_shutdown)(void);
static int (*nv_count)(unsigned *);
static int (*nv_handle)(unsigned, NvDevice *);
static int (*nv_name)(NvDevice, char *, unsigned);
static int (*nv_util)(NvDevice, NvUtil *);
static int (*nv_memory)(NvDevice, NvMemory *);
static int (*nv_temperature)(NvDevice, unsigned, unsigned *);
static int (*nv_clock)(NvDevice, unsigned, unsigned *);
static int (*nv_power)(NvDevice, unsigned *);
static int (*nv_limit)(NvDevice, unsigned *);
static int (*nv_pci_bus)(const char *, NvDevice *);
static void nv_init(void) {
    nv_library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!nv_library)
        return;
    int (*init)(void);
    *(void **)(&init) = dlsym(nv_library, "nvmlInit_v2");
    if (!init || init()) {
        dlclose(nv_library);
        nv_library = NULL;
        return;
    }
#define NV_LOAD(var, symbol) *(void **)(&(var)) = dlsym(nv_library, symbol)
    NV_LOAD(nv_shutdown, "nvmlShutdown");
    NV_LOAD(nv_count, "nvmlDeviceGetCount_v2");
    NV_LOAD(nv_handle, "nvmlDeviceGetHandleByIndex_v2");
    NV_LOAD(nv_name, "nvmlDeviceGetName");
    NV_LOAD(nv_util, "nvmlDeviceGetUtilizationRates");
    NV_LOAD(nv_memory, "nvmlDeviceGetMemoryInfo");
    NV_LOAD(nv_temperature, "nvmlDeviceGetTemperature");
    NV_LOAD(nv_clock, "nvmlDeviceGetClockInfo");
    NV_LOAD(nv_power, "nvmlDeviceGetPowerUsage");
    NV_LOAD(nv_limit, "nvmlDeviceGetPowerManagementLimit");
    NV_LOAD(nv_pci_bus, "nvmlDeviceGetHandleByPciBusId_v2");
#undef NV_LOAD
}
static void pci_name(Gpu *g) {
    FILE *f = fopen("/usr/share/hwdata/pci.ids", "r");
    if (!f)
        f = fopen("/usr/share/misc/pci.ids", "r");
    if (!f)
        return;
    char s[512];
    unsigned vendor = 0, id;
    while (fgets(s, sizeof s, f)) {
        if (s[0] != '\t' && sscanf(s, "%x", &id) == 1)
            vendor = id;
        else if (vendor == g->vendor_id && s[0] == '\t' && s[1] != '\t' &&
                 sscanf(s + 1, "%x", &id) == 1 && id == g->device_id) {
            char *p = s + 1;
            while (isxdigit((unsigned char)*p))
                p++;
            while (isspace((unsigned char)*p))
                p++;
            p[strcspn(p, "\r\n")] = 0;
            snprintf(g->name, sizeof g->name, "%.159s", p);
            break;
        }
    }
    fclose(f);
}
static void discover_gpus(void) {
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *e;
    if (dir) {
        while ((e = readdir(dir)) && metrics.gpu_count < MAX_GPUS) {
            if (strncmp(e->d_name, "card", 4) || !numeric(e->d_name + 4))
                continue;
            char root[PATH_MAX], path[PATH_MAX], s[PATH_MAX];
            snprintf(root, sizeof root, "/sys/class/drm/%s/device", e->d_name);
            double vendor = field(root, "vendor");
            /* vendor is hexadecimal; strtod does accept a 0x prefix. */
            if (vendor < 0)
                continue;
            Gpu *g = &metrics.gpus[metrics.gpu_count++];
            g->nv_index = -1;
            snprintf(g->card, sizeof g->card, "%.31s", e->d_name);
            g->vendor_id = (unsigned)vendor;
            g->device_id = (unsigned)field(root, "device");
            snprintf(g->vendor, sizeof g->vendor, "%s",
                     g->vendor_id == 0x8086   ? "Intel"
                     : g->vendor_id == 0x1002 ? "AMD"
                     : g->vendor_id == 0x10de ? "NVIDIA"
                                              : "Unknown vendor");
            snprintf(g->name, sizeof g->name, "GPU %04x:%04x", g->vendor_id, g->device_id);
            pci_name(g);
            snprintf(path, sizeof path, "/sys/class/drm/%s/device/driver", g->card);
            ssize_t n = readlink(path, s, sizeof s - 1);
            if (n >= 0) {
                s[n] = 0;
                char *p = strrchr(s, '/');
                snprintf(g->driver, sizeof g->driver, "%.63s", p ? p + 1 : s);
            }
            snprintf(path, sizeof path, "/sys/class/drm/%s/device", g->card);
            n = readlink(path, s, sizeof s - 1);
            if (n >= 0) {
                s[n] = 0;
                char *p = strrchr(s, '/');
                snprintf(g->pci, sizeof g->pci, "%.31s", p ? p + 1 : s);
            }
            field_text(root, "current_link_speed", g->link, sizeof g->link);
            field_text(root, "max_link_speed", g->max_link, sizeof g->max_link);
        }
        closedir(dir);
    }
    for (int i = 0; i < metrics.gpu_count; i++)
        for (int j = 0; j < HISTORY; j++)
            metrics.gpus[i].history[j] = -1;
    nv_init();
    if (nv_library && nv_count && nv_handle && nv_name) {
        unsigned count = 0;
        if (nv_count(&count))
            return;
        for (unsigned i = 0; i < count && metrics.gpu_count < MAX_GPUS; i++) {
            NvDevice dev;
            if (nv_handle(i, &dev))
                continue;
            Gpu *g = NULL;
            for (int j = 0; j < metrics.gpu_count; j++)
                if (metrics.gpus[j].vendor_id == 0x10de && nv_pci_bus) {
                    NvDevice other;
                    if (!nv_pci_bus(metrics.gpus[j].pci, &other) && other == dev) {
                        g = &metrics.gpus[j];
                        break;
                    }
                }
            if (!g) {
                g = &metrics.gpus[metrics.gpu_count++];
                g->vendor_id = 0x10de;
                snprintf(g->vendor, sizeof g->vendor, "NVIDIA");
                snprintf(g->driver, sizeof g->driver, "NVML");
            }
            g->nv_index = (int)i;
            nv_name(dev, g->name, sizeof g->name);
        }
    }
    for (int i = 0; i < metrics.gpu_count; i++)
        for (int j = 0; j < HISTORY; j++)
            metrics.gpus[i].history[j] = -1;
}
static void cpu_hardware(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *p = strchr(line, ':');
            if (!p)
                continue;
            p++;
            while (*p == ' ' || *p == '\t')
                p++;
            p[strcspn(p, "\n")] = 0;
            if (!strncmp(line, "model name", 10))
                snprintf(metrics.cpu_name, sizeof metrics.cpu_name, "%.159s", p);
            if (!strncmp(line, "vendor_id", 9))
                snprintf(metrics.cpu_vendor, sizeof metrics.cpu_vendor, "%.63s", p);
            if (!strncmp(line, "cache size", 10))
                snprintf(metrics.cache, sizeof metrics.cache, "%.63s", p);
            if (!strncmp(line, "microcode", 9))
                snprintf(metrics.microcode, sizeof metrics.microcode, "%.31s", p);
            if (!strncmp(line, "power management", 16))
                break;
        }
        fclose(f);
    }
    metrics.package_temperature = -1;
    for (int i = 0; i < MAX_CPUS; i++) {
        CpuInfo *c = &metrics.cpus[i];
        c->id = i;
        c->temperature = -1;
        c->mhz = c->min_mhz = c->max_mhz = -1;
        c->package = c->core = c->type = c->die = -1;
        c->policy_min_mhz = c->policy_max_mhz = c->base_mhz = -1;
    }
}
static void temperatures(void) {
    metrics.package_temperature = -1;
    for (int c = 0; c < metrics.cpu_limit; c++)
        metrics.cpus[c].temperature = -1;
    DIR *dir = opendir("/sys/class/hwmon");
    struct dirent *e;
    if (!dir)
        return;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.')
            continue;
        char root[PATH_MAX], name[80];
        snprintf(root, sizeof root, "/sys/class/hwmon/%s", e->d_name);
        field_text(root, "name", name, sizeof name);
        if (strcmp(name, "coretemp") && strcmp(name, "k10temp") && strcmp(name, "zenpower"))
            continue;
        char package_label[80];
        int sensor_package = -1;
        field_text(root, "temp1_label", package_label, sizeof package_label);
        if (sscanf(package_label, "Package id %d", &sensor_package) != 1)
            sensor_package = -1;
        for (int t = 1; t <= 128; t++) {
            char attr[64], label[80];
            snprintf(attr, sizeof attr, "temp%d_input", t);
            double v = field(root, attr);
            if (v < 0)
                continue;
            snprintf(attr, sizeof attr, "temp%d_label", t);
            field_text(root, attr, label, sizeof label);
            int core;
            if (!strncmp(label, "Package", 7) || !strcmp(label, "Tctl") || !strcmp(label, "Tdie")) {
                if (v / 1000 > metrics.package_temperature)
                    metrics.package_temperature = v / 1000;
            }
            if (sscanf(label, "Core %d", &core) == 1)
                for (int c = 0; c < metrics.cpu_limit; c++)
                    if (metrics.cpus[c].core == core &&
                        (metrics.packages == 1 || metrics.cpus[c].package == sensor_package))
                        metrics.cpus[c].temperature = v / 1000;
        }
    }
    closedir(dir);
}
static void cpu_metadata(CpuInfo *c, const char *root) {
    c->die = (int)field(root, "topology/die_id");
    field_text(root, "topology/thread_siblings_list", c->siblings, sizeof c->siblings);
    c->base_mhz = field(root, "cpufreq/base_frequency");
    if (c->base_mhz >= 0)
        c->base_mhz /= 1000;
    c->l1_data[0] = c->l1_instruction[0] = c->l2[0] = c->l3[0] = 0;
    for (int index = 0; index < 32; index++) {
        char cache_root[256], type[48], size[24];
        snprintf(cache_root, sizeof cache_root, "%s/cache/index%d", root, index);
        int level = (int)field(cache_root, "level");
        if (level < 0)
            continue;
        field_text(cache_root, "type", type, sizeof type);
        field_text(cache_root, "size", size, sizeof size);
        char *target = level == 1 && !strcmp(type, "Data")          ? c->l1_data
                       : level == 1 && !strcmp(type, "Instruction") ? c->l1_instruction
                       : level == 2                                 ? c->l2
                       : level == 3                                 ? c->l3
                                                                    : NULL;
        if (target)
            snprintf(target, sizeof c->l2, "%s", size);
    }
}
static void sample_cpus(int baseline) {
    FILE *f = fopen("/proc/stat", "r");
    char line[1024];
    metrics.cpu_count = metrics.cpu_limit = 0;
    unsigned char was_online[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++) {
        was_online[i] = (unsigned char)metrics.cpus[i].online;
        metrics.cpus[i].online = 0;
    }
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        int id;
        unsigned long long v[10] = {0};
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &id, &v[0],
                   &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) < 5 ||
            id < 0 || id >= MAX_CPUS)
            continue;
        CpuInfo *c = &metrics.cpus[id];
        uint64_t total = 0;
        for (int k = 0; k < 8; k++)
            total += v[k];
        uint64_t idle = v[3] + v[4], user = v[0] + v[1], system = v[2] + v[5] + v[6],
                 delta = total >= c->total ? total - c->total : 0;
        c->busy = c->user = c->system = c->iowait = 0;
        if (baseline && c->total && delta && idle >= c->idle && idle - c->idle <= delta) {
            c->busy = 100. * (delta - (idle - c->idle)) / delta;
            if (user >= c->user_ticks)
                c->user = 100. * (user - c->user_ticks) / delta;
            if (system >= c->system_ticks)
                c->system = 100. * (system - c->system_ticks) / delta;
            if (v[4] >= c->wait_ticks)
                c->iowait = 100. * (v[4] - c->wait_ticks) / delta;
        }
        c->total = total;
        c->idle = idle;
        c->user_ticks = user;
        c->system_ticks = system;
        c->wait_ticks = v[4];
        c->online = 1;
        metrics.cpu_count++;
        if (id + 1 > metrics.cpu_limit)
            metrics.cpu_limit = id + 1;
        char root[160];
        snprintf(root, sizeof root, "/sys/devices/system/cpu/cpu%d", id);
        if (!was_online[id])
            cpu_metadata(c, root);
        c->package = (int)field(root, "topology/physical_package_id");
        c->core = (int)field(root, "topology/core_id");
        c->type = (int)field(root, "topology/core_type");
        c->mhz = field(root, "cpufreq/scaling_cur_freq");
        if (c->mhz >= 0)
            c->mhz /= 1000;
        c->min_mhz = field(root, "cpufreq/cpuinfo_min_freq");
        if (c->min_mhz >= 0)
            c->min_mhz /= 1000;
        c->max_mhz = field(root, "cpufreq/cpuinfo_max_freq");
        if (c->max_mhz >= 0)
            c->max_mhz /= 1000;
        c->policy_min_mhz = field(root, "cpufreq/scaling_min_freq");
        c->policy_max_mhz = field(root, "cpufreq/scaling_max_freq");
        if (c->policy_min_mhz >= 0)
            c->policy_min_mhz /= 1000;
        if (c->policy_max_mhz >= 0)
            c->policy_max_mhz /= 1000;
        field_text(root, "cpufreq/scaling_governor", c->governor, sizeof c->governor);
        field_text(root, "cpufreq/scaling_driver", c->frequency_driver, sizeof c->frequency_driver);
        if (metrics.cpu_count == 1)
            field_text(root, "cpufreq/scaling_governor", metrics.governor, sizeof metrics.governor);
    }
    fclose(f);
    metrics.packages = metrics.physical_cores = 0;
    for (int i = 0; i < metrics.cpu_limit; i++)
        if (metrics.cpus[i].online) {
            int package_seen = 0, core_seen = 0;
            for (int j = 0; j < i; j++)
                if (metrics.cpus[j].online) {
                    if (metrics.cpus[i].package == metrics.cpus[j].package) {
                        package_seen = 1;
                        if (metrics.cpus[i].core == metrics.cpus[j].core)
                            core_seen = 1;
                    }
                }
            if (!package_seen && metrics.cpus[i].package >= 0)
                metrics.packages++;
            if (!core_seen && metrics.cpus[i].core >= 0)
                metrics.physical_cores++;
        }
    temperatures();
}

/* One record per (GPU, DRM client), deduplicating shared and duplicated FDs. */
#define CLIENTS 8192
typedef struct {
    int gpu, seen;
    uint64_t id, ticks[MAX_ENGINES];
    unsigned capacity[MAX_ENGINES];
} Client;
static Client clients[CLIENTS];
static int client_count, generation;
static int engine_index(Gpu *g, const char *name) {
    for (int i = 0; i < g->engine_count; i++)
        if (!strcmp(g->engines[i].name, name))
            return i;
    if (g->engine_count >= MAX_ENGINES)
        return -1;
    int i = g->engine_count++;
    snprintf(g->engines[i].name, sizeof g->engines[i].name, "%.47s", name);
    g->engines[i].capacity = 1;
    g->engines[i].busy = -1;
    for (int j = 0; j < HISTORY; j++)
        g->engines[i].history[j] = -1;
    return i;
}
static void gpu_fdinfo(FILE *f, double dt, int baseline) {
    char line[512], pci[64] = "", key[128], unit[32];
    unsigned long long id = 0, n;
    int have_id = 0, gpu = -1;
    long pos = ftell(f);
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "drm-client-id: %llu", &id) == 1)
            have_id = 1;
        if (sscanf(line, "drm-pdev: %63s", pci) == 1)
            for (int i = 0; i < metrics.gpu_count; i++)
                if (!strcmp(metrics.gpus[i].pci, pci))
                    gpu = i;
    }
    if (!have_id || gpu < 0)
        return;
    int ci;
    for (ci = 0; ci < client_count; ci++)
        if (clients[ci].gpu == gpu && clients[ci].id == id)
            break;
    int fresh = ci == client_count;
    if (fresh) {
        if (client_count == CLIENTS)
            return;
        clients[client_count++] = (Client){.gpu = gpu, .id = id};
    }
    Client *c = &clients[ci];
    if (c->seen == generation)
        return;
    int consecutive = c->seen == generation - 1 && !fresh;
    c->seen = generation;
    Gpu *g = &metrics.gpus[gpu];
    g->clients++;
    fseek(f, pos, SEEK_SET);
    uint64_t ticks[MAX_ENGINES] = {0};
    unsigned capacities[MAX_ENGINES];
    int present[MAX_ENGINES] = {0};
    for (int i = 0; i < MAX_ENGINES; i++)
        capacities[i] = 1;
    while (fgets(line, sizeof line, f)) {
        unit[0] = 0;
        if (sscanf(line, "%127[^:]: %llu %31s", key, &n, unit) < 2)
            continue;
        if (!strncmp(key, "drm-engine-capacity-", 20)) {
            int i = engine_index(g, key + 20);
            if (i >= 0)
                capacities[i] = n > 0 ? (unsigned)n : 1;
        } else if (!strncmp(key, "drm-engine-", 11)) {
            int i = engine_index(g, key + 11);
            if (i >= 0) {
                ticks[i] = n;
                present[i] = 1;
            }
        } else if (!strncmp(key, "drm-resident-", 13)) {
            double scale = !strcmp(unit, "KiB") ? 1024 : !strcmp(unit, "MiB") ? 1048576 : 1;
            g->client_memory += (double)n * scale;
            g->memory_available = 1;
        }
    }
    for (int i = 0; i < g->engine_count; i++)
        if (present[i]) {
            if (g->engines[i].busy < 0)
                g->engines[i].busy = 0;
            g->engines[i].capacity = capacities[i];
            if (baseline && consecutive && ticks[i] >= c->ticks[i])
                g->engines[i].busy += 100. * (ticks[i] - c->ticks[i]) / (dt * 1e9 * capacities[i]);
            if (!consecutive || ticks[i] > c->ticks[i])
                c->ticks[i] = ticks[i];
        }
}
static void gpu_process(int pid, double dt, int baseline) {
    char root[128], path[512], link[512];
    snprintf(root, sizeof root, "/proc/%d/fd", pid);
    DIR *dir = opendir(root);
    if (!dir) {
        if (errno == EACCES || errno == EPERM)
            for (int i = 0; i < metrics.gpu_count; i++)
                metrics.gpus[i].restricted = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!numeric(e->d_name))
            continue;
        snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n < 0)
            continue;
        link[n] = 0;
        if (strncmp(link, "/dev/dri/", 9))
            continue;
        snprintf(path, sizeof path, "/proc/%d/fdinfo/%s", pid, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        gpu_fdinfo(f, dt, baseline);
        fclose(f);
    }
    closedir(dir);
}
static void gpu_sensors(Gpu *g) {
    char root[PATH_MAX], card[128];
    snprintf(root, sizeof root, "/sys/class/drm/%s/device", g->card);
    snprintf(card, sizeof card, "/sys/class/drm/%s", g->card);
    g->clock = field(card, "gt_act_freq_mhz");
    if (g->clock < 0)
        g->clock = field(card, "gt_cur_freq_mhz");
    g->min_clock = field(card, "gt_RPn_freq_mhz");
    g->max_clock = field(card, "gt_RP0_freq_mhz");
    if (g->clock < 0)
        g->clock = field(root, "tile0/gt0/freq0/act_freq");
    if (g->min_clock < 0)
        g->min_clock = field(root, "tile0/gt0/freq0/rpn_freq");
    if (g->max_clock < 0)
        g->max_clock = field(root, "tile0/gt0/freq0/rp0_freq");
    g->busy = field(root, "gpu_busy_percent");
    g->vram_total = field(root, "mem_info_vram_total");
    g->vram_used = field(root, "mem_info_vram_used");
    g->gtt_total = field(root, "mem_info_gtt_total");
    g->gtt_used = field(root, "mem_info_gtt_used");
    g->temperature = g->power = g->power_limit = -1;
    char hwmon[PATH_MAX];
    snprintf(hwmon, sizeof hwmon, "/sys/class/drm/%s/device/hwmon", g->card);
    DIR *dir = opendir(hwmon);
    struct dirent *e;
    if (dir) {
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.')
                continue;
            char h[PATH_MAX];
            snprintf(h, sizeof h, "/sys/class/drm/%s/device/hwmon/%s", g->card, e->d_name);
            double v = field(h, "temp1_input");
            if (v >= 0)
                g->temperature = v / 1000;
            v = field(h, "power1_average");
            if (v < 0)
                v = field(h, "power1_input");
            if (v >= 0)
                g->power = v / 1e6;
            v = field(h, "power1_cap");
            if (v >= 0)
                g->power_limit = v / 1e6;
            if (g->clock < 0) {
                v = field(h, "freq1_input");
                if (v >= 0)
                    g->clock = v / 1e6;
            }
        }
        closedir(dir);
    }
    if (g->nv_index >= 0 && nv_handle) {
        NvDevice dev;
        if (nv_handle((unsigned)g->nv_index, &dev))
            return;
        NvUtil util;
        NvMemory mem;
        unsigned n;
        if (nv_util && !nv_util(dev, &util))
            g->busy = util.gpu;
        if (nv_memory && !nv_memory(dev, &mem)) {
            g->vram_total = (double)mem.total;
            g->vram_used = (double)mem.used;
        }
        if (nv_temperature && !nv_temperature(dev, 0, &n))
            g->temperature = n;
        if (nv_clock && !nv_clock(dev, 0, &n))
            g->clock = n;
        if (nv_power && !nv_power(dev, &n))
            g->power = n / 1000.;
        if (nv_limit && !nv_limit(dev, &n))
            g->power_limit = n / 1000.;
    }
}
int metrics_init(void) {
    struct utsname u;
    if (!uname(&u)) {
        snprintf(metrics.kernel, sizeof metrics.kernel, "%.60s %.90s", u.sysname, u.release);
        snprintf(metrics.architecture, sizeof metrics.architecture, "%.31s", u.machine);
    }
    gethostname(metrics.hostname, sizeof metrics.hostname - 1);
    cpu_hardware();
    discover_gpus();
    return 1;
}
int metrics_update(double dt, int baseline, int slot) {
    if (dt <= 0)
        dt = 1;
    sample_cpus(baseline);
    generation++;
    for (int i = 0; i < metrics.gpu_count; i++) {
        Gpu *g = &metrics.gpus[i];
        g->clients = 0;
        g->client_memory = 0;
        g->memory_available = 0;
        g->restricted = 0;
        for (int j = 0; j < g->engine_count; j++)
            g->engines[j].busy = -1;
        gpu_sensors(g);
    }
    old_count = metrics.process_count;
    memcpy(old_processes, metrics.processes, (size_t)old_count * sizeof(Process));
    /* metrics.processes stays in PID order; UI sorts its own index array. */
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    metrics.process_count = 0;
    if (dir) {
        while ((entry = readdir(dir))) {
            if (!numeric(entry->d_name))
                continue;
            int pid = atoi(entry->d_name);
            if (metrics.process_count >= MAX_PROCESSES)
                break;
            Process p;
            if (!process_read(pid, &p))
                continue;
            const Process *old = find_old(pid);
            if (baseline && old && old->start == p.start) {
                if (p.ticks >= old->ticks)
                    p.cpu = 100. * (p.ticks - old->ticks) / sysconf(_SC_CLK_TCK) / dt;
                if ((p.io_available & old->io_available & 1) && p.read_bytes >= old->read_bytes)
                    p.read_rate = (p.read_bytes - old->read_bytes) / dt;
                if ((p.io_available & old->io_available & 2) && p.write_bytes >= old->write_bytes)
                    p.write_rate = (p.write_bytes - old->write_bytes) / dt;
            }
            metrics.processes[metrics.process_count++] = p;
            gpu_process(pid, dt, baseline);
        }
        closedir(dir);
    }
    qsort(metrics.processes, (size_t)metrics.process_count, sizeof(Process), process_compare);
    /* Drop vanished clients without converting disappearance into negative activity. */
    for (int i = 0; i < client_count;) {
        if (clients[i].seen != generation)
            clients[i] = clients[--client_count];
        else
            i++;
    }
    for (int i = 0; i < metrics.gpu_count; i++) {
        Gpu *g = &metrics.gpus[i];
        double peak = -1;
        for (int j = 0; j < g->engine_count; j++) {
            Engine *e = &g->engines[j];
            if (e->busy > 100)
                e->busy = 100;
            if (e->busy > peak)
                peak = e->busy;
            e->history[slot] = (float)e->busy;
        }
        if (g->busy < 0)
            g->busy = peak;
        g->history[slot] = (float)g->busy;
    }
    return metrics.cpu_count > 0;
}
void metrics_close(void) {
    if (nv_library) {
        if (nv_shutdown)
            nv_shutdown();
        dlclose(nv_library);
        nv_library = NULL;
    }
}

static FILE *drm_fixture(unsigned long long ticks) {
    FILE *f = tmpfile();
    if (!f)
        return NULL;
    fprintf(f,
            "drm-driver: i915\ndrm-client-id: 42\ndrm-pdev: 0000:01:00.0\ndrm-resident-system0: 64 "
            "KiB\ndrm-engine-capacity-render: 2\ndrm-engine-render: %llu ns\n",
            ticks);
    rewind(f);
    return f;
}
static int test_drm(void) {
    Gpu saved = metrics.gpus[0];
    int saved_count = metrics.gpu_count;
    metrics.gpu_count = 1;
    metrics.gpus[0] = (Gpu){.pci = "0000:01:00.0"};
    client_count = 0;
    generation = 1;
    Gpu *g = &metrics.gpus[0];
    FILE *f = drm_fixture(100);
    if (!f)
        return 0;
    gpu_fdinfo(f, 1, 0);
    rewind(f);
    gpu_fdinfo(f, 1, 0);
    fclose(f);
    int ok = g->clients == 1 && g->engine_count == 1 && g->client_memory == 65536 &&
             g->engines[0].busy == 0;
    generation++;
    g->clients = 0;
    g->client_memory = 0;
    g->engines[0].busy = -1;
    f = drm_fixture(1000000100ULL);
    if (!f)
        return 0;
    gpu_fdinfo(f, 1, 1);
    fclose(f);
    ok = ok && fabs(g->engines[0].busy - 50) < .00001 && g->engines[0].capacity == 2;
    generation++;
    g->engines[0].busy = -1;
    f = drm_fixture(100);
    if (!f)
        return 0;
    gpu_fdinfo(f, 1, 1);
    fclose(f);
    ok = ok && g->engines[0].busy == 0 && clients[0].ticks[0] == 1000000100ULL;
    generation++;
    g->engines[0].busy = -1;
    f = drm_fixture(2000000100ULL);
    if (!f)
        return 0;
    gpu_fdinfo(f, 1, 1);
    fclose(f);
    ok = ok && fabs(g->engines[0].busy - 50) < .00001;
    metrics.gpus[0] = saved;
    metrics.gpu_count = saved_count;
    printf("%s: DRM client deduplication, memory units, engine capacity, counter regression and "
           "recovery\n",
           ok ? "PASS" : "FAIL");
    return ok;
}
int metrics_tests(void) {
    char fixture[] =
        "42 (a tricky ) name) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23";
    Process parsed = {0};
    if (!parse_stat(fixture, &parsed) || strcmp(parsed.name, "a tricky ) name") ||
        parsed.ticks != 23 || parsed.start != 19) {
        fprintf(stderr, "FAIL: proc stat parser\n");
        return 0;
    }
    if (!test_drm())
        return 0;
    int signals[] = {SIGTERM, SIGKILL};
    for (int i = 0; i < 2; i++) {
        pid_t child = fork();
        if (child < 0)
            return 0;
        if (child == 0) {
            signal(SIGTERM, SIG_DFL);
            for (;;)
                pause();
        }
        Process p;
        int fd = -1;
        if (process_read(child, &p))
            fd = process_handle(&p);
        if (fd < 0) {
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            perror("FAIL: child pidfd");
            return 0;
        }
        Process wrong = p;
        wrong.start++;
        int rejected = process_handle(&wrong);
        if (rejected >= 0) {
            close(rejected);
            close(fd);
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            return 0;
        }
        if (process_signal(fd, signals[i])) {
            close(fd);
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            return 0;
        }
        int status;
        waitpid(child, &status, 0);
        errno = 0;
        int stale = process_signal(fd, signals[i]);
        int stale_errno = errno;
        close(fd);
        if (stale != -1 || stale_errno != ESRCH)
            return 0;
        if (!WIFSIGNALED(status) || WTERMSIG(status) != signals[i])
            return 0;
    }
    Process self = {.pid = getpid()}, init = {.pid = 1};
    if (process_handle(&self) >= 0 || process_handle(&init) >= 0)
        return 0;
    puts("PASS: process stat parsing, stale identity rejection, protected PIDs, SIGTERM and "
         "SIGKILL on test children");
    return 1;
}

void metrics_copy(Metrics *destination) {
    /* Copy live entries only; unused process slots need no physical pages. */
    memcpy(destination, &metrics, offsetof(Metrics, processes));
    memcpy(destination->processes, metrics.processes,
           (size_t)metrics.process_count * sizeof(Process));
    memcpy(destination->gpus, metrics.gpus, (size_t)metrics.gpu_count * sizeof(Gpu));
}
