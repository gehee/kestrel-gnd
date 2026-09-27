
#ifndef MATH_UTILS_H
#define MATH_UTILS_H

#include <cmath>
#include <cstring>

namespace math {

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;
};

typedef float Mat4[16];

inline void perspective(Mat4 m, float fov, float aspect, float near, float far) {
    float f = 1.0f / tan(fov / 2.0f);
    memset(m, 0, sizeof(Mat4));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1.0f;
    m[14] = (2.0f * far * near) / (near - far);
}

inline void lookAt(Mat4 m, Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = {center.x - eye.x, center.y - eye.y, center.z - eye.z};
    float lf = sqrt(f.x*f.x + f.y*f.y + f.z*f.z);
    f.x /= lf; f.y /= lf; f.z /= lf;

    Vec3 s = {f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x};
    float ls = sqrt(s.x*s.x + s.y*s.y + s.z*s.z);
    s.x /= ls; s.y /= ls; s.z /= ls;

    Vec3 u = {s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x};

    memset(m, 0, sizeof(Mat4));
    m[0] = s.x; m[4] = s.y; m[8] = s.z;
    m[1] = u.x; m[5] = u.y; m[9] = u.z;
    m[2] = -f.x; m[6] = -f.y; m[10] = -f.z;
    m[12] = -(s.x*eye.x + s.y*eye.y + s.z*eye.z);
    m[13] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
    m[14] = f.x*eye.x + f.y*eye.y + f.z*eye.z;
    m[15] = 1.0f;
}

inline void rotateY(Mat4 m, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    memset(m, 0, sizeof(Mat4));
    m[0] = c;  m[8] = s;
    m[5] = 1;
    m[2] = -s; m[10] = c;
    m[15] = 1;
}

inline void rotateZ(Mat4 m, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    memset(m, 0, sizeof(Mat4));
    m[0] = c;  m[4] = -s;
    m[1] = s;  m[5] = c;
    m[10] = 1;
    m[15] = 1;
}

inline void multiply(Mat4 res, const Mat4 a, const Mat4 b) {
    Mat4 tmp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                             a[i * 4 + 1] * b[1 * 4 + j] +
                             a[i * 4 + 2] * b[2 * 4 + j] +
                             a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
    memcpy(res, tmp, sizeof(Mat4));
}

}

#endif
