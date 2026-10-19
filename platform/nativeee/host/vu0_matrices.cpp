// platform/nativeee (host side) -- the VU0 matrix half of the game's maths library (include/game/math.h, PS2 bodies
// src/platform/ps2/matrices.cpp: macro-mode asm), as the second seam's imports (tools/second_seam.tsv). PROVISIONAL: plain IEEE arithmetic in the PS2 sequences' order, so the UI's placed sprites and matrices work; the VU0 rounding
// (multiply-adds without guard bits, no NaN/Inf) and the retail-exact conventions are not pinned (the
// second seam). Matrices are four rows of four floats; vectors transform as ROW vectors (p' = p.x·row0 +
// p.y·row1 + p.z·row2 + row3), as the asm's vmulax/vmadday/vmaddaz/vmaddw sequences do.
#include <cmath>
#include <cstring>

#include "ps2e_seam.h"
#include "te_host.h"

namespace {

struct V4 {
    float x, y, z, w;
};
struct M4 {
    V4 r[4];
};

const M4* Mat(uint32_t a) { return reinterpret_cast<const M4*>(te::GuestPointer(a, sizeof(M4))); }
M4* MatW(uint32_t a) { return reinterpret_cast<M4*>(te::GuestPointer(a, sizeof(M4))); }
const V4* Vec(uint32_t a) { return reinterpret_cast<const V4*>(te::GuestPointer(a, sizeof(V4))); }
V4* VecW(uint32_t a) { return reinterpret_cast<V4*>(te::GuestPointer(a, sizeof(V4))); }

V4 Scale(const V4& v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
V4 Add(const V4& a, const V4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }

// p.x·m0 + p.y·m1 + p.z·m2 + w·m3 in the asm's accumulation order
V4 RowCombine(const M4& m, const V4& p, float w4) {
    V4 acc = Scale(m.r[0], p.x);
    acc = Add(acc, Scale(m.r[1], p.y));
    acc = Add(acc, Scale(m.r[2], p.z));
    acc = Add(acc, Scale(m.r[3], w4));
    return acc;
}

}  // namespace

extern "C" {

void ps2e_vu0_init_identity_matrix(ps2e_u32 matrix) {
    M4* m = MatW(matrix);
    if (!m) return;
    *m = M4{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

// VuTransformPoint (MultiplyMatrixByVector): the translation row added once (vf0.w = 1), whatever the point's w
void ps2e_vu0_transform_point(ps2e_u32 matrix, ps2e_u32 point, ps2e_u32 out) {
    const M4* m = Mat(matrix);
    const V4* p = Vec(point);
    V4* o = VecW(out);
    if (!m || !p || !o) return;
    const V4 r = RowCombine(*m, *p, 1.0f);
    *o = r;
}

// VuTransformByRows: the point's own w scales the fourth row
void ps2e_vu0_transform_by_rows(ps2e_u32 rows, ps2e_u32 point, ps2e_u32 out) {
    const M4* m = Mat(rows);
    const V4* p = Vec(point);
    V4* o = VecW(out);
    if (!m || !p || !o) return;
    const V4 r = RowCombine(*m, *p, p->w);
    *o = r;
}

// VuRotateVector (TransformVector_): xyz through the three rotation rows; w stays the vector's
void ps2e_vu0_rotate_vector(ps2e_u32 matrix, ps2e_u32 vector, ps2e_u32 out) {
    const M4* m = Mat(matrix);
    const V4* v = Vec(vector);
    V4* o = VecW(out);
    if (!m || !v || !o) return;
    V4 acc = Scale(m->r[0], v->x);
    acc = Add(acc, Scale(m->r[1], v->y));
    acc = Add(acc, Scale(m->r[2], v->z));
    *o = {acc.x, acc.y, acc.z, v->w};
}

// VuMultiplyMatrices(a, b, out): out = a × b (each row of a through b)
void ps2e_vu0_multiply_matrices(ps2e_u32 a, ps2e_u32 b, ps2e_u32 out) {
    const M4* ma = Mat(a);
    const M4* mb = Mat(b);
    M4* o = MatW(out);
    if (!ma || !mb || !o) return;
    M4 r;
    for (int i = 0; i < 4; ++i) r.r[i] = RowCombine(*mb, ma->r[i], ma->r[i].w);
    *o = r;
}

// MultiplyRotations (RotateVecByVec): the quaternion product -- a.w·b + b.w·a + a × b; w = a.w·b.w − a·b
void ps2e_vu0_multiply_rotations(ps2e_u32 out, ps2e_u32 a, ps2e_u32 b) {
    const V4* qa = Vec(a);
    const V4* qb = Vec(b);
    V4* o = VecW(out);
    if (!qa || !qb || !o) return;
    const V4 A = *qa, B = *qb;
    V4 r;
    r.x = (A.y * B.z - A.z * B.y) + A.x * B.w + B.x * A.w;
    r.y = (A.z * B.x - A.x * B.z) + A.y * B.w + B.y * A.w;
    r.z = (A.x * B.y - A.y * B.x) + A.z * B.w + B.z * A.w;
    r.w = A.w * B.w - A.z * B.z - A.y * B.y - A.x * B.x;
    *o = r;
}

// MatrixFromRotation (Rotate_): a unit quaternion's three rotation rows (w 0); the fourth row is left as it was
void ps2e_vu0_matrix_from_rotation(ps2e_u32 matrix, ps2e_u32 rotation) {
    M4* m = MatW(matrix);
    const V4* q = Vec(rotation);
    if (!m || !q) return;
    const float x = q->x, y = q->y, z = q->z, w = q->w;
    const float xx = 2 * x * x, yy = 2 * y * y, zz = 2 * z * z;
    const float xy = 2 * x * y, xz = 2 * x * z, yz = 2 * y * z, wx = 2 * w * x, wy = 2 * w * y, wz = 2 * w * z;
    m->r[0] = {1 - yy - zz, xy + wz, xz - wy, 0};
    m->r[1] = {xy - wz, 1 - xx - zz, yz + wx, 0};
    m->r[2] = {xz + wy, yz - wx, 1 - xx - yy, 0};
}

// VuInvertRigid: the rotation transposed, the translation turned by it and negated, w 1
void ps2e_vu0_invert_rigid(ps2e_u32 out, ps2e_u32 matrix) {
    const M4* m = Mat(matrix);
    M4* o = MatW(out);
    if (!m || !o) return;
    const M4 s = *m;
    M4 r;
    r.r[0] = {s.r[0].x, s.r[1].x, s.r[2].x, 0};
    r.r[1] = {s.r[0].y, s.r[1].y, s.r[2].y, 0};
    r.r[2] = {s.r[0].z, s.r[1].z, s.r[2].z, 0};
    const V4 t = {-s.r[3].x, -s.r[3].y, -s.r[3].z, 1};
    V4 acc = Scale(r.r[0], t.x);
    acc = Add(acc, Scale(r.r[1], t.y));
    acc = Add(acc, Scale(r.r[2], t.z));
    r.r[3] = {acc.x, acc.y, acc.z, 1};
    *o = r;
}

void ps2e_vu0_invert_rigid_in_place(ps2e_u32 matrix) { ps2e_vu0_invert_rigid(matrix, matrix); }

// PlaneThroughTriangle: the unit normal of the edges' cross product, w minus the first vertex's distance along it;
// whether the cross product was longer than 5e-05
ps2e_u32 ps2e_vu0_plane_through_triangle(ps2e_u32 plane, ps2e_u32 first, ps2e_u32 second, ps2e_u32 third) {
    V4* o = VecW(plane);
    const V4* a = Vec(first);
    const V4* b = Vec(second);
    const V4* c = Vec(third);
    if (!o || !a || !b || !c) return 0;
    const V4 e1 = {b->x - a->x, b->y - a->y, b->z - a->z, 0};
    const V4 e2 = {c->x - a->x, c->y - a->y, c->z - a->z, 0};
    V4 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x, 0};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    const float inv = length != 0.f ? 1.0f / length : 0.f;
    n = {n.x * inv, n.y * inv, n.z * inv, 0};
    const float d = n.x * a->x + n.y * a->y + n.z * a->z;
    *o = {n.x, n.y, n.z, -d};
    return length > 5e-05f ? 1u : 0u;
}

}  // extern "C"
