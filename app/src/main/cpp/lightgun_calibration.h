#pragma once

#include <openxr/openxr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace qrd {

// The lightgun surface is the same parametric surface that the immersive
// renderer draws for the reference game layer.  Keeping this description and
// the ray intersection here prevents the input path from silently falling
// back to a different (flat) screen when the visual screen is curved/tilted.
struct LightgunSurface {
    XrVector3f center = {0.0f, 0.0f, -1.5f};
    XrVector3f right  = {1.0f, 0.0f, 0.0f};
    XrVector3f up     = {0.0f, 1.0f, 0.0f};
    XrVector3f normal = {0.0f, 0.0f, -1.0f};
    float width  = 2.56f;
    float height = 1.44f;
    // Renderer shader value: screen_curve * width * 0.18 * edge^2.
    float screen_curve = 0.0f;
};

struct LightgunUv {
    float u = 0.5f;
    float v = 0.5f;
};

// Calibration measures the controller's raw aim error, so its capture region
// is intentionally larger than the visible game canvas.  The renderer does
// not need to draw this continuation: the raycast already evaluates the same
// flat/curved screen surface mathematically.  Keep the margin finite so a
// wildly mis-aimed trigger pull cannot create an unstable profile.
inline bool lightgun_calibration_uv_captureable(const LightgunUv& uv) {
    constexpr float kCaptureMargin = 0.5f; // visible canvas plus 50% on each side
    return std::isfinite(uv.u) && std::isfinite(uv.v) &&
           uv.u >= -kCaptureMargin && uv.u <= 1.0f + kCaptureMargin &&
           uv.v >= -kCaptureMargin && uv.v <= 1.0f + kCaptureMargin;
}

inline LightgunUv lightgun_calibration_target(int index) {
    static constexpr LightgunUv targets[5] = {
        {0.10f, 0.10f}, {0.90f, 0.10f}, {0.90f, 0.90f},
        {0.10f, 0.90f}, {0.50f, 0.50f}
    };
    return (index >= 0 && index < 5) ? targets[index] : LightgunUv{};
}

