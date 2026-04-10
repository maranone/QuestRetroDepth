#pragma once
// -----------------------------------------------------------------------
// VrState share-code: compact alphanumeric string (0-9, A-Z, case-insensitive).
//
// FORMAT:
//   Chars 0-4  — base block:
//     [0]  all 5 bools packed: layers_3d(1)+upscale(2)+ambilight(4)+passthrough(8)+depthmap(16)
//              → value 0–31, fits in base-36
//     [1]  gamma       [0.5..2.0] step 0.1  → base-36 index (0–15)
//     [2]  contrast    [0.5..2.0] step 0.1  → base-36 index (0–15)
//     [3]  saturation  [0.0..2.0] step 0.1  → base-36 index (0–20)
//     [4]  brightness  [0.5..2.0] step 0.1  → base-36 index (0–15)
//
//   Optional Char 5 — SNES layer-filter mode (0-3) when present.
//   Remaining chars — per-layer block (2 chars per layer, N layers):
//     [0]  layer_order index  0–35          → base-36
//     [1]  flags: enabled(1) + ambilight(2) → 0–3
//
// Total length = 5 + 2*N, or 6 + 2*N when the optional filter-mode char is present.
// -----------------------------------------------------------------------

#include "vr_state.h"
#include "game_config.h"
#include <string>
#include <vector>
#include <cmath>
#include <cctype>

namespace qrd {

static inline char int_to_b36(int v) {
    if (v < 10) return (char)('0' + v);
    return (char)('A' + v - 10);
}

static inline int b36_to_int(char c) {
    c = (char)toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static inline int float_to_idx(float v, float lo, float step) {
    int idx = (int)std::round((v - lo) / step);
    if (idx < 0) idx = 0;
    return idx;
}

static inline float idx_to_float(int idx, float lo, float step, float hi) {
    float v = lo + idx * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static inline std::string vr_state_encode(
    const VrState& vs,
    const GameConfig*        cfg   = nullptr,
    const std::vector<int>*  order = nullptr,
    const std::vector<bool>* ena   = nullptr,
    const std::vector<bool>* amb   = nullptr,
    int layer_filter_mode = -1)
{
    char base[6];
    int flags = (vs.layers_3d  ?  1 : 0)
              | (vs.upscale    ?  2 : 0)
              | (vs.ambilight  ?  4 : 0)
              | (vs.shadows    ?  8 : 0)
              | (vs.depthmap   ? 16 : 0);
    base[0] = int_to_b36(flags);
    base[1] = int_to_b36(float_to_idx(vs.gamma,      0.5f, 0.1f));
    base[2] = int_to_b36(float_to_idx(vs.contrast,   0.5f, 0.1f));
    base[3] = int_to_b36(float_to_idx(vs.saturation, 0.0f, 0.1f));
    base[4] = int_to_b36(float_to_idx(vs.brightness, 0.5f, 0.1f));
    base[5] = '\0';

    std::string code(base);

    if (!cfg) return code;

    if (layer_filter_mode >= 0)
        code += int_to_b36(layer_filter_mode);

    int n = (int)cfg->layers.size();
    for (int i = 0; i < n; ++i) {
        int ord = (order && i < (int)order->size()) ? (*order)[i] : i;
        ord = std::max(0, std::min(35, ord));

        bool is_ena = (!ena || i >= (int)ena->size()) ? true : (*ena)[i];
        bool is_amb = (!amb || i >= (int)amb->size()) ? true : (*amb)[i];
        int lflags = (is_ena ? 1 : 0) | (is_amb ? 2 : 0);

        code += int_to_b36(ord);
        code += int_to_b36(lflags);
    }

    return code;
}

static inline bool vr_state_decode(
    const std::string& raw,
    VrState& vs,
    GameConfig*          cfg   = nullptr,
    std::vector<int>*    order = nullptr,
    std::vector<bool>*   ena   = nullptr,
    std::vector<bool>*   amb   = nullptr,
    int* layer_filter_mode = nullptr)
{
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '-' || c == '_') continue;
        s += (char)toupper((unsigned char)c);
    }

    for (char c : s)
        if (!isalnum((unsigned char)c)) return false;

    int n = cfg ? (int)cfg->layers.size() : 0;
    const int expected_old = 5 + 2 * n;
    const int expected_new = 6 + 2 * n;
    const bool has_filter_mode = layer_filter_mode && (int)s.size() == expected_new;
    if ((int)s.size() != expected_old && !has_filter_mode) return false;

    int gc      = b36_to_int(s[0]); if (gc < 0 || gc > 31) return false;
    int i_gamma = b36_to_int(s[1]); if (i_gamma < 0)        return false;
    int i_con   = b36_to_int(s[2]); if (i_con   < 0)        return false;
    int i_sat   = b36_to_int(s[3]); if (i_sat   < 0)        return false;
    int i_bri   = b36_to_int(s[4]); if (i_bri   < 0)        return false;

    vs.immersive_beta_enabled = false;
    vs.layers_3d    = (gc &  1) != 0;
    vs.upscale      = (gc &  2) != 0;
    vs.ambilight    = (gc &  4) != 0;
    vs.shadows      = (gc &  8) != 0;
    vs.depthmap     = (gc & 16) != 0;
    vs.solid_stack  = false;
    vs.gamma        = idx_to_float(i_gamma, 0.5f, 0.1f, 2.0f);
    vs.contrast     = idx_to_float(i_con,   0.5f, 0.1f, 2.0f);
    vs.saturation   = idx_to_float(i_sat,   0.0f, 0.1f, 2.0f);
    vs.brightness   = idx_to_float(i_bri,   0.5f, 0.1f, 2.0f);
    vs.roundness    = 0.0f;
    vs.screen_curve = 0.0f;

    if (n == 0) return true;

    int base_pos = 5;
    if (has_filter_mode) {
        const int mode = b36_to_int(s[5]);
        if (mode < 0) return false;
        *layer_filter_mode = mode;
        base_pos = 6;
    } else if (layer_filter_mode) {
        *layer_filter_mode = -1;
    }

    if (order) { order->resize(n); for (int i = 0; i < n; ++i) (*order)[i] = i; }
    if (ena)   ena->assign(n, true);
    if (amb)   amb->assign(n, true);

    for (int i = 0; i < n; ++i) {
        int pos   = base_pos + i * 2;
        int ord   = b36_to_int(s[pos]);     if (ord   < 0) return false;
        int flags = b36_to_int(s[pos + 1]); if (flags < 0) return false;

        if (order) (*order)[i] = ord;
        if (ena)   (*ena)[i]   = (flags & 1) != 0;
        if (amb)   (*amb)[i]   = (flags & 2) != 0;
    }

    return true;
}

} // namespace qrd
