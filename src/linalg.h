// linalg.h — small, header-only linear algebra for xr-workspace.
// Column-major 4×4 matrices and quaternions. All functions inline; no TU cost.
// (Named linalg, not math, so it never shadows the C standard <math.h>.)
#pragma once

#include <cmath>

struct Quat { float x, y, z, w; };

struct Mat4 {
    float m[16]; // column-major
    static Mat4 identity() {
        Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.f; return r;
    }
};

// C = A * B (column-major 4×4)
static inline Mat4 mat4_mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int col = 0; col < 4; col++) {
        const float *bc = &b.m[col*4];
        float *rc = &r.m[col*4];
        for (int row = 0; row < 4; row++) {
            rc[row] = a.m[0*4+row]*bc[0] + a.m[1*4+row]*bc[1]
                    + a.m[2*4+row]*bc[2] + a.m[3*4+row]*bc[3];
        }
    }
    return r;
}

static inline Mat4 quat_to_mat4(Quat q) {
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    Mat4 r = Mat4::identity();
    r.m[0]  = 1.f-2.f*(y*y+z*z);  r.m[4]  =     2.f*(x*y-z*w);  r.m[8]  =     2.f*(x*z+y*w);
    r.m[1]  =     2.f*(x*y+z*w);  r.m[5]  = 1.f-2.f*(x*x+z*z);  r.m[9]  =     2.f*(y*z-x*w);
    r.m[2]  =     2.f*(x*z-y*w);  r.m[6]  =     2.f*(y*z+x*w);  r.m[10] = 1.f-2.f*(x*x+y*y);
    return r;
}

// Inverse of a pure rotation matrix = transpose of the 3×3 block
static inline Mat4 mat4_rot_inverse(const Mat4 &m) {
    Mat4 r = Mat4::identity();
    r.m[0]=m.m[0]; r.m[4]=m.m[1]; r.m[8] =m.m[2];
    r.m[1]=m.m[4]; r.m[5]=m.m[5]; r.m[9] =m.m[6];
    r.m[2]=m.m[8]; r.m[6]=m.m[9]; r.m[10]=m.m[10];
    return r;
}

// Translation matrix (column-major).
static inline Mat4 mat4_translate(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

static inline Mat4 mat4_perspective(float fovy_rad, float aspect, float near, float far) {
    Mat4 r{};
    const float f = 1.f / tanf(fovy_rad * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.f;
    r.m[14] = (2.f * far * near) / (near - far);
    return r;
}

static inline Quat quat_mul(Quat a, Quat b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

static inline Quat quat_conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

static inline Quat quat_normalize(Quat q) {
    const float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (n <= 1e-8f) return {0.f, 0.f, 0.f, 1.f};
    const float inv = 1.f / n;
    return {q.x*inv, q.y*inv, q.z*inv, q.w*inv};
}

// Spherical-linear interpolation, used for optional pose smoothing.
static inline Quat quat_slerp(Quat a, Quat b, float t) {
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (dot < 0.f) { b = {-b.x, -b.y, -b.z, -b.w}; dot = -dot; }
    if (dot > 0.9995f) { // nearly parallel → linear, then normalize
        Quat r = { a.x+t*(b.x-a.x), a.y+t*(b.y-a.y),
                   a.z+t*(b.z-a.z), a.w+t*(b.w-a.w) };
        return quat_normalize(r);
    }
    const float theta0 = acosf(dot);
    const float theta  = theta0 * t;
    const float s0 = cosf(theta) - dot * sinf(theta) / sinf(theta0);
    const float s1 = sinf(theta) / sinf(theta0);
    return { a.x*s0 + b.x*s1, a.y*s0 + b.y*s1,
             a.z*s0 + b.z*s1, a.w*s0 + b.w*s1 };
}
