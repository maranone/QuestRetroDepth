#pragma once
#include <cmath>
#include <openxr/openxr.h>

// Column-major 4×4 float matrix (matches glUniformMatrix4fv with transpose=GL_FALSE).
// Layout: m[col][row], flat: m[col*4+row]
struct Mat4 {
    float m[16] = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1   // identity
    };

    const float* data() const { return m; }

    static Mat4 identity() {
        return Mat4{};
    }

    // Column-major: m[col*4 + row]
    static Mat4 mul(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                    s += a.m[k * 4 + row] * b.m[col * 4 + k];
                r.m[col * 4 + row] = s;
            }
        }
        return r;
    }

    // Build a view matrix from an XrPosef (camera position + orientation in world space).
    // Result transforms world coords into camera/eye space.
    static Mat4 view_from_pose(const XrPosef& pose) {
        const float qx = pose.orientation.x;
        const float qy = pose.orientation.y;
        const float qz = pose.orientation.z;
        const float qw = pose.orientation.w;
        const float px = pose.position.x;
        const float py = pose.position.y;
        const float pz = pose.position.z;

        // Rotation matrix rows from quaternion
        const float r00 = 1.0f - 2.0f * (qy*qy + qz*qz);
        const float r01 = 2.0f * (qx*qy - qw*qz);
        const float r02 = 2.0f * (qx*qz + qw*qy);
        const float r10 = 2.0f * (qx*qy + qw*qz);
        const float r11 = 1.0f - 2.0f * (qx*qx + qz*qz);
        const float r12 = 2.0f * (qy*qz - qw*qx);
        const float r20 = 2.0f * (qx*qz - qw*qy);
        const float r21 = 2.0f * (qy*qz + qw*qx);
        const float r22 = 1.0f - 2.0f * (qx*qx + qy*qy);

        // V = R^T * T(-P), stored column-major.
        // R^T col j = R row j → each column of V's rotation block is a ROW of R.
        // Translation = -R^T * P = -(R col_i dot P) for each component i.
        Mat4 v;
        // col 0 = R^T col 0 = R row 0
        v.m[ 0] = r00;  v.m[ 1] = r01;  v.m[ 2] = r02;  v.m[ 3] = 0;
        // col 1 = R^T col 1 = R row 1
        v.m[ 4] = r10;  v.m[ 5] = r11;  v.m[ 6] = r12;  v.m[ 7] = 0;
        // col 2 = R^T col 2 = R row 2
        v.m[ 8] = r20;  v.m[ 9] = r21;  v.m[10] = r22;  v.m[11] = 0;
        // col 3 = -R^T * P = -(R col_i dot P)
        v.m[12] = -(r00*px + r10*py + r20*pz);
        v.m[13] = -(r01*px + r11*py + r21*pz);
        v.m[14] = -(r02*px + r12*py + r22*pz);
        v.m[15] = 1;
        return v;
    }

    // Build a model matrix (local → world) from an XrPosef.
    // Inverse of view_from_pose: no transpose on the rotation block, translation in col 3.
    static Mat4 from_pose(const XrPosef& pose) {
        const float qx = pose.orientation.x, qy = pose.orientation.y;
        const float qz = pose.orientation.z, qw = pose.orientation.w;
        const float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
        const float r00 = 1-2*(qy*qy+qz*qz), r10 = 2*(qx*qy+qw*qz), r20 = 2*(qx*qz-qw*qy);
        const float r01 = 2*(qx*qy-qw*qz),   r11 = 1-2*(qx*qx+qz*qz), r21 = 2*(qy*qz+qw*qx);
        const float r02 = 2*(qx*qz+qw*qy),   r12 = 2*(qy*qz-qw*qx),   r22 = 1-2*(qx*qx+qy*qy);
        Mat4 m;
        m.m[ 0]=r00; m.m[ 1]=r10; m.m[ 2]=r20; m.m[ 3]=0;
        m.m[ 4]=r01; m.m[ 5]=r11; m.m[ 6]=r21; m.m[ 7]=0;
        m.m[ 8]=r02; m.m[ 9]=r12; m.m[10]=r22; m.m[11]=0;
        m.m[12]=px;  m.m[13]=py;  m.m[14]=pz;  m.m[15]=1;
        return m;
    }

    // Build a projection matrix from OpenXR fov angles.
    // Standard asymmetric frustum, z maps to [-1, +1] (OpenGL convention).
    static Mat4 proj_from_fov(const XrFovf& fov, float near_z, float far_z) {
        const float tl = tanf(fov.angleLeft);
        const float tr = tanf(fov.angleRight);
        const float td = tanf(fov.angleDown);
        const float tu = tanf(fov.angleUp);
        const float tw = tr - tl;
        const float th = tu - td;
        const float inv_d = 1.0f / (near_z - far_z);

        Mat4 p;
        p.m[ 0] = 2.0f / tw;           p.m[ 1] = 0;                   p.m[ 2] = 0;                          p.m[ 3] = 0;
        p.m[ 4] = 0;                    p.m[ 5] = 2.0f / th;           p.m[ 6] = 0;                          p.m[ 7] = 0;
        p.m[ 8] = (tr + tl) / tw;      p.m[ 9] = (tu + td) / th;      p.m[10] = (far_z + near_z) * inv_d;  p.m[11] = -1;
        p.m[12] = 0;                    p.m[13] = 0;                    p.m[14] = 2.0f * far_z * near_z * inv_d; p.m[15] = 0;
        return p;
    }
};
