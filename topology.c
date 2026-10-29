#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "topology.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

int topology_layout(Topology *t, int focus) {
    int n = t->count;
    if (n < 1 || n > TOPOLOGY_NODES)
        return 0;
    int first[TOPOLOGY_NODES], next[TOPOLOGY_NODES], order[TOPOLOGY_NODES], queue[TOPOLOGY_NODES];
    unsigned char state[TOPOLOGY_NODES] = {0};
    float weight[TOPOLOGY_NODES], start[TOPOLOGY_NODES], span[TOPOLOGY_NODES];
    t->nodes[0].parent = -1;
    for (int i = 1; i < n; i++)
        if (t->nodes[i].parent < 0 || t->nodes[i].parent >= n || t->nodes[i].parent == i)
            t->nodes[i].parent = 0;
    for (int i = 1; i < n; i++) {
        int j = i;
        while (j > 0 && state[j] == 0) {
            state[j] = 1;
            j = t->nodes[j].parent;
        }
        if (j > 0 && state[j] == 1)
            t->nodes[j].parent = 0;
        j = i;
        while (j > 0 && state[j] == 1) {
            state[j] = 2;
            j = t->nodes[j].parent;
        }
    }
    for (int i = 0; i < n; i++) {
        first[i] = next[i] = -1;
        t->nodes[i].children = 0;
        t->nodes[i].depth = -1;
    }
    for (int i = n - 1; i > 0; i--) {
        int p = t->nodes[i].parent;
        next[i] = first[p];
        first[p] = i;
        t->nodes[p].children++;
    }
    if (focus < 0 || focus >= n)
        focus = 0;
    t->root = focus;
    int read = 0, write = 1, used = 0;
    queue[0] = focus;
    t->nodes[focus].depth = 0;
    while (read < write) {
        int i = queue[read++];
        order[used++] = i;
        for (int child = first[i]; child >= 0; child = next[child]) {
            t->nodes[child].depth = t->nodes[i].depth + 1;
            queue[write++] = child;
        }
    }
    for (int k = used - 1; k >= 0; k--) {
        int i = order[k];
        weight[i] = 0;
        for (int c = first[i]; c >= 0; c = next[c])
            weight[i] += weight[c];
        if (weight[i] < 1)
            weight[i] = 1;
    }
    start[focus] = -.5f * (float)3.141592653589793;
    span[focus] = 2 * (float)3.141592653589793;
    float minimum[3] = {0, 0, 0}, maximum[3] = {0, 0, 0};
    float outer = fmaxf(3.5f, weight[focus] * .62f / (2 * (float)3.141592653589793));
    for (int k = 0; k < used; k++) {
        int i = order[k];
        TopologyNode *p = &t->nodes[i];
        int d = p->depth;
        float angle = start[i] + span[i] * .5f;
        float radius = d ? outer * (.7f + .13f * d) : 0;
        p->x = radius * cosf(angle);
        p->y = -d * 3.f;
        p->z = radius * sinf(angle);
        float v[3] = {p->x, p->y, p->z};
        for (int a = 0; a < 3; a++) {
            if (v[a] < minimum[a])
                minimum[a] = v[a];
            if (v[a] > maximum[a])
                maximum[a] = v[a];
        }
        float cursor = start[i];
        for (int c = first[i]; c >= 0; c = next[c]) {
            start[c] = cursor;
            span[c] = span[i] * weight[c] / weight[i];
            cursor += span[c];
        }
    }
    t->visible = used;
    for (int a = 0; a < 3; a++)
        t->center[a] = (minimum[a] + maximum[a]) * .5f;
    t->extent = 1;
    for (int k = 0; k < used; k++) {
        TopologyNode *p = &t->nodes[order[k]];
        float x = p->x - t->center[0], y = p->y - t->center[1], z = p->z - t->center[2];
        float r = sqrtf(x * x + y * y + z * z) + p->radius;
        if (r > t->extent)
            t->extent = r;
    }
    return used;
}
int topology_tests(void) {
    Topology *t = calloc(1, sizeof *t);
    if (!t)
        return 0;
    t->count = 7;
    t->nodes[1].parent = 0;
    t->nodes[2].parent = 1;
    t->nodes[3].parent = 1;
    t->nodes[4].parent = 2;
    t->nodes[5].parent = 6;
    t->nodes[6].parent = 5;
    int ok = topology_layout(t, 0) == 7 && t->nodes[4].depth == 3 && t->nodes[1].children == 2;
    for (int i = 0; i < t->count; i++)
        if (!isfinite(t->nodes[i].x) || !isfinite(t->nodes[i].y) || !isfinite(t->nodes[i].z))
            ok = 0;
    ok = ok && topology_layout(t, 1) == 4 && t->nodes[1].depth == 0 && t->nodes[4].depth == 2 &&
         t->nodes[0].depth == -1;
    t->count = TOPOLOGY_NODES;
    for (int i = 1; i < t->count; i++)
        t->nodes[i].parent = i - 1;
    ok = ok && topology_layout(t, 0) == TOPOLOGY_NODES &&
         t->nodes[t->count - 1].depth == t->count - 1 && isfinite(t->extent);
    free(t);
    printf("%s: 3D tree layout, subtree focus, absent parents, cycles and deep chains\n",
           ok ? "PASS" : "FAIL");
    return ok;
}

