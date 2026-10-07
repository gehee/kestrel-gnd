#pragma once
// Just enough quaternion and 3x3 maths for the stabilizer. Rotations are (w, x, y, z);
// a "rotation vector" is axis * angle in radians.
#include <cmath>

namespace stab {

struct Quat {
    double w = 1, x = 0, y = 0, z = 0;
};

inline Quat qmul(const Quat& a, const Quat& b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

inline Quat qconj(const Quat& q) { return { q.w, -q.x, -q.y, -q.z }; }

inline Quat qnorm(Quat q) {
    double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-12) return {};
    return { q.w / n, q.x / n, q.y / n, q.z / n };
}

// exp of a rotation vector
inline Quat qexp(const double v[3]) {
    double a = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (a < 1e-12) return { 1, v[0] * 0.5, v[1] * 0.5, v[2] * 0.5 };
    double s = std::sin(a * 0.5) / a;
    return { std::cos(a * 0.5), v[0] * s, v[1] * s, v[2] * s };
}

// log of a rotation, as a rotation vector
inline void qlog(const Quat& q, double out[3]) {
    double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-12) { out[0] = out[1] = out[2] = 0; return; }
    double a = 2.0 * std::atan2(s, q.w);
    if (a > 3.14159265358979) a -= 2.0 * 3.14159265358979;     // the short way round
    double k = a / s;
    out[0] = q.x * k; out[1] = q.y * k; out[2] = q.z * k;
}

// a followed by t of the way to b
inline Quat qslerp(const Quat& a, const Quat& b, double t) {
    double d[3];
    qlog(qmul(qconj(a), b), d);
    d[0] *= t; d[1] *= t; d[2] *= t;
    return qnorm(qmul(a, qexp(d)));
}

// row-major 3x3 of a rotation
inline void qmat(const Quat& q, double m[9]) {
    double w = q.w, x = q.x, y = q.y, z = q.z;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// out = a^T * b (all row-major 3x3)
inline void mat_tmul(const double a[9], const double b[9], double out[9]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i * 3 + j] = a[0 * 3 + i] * b[0 * 3 + j] + a[1 * 3 + i] * b[1 * 3 + j] + a[2 * 3 + i] * b[2 * 3 + j];
}

}  // namespace stab
