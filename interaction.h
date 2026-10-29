#ifndef LSM_INTERACTION_H
#define LSM_INTERACTION_H
#include <stdint.h>
#include "topology.h"
#define EDITOR_LAYOUTS 8192
#define EDITOR_UNDO 64
#define PICK_LABELS 128

typedef struct {
    uint64_t a, b;
} NodeKey;
typedef struct {
    NodeKey key;
    int source, shape;
    double position[3], radius, sx, sy, depth, eye_depth, pixel_radius;
} PickNode;
typedef struct {
    int target;
    double x, y, w, h;
} PickLabel;
typedef struct {
    int kind, count, label_count, valid, window_height, viewport[4];
    uint64_t scope;
    double model[16], projection[16];
    PickNode nodes[TOPOLOGY_NODES];
    PickLabel labels[PICK_LABELS];
} PickFrame;
typedef struct {
    int kind;
    uint64_t scope;
    NodeKey node;
} LayoutKey;
typedef struct {
    LayoutKey key;
    double position[3];
} NodePlacement;
typedef struct {
    LayoutKey key;
    int had_before;
    double before[3], after[3];
} PlacementUndo;
typedef struct {
    int count, undo_count;
    NodePlacement entries[EDITOR_LAYOUTS];
    PlacementUndo undo[EDITOR_UNDO];
} LayoutEditor;
int node_key_equal(NodeKey a, NodeKey b);
NodeKey node_path_key(const char *path);
void pick_frame_begin(PickFrame *frame, int kind, uint64_t scope, const double *model,
                      const double *projection, const int *viewport, int window_height);
int pick_project(const PickFrame *frame, const double point[3], double *x, double *y, double *depth,
                 double *eye_depth);
int pick_add(PickFrame *frame, NodeKey key, int source, int shape, const double point[3],
             double radius);
void pick_label_add(PickFrame *frame, int target, double x, double y, double w, double h);
int pick_target(const PickFrame *frame, double x, double y);
int pick_drag_delta(const PickFrame *frame, const double anchor[3], double x0, double y0, double x1,
                    double y1, double delta[3]);
int gesture_moved(int x0, int y0, int x, int y);
int layout_get(const LayoutEditor *editor, LayoutKey key, double out[3]);
int layout_set(LayoutEditor *editor, LayoutKey key, const double position[3]);
void layout_remove(LayoutEditor *editor, LayoutKey key);
void layout_commit(LayoutEditor *editor, LayoutKey key, int had_before, const double before[3],
                   const double after[3]);
int layout_undo(LayoutEditor *editor, int kind, uint64_t scope);
void layout_reset(LayoutEditor *editor, int kind, uint64_t scope);
int interaction_tests(void);
#endif
