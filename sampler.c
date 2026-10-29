#define _POSIX_C_SOURCE 200809L
#include "sampler.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef struct {
    unsigned long long total, idle;
} Cpu;
static struct {
    SampleData data;
    Cpu previous[MAX_CPUS + 1];
    unsigned long long oldrx, oldtx, oldread, oldwrite;
    uint64_t pressure_total[3];
    int baseline, pressure_previous[3], valid;
    double last;
    MemoryBytes cached_bytes;
} state;
typedef struct {
    int pid;
    uint64_t start, address, serial;
    size_t length;
} ByteRequest;
static struct {
    int memory_pid;
    uint64_t memory_start;
    ByteRequest peek;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    Snapshot *frames[3];
    int displayed, latest, stop, request, running, force_metrics;
    uint64_t sequence;
} sampler;
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static struct timespec deadline(unsigned ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    return t;
}
static int parse_pressure(const char *text, Pressure *p) {
    double a, b, c;
    unsigned long long total;
    if (sscanf(text, "some avg10=%lf avg60=%lf avg300=%lf total=%llu", &a, &b, &c, &total) == 4) {
        if (!isfinite(a) || !isfinite(b) || !isfinite(c) || a < 0 || a > 100 || b < 0 || b > 100 ||
            c < 0 || c > 100)
            return 0;
        p->avg10 = a;
        p->avg60 = b;
        p->avg300 = c;
        p->total = total;
        p->available = 1;
        return 1;
    }
    if (sscanf(text, "full avg10=%lf avg60=%lf avg300=%lf total=%llu", &a, &b, &c, &total) == 4 &&
        isfinite(a) && a >= 0 && a <= 100) {
        p->full10 = a;
        p->full_available = 1;
        return 1;
    }
    return 0;
}
static void collect_pressure(double dt) {
    const char *names[] = {"cpu", "memory", "io"};
    for (int i = 0; i < 3; i++) {
        Pressure *p = &state.data.pressure[i];
        p->available = p->full_available = 0;
        char path[64], line[256];
        snprintf(path, sizeof path, "/proc/pressure/%s", names[i]);
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof line, f))
                parse_pressure(line, p);
            fclose(f);
        }
        float fraction = -1;
        if (p->available) {
            fraction = 0;
            if (state.baseline && state.pressure_previous[i] && p->total >= state.pressure_total[i])
                fraction =
                    (float)fmin(100, 100. * (p->total - state.pressure_total[i]) / (dt * 1e6));
            state.pressure_total[i] = p->total;
        }
        p->history[state.data.history_head] = fraction;
        state.pressure_previous[i] = p->available;
    }
}
static int collect(void) {
    double now = seconds(), dt = state.last ? now - state.last : 1;
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return 0;
    state.last = now;
    char line[1024];
    state.data.cpu_limit = 0;
    while (fgets(line, sizeof line, f)) {
        char label[32];
        unsigned long long v[10] = {0};
        int n = sscanf(line, "%31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", label, &v[0],
                       &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (n < 5 || strncmp(label, "cpu", 3))
            continue;
        int i = !label[3] ? 0 : atoi(label + 3) + 1;
        if (i > MAX_CPUS)
            continue;
        Cpu c = {0, v[3] + v[4]};
        for (int k = 0; k < 8; k++)
            c.total += v[k];
        unsigned long long total = c.total - state.previous[i].total,
                           idle = c.idle - state.previous[i].idle;
        state.data.cpu_history[state.data.history_head][i] =
            state.baseline && c.total >= state.previous[i].total && total && idle <= total
                ? 100.f * (total - idle) / total
                : 0;
        state.previous[i] = c;
        if (i > state.data.cpu_limit)
            state.data.cpu_limit = i;
    }
    fclose(f);
    unsigned long long total = 0, available = 0, st = 0, sf = 0;
    memset(&state.data.system_memory, 0, sizeof state.data.system_memory);
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            memory_system_line(&state.data.system_memory, line);
            unsigned long long v;
            if (sscanf(line, "MemTotal: %llu", &v) == 1)
                total = v;
            if (sscanf(line, "MemAvailable: %llu", &v) == 1)
                available = v;
            if (sscanf(line, "SwapTotal: %llu", &v) == 1)
                st = v;
            if (sscanf(line, "SwapFree: %llu", &v) == 1)
                sf = v;
        }
        fclose(f);
    }
    state.data.memory_gib = total / 1048576.;
    state.data.memory_percent = total ? 100. * (total - available) / total : 0;
    state.data.swap_percent = st ? 100. * (st - sf) / st : 0;
    unsigned long long rx = 0, tx = 0, rd = 0, wr = 0;
    f = fopen("/proc/net/dev", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *p = strchr(line, ':');
            if (!p)
                continue;
            *p = 0;
            char name[64];
            unsigned long long v[16] = {0};
            sscanf(line, "%63s", name);
            if (!strcmp(name, "lo"))
                continue;
            if (sscanf(p + 1,
                       "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                       "%llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10],
                       &v[11], &v[12], &v[13], &v[14], &v[15]) == 16) {
                rx += v[0];
                tx += v[8];
            }
        }
        fclose(f);
    }
    f = fopen("/proc/diskstats", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            unsigned ma, mi;
            char name[64], path[160];
            unsigned long long a, b, c, d, e, g, h;
            if (sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu", &ma, &mi, name, &a,
                       &b, &c, &d, &e, &g, &h) != 10)
                continue;
            snprintf(path, sizeof path, "/sys/block/%s", name);
            if (access(path, F_OK) || !strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) ||
                !strncmp(name, "dm-", 3) || !strncmp(name, "md", 2))
                continue;
            rd += c * 512;
            wr += h * 512;
        }
        fclose(f);
    }
    state.data.rx_rate = state.baseline && rx >= state.oldrx ? (rx - state.oldrx) / dt : 0;
    state.data.tx_rate = state.baseline && tx >= state.oldtx ? (tx - state.oldtx) / dt : 0;
    state.data.read_rate = state.baseline && rd >= state.oldread ? (rd - state.oldread) / dt : 0;
    state.data.write_rate = state.baseline && wr >= state.oldwrite ? (wr - state.oldwrite) / dt : 0;
    state.oldrx = rx;
    state.oldtx = tx;
    state.oldread = rd;
    state.oldwrite = wr;
    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf %lf %lf", &state.data.load_average[0], &state.data.load_average[1],
                   &state.data.load_average[2]) != 3)
            memset(state.data.load_average, 0, sizeof state.data.load_average);
        fclose(f);
    }
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &state.data.uptime) != 1)
            state.data.uptime = 0;
        fclose(f);
    }
    if (!metrics_update(dt, state.baseline, state.data.history_head))
        return 0;
    state.data.mem_history[state.data.history_head] = (float)state.data.memory_percent;
    state.data.net_history[state.data.history_head] =
        (float)((state.data.rx_rate + state.data.tx_rate) / 1048576);
    state.data.disk_history[state.data.history_head] =
        (float)((state.data.read_rate + state.data.write_rate) / 1048576);
    collect_pressure(dt);
    state.data.timestamps[state.data.history_head] = now;
    if (state.baseline) {
        state.data.history_head = (state.data.history_head + 1) % HISTORY;
        if (state.data.history_count < HISTORY)
            state.data.history_count++;
    }
    state.baseline = 1;
    state.data.sample_number++;
    return total > 0 && state.data.cpu_limit > 0;
}

