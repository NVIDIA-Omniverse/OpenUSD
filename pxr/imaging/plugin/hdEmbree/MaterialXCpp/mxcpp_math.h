//
// MaterialXCpp math types — pxr-independent, Imath-based.
//
#ifndef MXCPP_MATH_H
#define MXCPP_MATH_H

#include <Imath/ImathVec.h>
#include <Imath/ImathMatrix.h>
#include <cmath>

namespace mxcpp {

using Vec2f = Imath::V2f;
using Vec3f = Imath::V3f;
using Vec4f = Imath::V4f;
using Mat3f = Imath::M33f;
using Mat4f = Imath::M44f;

inline float Dot(const Vec2f& a, const Vec2f& b) { return a.dot(b); }
inline float Dot(const Vec3f& a, const Vec3f& b) { return a.dot(b); }

inline Vec3f Cross(const Vec3f& a, const Vec3f& b) { return a.cross(b); }

inline Vec2f CompMult(const Vec2f& a, const Vec2f& b) {
    return Vec2f(a[0]*b[0], a[1]*b[1]);
}
inline Vec3f CompMult(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a[0]*b[0], a[1]*b[1], a[2]*b[2]);
}
inline Vec4f CompMult(const Vec4f& a, const Vec4f& b) {
    return Vec4f(a[0]*b[0], a[1]*b[1], a[2]*b[2], a[3]*b[3]);
}

inline void BuildOrthonormalFrame(const Vec3f& n, Vec3f* t, Vec3f* b) {
    Vec3f helper = (std::abs(n[0]) < 0.9f)
        ? Vec3f(1, 0, 0) : Vec3f(0, 1, 0);
    *t = n.cross(helper).normalized();
    *b = n.cross(*t);
}

} // namespace mxcpp

#endif // MXCPP_MATH_H
