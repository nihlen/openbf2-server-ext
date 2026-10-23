// Yaw, pitch and roll in docs/protocol.md's convention, to and from the rotation rows of an engine
// matrix (row-major, rows right/up/forward/position). The same convention as Python's
// getRotation/setRotation (docs/findings/admin-phase1.md, Framing and conventions). Engine-free.

#ifndef SERVEREXT_ROTATION_H
#define SERVEREXT_ROTATION_H

#include <cmath>

namespace serverext {

// ypr from m[0..10]: yaw atan2(m8, m10), pitch -asin(m9), roll atan2(m1, m5), degrees.
inline void attitudeFromMatrix(const float m[16], float ypr[3])
{
    const float r2d = 57.29577951f;
    float s = m[9];
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    ypr[0] = std::atan2(m[8], m[10]) * r2d;
    ypr[1] = -std::asin(s) * r2d;
    ypr[2] = std::atan2(m[1], m[5]) * r2d;
}

// Writes the rotation rows m[0..2], m[4..6], m[8..10] for ypr and leaves the rest of m alone. It is
// the engine's setRotationZXY(m, -roll, -pitch, -yaw) (Math/Mat4.cpp): Rz * Rx * Ry.
inline void matrixFromAttitude(const float ypr[3], float m[16])
{
    const float d2r = 0.0174532924f;
    const float z = -ypr[2] * d2r, x = -ypr[1] * d2r, y = -ypr[0] * d2r;
    const float cz = std::cos(z), sz = std::sin(z);
    const float cx = std::cos(x), sx = std::sin(x);
    const float cy = std::cos(y), sy = std::sin(y);
    // Rz = [cz -sz 0; sz cz 0; 0 0 1], Rx = [1 0 0; 0 cx -sx; 0 sx cx], Ry = [cy 0 sy; 0 1 0; -sy 0 cy]
    const float rx[9] = { cy, 0.0f, sy,  sx * sy, cx, -sx * cy,  -cx * sy, sx, cx * cy };   // Rx * Ry
    const float r[9] = {
        cz * rx[0] - sz * rx[3], cz * rx[1] - sz * rx[4], cz * rx[2] - sz * rx[5],
        sz * rx[0] + cz * rx[3], sz * rx[1] + cz * rx[4], sz * rx[2] + cz * rx[5],
        rx[6], rx[7], rx[8] };
    m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
    m[4] = r[3]; m[5] = r[4]; m[6] = r[5];
    m[8] = r[6]; m[9] = r[7]; m[10] = r[8];
}

} // namespace serverext

#endif // SERVEREXT_ROTATION_H
