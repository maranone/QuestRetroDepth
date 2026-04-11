#include "panel_layout.h"

static float clamp_row_h(float h, float lo, float hi) {
    return std::max(lo, std::min(h, hi));
}

const PanelLayoutItem* PanelLayout::hit(float u, float v) const {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->rect.contains(u, v)) return &*it;
    }
    return nullptr;
}

const PanelLayoutItem* PanelLayout::row_item(int row) const {
    for (const auto& item : items) {
        if (item.row == row && item.role == PanelRole::Row) return &item;
    }
    return nullptr;
}

PanelMetrics panel_metrics(PanelKind kind) {
    switch (kind) {
    case PanelKind::MainMenu: return {1024, 1536, 0.80f, 1.20f};
    case PanelKind::Browser:  return {1536, 1536, 1.20f, 1.20f};
    case PanelKind::Layers:   return {1120, 1280, 0.88f, 0.88f * (1280.0f / 1120.0f)};
    case PanelKind::Settings: return {1280, 2176, 1.10f, 1.10f * (2176.0f / 1280.0f)};
    case PanelKind::Code:     return {1536,  768, 1.20f, 0.60f};
    case PanelKind::CtrlMap:  return {1408, 1536, 1.20f, 1.20f * (1536.0f / 1408.0f)};
    case PanelKind::Help:     return {1024, 1536, 0.82f, 1.23f};
    }
    return {};
}

static void add_row(PanelLayout& layout, int row, int id, float v0, float v1) {
    layout.items.push_back({{0.0f, v0, 1.0f, v1}, row, id, PanelRole::Row});
}

PanelLayout make_main_menu_layout(int item_count) {
    PanelLayout layout;
    layout.kind = PanelKind::MainMenu;
    if (item_count <= 0) return layout;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 88.0f / (float)m.tex_h;
    const float row_h = (1.0f - title_v) / (float)item_count;
    for (int i = 0; i < item_count; ++i) {
        add_row(layout, i, i, title_v + i * row_h, title_v + (i + 1) * row_h);
    }
    return layout;
}

PanelLayout make_browser_layout(int visible_count, int scroll_offset) {
    PanelLayout layout;
    layout.kind = PanelKind::Browser;
    if (visible_count <= 0) return layout;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 88.0f / (float)m.tex_h;
    const float row_h = (1.0f - title_v) / (float)visible_count;
    for (int i = 0; i < visible_count; ++i) {
        add_row(layout, scroll_offset + i, scroll_offset + i, title_v + i * row_h, title_v + (i + 1) * row_h);
    }
    return layout;
}

PanelLayout make_layers_layout(int layer_count, bool has_filter_row) {
    PanelLayout layout;
    layout.kind = PanelKind::Layers;
    const auto m = panel_metrics(layout.kind);
    const int total = layer_count + 2 + (has_filter_row ? 1 : 0);
    if (total <= 0) return layout;
    const float title_v = 88.0f / (float)m.tex_h;
    const float row_h = std::max((1.0f - title_v) / (float)total, 56.0f / (float)m.tex_h);
    for (int i = 0; i < total; ++i) {
        add_row(layout, i, i, title_v + i * row_h, title_v + (i + 1) * row_h);
        if (i < layer_count) {
            layout.items.push_back({{0.60f, title_v + i * row_h, 0.80f, title_v + (i + 1) * row_h}, i, i, PanelRole::Visibility});
            layout.items.push_back({{0.80f, title_v + i * row_h, 1.00f, title_v + (i + 1) * row_h}, i, i, PanelRole::Ambilight});
        }
    }
    return layout;
}

PanelLayout make_settings_layout(int row_count) {
    PanelLayout layout;
    layout.kind = PanelKind::Settings;
    if (row_count <= 0) return layout;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 88.0f / (float)m.tex_h;
    const float row_h = clamp_row_h((1.0f - title_v) / (float)row_count, 52.0f / (float)m.tex_h, 96.0f / (float)m.tex_h);
    for (int i = 0; i < row_count; ++i) {
        add_row(layout, i, i, title_v + i * row_h, title_v + (i + 1) * row_h);
        const bool has_step_buttons = (i >= 6 && i <= 11);
        if (has_step_buttons) {
            layout.items.push_back({{0.0f, title_v + i * row_h, 0.20f, title_v + (i + 1) * row_h}, i, i, PanelRole::Minus});
            layout.items.push_back({{0.80f, title_v + i * row_h, 1.00f, title_v + (i + 1) * row_h}, i, i, PanelRole::Plus});
        }
    }
    return layout;
}

PanelLayout make_code_layout() {
    PanelLayout layout;
    layout.kind = PanelKind::Code;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 80.0f / (float)m.tex_h;
    const float row_h = (1.0f - title_v) / 4.0f;
    const float col_w = 1.0f / 10.0f;
    for (int row = 0; row < 4; ++row) {
        int cols = row < 3 ? 10 : 7;
        for (int col = 0; col < cols; ++col) {
            int key = 0;
            if (row == 0) key = col;
            else if (row == 1) key = 10 + col;
            else if (row == 2) key = 20 + col;
            else key = col < 6 ? 30 + col : 36;
            layout.items.push_back({{col * col_w, title_v + row * row_h, (col + 1) * col_w, title_v + (row + 1) * row_h}, row, key, PanelRole::Key});
        }
    }
    return layout;
}

PanelLayout make_ctrlmap_layout(int button_count, int action_count) {
    PanelLayout layout;
    layout.kind = PanelKind::CtrlMap;
    const int total = button_count + action_count;
    if (total <= 0) return layout;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 88.0f / (float)m.tex_h;
    const float row_h = clamp_row_h((1.0f - title_v) / (float)total, 44.0f / (float)m.tex_h, 80.0f / (float)m.tex_h);
    for (int i = 0; i < total; ++i) {
        add_row(layout, i, i, title_v + i * row_h, title_v + (i + 1) * row_h);
    }
    return layout;
}