static void fill_frame(Snapshot *f, int memory_pid, uint64_t memory_start, ByteRequest peek,
                       int force) {
    double begin = seconds();
    if (force || !state.baseline || seconds() - state.last >= 1)
        state.valid = collect();
    f->valid = state.valid;
    metrics_copy(&f->hardware);
    memory_report_read(memory_pid, memory_start, &f->memory);
    if (!memory_pid || peek.pid != memory_pid || peek.start != memory_start) {
        memset(&state.cached_bytes, 0, sizeof state.cached_bytes);
        state.cached_bytes.serial = peek.serial;
    } else if (peek.serial && peek.serial != state.cached_bytes.serial) {
        MemoryBytes *b = &state.cached_bytes;
        memset(b, 0, sizeof *b);
        b->pid = peek.pid;
        b->start = peek.start;
        b->address = peek.address;
        b->serial = peek.serial;
        int n = memory_peek(peek.pid, peek.start, peek.address, b->data, peek.length);
        if (n < 0)
            b->error = errno;
        else
            b->count = n;
    }
    f->bytes = state.cached_bytes;
    f->data = state.data;
    f->collected_at = state.last;
    f->collection_ms = (seconds() - begin) * 1000;
}
static void *run(void *unused) {
    (void)unused;
    struct timespec due = deadline(1000);
    pthread_mutex_lock(&sampler.mutex);
    for (;;) {
        while (!sampler.stop && !sampler.request) {
            int result = pthread_cond_timedwait(&sampler.wake, &sampler.mutex, &due);
            if (result == ETIMEDOUT)
                break;
            if (result) {
                sampler.stop = 1;
                break;
            }
        }
        if (sampler.stop)
            break;
        sampler.request = 0;
        int slot = 0;
        while (slot == sampler.displayed || slot == sampler.latest)
            slot++;
        int memory_pid = sampler.memory_pid;
        uint64_t memory_start = sampler.memory_start;
        ByteRequest peek = sampler.peek;
        int force = sampler.force_metrics;
        sampler.force_metrics = 0;
        pthread_mutex_unlock(&sampler.mutex);
        fill_frame(sampler.frames[slot], memory_pid, memory_start, peek, force);
        due = deadline(state.valid ? (unsigned)fmax(10, ceil((state.last + 1 - seconds()) * 1000))
                                   : 1000);
        pthread_mutex_lock(&sampler.mutex);
        sampler.frames[slot]->sequence = ++sampler.sequence;
        sampler.latest = slot;
        pthread_cond_broadcast(&sampler.wake);
    }
    pthread_mutex_unlock(&sampler.mutex);
    return NULL;
}
int sampler_start(void) {
    memset(&state, 0, sizeof state);
    memset(&sampler, 0, sizeof sampler);
    for (int i = 0; i < 3; i++) {
        sampler.frames[i] = calloc(1, sizeof(Snapshot));
        if (!sampler.frames[i]) {
            for (int j = 0; j < i; j++)
                free(sampler.frames[j]);
            return 0;
        }
    }
    int result = pthread_mutex_init(&sampler.mutex, NULL);
    if (result)
        goto fail;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!result)
        result = pthread_cond_init(&sampler.wake, &attr);
    pthread_condattr_destroy(&attr);
    if (result) {
        pthread_mutex_destroy(&sampler.mutex);
        goto fail;
    }
    metrics_init();
    fill_frame(sampler.frames[0], 0, 0, (ByteRequest){0}, 1);
    sampler.frames[0]->sequence = ++sampler.sequence;
    if (!sampler.frames[0]->valid) {
        pthread_cond_destroy(&sampler.wake);
        pthread_mutex_destroy(&sampler.mutex);
        metrics_close();
        goto fail;
    }
    result = pthread_create(&sampler.worker, NULL, run, NULL);
    if (result) {
        pthread_cond_destroy(&sampler.wake);
        pthread_mutex_destroy(&sampler.mutex);
        metrics_close();
        goto fail;
    }
    sampler.running = 1;
    return 1;