static struct {
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    FileTree *frames[3];
    int displayed, latest, pending, stopping, running, depth, hidden;
    char path[TOPOLOGY_PATH];
    _Atomic uint64_t requested;
} browser;
static int should_stop(uint64_t serial) {
    return serial && serial != atomic_load(&browser.requested);
}
int files_path(const FileTree *tree, int index, char *path, size_t size) {
    if (index < 0 || index >= tree->count || !size) {
        errno = EINVAL;
        return 0;
    }
    int chain[FILE_TREE_NODES], n = 0;
    for (int i = index; i > 0; i = tree->nodes[i].parent) {
        if (i >= tree->count || n >= FILE_TREE_NODES || tree->nodes[i].parent >= i ||
            tree->nodes[i].parent < 0) {
            errno = EINVAL;
            return 0;
        }
        chain[n++] = i;
    }
    size_t used = strlen(tree->root);
    if (used >= size) {
        errno = ENAMETOOLONG;
        return 0;
    }
    memcpy(path, tree->root, used + 1);
    for (int k = n - 1; k >= 0; k--) {
        const char *name = tree->nodes[chain[k]].name;
        size_t len = strlen(name);
        int slash = used && path[used - 1] != '/';
        if (used + slash + len >= size) {
            errno = ENAMETOOLONG;
            return 0;
        }
        if (slash)
            path[used++] = '/';
        memcpy(path + used, name, len + 1);
        used += len;
    }
    return 1;
}
static int node_compare(const void *a, const void *b) {
    const FileNode *p = a, *q = b;
    if (p->directory != q->directory)
        return q->directory - p->directory;
    return strcmp(p->name, q->name);
}
static void node_stat(FileNode *n, const struct stat *st) {
    n->directory = S_ISDIR(st->st_mode);
    n->symlink = S_ISLNK(st->st_mode);
    n->bytes = st->st_size > 0 ? (uint64_t)st->st_size : 0;
    n->allocated = st->st_blocks > 0 ? (uint64_t)st->st_blocks * 512 : 0;
    n->mode = (unsigned)st->st_mode;
    n->uid = (unsigned)st->st_uid;
    n->modified = (int64_t)st->st_mtime;
}
static int scan(const char *root, int depth, int hidden, FileTree *tree, uint64_t serial) {
    memset(tree, 0, offsetof(FileTree, nodes));
    tree->serial = serial;
    tree->depth = depth < 1 ? 1 : depth > 4 ? 4 : depth;
    tree->hidden = hidden;
    if (!root || !*root) {
        tree->error = EINVAL;
        return 0;
    }
    if (!realpath(root, tree->root)) {
        tree->error = errno ? errno : EINVAL;
        return 0;
    }
    int rootfd = open(tree->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootfd < 0) {
        tree->error = errno;
        return 0;
    }
    struct stat root_stat;
    if (fstat(rootfd, &root_stat)) {
        tree->error = errno;
        close(rootfd);
        return 0;
    }
    tree->count = 1;
    memset(&tree->nodes[0], 0, sizeof(FileNode));
    tree->nodes[0].parent = -1;
    snprintf(tree->nodes[0].name, sizeof tree->nodes[0].name, "%.255s", tree->root);
    node_stat(&tree->nodes[0], &root_stat);
    for (int i = 0; i < tree->count; i++) {
        if (should_stop(serial)) {
            tree->error = ECANCELED;
            close(rootfd);
            return 0;
        }
        FileNode *parent = &tree->nodes[i];
        if (!parent->directory || parent->symlink || parent->boundary ||
            parent->depth >= tree->depth)
            continue;
        int chain[FILE_TREE_NODES], n = 0, fd = dup(rootfd);
        for (int j = i; j > 0; j = tree->nodes[j].parent)
            chain[n++] = j;
        for (int k = n - 1; k >= 0 && fd >= 0; k--) {
            int next = openat(fd, tree->nodes[chain[k]].name,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            int error = errno;
            close(fd);
            fd = next;
            if (fd < 0)
                parent->error = error;
        }
        if (fd < 0)
            continue;
        DIR *dir = fdopendir(fd);
        if (!dir) {
            parent->error = errno;
            close(fd);
            continue;
        }
        tree->scanned++;
        int begin = tree->count;
        struct dirent *entry;
        for (;;) {
            if (should_stop(serial)) {
                closedir(dir);
                close(rootfd);
                tree->error = ECANCELED;
                return 0;
            }
            errno = 0;
            entry = readdir(dir);
            if (!entry) {
                if (errno)
                    parent->error = errno;
                break;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
                (!hidden && entry->d_name[0] == '.'))
                continue;
            if (tree->count == FILE_TREE_NODES) {
                tree->truncated = 1;
                break;
            }
            struct stat st;
            int error = 0;
            if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
                error = errno;
                if (error == ENOENT)
                    continue;
            }
            FileNode *child = &tree->nodes[tree->count++];
            memset(child, 0, sizeof *child);
            child->parent = i;
            child->depth = parent->depth + 1;
            child->error = error;
            snprintf(child->name, sizeof child->name, "%s", entry->d_name);
            if (!error) {
                node_stat(child, &st);
                child->boundary = child->directory && st.st_dev != root_stat.st_dev;
                if (child->symlink) {
                    ssize_t len =
                        readlinkat(fd, entry->d_name, child->target, sizeof child->target - 1);
                    if (len >= 0)
                        child->target[len] = 0;
                }
            }
        }
        closedir(dir);
        qsort(tree->nodes + begin, (size_t)(tree->count - begin), sizeof(FileNode), node_compare);
        if (tree->truncated)
            break;
    }
    close(rootfd);
    return 1;
}
int files_scan(const char *root, int depth, int hidden, FileTree *tree) {
    return scan(root, depth, hidden, tree, 0);
}
static void *files_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&browser.mutex);
    while (!browser.stopping) {
        while (!browser.pending && !browser.stopping)
            pthread_cond_wait(&browser.wake, &browser.mutex);
        if (browser.stopping)
            break;
        char root[TOPOLOGY_PATH];
        snprintf(root, sizeof root, "%s", browser.path);
        int depth = browser.depth, hidden = browser.hidden;
        uint64_t serial = atomic_load(&browser.requested);
        browser.pending = 0;
        int slot = 0;
        while (slot == browser.displayed || slot == browser.latest)
            slot++;
        pthread_mutex_unlock(&browser.mutex);
        scan(root, depth, hidden, browser.frames[slot], serial);
        pthread_mutex_lock(&browser.mutex);
        if (serial == atomic_load(&browser.requested) && !browser.stopping)
            browser.latest = slot;
        pthread_cond_broadcast(&browser.wake);
    }
    pthread_mutex_unlock(&browser.mutex);
    return NULL;
}
int files_start(void) {
    memset(&browser, 0, sizeof browser);
    atomic_init(&browser.requested, 0);
    for (int i = 0; i < 3; i++) {
        browser.frames[i] = calloc(1, sizeof(FileTree));
        if (!browser.frames[i]) {
            for (int j = 0; j < i; j++)
                free(browser.frames[j]);
            return 0;
        }
    }
    if (pthread_mutex_init(&browser.mutex, NULL))
        goto fail;
    if (pthread_cond_init(&browser.wake, NULL)) {
        pthread_mutex_destroy(&browser.mutex);
        goto fail;
    }
    if (pthread_create(&browser.worker, NULL, files_worker, NULL)) {
        pthread_cond_destroy(&browser.wake);
        pthread_mutex_destroy(&browser.mutex);
        goto fail;
    }
    browser.running = 1;
    return 1;
fail:
    for (int i = 0; i < 3; i++)
        free(browser.frames[i]);
    return 0;
}
uint64_t files_request(const char *path, int depth, int hidden) {
    if (!browser.running || !path || !*path || strlen(path) >= TOPOLOGY_PATH) {
        errno = EINVAL;
        return 0;
    }
    pthread_mutex_lock(&browser.mutex);
    snprintf(browser.path, sizeof browser.path, "%s", path);
    browser.depth = depth;
    browser.hidden = hidden;
    browser.pending = 1;
    uint64_t serial = atomic_fetch_add(&browser.requested, 1) + 1;
    pthread_cond_signal(&browser.wake);
    pthread_mutex_unlock(&browser.mutex);
    return serial;
}
const FileTree *files_poll(void) {
    pthread_mutex_lock(&browser.mutex);
    browser.displayed = browser.latest;
    const FileTree *t = browser.frames[browser.displayed];
    pthread_mutex_unlock(&browser.mutex);
    return t;
}
int files_wait(uint64_t serial, unsigned timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&browser.mutex);
    while (!browser.stopping && atomic_load(&browser.requested) == serial &&
           browser.frames[browser.latest]->serial != serial) {
        if (pthread_cond_timedwait(&browser.wake, &browser.mutex, &until)) {
            pthread_mutex_unlock(&browser.mutex);
            return 0;
        }
    }
    int ready = browser.frames[browser.latest]->serial == serial;
    pthread_mutex_unlock(&browser.mutex);
    return ready;
}
void files_stop(void) {
    if (!browser.running)
        return;
    pthread_mutex_lock(&browser.mutex);
    browser.stopping = 1;
    atomic_fetch_add(&browser.requested, 1);
    pthread_cond_signal(&browser.wake);
    pthread_mutex_unlock(&browser.mutex);
    pthread_join(browser.worker, NULL);
    pthread_cond_destroy(&browser.wake);
    pthread_mutex_destroy(&browser.mutex);
    for (int i = 0; i < 3; i++)
        free(browser.frames[i]);
    browser.running = 0;
}
int files_tests(void) {
    char root[] = "/tmp/lsm-files-XXXXXX";
    if (!mkdtemp(root))
        return 0;
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int ok = dir >= 0;
    if (dir >= 0) {
        if (mkdirat(dir, "folder", 0700))
            ok = 0;
        int file = openat(dir, "a file.txt", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (file < 0)
            ok = 0;
        else {
            if (ftruncate(file, 8192))
                ok = 0;
            close(file);
        }
        file = openat(dir, ".hidden", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (file >= 0)
            close(file);
        else
            ok = 0;
        if (symlinkat(".", dir, "cycle"))
            ok = 0;
    }
    FileTree *t = calloc(1, sizeof *t);
    if (!t)
        ok = 0;
    if (t && dir >= 0) {
        ok = ok && files_scan(root, 3, 0, t) && t->count == 4;
        int link_seen = 0, file_seen = 0;
        char path[TOPOLOGY_PATH];
        for (int i = 1; i < t->count; i++) {
            if (t->nodes[i].symlink)
                link_seen++;
            if (!strcmp(t->nodes[i].name, "a file.txt")) {
                file_seen = t->nodes[i].bytes == 8192 && files_path(t, i, path, sizeof path) &&
                            strstr(path, "a file.txt") != NULL;
            }
            if (t->nodes[i].parent >= i)
                ok = 0;
        }
        ok = ok && link_seen == 1 && file_seen && files_scan(root, 2, 1, t) && t->count == 5;
        ok = ok && !files_scan("/path/that/does/not/exist/lsm", 2, 0, t) && t->error == ENOENT;
        if (!files_start())
            ok = 0;
        else {
            uint64_t serial = files_request(root, 2, 0);
            ok = ok && files_wait(serial, 3000);
            const FileTree *held = files_poll();
            uint64_t held_serial = held->serial;
            serial = files_request(root, 2, 1);
            int ready = files_wait(serial, 3000);
            ok = ok && ready && held->serial == held_serial && files_poll()->count == 5;
            files_stop();
        }
        char file_path[TOPOLOGY_PATH];
        snprintf(file_path, sizeof file_path, "%s/a file.txt", root);
        ok = ok && !files_scan(file_path, 1, 0, t) && t->error == ENOTDIR;
        for (int i = 0; i < FILE_TREE_NODES; i++) {
            char name[64];
            snprintf(name, sizeof name, "limit-%04d", i);
            int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0)
                ok = 0;
            else
                close(fd);
        }
        ok = ok && files_scan(root, 1, 0, t) && t->truncated && t->count == FILE_TREE_NODES;
        for (int i = 0; i < FILE_TREE_NODES; i++) {
            char name[64];
            snprintf(name, sizeof name, "limit-%04d", i);
            unlinkat(dir, name, 0);
        }
        unlinkat(dir, "a file.txt", 0);
        unlinkat(dir, ".hidden", 0);
        unlinkat(dir, "cycle", 0);
        unlinkat(dir, "folder", AT_REMOVEDIR);
    }
    if (dir >= 0)
        close(dir);
    rmdir(root);
    free(t);
    printf("%s: filesystem metadata, sizes, hidden entries, paths and symlink cycles\n",
           ok ? "PASS" : "FAIL");
    return ok;
}
