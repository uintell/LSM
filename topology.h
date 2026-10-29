#ifndef LSM_TOPOLOGY_H
#define LSM_TOPOLOGY_H
#include <stdint.h>
#include <stddef.h>
#define TOPOLOGY_NODES 4096
#define FILE_TREE_NODES 2048
#define TOPOLOGY_PATH 4096

typedef struct {
    int parent, depth, children, source, id;
    uint64_t identity;
    float x, y, z, radius;
} TopologyNode;
typedef struct {
    int count, root, visible;
    float center[3], extent;
    TopologyNode nodes[TOPOLOGY_NODES];
} Topology;
/* Root is parent -1. Other absent/invalid/cyclic parent links are attached to
   root. Layout and traversal are iterative, including for deep process chains. */
int topology_layout(Topology *tree, int focus);
int topology_tests(void);

typedef struct {
    int parent, depth, directory, symlink, boundary, error;
    uint64_t bytes, allocated;
    unsigned mode, uid;
    int64_t modified;
    char name[256], target[256];
} FileNode;
typedef struct {
    uint64_t serial;
    int count, error, truncated, depth, hidden, scanned;
    char root[TOPOLOGY_PATH];
    FileNode nodes[FILE_TREE_NODES];
} FileTree;
int files_scan(const char *root, int depth, int hidden, FileTree *tree);
int files_path(const FileTree *tree, int index, char *path, size_t size);
int files_start(void);
uint64_t files_request(const char *path, int depth, int hidden);
const FileTree *files_poll(void);
int files_wait(uint64_t serial, unsigned timeout_ms);
void files_stop(void);
int files_tests(void);
#endif
