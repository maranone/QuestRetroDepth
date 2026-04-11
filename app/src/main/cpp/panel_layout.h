#pragma once

#include <algorithm>
#include <vector>

enum class PanelKind {
    MainMenu,
    Browser,
    Layers,
    Settings,
    Code,
    CtrlMap,
    Help,
};

enum class PanelRole {
    None,
    Row,
    Minus,
    Plus,
    Visibility,
    Ambilight,
    Key,
};

struct PanelMetrics {
    int   tex_w = 0;
    int   tex_h = 0;
    float world_w = 0.0f;
    float world_h = 0.0f;
};

struct UvRect {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;

    bool contains(float u, float v) const {
        return u >= u0 && u <= u1 && v >= v0 && v <= v1;
    }
};

struct PanelLayoutItem {
    UvRect rect;
    int row = -1;
    int id = -1;
    PanelRole role = PanelRole::None;
};

struct PanelLayout {
    PanelKind kind = PanelKind::MainMenu;
    std::vector<PanelLayoutItem> items;

    const PanelLayoutItem* hit(float u, float v) const;
    const PanelLayoutItem* row_item(int row) const;
};

PanelMetrics panel_metrics(PanelKind kind);
PanelLayout make_main_menu_layout(int item_count);
PanelLayout make_browser_layout(int visible_count, int scroll_offset);
PanelLayout make_layers_layout(int layer_count, bool has_filter_row);
PanelLayout make_settings_layout(int row_count);
PanelLayout make_code_layout();
PanelLayout make_ctrlmap_layout(int button_count, int action_count);
