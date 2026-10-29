#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "memory.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void memory_system_line(SystemMemory *m, const char *line) {
    char name[64];
    unsigned long long n;
    if (sscanf(line, "%63[^:]: %llu", name, &n) != 2)
        return;
#define FIELD(key, member)                                                                         \
    if (!strcmp(name, key))                                                                        \
    m->member = n
    FIELD("MemTotal", total);
    FIELD("MemAvailable", available);
    FIELD("MemFree", free);
    FIELD("Cached", cached);
    FIELD("Buffers", buffers);
    FIELD("Slab", slab);
    FIELD("SReclaimable", reclaimable);
    FIELD("SUnreclaim", unreclaimable);
    FIELD("AnonPages", anonymous);
    FIELD("Shmem", shared);
    FIELD("PageTables", page_tables);
    FIELD("KernelStack", kernel_stack);
    FIELD("SwapTotal", swap_total);
    FIELD("SwapFree", swap_free);
    FIELD("Dirty", dirty);
    FIELD("Writeback", writeback);
#undef FIELD
}
const char *memory_kind_name(MemoryKind kind) {
    static const char *names[] = {"Heap", "Stack", "File", "Shared", "Anonymous", "Special"};
    return kind >= 0 && kind < MEM_KINDS ? names[kind] : "Unknown";
}
static int region_header(const char *line, MemoryRegion *r) {
    unsigned long long begin, end, offset, inode;
    unsigned major, minor;
    char perms[5];
    int used = 0;
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &begin, &end, perms, &offset, &major,
               &minor, &inode, &used) != 7 ||
        !used || end <= begin || strlen(perms) != 4)
        return 0;
    memset(r, 0, sizeof *r);
    r->begin = begin;
    r->end = end;
    r->offset = offset;
    memcpy(r->permissions, perms, 5);
    const char *name = line + used;
    while (*name == ' ' || *name == '\t')
        name++;
    snprintf(r->name, sizeof r->name, "%.383s", name);
    r->name[strcspn(r->name, "\r\n")] = 0;
    for (char *p = r->name; *p; p++)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
            *p = '?';
    if (!strcmp(r->name, "[heap]"))
        r->kind = MEM_HEAP;
    else if (!strncmp(r->name, "[stack", 6))
        r->kind = MEM_STACK;
    else if (!strncmp(r->name, "[anon_shmem:", 12) || !strncmp(r->name, "/dev/shm/", 9) ||
             !strncmp(r->name, "/memfd:", 7) || (!r->name[0] && perms[3] == 's'))
        r->kind = MEM_SHARED;
    else if (!r->name[0] || !strncmp(r->name, "[anon:", 6))
        r->kind = MEM_ANON;
    else if (r->name[0] == '[')
        r->kind = MEM_SPECIAL;
    else
        r->kind = MEM_FILE;
    return 1;
}
static void finish_region(MemoryReport *m, const MemoryRegion *r) {
    m->total_regions++;
    m->rss += r->rss;
    m->pss += r->pss;
    m->private_kib += r->private_kib;
    m->shared_kib += r->shared_kib;
    m->anonymous += r->anonymous;
    m->swap += r->swap;
    m->virtual_kib += (r->end - r->begin) / 1024;
    m->category_rss[r->kind] += r->rss;
    if (m->stored < MEMORY_REGIONS)
        m->regions[m->stored++] = *r;
    else
        m->truncated = 1;
}
static int parse_maps(FILE *f, MemoryReport *m) {
    char line[2048];
    MemoryRegion current = {0};
    int have = 0;
    while (fgets(line, sizeof line, f)) {
        MemoryRegion next;
        if (region_header(line, &next)) {
            if (have)
                finish_region(m, &current);
            current = next;
            have = 1;
            continue;
        }
        if (!have)
            continue;
        char key[64];
        unsigned long long n;
        if (sscanf(line, "%63[^:]: %llu", key, &n) != 2)
            continue;
        if (!strcmp(key, "Rss"))
            current.rss = n;
        else if (!strcmp(key, "Pss"))
            current.pss = n;
        else if (!strcmp(key, "Private_Clean") || !strcmp(key, "Private_Dirty"))
            current.private_kib += n;
        else if (!strcmp(key, "Shared_Clean") || !strcmp(key, "Shared_Dirty"))
            current.shared_kib += n;
        else if (!strcmp(key, "Anonymous"))
            current.anonymous = n;
        else if (!strcmp(key, "Swap"))
            current.swap = n;
        if (!strcmp(key, "Private_Dirty") || !strcmp(key, "Shared_Dirty"))
            current.dirty += n;
    }
    if (have)
        finish_region(m, &current);
    return !ferror(f);
}
static int start_matches(int directory, uint64_t start) {
    int fd = openat(directory, "stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    char line[8192];
    ssize_t n = read(fd, line, sizeof line - 1);
    close(fd);
    if (n <= 0) {
        errno = ESRCH;
        return 0;
    }
    line[n] = 0;
    char *end = strrchr(line, ')'), *save;
    if (!end || end[1] != ' ') {
        errno = ESRCH;
        return 0;
    }
    int field = 3;
    char *token = strtok_r(end + 2, " ", &save);
    while (token && field < 22) {
        token = strtok_r(NULL, " ", &save);
        field++;
    }
    if (!token || strtoull(token, NULL, 10) != start) {
        errno = ESRCH;
        return 0;
    }
    return 1;
}
static int open_process(int pid, uint64_t start) {
    if (pid <= 0 || !start) {
        errno = EINVAL;
        return -1;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/%d", pid);
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return -1;
    if (!start_matches(directory, start)) {
        int error = errno;
        close(directory);
        errno = error ? error : ESRCH;
        return -1;
    }
    return directory;
}
int memory_report_read(int pid, uint64_t start, MemoryReport *m) {
    memset(m, 0, offsetof(MemoryReport, regions));
    m->pid = pid;
    m->start = start;
    if (!pid)
        return 1;
    int directory = open_process(pid, start);
    if (directory < 0) {
        m->error = errno;
        return 0;
    }
    int fd = openat(directory, "smaps", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
        m->accounting = 1;
    else
        fd = openat(directory, "maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        m->error = errno;
        close(directory);
        return 0;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) {
        m->error = errno;
        close(fd);
        close(directory);
        return 0;
    }
    int ok = parse_maps(f, m);
    int error = ferror(f) ? EIO : 0;
    fclose(f);
    if (ok && !start_matches(directory, start)) {
        ok = 0;
        error = ESRCH;
    }
    close(directory);
    m->available = ok;
    m->error = error;
    return ok;
}
int memory_peek(int pid, uint64_t start, uint64_t address, unsigned char *data, size_t length) {
    if (!length || length > MEMORY_BYTES || address > INT64_MAX ||
        length > (uint64_t)INT64_MAX - address) {
        errno = EINVAL;
        return -1;
    }
    int directory = open_process(pid, start);
    if (directory < 0)
        return -1;
    int mapfd = openat(directory, "maps", O_RDONLY | O_CLOEXEC);
    if (mapfd < 0) {
        int e = errno;
        close(directory);
        errno = e;
        return -1;
    }
    FILE *f = fdopen(mapfd, "r");
    if (!f) {
        int e = errno;
        close(mapfd);
        close(directory);
        errno = e;
        return -1;
    }
    char line[2048];
    int readable = 0;
    while (fgets(line, sizeof line, f)) {
        MemoryRegion region;
        if (region_header(line, &region) && region.permissions[0] == 'r' &&
            address >= region.begin && address < region.end && length <= region.end - address) {
            readable = 1;
            break;
        }
    }
    fclose(f);
    if (!readable) {
        close(directory);
        errno = EACCES;
        return -1;
    }
    /* Opening mem relative to the checked /proc directory binds this descriptor
       to that process's mm. PID reuse cannot retarget a later pread. */
    int fd = openat(directory, "mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        close(directory);
        errno = e;
        return -1;
    }
    if (!start_matches(directory, start)) {
        close(fd);
        close(directory);
        errno = ESRCH;
        return -1;
    }
    ssize_t n = pread(fd, data, length, (off_t)address);
    int error = errno;
    close(fd);
    close(directory);
    if (n == 0) {
        errno = ESRCH;
        return -1;
    }
    if (n < 0) {
        errno = error;
        return -1;
    }
    return (int)n;
}
int memory_tests(void) {
    SystemMemory sys = {0};
    memory_system_line(&sys, "Cached: 1024 kB");
    memory_system_line(&sys, "MemAvailable: 2048 kB");
    int ok = sys.cached == 1024 && sys.available == 2048;
    FILE *f = tmpfile();
    if (!f)
        return 0;
    fputs("1000-2000 rw-p 00000000 00:00 0 [heap]\nRss: 8 kB\nPss: 6 kB\nPrivate_Clean: 2 "
          "kB\nPrivate_Dirty: 4 kB\nShared_Clean: 2 kB\nAnonymous: 8 kB\nSwap: 1 kB\n2000-3000 "
          "r--p 00000000 00:01 2 /a file with spaces\nRss: 4 kB\nPss: 2 kB\nShared_Clean: 4 kB\n",
          f);
    rewind(f);
    MemoryReport *m = calloc(1, sizeof *m);
    if (!m) {
        fclose(f);
        return 0;
    }
    ok = ok && parse_maps(f, m) && m->stored == 2 && m->rss == 12 && m->pss == 8 &&
         m->private_kib == 6 && m->shared_kib == 6 && m->swap == 1 &&
         m->category_rss[MEM_HEAP] == 8 && !strcmp(m->regions[1].name, "/a file with spaces");
    fclose(f);
    int directory = open("/proc/self", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char line[8192];
    uint64_t start = 0;
    int statfd = directory >= 0 ? openat(directory, "stat", O_RDONLY | O_CLOEXEC) : -1;
    if (statfd >= 0) {
        ssize_t n = read(statfd, line, sizeof line - 1);
        close(statfd);
        if (n > 0) {
            line[n] = 0;
            char *p = strrchr(line, ')'), *save;
            if (p) {
                int field = 3;
                char *tok = strtok_r(p + 2, " ", &save);
                while (tok && field < 22) {
                    tok = strtok_r(NULL, " ", &save);
                    field++;
                }
                if (tok)
                    start = strtoull(tok, NULL, 10);
            }
        }
    }
    if (directory >= 0)
        close(directory);
    ok = ok && start && memory_report_read(getpid(), start, m) && m->available && m->stored > 0 &&
         m->accounting;
    static const unsigned char sentinel[] = "lsm memory inspector test: known bytes only.";
    unsigned char bytes[MEMORY_BYTES];
    int n = memory_peek(getpid(), start, (uint64_t)(uintptr_t)sentinel, bytes, sizeof sentinel);
    ok = ok && n == (int)sizeof sentinel && !memcmp(bytes, sentinel, sizeof sentinel);
    errno = 0;
    ok = ok &&
         memory_peek(getpid(), start + 1, (uint64_t)(uintptr_t)sentinel, bytes, sizeof sentinel) ==
             -1 &&
         errno == ESRCH;
    ok = ok && memory_peek(getpid(), start, UINT64_MAX, bytes, 16) == -1;
    long page_size = sysconf(_SC_PAGESIZE);
    void *guard = mmap(NULL, (size_t)page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard == MAP_FAILED)
        ok = 0;
    else {
        errno = 0;
        int denied = memory_peek(getpid(), start, (uint64_t)(uintptr_t)guard, bytes, 16);
        ok = ok && denied == -1 && errno == EACCES;
        munmap(guard, (size_t)page_size);
    }
    f = tmpfile();
    if (!f)
        ok = 0;
    else {
        memset(m, 0, sizeof *m);
        for (unsigned i = 0; i < MEMORY_REGIONS + 1; i++)
            fprintf(f, "%llx-%llx rw-p 0 00:00 0\nRss: 1 kB\n", (unsigned long long)(i + 1) * 4096,
                    (unsigned long long)(i + 2) * 4096);
        rewind(f);
        ok = ok && parse_maps(f, m) && m->stored == MEMORY_REGIONS &&
             m->total_regions == MEMORY_REGIONS + 1 && m->truncated && m->rss == MEMORY_REGIONS + 1;
        fclose(f);
    }
    free(m);
    printf(
        "%s: memory accounting, map parsing, process identity and bounded read-only byte access\n",
        ok ? "PASS" : "FAIL");
    return ok;
}