inline XrVector3f lightgun_add(const XrVector3f& a, const XrVector3f& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline XrVector3f lightgun_scale(const XrVector3f& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}

inline float lightgun_dot(const XrVector3f& a, const XrVector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline XrVector3f lightgun_surface_point(const LightgunSurface& surface, float u, float v) {
    const float x = (u - 0.5f) * surface.width;
    const float y = (0.5f - v) * surface.height;
    const float edge = 2.0f * u - 1.0f;
    const float curve = surface.screen_curve * surface.width * 0.18f * edge * edge;
    return lightgun_add(surface.center,
           lightgun_add(lightgun_scale(surface.right, x),
           lightgun_add(lightgun_scale(surface.up, y),
                        lightgun_scale(surface.normal, curve))));
}

// Intersects a controller ray with the curved screen.  The curve is quadratic
// in u, so the 3-D ray/surface intersection reduces to a quadratic in t.  The
// returned UV is deliberately not clamped; callers can preserve off-screen
// lightgun shots while clamping only the backend's integer coordinates.
inline bool lightgun_raycast(const XrVector3f& origin,
                             const XrVector3f& direction,
                             const LightgunSurface& surface,
                             LightgunUv& uv_out,
                             bool& offscreen_out) {
    if (surface.width <= 0.0001f || surface.height <= 0.0001f) return false;

    const XrVector3f rel = {origin.x - surface.center.x,
                            origin.y - surface.center.y,
                            origin.z - surface.center.z};
    const float r0 = lightgun_dot(rel, surface.right);
    const float rd = lightgun_dot(direction, surface.right);
    const float n0 = lightgun_dot(rel, surface.normal);
    const float nd = lightgun_dot(direction, surface.normal);
    const float curve_k = surface.screen_curve * surface.width * 0.18f;
    const float a0 = 2.0f * r0 / surface.width;
    const float a1 = 2.0f * rd / surface.width;

    // n0 + t*nd = curve_k * (a0 + t*a1)^2
    const float qa = -curve_k * a1 * a1;
    const float qb = nd - 2.0f * curve_k * a0 * a1;
    const float qc = n0 - curve_k * a0 * a0;
    float t = std::numeric_limits<float>::infinity();

    if (std::abs(qa) < 1.0e-7f) {
        if (std::abs(qb) > 1.0e-7f) {
            const float candidate = -qc / qb;
            if (candidate > 0.01f) t = candidate;
        }
    } else {
        const float disc = qb * qb - 4.0f * qa * qc;
        if (disc >= 0.0f) {
            const float root = std::sqrt(disc);
            const float t0 = (-qb - root) / (2.0f * qa);
            const float t1 = (-qb + root) / (2.0f * qa);
            if (t0 > 0.01f) t = std::min(t, t0);
            if (t1 > 0.01f) t = std::min(t, t1);
        }
    }

    // A nearly edge-on or numerically degenerate curved solve can miss even
    // though the flat surface would have a usable hit.  This is also the
    // exact path for a non-curved screen.
    if (!std::isfinite(t) && std::abs(nd) > 0.001f) {
        const float flat_t = -n0 / nd;
        if (flat_t > 0.01f) t = flat_t;
    }
    if (!std::isfinite(t)) return false;

    const XrVector3f hit = {origin.x + direction.x * t,
                            origin.y + direction.y * t,
                            origin.z + direction.z * t};
    const XrVector3f hit_rel = {hit.x - surface.center.x,
                                hit.y - surface.center.y,
                                hit.z - surface.center.z};
    uv_out.u = 0.5f + lightgun_dot(hit_rel, surface.right) / surface.width;
    uv_out.v = 0.5f - lightgun_dot(hit_rel, surface.up) / surface.height;
    offscreen_out = uv_out.u < 0.0f || uv_out.u > 1.0f ||
                    uv_out.v < 0.0f || uv_out.v > 1.0f;
    return true;
}

// Five landmarks are mapped with four affine triangles sharing the measured
// center.  This exactly honors all five captured points and remains stable
// when the controller's optical error is not well represented by one global
// homography.  The shared edges use identical barycentric coordinates, so the
// mapping is continuous across the whole screen.
struct LightgunCalibrationProfile {
    static constexpr int kPointCount = 5;
    std::array<LightgunUv, kPointCount> raw{}; // TL, TR, BR, BL, center
    int hand = 0;
    int backend = 0;
    int frame_width = 0;
    int frame_height = 0;
    int upscale_mode = 0;
    float canvas_x = 0.0f;
    float canvas_y = 0.0f;
    float canvas_az = 0.0f;
    float canvas_el = 0.0f;
    float canvas_scale = 1.0f;
    float screen_curve = 0.0f;
    float tilt_x = 0.0f;
    float tilt_y = 0.0f;
    float world_scale = 1.0f;
    float world_forward_offset = 0.0f;

    bool matches(int hand_in, int backend_in, int width_in, int height_in,
                 int upscale_in, float x, float y, float az, float el,
                 float scale, float curve, float tx, float ty,
                 float world_scale_in, float world_offset_in) const {
        auto close = [](float a, float b) { return std::abs(a - b) <= 0.006f; };
        return hand == hand_in && backend == backend_in &&
               frame_width == width_in && frame_height == height_in &&
               upscale_mode == upscale_in && close(canvas_x, x) && close(canvas_y, y) &&
               close(canvas_az, az) && close(canvas_el, el) && close(canvas_scale, scale) &&
               close(screen_curve, curve) && close(tilt_x, tx) && close(tilt_y, ty) &&
               close(world_scale, world_scale_in) && close(world_forward_offset, world_offset_in);
    }
};

inline bool lightgun_barycentric(const LightgunUv& p,
                                 const LightgunUv& a,
                                 const LightgunUv& b,
                                 const LightgunUv& c,
                                 float& wa, float& wb, float& wc) {
    const float den = (b.v - c.v) * (a.u - c.u) +
                      (c.u - b.u) * (a.v - c.v);
    if (std::abs(den) < 1.0e-6f) return false;
    wa = ((b.v - c.v) * (p.u - c.u) + (c.u - b.u) * (p.v - c.v)) / den;
    wb = ((c.v - a.v) * (p.u - c.u) + (a.u - c.u) * (p.v - c.v)) / den;
    wc = 1.0f - wa - wb;
    return true;
}

inline LightgunUv lightgun_apply_calibration(const LightgunCalibrationProfile& profile,
                                              const LightgunUv& raw) {
    static constexpr int triangles[4][3] = {
        {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}
    };
    int best = 0;
    float best_score = -std::numeric_limits<float>::infinity();
    float best_w0 = 0.0f, best_w1 = 0.0f, best_w2 = 0.0f;
    for (int i = 0; i < 4; ++i) {
        float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f;
        if (!lightgun_barycentric(raw,
                                  profile.raw[triangles[i][0]],
                                  profile.raw[triangles[i][1]],
                                  profile.raw[triangles[i][2]],
                                  w0, w1, w2)) continue;
        const float score = std::min(w0, std::min(w1, w2));
        if (score > best_score) {
            best_score = score;
            best = i;
            best_w0 = w0; best_w1 = w1; best_w2 = w2;
        }
        if (score >= -0.0001f) break;
    }
    const int* tri = triangles[best];
    return {
        best_w0 * lightgun_calibration_target(tri[0]).u +
            best_w1 * lightgun_calibration_target(tri[1]).u +
            best_w2 * lightgun_calibration_target(tri[2]).u,
        best_w0 * lightgun_calibration_target(tri[0]).v +
            best_w1 * lightgun_calibration_target(tri[1]).v +
            best_w2 * lightgun_calibration_target(tri[2]).v
    };
}

inline bool lightgun_profile_valid(const LightgunCalibrationProfile& profile) {
    if (profile.hand < 1 || profile.frame_width <= 0 || profile.frame_height <= 0) return false;
    for (const LightgunUv& p : profile.raw) {
        if (!std::isfinite(p.u) || !std::isfinite(p.v)) return false;
    }
    // Every triangle must have area. This also rejects captures where the
    // trigger was pulled without the aim ray being tracked.
    static constexpr int triangles[4][3] = {{0,1,4},{1,2,4},{2,3,4},{3,0,4}};
    for (const auto& tri : triangles) {
        float a = 0.0f, b = 0.0f, c = 0.0f;
        if (!lightgun_barycentric(profile.raw[tri[0]], profile.raw[tri[0]],
                                  profile.raw[tri[1]], profile.raw[tri[2]], a, b, c)) return false;
    }
    return true;
}

} // namespace qrd
