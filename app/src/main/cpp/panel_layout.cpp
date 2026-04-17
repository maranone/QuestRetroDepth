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
    case PanelKind::QuickEdit: return {1536, 1280, 1.18f, 1.18f * (1280.0f / 1536.0f)};
    case PanelKind::Browser:  return {1536, 1536, 1.20f, 1.20f};
    case PanelKind::Layers:   return {1120, 1280, 0.88f, 0.88f * (1280.0f / 1120.0f)};
    case PanelKind::Settings: return {1280, 2176, 1.10f, 1.10f * (2176.0f / 1280.0f)};
    case PanelKind::SaveStates: return {1280, 1280, 1.10f, 1.10f};
    case PanelKind::Code:     return {1536,  768, 1.20f, 0.60f};
    case PanelKind::CtrlMap:  return {1408, 1536, 1.20f, 1.20f * (1536.0f / 1408.0f)};
    case PanelKind::Help:     return {1024, 1536, 0.82f, 1.23f};
    case PanelKind::Homebrew: return {1024, 1536, 0.80f, 1.20f};
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

PanelLayout make_quick_edit_layout(int settings_preset_count, int layer_preset_count) {
    PanelLayout layout;
    layout.kind = PanelKind::QuickEdit;
    const auto m = panel_metrics(layout.kind);
    const float gap_px = 24.0f;
    const float title_h_px = 96.0f;
    const float section_top_px = title_h_px + 32.0f;
    const float section_header_h_px = 64.0f;
    const float row_start_px = section_top_px + 84.0f;
    const float section_bottom_px = (float)m.tex_h - 24.0f;
    const float column_w_px = ((float)m.tex_w - gap_px * 3.0f) * 0.5f;
    const float left_x_px = gap_px;
    const float right_x_px = left_x_px + column_w_px + gap_px;

    const float left_u0 = left_x_px / (float)m.tex_w;
    const float left_u1 = (left_x_px + column_w_px) / (float)m.tex_w;
    const float right_u0 = right_x_px / (float)m.tex_w;
    const float right_u1 = (right_x_px + column_w_px) / (float)m.tex_w;

    const int visible_layer_presets = std::max(layer_preset_count, 5);
    const int left_rows = settings_preset_count + 3;
    const int right_rows = visible_layer_presets + 2;

    auto add_section_rows = [&](float u0, float u1, int row_count, PanelRole preset_role,
                                PanelRole save_role, PanelRole reset_role,
                                PanelRole manual_role, int preset_count) {
        if (row_count <= 0) return;
        const float x_px = u0 * (float)m.tex_w;
        const float row_gap_px = 12.0f;
        const float total_gap_px = row_gap_px * (float)(row_count - 1);
        const float row_h_px = ((section_bottom_px - row_start_px) - total_gap_px) / (float)row_count;
        for (int i = 0; i < row_count; ++i) {
            const float y0_px = row_start_px + i * (row_h_px + row_gap_px);
            const float y1_px = y0_px + row_h_px;
            const float v0 = y0_px / (float)m.tex_h;
            const float v1 = y1_px / (float)m.tex_h;
            if (i < preset_count) {
                layout.items.push_back({{u0, v0, u1, v1}, i, i, preset_role});
            } else if (preset_role == PanelRole::QuickSettingsPreset) {
                const int extra = i - preset_count;
                PanelRole role = PanelRole::QuickManualEdit;
                int id = 0;
                if (extra == 0) {
                    role = reset_role;
                } else if (extra == 1) {
                    role = PanelRole::QuickManualEdit;
                    id = 0;
                } else {
                    role = PanelRole::QuickManualVisual;
                    id = 1;
                }
                layout.items.push_back({{u0, v0, u1, v1}, i, id, role});
            } else {
                const int extra = i - preset_count;
                const PanelRole role = (extra == 0) ? reset_role : manual_role;
                layout.items.push_back({{u0, v0, u1, v1}, i, 0, role});
            }
        }
    };

    add_section_rows(left_u0, left_u1, left_rows,
                     PanelRole::QuickSettingsPreset, PanelRole::QuickSettingsSave,
                     PanelRole::QuickResetSettings, PanelRole::QuickManualEdit,
                     settings_preset_count);
    add_section_rows(right_u0, right_u1, right_rows,
                     PanelRole::QuickLayersPreset, PanelRole::QuickLayersSave,
                     PanelRole::QuickResetLayers, PanelRole::QuickManualLayers,
                     visible_layer_presets);
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
        const bool has_step_buttons = (i == 4) || (i >= 6 && i <= 11);
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
    layout.items.push_back({{0.00f, 0.0f, 0.22f, title_v}, -1, 100, PanelRole::CodeCancel});
    layout.items.push_back({{0.35f, 0.0f, 0.65f, title_v}, -1, 101, PanelRole::CodeSpace});
    layout.items.push_back({{0.78f, 0.0f, 1.00f, title_v}, -1, 102, PanelRole::CodeConfirm});
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

PanelLayout make_save_state_layout() {
    PanelLayout layout;
    layout.kind = PanelKind::SaveStates;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 88.0f / (float)m.tex_h;
    const float row_h = (1.0f - title_v) / 4.0f;
    const float col_w = 1.0f / 3.0f;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int id = row * 3 + col;
            const PanelRole role = row == 0 ? PanelRole::SaveLoadSlot : PanelRole::SaveSaveSlot;
            layout.items.push_back({{col * col_w, title_v + row * row_h, (col + 1) * col_w, title_v + (row + 1) * row_h}, row, id, role});
        }
    }
    layout.items.push_back({{0.0f, title_v + 2.0f * row_h, 1.0f, title_v + 3.0f * row_h}, 2, 6, PanelRole::SaveAutosaveOption});
    layout.items.push_back({{0.0f, title_v + 3.0f * row_h, 1.0f, 1.0f}, 3, 7, PanelRole::SaveAutoloadOption});
    return layout;
}

PanelLayout make_homebrew_layout(int entry_count, int view) {
    PanelLayout layout;
    layout.kind = PanelKind::Homebrew;
    const auto m = panel_metrics(layout.kind);
    const float title_v = 88.0f / (float)m.tex_h;
    if (view == 1) {
        // detail view: fixed rows for back, download, delete, open website
        const int detail_rows = 4;
        const float row_h = (1.0f - title_v) / (float)detail_rows;
        for (int i = 0; i < detail_rows; ++i) {
            add_row(layout, i, i, title_v + i * row_h, title_v + (i + 1) * row_h);
        }
    } else {
        // list view: feed toggle (row 0) + entry rows + back row
        const int total = entry_count + 2;
        if (total <= 2) {
            add_row(layout, 0, 0, title_v, title_v + (1.0f - title_v) / 2.0f);
            add_row(layout, 1, 1, title_v + (1.0f - title_v) / 2.0f, 1.0f);
            return layout;
        }
        const float row_h = clamp_row_h((1.0f - title_v) / (float)total, 44.0f / (float)m.tex_h, 80.0f / (float)m.tex_h);
        for (int i = 0; i < total; ++i) {
            add_row(layout, i, i, title_v + i * row_h, title_v + (i + 1) * row_h);
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
