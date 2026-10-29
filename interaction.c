#include "interaction.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int node_key_equal(NodeKey a, NodeKey b) {
    return a.a == b.a && a.b == b.b;
}
NodeKey node_path_key(const char *path) {
    NodeKey key = {1469598103934665603ULL, 0x9e3779b97f4a7c15ULL};
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        key.a = (key.a ^ *p) * 1099511628211ULL;
        key.b ^= (uint64_t)*p + 0x9e3779b97f4a7c15ULL + (key.b << 6) + (key.b >> 2);
    }
    return key;
}
static int layout_key_equal(LayoutKey a, LayoutKey b) {
    return a.kind == b.kind && a.scope == b.scope && node_key_equal(a.node, b.node);
}
void pick_frame_begin(PickFrame *f, int kind, uint64_t scope, const double *model,
                      const double *projection, const int *viewport, int height) {
    f->count = f->label_count = 0;
    f->kind = kind;
    f->scope = scope;
    f->window_height = height;
    memcpy(f->model, model, sizeof f->model);
    memcpy(f->projection, projection, sizeof f->projection);
    memcpy(f->viewport, viewport, sizeof f->viewport);
    f->valid = viewport[2] > 0 && viewport[3] > 0 && projection[0] > 0 && projection[5] > 0;
}
int pick_project(const PickFrame *f, const double p[3], double *x, double *y, double *depth,
                 double *eye_depth) {
    if (!f->valid)
        return 0;
    double eye[4], clip[4];
    for (int i = 0; i < 4; i++)
        eye[i] =
            f->model[i] * p[0] + f->model[4 + i] * p[1] + f->model[8 + i] * p[2] + f->model[12 + i];
    for (int i = 0; i < 4; i++)
        clip[i] = f->projection[i] * eye[0] + f->projection[4 + i] * eye[1] +
                  f->projection[8 + i] * eye[2] + f->projection[12 + i] * eye[3];
    if (clip[3] <= 0)
        return 0;
    *x = f->viewport[0] + (clip[0] / clip[3] + 1) * f->viewport[2] / 2;
    *y = f->window_height - f->viewport[1] - (clip[1] / clip[3] + 1) * f->viewport[3] / 2;
    *depth = clip[2] / clip[3];
    *eye_depth = -eye[2];
    return *depth >= -1 && *depth <= 1 && *eye_depth > 0;
}
int pick_add(PickFrame *f, NodeKey key, int source, int shape, const double p[3], double radius) {
    if (f->count >= TOPOLOGY_NODES || !f->valid || radius <= 0)
        return -1;
    PickNode n = {.key = key, .source = source, .shape = shape, .radius = radius};
    memcpy(n.position, p, sizeof n.position);
    if (!pick_project(f, p, &n.sx, &n.sy, &n.depth, &n.eye_depth))
        return -1;
    n.pixel_radius = radius * f->projection[5] * f->viewport[3] / (2 * n.eye_depth);
    int index = f->count++;
    f->nodes[index] = n;
    return index;
}
void pick_label_add(PickFrame *f, int target, double x, double y, double w, double h) {
    if (target < 0 || target >= f->count || f->label_count >= PICK_LABELS || w <= 0 || h <= 0)
        return;
    f->labels[f->label_count++] = (PickLabel){target, x, y, w, h};
}
static int ray(const PickFrame *f, double x, double y, double origin[3], double direction[3]) {
    if (!f->valid)
        return 0;
    double nx = 2 * (x - f->viewport[0]) / f->viewport[2] - 1,
           ny = 2 * (f->window_height - y - f->viewport[1]) / f->viewport[3] - 1;
    double r[3] = {nx / f->projection[0], ny / f->projection[5], -1};
    for (int i = 0; i < 3; i++) {
        origin[i] = -(f->model[i * 4] * f->model[12] + f->model[i * 4 + 1] * f->model[13] +
                      f->model[i * 4 + 2] * f->model[14]);
        direction[i] =
            f->model[i * 4] * r[0] + f->model[i * 4 + 1] * r[1] + f->model[i * 4 + 2] * r[2];
    }
    return 1;
}
static int geometry_hit(const PickFrame *f, double x, double y) {
    double o[3], d[3];
    if (!ray(f, x, y, o, d))
        return -1;
    double a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], best = 1e300;
    int hit = -1;
    for (int i = 0; i < f->count; i++) {
        const PickNode *n = &f->nodes[i];
        double t = -1;
        if (n->shape) {
            double low = 0, high = 1e300;
            for (int k = 0; k < 3; k++) {
                double r = n->radius * .8;
                if (fabs(d[k]) < 1e-12) {
                    if (fabs(o[k] - n->position[k]) > r) {
                        high = -1;
                        break;
                    }
                } else {
                    double t0 = (n->position[k] - r - o[k]) / d[k],
                           t1 = (n->position[k] + r - o[k]) / d[k];
                    if (t0 > t1) {
                        double tmp = t0;
                        t0 = t1;
                        t1 = tmp;
                    }
                    if (t0 > low)
                        low = t0;
                    if (t1 < high)
                        high = t1;
                }
            }
            if (high >= low)
                t = low > 0 ? low : high;
        } else {
            double v[3] = {o[0] - n->position[0], o[1] - n->position[1], o[2] - n->position[2]};
            double b = v[0] * d[0] + v[1] * d[1] + v[2] * d[2],
                   c = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - n->radius * n->radius,
                   disc = b * b - a * c;
            if (disc >= 0) {
                t = (-b - sqrt(disc)) / a;
                if (t <= 0)
                    t = (-b + sqrt(disc)) / a;
            }
        }
        if (t > 0 && t < best) {
            best = t;
            hit = i;
        }
    }
    return hit;
}
int pick_target(const PickFrame *f, double x, double y) {
    if (!f->valid || x < f->viewport[0] || x >= f->viewport[0] + f->viewport[2] ||
        y < f->window_height - f->viewport[1] - f->viewport[3] ||
        y >= f->window_height - f->viewport[1])
        return -1;
    for (int i = f->label_count - 1; i >= 0; i--) {
        const PickLabel *l = &f->labels[i];
        if (x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h)
            return l->target;
    }
    int exact = geometry_hit(f, x, y);
    if (exact >= 0)
        return exact;
    /* Expanded handles remain useful for small nodes, but ignore centers hidden
       by another rendered node. Cap retries in dense overlapping clusters. */
    int candidates[16];
    double scores[16];
    int used = 0;
    for (int i = 0; i < f->count; i++) {
        const PickNode *n = &f->nodes[i];
        double dx = x - n->sx, dy = y - n->sy, d2 = dx * dx + dy * dy,
               r = fmax(8, n->pixel_radius + 3);
        if (d2 > r * r)
            continue;
        double score = d2 + n->eye_depth * 1e-8;
        int at = used;
        while (at > 0 && score < scores[at - 1])
            at--;
        if (at >= 16)
            continue;
        int end = used < 16 ? used++ : 15;
        for (int j = end; j > at; j--) {
            candidates[j] = candidates[j - 1];
            scores[j] = scores[j - 1];
        }
        candidates[at] = i;
        scores[at] = score;
    }
    for (int k = 0; k < used; k++) {
        int i = candidates[k];
        if (geometry_hit(f, f->nodes[i].sx, f->nodes[i].sy) == i)
            return i;
    }
    return -1;
}
int pick_drag_delta(const PickFrame *f, const double anchor[3], double x0, double y0, double x1,
                    double y1, double delta[3]) {
    double o[3], d[3], normal[3] = {f->model[2], f->model[6], f->model[10]}, points[2][3];
    double xy[2][2] = {{x0, y0}, {x1, y1}};
    for (int j = 0; j < 2; j++) {
        if (!ray(f, xy[j][0], xy[j][1], o, d))
            return 0;
        double denominator = 0, numerator = 0;
        for (int k = 0; k < 3; k++) {
            denominator += normal[k] * d[k];
            numerator += normal[k] * (anchor[k] - o[k]);
        }
        if (fabs(denominator) < 1e-12)
            return 0;
        double t = numerator / denominator;
        if (t <= 0)
            return 0;
        for (int k = 0; k < 3; k++)
            points[j][k] = o[k] + t * d[k];
    }
    for (int k = 0; k < 3; k++)
        delta[k] = points[1][k] - points[0][k];
    return 1;
}
int gesture_moved(int x0, int y0, int x, int y) {
    double dx = (double)x - x0, dy = (double)y - y0;
    return dx * dx + dy * dy > 36;
}
int layout_get(const LayoutEditor *e, LayoutKey key, double out[3]) {
    for (int i = 0; i < e->count; i++)
        if (layout_key_equal(e->entries[i].key, key)) {
            memcpy(out, e->entries[i].position, 3 * sizeof(double));
            return 1;
        }
    return 0;
}
int layout_set(LayoutEditor *e, LayoutKey key, const double p[3]) {
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
        return 0;
    for (int i = 0; i < e->count; i++)
        if (layout_key_equal(e->entries[i].key, key)) {
            memcpy(e->entries[i].position, p, 3 * sizeof(double));
            return 1;
        }
    if (e->count == EDITOR_LAYOUTS)
        return 0;
    e->entries[e->count].key = key;
    memcpy(e->entries[e->count++].position, p, 3 * sizeof(double));
    return 1;
}
void layout_remove(LayoutEditor *e, LayoutKey key) {
    for (int i = 0; i < e->count; i++)
        if (layout_key_equal(e->entries[i].key, key)) {
            memmove(e->entries + i, e->entries + i + 1,
                    (size_t)(e->count - i - 1) * sizeof e->entries[0]);
            e->count--;
            return;
        }
}
void layout_commit(LayoutEditor *e, LayoutKey key, int had, const double before[3],
                   const double after[3]) {
    if (e->undo_count == EDITOR_UNDO) {
        memmove(e->undo, e->undo + 1, (EDITOR_UNDO - 1) * sizeof e->undo[0]);
        e->undo_count--;
    }
    PlacementUndo *u = &e->undo[e->undo_count++];
    u->key = key;
    u->had_before = had;
    memcpy(u->before, before, 3 * sizeof(double));
    memcpy(u->after, after, 3 * sizeof(double));
}
int layout_undo(LayoutEditor *e, int kind, uint64_t scope) {
    for (int i = e->undo_count - 1; i >= 0; i--) {
        PlacementUndo u = e->undo[i];
        if (u.key.kind != kind || u.key.scope != scope)
            continue;
        if (u.had_before)
            layout_set(e, u.key, u.before);
        else
            layout_remove(e, u.key);
        memmove(e->undo + i, e->undo + i + 1, (size_t)(e->undo_count - i - 1) * sizeof e->undo[0]);
        e->undo_count--;
        return 1;
    }
    return 0;
}
void layout_reset(LayoutEditor *e, int kind, uint64_t scope) {
    for (int i = 0; i < e->count;) {
        if (e->entries[i].key.kind == kind && e->entries[i].key.scope == scope) {
            memmove(e->entries + i, e->entries + i + 1,
                    (size_t)(e->count - i - 1) * sizeof e->entries[0]);
            e->count--;
        } else
            i++;
    }
    for (int i = 0; i < e->undo_count;) {
        if (e->undo[i].key.kind == kind && e->undo[i].key.scope == scope) {
            memmove(e->undo + i, e->undo + i + 1,
                    (size_t)(e->undo_count - i - 1) * sizeof e->undo[0]);
            e->undo_count--;
        } else
            i++;
    }
}
int interaction_tests(void) {
    PickFrame *f = calloc(1, sizeof *f);
    LayoutEditor *e = calloc(1, sizeof *e);
    if (!f || !e) {
        free(f);
        free(e);
        return 0;
    }
    double model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -10, 1}, projection[16] = {0};
    double scale = 1 / tan(.3926990817);
    projection[0] = scale;
    projection[5] = scale;
    projection[10] = -1.0002;
    projection[11] = -1;
    projection[14] = -.20002;
    int vp[4] = {70, 80, 600, 600};
    pick_frame_begin(f, 6, 123, model, projection, vp, 900);
    double p[3] = {0, 0, 0};
    int a = pick_add(f, (NodeKey){42, 100}, 7, 0, p, .02);
    int ok = a == 0 && pick_target(f, 370, 520) == a && pick_target(f, 376, 520) == a &&
             pick_target(f, 390, 520) == -1 && pick_target(f, 0, 0) == -1;
    p[2] = 1;
    int near = pick_add(f, (NodeKey){43, 101}, 8, 0, p, .3);
    ok = ok && pick_target(f, 370, 520) == near;
    pick_label_add(f, a, 400, 500, 60, 15);
    ok = ok && pick_target(f, 420, 507) == a;
    double delta[3];
    p[0] = p[1] = p[2] = 0;
    ok = ok && pick_drag_delta(f, p, 370, 520, 420, 550, delta);
    double moved[3] = {delta[0], delta[1], delta[2]}, x, y, z, eye;
    ok = ok && pick_project(f, moved, &x, &y, &z, &eye) && fabs(x - 420) < 1e-6 &&
         fabs(y - 550) < 1e-6;
    double cp = cos(.4), sp = sin(.4), cy = cos(.7), sy = sin(.7);
    double rotated[16] = {cy, sp * sy,  -cp * sy, 0, 0, cp, sp,  0,
                          sy, -sp * cy, cp * cy,  0, 0, 0,  -10, 1};
    pick_frame_begin(f, 6, 123, rotated, projection, vp, 900);
    double anchor[3] = {1, 2, -1}, sx, sy_screen, depth, eye_depth;
    ok = ok && pick_project(f, anchor, &sx, &sy_screen, &depth, &eye_depth) &&
         pick_drag_delta(f, anchor, sx, sy_screen, sx + 45, sy_screen - 30, delta);
    for (int k = 0; k < 3; k++)
        moved[k] = anchor[k] + delta[k];
    ok = ok && fabs(delta[2]) > .01 && pick_project(f, moved, &x, &y, &z, &eye) &&
         fabs(x - (sx + 45)) < 1e-6 && fabs(y - (sy_screen - 30)) < 1e-6;
    ok = ok && !gesture_moved(10, 10, 13, 13) && gesture_moved(10, 10, 17, 10);
    LayoutKey key = {6, 123, {42, 100}}, reused = {6, 123, {42, 101}}, other = {7, 123, {42, 100}};
    double out[3];
    ok = ok && layout_set(e, key, moved) && layout_get(e, key, out) &&
         !layout_get(e, reused, out) && !layout_get(e, other, out);
    layout_commit(e, key, 0, p, moved);
    ok = ok && layout_undo(e, 6, 123) && !layout_get(e, key, out);
    layout_set(e, key, moved);
    layout_set(e, other, p);
    layout_reset(e, 6, 123);
    ok = ok && !layout_get(e, key, out) && layout_get(e, other, out);
    ok = ok && node_key_equal(node_path_key("/tmp/a file"), node_path_key("/tmp/a file")) &&
         !node_key_equal(node_path_key("/tmp/a"), node_path_key("/tmp/b"));
    free(e);
    free(f);
    printf("%s: off-center/occluded/label picking, pixel-accurate dragging, click slop, stable "
           "identities and undo\n",
           ok ? "PASS" : "FAIL");
    return ok;
}