fail:
    for (int i = 0; i < 3; i++)
        free(sampler.frames[i]);
    return 0;
}
const Snapshot *sampler_poll(void) {
    pthread_mutex_lock(&sampler.mutex);
    sampler.displayed = sampler.latest;
    const Snapshot *f = sampler.frames[sampler.displayed];
    pthread_mutex_unlock(&sampler.mutex);
    return f;
}
int sampler_refresh(unsigned timeout_ms) {
    struct timespec until = deadline(timeout_ms);
    pthread_mutex_lock(&sampler.mutex);
    uint64_t before = sampler.frames[sampler.latest]->data.sample_number;
    sampler.force_metrics = 1;
    sampler.request = 1;
    pthread_cond_broadcast(&sampler.wake);
    while (sampler.frames[sampler.latest]->data.sample_number == before && !sampler.stop) {
        int result = pthread_cond_timedwait(&sampler.wake, &sampler.mutex, &until);
        if (result) {
            pthread_mutex_unlock(&sampler.mutex);
            return 0;
        }
    }
    int ready = sampler.frames[sampler.latest]->data.sample_number > before;
    pthread_mutex_unlock(&sampler.mutex);
    return ready;
}
void sampler_memory_select(int pid, uint64_t start) {
    pthread_mutex_lock(&sampler.mutex);
    sampler.memory_pid = pid;
    sampler.memory_start = start;
    sampler.request = 1;
    pthread_cond_broadcast(&sampler.wake);
    pthread_mutex_unlock(&sampler.mutex);
}
uint64_t sampler_memory_peek(int pid, uint64_t start, uint64_t address, size_t length) {
    pthread_mutex_lock(&sampler.mutex);
    uint64_t serial = sampler.peek.serial + 1;
    sampler.peek = (ByteRequest){pid, start, address, serial, length};
    sampler.request = 1;
    pthread_cond_broadcast(&sampler.wake);
    pthread_mutex_unlock(&sampler.mutex);
    return serial;
}
void sampler_stop(void) {
    if (!sampler.running)
        return;
    pthread_mutex_lock(&sampler.mutex);
    sampler.stop = 1;
    pthread_cond_broadcast(&sampler.wake);
    pthread_mutex_unlock(&sampler.mutex);
    pthread_join(sampler.worker, NULL);
    sampler.running = 0;
    metrics_close();
    pthread_cond_destroy(&sampler.wake);
    pthread_mutex_destroy(&sampler.mutex);
    for (int i = 0; i < 3; i++)
        free(sampler.frames[i]);
}
int sampler_tests(void) {
    Pressure p = {0};
    int ok = parse_pressure("some avg10=1.25 avg60=2.50 avg300=3.75 total=4000", &p) &&
             p.available && p.total == 4000 && p.avg10 == 1.25;
    ok = ok && parse_pressure("full avg10=0.50 avg60=1.00 avg300=1.50 total=1000", &p) &&
         p.full_available && p.full10 == .5;
    ok = ok && !parse_pressure("some avg10=nan avg60=0 avg300=0 total=0", &p);
    ok = ok && !parse_pressure("some avg10=101 avg60=0 avg300=0 total=0", &p);
    ok = ok && !parse_pressure("malformed", &p);
    const Snapshot *held = sampler_poll();
    uint64_t original = held->sequence;
    for (int i = 0; i < 4; i++)
        if (!sampler_refresh(3000) || held->sequence != original)
            ok = 0;
    const Snapshot *next = sampler_poll();
    ok = ok && next->sequence > original && next->data.history_count > 0;
    Process self;
    static const unsigned char probe[MEMORY_BYTES] = "lsm sampler test: known bytes.";
    if (!process_read(getpid(), &self))
        ok = 0;
    else {
        const Snapshot *before = sampler_poll();
        uint64_t number = before->data.sample_number, sequence = before->sequence;
        double collected = before->collected_at;
        sampler_memory_select(self.pid, self.start);
        struct timespec until = deadline(3000);
        pthread_mutex_lock(&sampler.mutex);
        while (sampler.sequence == sequence && !sampler.stop)
            if (pthread_cond_timedwait(&sampler.wake, &sampler.mutex, &until)) {
                ok = 0;
                break;
            }
        pthread_mutex_unlock(&sampler.mutex);
        const Snapshot *published = sampler_poll();
        if (published->data.sample_number != number && published->collected_at - collected < .999)
            ok = 0;
        const Snapshot *memory_frame = NULL;
        for (int i = 0; i < 5; i++) {
            if (!sampler_refresh(3000))
                ok = 0;
            memory_frame = sampler_poll();
            if (memory_frame->memory.pid == self.pid && memory_frame->memory.start == self.start &&
                memory_frame->memory.available)
                break;
        }
        if (!memory_frame || !memory_frame->memory.available || !memory_frame->memory.stored)
            ok = 0;
        uint64_t serial =
            sampler_memory_peek(self.pid, self.start, (uint64_t)(uintptr_t)probe, sizeof probe);
        for (int i = 0; i < 5; i++) {
            if (!sampler_refresh(3000))
                ok = 0;
            memory_frame = sampler_poll();
            if (memory_frame->bytes.serial == serial)
                break;
        }
        if (memory_frame->bytes.serial != serial || memory_frame->bytes.error ||
            memory_frame->bytes.count != sizeof probe ||
            memcmp(memory_frame->bytes.data, probe, sizeof probe))
            ok = 0;
        sampler_memory_select(0, 0);
    }
    printf("%s: PSI parsing, metadata-independent history, background publication and immutable "
           "frozen "
           "snapshots\n",
           ok ? "PASS" : "FAIL");
    return ok;
}
