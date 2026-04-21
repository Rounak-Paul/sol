#include "sol_ui_system.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "style.h"

typedef struct SolWorkspaceVisitorContext {
    SolUISystem *ui;
} SolWorkspaceVisitorContext;

#define SOL_UI_MAX_COMMAND_FLOWS 64u
#define SOL_UI_MAX_ACTION_LEN 63u
#define SOL_UI_MAX_LABEL_LEN 95u
#define SOL_UI_MAX_FLOW_SEQUENCE_LEN 8u
#define SOL_UI_MAX_SUGGESTIONS 32u

typedef struct SolCommandFlowBinding {
    char action[SOL_UI_MAX_ACTION_LEN + 1u];
    char label[SOL_UI_MAX_LABEL_LEN + 1u];
    SolKeyCode sequence[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t sequence_length;
    SolInputActionCallback callback;
    void *user_data;
} SolCommandFlowBinding;

typedef struct SolFlowSuggestion {
    SolKeyCode key;
    const char *label;
    uint32_t continuation_count;
} SolFlowSuggestion;

typedef struct SolFlowEditContext {
    SolUISystem *ui;
    size_t index;
} SolFlowEditContext;

typedef struct SolLeaderOptionContext {
    SolUISystem *ui;
    SolModifierMask modifier;
    const char *label;
} SolLeaderOptionContext;

static void sol_ui_refresh_overlay_now(struct SolUISystem *ui);

struct SolUISystem {
    Ca_Instance *instance;
    Ca_Window *primary_window;
    SolBufferSystem *buffers;

    int viewport_width;
    int viewport_height;

    Ca_Stylesheet *stylesheet;

    Ca_Div *workspace_host;
    Ca_Div *workspace_content_host;
    Ca_Div *overlay_host;

    SolModifierMask leader_modifier;
    bool leader_active;
    SolKeyCode leader_prefix[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t leader_prefix_length;
    bool leader_no_match;
    SolKeyCode leader_last_invalid_key;
    bool settings_visible;

    SolCommandFlowBinding command_flows[SOL_UI_MAX_COMMAND_FLOWS];
    size_t command_flow_count;

    SolFlowEditContext flow_edit_contexts[SOL_UI_MAX_COMMAND_FLOWS];
    SolLeaderOptionContext leader_options[4];

    bool workspace_dirty;
    bool overlay_dirty;
};

static void sol_ui_mark_workspace_dirty(SolUISystem *ui)
{
    if (!ui) {
        return;
    }
    if (ui->workspace_content_host) {
        ca_div_invalidate(ui->workspace_content_host);
        ui->workspace_dirty = false;
        return;
    }

    ui->workspace_dirty = true;
}

static void sol_ui_mark_overlay_dirty(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    if (ui->overlay_host) {
        sol_ui_refresh_overlay_now(ui);
        ui->overlay_dirty = false;
        return;
    }

    ui->overlay_dirty = true;
}

static void sol_ui_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0u) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static bool sol_ui_is_modifier_key(SolKeyCode key)
{
    switch (key) {
        case SOL_KEY_LEFT_SHIFT:
        case SOL_KEY_RIGHT_SHIFT:
        case SOL_KEY_LEFT_CTRL:
        case SOL_KEY_RIGHT_CTRL:
        case SOL_KEY_LEFT_ALT:
        case SOL_KEY_RIGHT_ALT:
        case SOL_KEY_LEFT_SUPER:
        case SOL_KEY_RIGHT_SUPER:
            return true;
        default:
            return false;
    }
}

static bool sol_ui_is_leader_key(const SolUISystem *ui, SolKeyCode key)
{
    if (!ui) {
        return false;
    }

    if (ui->leader_modifier == SOL_MOD_CTRL) {
        return key == SOL_KEY_LEFT_CTRL || key == SOL_KEY_RIGHT_CTRL;
    }
    if (ui->leader_modifier == SOL_MOD_ALT) {
        return key == SOL_KEY_LEFT_ALT || key == SOL_KEY_RIGHT_ALT;
    }
    if (ui->leader_modifier == SOL_MOD_SHIFT) {
        return key == SOL_KEY_LEFT_SHIFT || key == SOL_KEY_RIGHT_SHIFT;
    }
    if (ui->leader_modifier == SOL_MOD_SUPER) {
        return key == SOL_KEY_LEFT_SUPER || key == SOL_KEY_RIGHT_SUPER;
    }

    return false;
}

static void sol_ui_format_key_name(SolKeyCode key, char *out, size_t out_size)
{
    if (!out || out_size == 0u) {
        return;
    }

    if (key >= 'a' && key <= 'z') {
        out[0] = (char)toupper((unsigned char)key);
        out[1] = '\0';
        return;
    }

    if (key >= 32u && key <= 126u) {
        out[0] = (char)key;
        out[1] = '\0';
        return;
    }

    snprintf(out, out_size, "K%u", (unsigned int)key);
}

static SolKeyCode sol_ui_normalize_flow_key(SolKeyCode key)
{
    if (key >= 'a' && key <= 'z') {
        return (SolKeyCode)(key - ('a' - 'A'));
    }
    return key;
}

static SolCommandFlowBinding *sol_ui_find_flow_by_action(SolUISystem *ui, const char *action)
{
    if (!ui || !action) {
        return NULL;
    }

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        if (strcmp(ui->command_flows[i].action, action) == 0) {
            return &ui->command_flows[i];
        }
    }

    return NULL;
}

/* ---- Floating panel positioning ------------------------------------------------
   Panels use CA_POSITION_FIXED coordinates, which are relative to the content
   area that begins immediately below the 22 px custom title bar.
   ------------------------------------------------------------------------------- */
typedef enum SolPanelSide {
    SOL_PANEL_SIDE_TOP          = 0,
    SOL_PANEL_SIDE_BOTTOM       = 1,
    SOL_PANEL_SIDE_LEFT         = 2,
    SOL_PANEL_SIDE_RIGHT        = 3,
    SOL_PANEL_SIDE_CENTER       = 4,
    SOL_PANEL_SIDE_TOP_LEFT     = 5,
    SOL_PANEL_SIDE_TOP_RIGHT    = 6,
    SOL_PANEL_SIDE_BOTTOM_LEFT  = 7,
    SOL_PANEL_SIDE_BOTTOM_RIGHT = 8,
} SolPanelSide;

typedef struct SolPanelRect {
    float x, y, width, height;
} SolPanelRect;

/* Returns a CA_POSITION_FIXED rectangle anchored to the requested side.
   Pass panel_width  <= 0 to fill the full content width.
   Pass panel_height <= 0 to default to 60 px. */
static SolPanelRect sol_ui_get_panel_rect(
    const SolUISystem *ui,
    SolPanelSide       side,
    float              panel_width,
    float              panel_height
)
{
    const float title_bar_h = 22.0f;
    const float win_w = (ui && ui->viewport_width  > 0)
        ? (float)ui->viewport_width  : (float)SOL_UI_WINDOW_WIDTH;
    const float win_h = (ui && ui->viewport_height > 0)
        ? (float)ui->viewport_height : (float)SOL_UI_WINDOW_HEIGHT;
    const float content_w = win_w;
    const float content_h = win_h - title_bar_h;

    if (panel_width  <= 0.0f) panel_width  = content_w;
    if (panel_height <= 0.0f) panel_height = 60.0f;

    float x = 0.0f;
    float y = 0.0f;

    switch (side) {
        case SOL_PANEL_SIDE_TOP:
            x = (content_w - panel_width) * 0.5f;
            y = 0.0f;
            break;
        case SOL_PANEL_SIDE_BOTTOM:
            x = 0.0f;
            y = content_h - panel_height;
            break;
        case SOL_PANEL_SIDE_LEFT:
            x = 0.0f;
            y = (content_h - panel_height) * 0.5f;
            break;
        case SOL_PANEL_SIDE_RIGHT:
            x = content_w - panel_width;
            y = (content_h - panel_height) * 0.5f;
            break;
        case SOL_PANEL_SIDE_CENTER:
            x = (content_w - panel_width) * 0.5f;
            y = (content_h - panel_height) * 0.5f;
            break;
        case SOL_PANEL_SIDE_TOP_LEFT:
            x = 0.0f;
            y = 0.0f;
            break;
        case SOL_PANEL_SIDE_TOP_RIGHT:
            x = content_w - panel_width;
            y = 0.0f;
            break;
        case SOL_PANEL_SIDE_BOTTOM_LEFT:
            x = 0.0f;
            y = content_h - panel_height;
            break;
        case SOL_PANEL_SIDE_BOTTOM_RIGHT:
            x = content_w - panel_width;
            y = content_h - panel_height;
            break;
        default:
            x = 0.0f;
            y = content_h - panel_height;
            break;
    }

    return (SolPanelRect){ x, y, panel_width, panel_height };
}

static void sol_ui_reset_leader_prefix(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    ui->leader_prefix_length = 0u;
    ui->leader_no_match = false;
    ui->leader_last_invalid_key = SOL_KEY_UNKNOWN;
}

static void sol_ui_open_leader_popup(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    ui->leader_active = true;
    sol_ui_reset_leader_prefix(ui);
    sol_ui_mark_overlay_dirty(ui);
}

static void sol_ui_close_leader_popup(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    ui->leader_active = false;
    sol_ui_reset_leader_prefix(ui);
    sol_ui_mark_overlay_dirty(ui);
}

static bool sol_ui_flow_matches_prefix(
    const SolCommandFlowBinding *flow,
    const SolKeyCode *prefix,
    size_t prefix_length
)
{
    if (!flow || prefix_length > flow->sequence_length) {
        return false;
    }

    for (size_t i = 0u; i < prefix_length; ++i) {
        if (flow->sequence[i] != prefix[i]) {
            return false;
        }
    }

    return true;
}

static SolKeyCode sol_ui_flow_display_key(const SolCommandFlowBinding *flow)
{
    if (!flow || flow->sequence_length == 0u) {
        return SOL_KEY_UNKNOWN;
    }

    return flow->sequence[flow->sequence_length - 1u];
}

static const char *sol_ui_flow_label_for_next(
    const SolUISystem *ui,
    const SolKeyCode *prefix,
    size_t prefix_length,
    SolKeyCode next_key
)
{
    if (!ui) {
        return "More";
    }

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, prefix, prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= prefix_length || flow->sequence[prefix_length] != next_key) {
            continue;
        }
        if (flow->sequence_length == prefix_length + 1u) {
            return flow->label[0] != '\0' ? flow->label : flow->action;
        }
    }

    return "More";
}

static uint32_t sol_ui_flow_continuation_count(
    const SolUISystem *ui,
    const SolKeyCode *prefix,
    size_t prefix_length,
    SolKeyCode next_key
)
{
    if (!ui || prefix_length + 1u >= SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        return 0u;
    }

    SolKeyCode unique[SOL_UI_MAX_FLOW_SEQUENCE_LEN];
    size_t unique_count = 0u;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, prefix, prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= prefix_length + 1u) {
            continue;
        }
        if (flow->sequence[prefix_length] != next_key) {
            continue;
        }

        const SolKeyCode child = flow->sequence[prefix_length + 1u];
        bool exists = false;
        for (size_t j = 0u; j < unique_count; ++j) {
            if (unique[j] == child) {
                exists = true;
                break;
            }
        }

        if (!exists && unique_count < SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
            unique[unique_count++] = child;
        }
    }

    return (uint32_t)unique_count;
}

static size_t sol_ui_collect_suggestions(SolUISystem *ui, SolFlowSuggestion *out, size_t capacity)
{
    if (!ui || !out || capacity == 0u) {
        return 0u;
    }

    size_t suggestion_count = 0u;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        const SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, ui->leader_prefix, ui->leader_prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= ui->leader_prefix_length) {
            continue;
        }

        const SolKeyCode next_key = flow->sequence[ui->leader_prefix_length];

        size_t existing_index = suggestion_count;
        for (size_t j = 0u; j < suggestion_count; ++j) {
            if (out[j].key == next_key) {
                existing_index = j;
                break;
            }
        }

        if (existing_index == suggestion_count) {
            if (suggestion_count >= capacity) {
                continue;
            }

            out[suggestion_count].key = next_key;
            out[suggestion_count].label = sol_ui_flow_label_for_next(
                ui,
                ui->leader_prefix,
                ui->leader_prefix_length,
                next_key
            );
            out[suggestion_count].continuation_count = sol_ui_flow_continuation_count(
                ui,
                ui->leader_prefix,
                ui->leader_prefix_length,
                next_key
            );
            ++suggestion_count;
        }
    }

    return suggestion_count;
}

static int sol_ui_split_direction_to_ca(SolBufferSplitDirection direction)
{
    if (direction == SOL_BUFFER_SPLIT_VERTICAL) {
        return CA_HORIZONTAL;
    }
    return CA_VERTICAL;
}

static void sol_ui_visit_begin_split(SolBufferSplitDirection direction, float ratio, void *user_data)
{
    (void)ratio;

    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }

    ca_split_begin(&(Ca_SplitDesc){
        .direction = sol_ui_split_direction_to_ca(direction),
        .ratio = ratio,
        .bar_size = SOL_UI_SPLIT_BAR_SIZE,
        .bar_color = SOL_UI_SPLIT_BAR_COLOR,
        .bar_hover_color = SOL_UI_SPLIT_BAR_HOVER_COLOR,
    });
}

static void sol_ui_visit_end_split(void *user_data)
{
    (void)user_data;
    ca_split_end();
}

static void sol_ui_visit_render_leaf(SolBuffer *buffer, SolBufferNodeId leaf_id, bool is_active, void *user_data)
{
    (void)leaf_id;

    SolWorkspaceVisitorContext *ctx = (SolWorkspaceVisitorContext *)user_data;
    if (!ctx || !ctx->ui) {
        return;
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = is_active ? "buffer-pane buffer-pane-active" : "buffer-pane",
    });

    if (buffer) {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "buffer-body",
        });

        SolBufferRenderArgs args;
        args.is_active = is_active;
        args.ui_context = ctx->ui;
        sol_buffer_render(buffer, &args);

        ca_div_end();
    } else {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "buffer-body",
        });

        ca_div_end();
    }

    ca_div_end();
}

static void sol_ui_render_workspace_tree(SolUISystem *ui)
{
    if (!ui || !ui->buffers) {
        return;
    }

    if (sol_buffer_count(ui->buffers) == 0u) {
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "buffer-pane",
        });

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL,
            .style = "buffer-body",
        });

        ca_div_end();
        ca_div_end();
        return;
    }

    SolWorkspaceVisitorContext visitor_context;
    visitor_context.ui = ui;

    SolBufferWorkspaceVisitor visitor;
    memset(&visitor, 0, sizeof(visitor));
    visitor.begin_split = sol_ui_visit_begin_split;
    visitor.end_split = sol_ui_visit_end_split;
    visitor.render_leaf = sol_ui_visit_render_leaf;

    sol_buffer_workspace_visit(ui->buffers, &visitor, &visitor_context);
}

static void sol_ui_on_settings_menu_action(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }

    ui->settings_visible = !ui->settings_visible;
    sol_ui_mark_workspace_dirty(ui);
    sol_ui_mark_overlay_dirty(ui);
}

static void sol_ui_on_leader_option_click(Ca_Button *button, void *user_data)
{
    (void)button;

    SolLeaderOptionContext *context = (SolLeaderOptionContext *)user_data;
    if (!context || !context->ui) {
        return;
    }

    context->ui->leader_modifier = context->modifier;
    context->ui->leader_active = false;
    sol_ui_reset_leader_prefix(context->ui);
    sol_ui_mark_workspace_dirty(context->ui);
    sol_ui_mark_overlay_dirty(context->ui);
}

static void sol_ui_on_settings_close_click(Ca_Button *button, void *user_data)
{
    (void)button;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }

    ui->settings_visible = false;
    sol_ui_mark_workspace_dirty(ui);
    sol_ui_mark_overlay_dirty(ui);
}

static void sol_ui_on_binding_input_change(Ca_TextInput *input, void *user_data)
{
    SolFlowEditContext *context = (SolFlowEditContext *)user_data;
    if (!context || !context->ui || context->index >= context->ui->command_flow_count || !input) {
        return;
    }

    SolCommandFlowBinding *flow = &context->ui->command_flows[context->index];
    if (flow->sequence_length == 0u) {
        return;
    }

    const char *text = ca_get_text(input);
    if (!text || *text == '\0') {
        return;
    }

    unsigned char raw = (unsigned char)text[0];
    if (isalpha(raw)) {
        raw = (unsigned char)toupper(raw);
    }

    if (raw < 32u || raw > 126u) {
        return;
    }

    flow->sequence[flow->sequence_length - 1u] = (SolKeyCode)raw;

    char normalized[2];
    normalized[0] = (char)raw;
    normalized[1] = '\0';
    ca_set_text(input, normalized);

    sol_ui_mark_workspace_dirty(context->ui);
    sol_ui_mark_overlay_dirty(context->ui);
}

#define SOL_CF_PANEL_HEIGHT 68.0f

static void sol_ui_render_command_flow_panel(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    const SolPanelRect rect =
        sol_ui_get_panel_rect(ui, SOL_PANEL_SIDE_BOTTOM, 0.0f, SOL_CF_PANEL_HEIGHT);

    SolFlowSuggestion suggestions[SOL_UI_MAX_SUGGESTIONS];
    const size_t suggestion_count =
        sol_ui_collect_suggestions(ui, suggestions, SOL_UI_MAX_SUGGESTIONS);

    /* ---- outer panel ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction       = CA_VERTICAL,
        .position        = CA_POSITION_FIXED,
        .pos_x           = rect.x,
        .pos_y           = rect.y,
        .width           = rect.width,
        .height          = rect.height,
        .z_index         = 50,
        .border_width    = 1.0f,
        .border_color    = ca_color(0.14f, 0.21f, 0.32f, 1.0f),
        .shadow_offset_x = 0.0f,
        .shadow_offset_y = -4.0f,
        .shadow_blur     = 20.0f,
        .shadow_color    = ca_color(0.0f, 0.0f, 0.0f, 0.55f),
        .style           = "cf-panel",
    });

    /* ---- header: leader modifier + pressed-prefix breadcrumb ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "cf-header",
    });

    /* Find the label for the active leader modifier. */
    const char *leader_name = "Keys";
    for (size_t i = 0u; i < 4u; ++i) {
        if (ui->leader_options[i].modifier == ui->leader_modifier) {
            leader_name = ui->leader_options[i].label;
            break;
        }
    }
    ca_text(&(Ca_TextDesc){ .text = leader_name, .style = "cf-title" });

    for (size_t i = 0u; i < ui->leader_prefix_length; ++i) {
        char prefix_key[24];
        sol_ui_format_key_name(ui->leader_prefix[i], prefix_key, sizeof(prefix_key));
        ca_text(&(Ca_TextDesc){ .text = ">", .style = "cf-prefix-sep" });
        ca_text(&(Ca_TextDesc){ .text = prefix_key, .style = "cf-prefix-key" });
    }

    ca_div_end(); /* cf-header */

    /* ---- items row: one chip per available next key ---- */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style     = "cf-items",
    });

    if (ui->leader_no_match) {
        char bad_key[24];
        sol_ui_format_key_name(ui->leader_last_invalid_key, bad_key, sizeof(bad_key));

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style     = "cf-chip cf-chip-error",
        });
        ca_text(&(Ca_TextDesc){ .text = bad_key,          .style = "cf-chip-key" });
        ca_text(&(Ca_TextDesc){ .text = "No matching flow", .style = "cf-chip-label cf-label-error" });
        ca_div_end();

    } else if (suggestion_count == 0u) {
        ca_text(&(Ca_TextDesc){ .text = "No bindings", .style = "cf-empty" });

    } else {
        for (size_t i = 0u; i < suggestion_count; ++i) {
            char key_name[24];
            sol_ui_format_key_name(suggestions[i].key, key_name, sizeof(key_name));

            ca_div_begin(&(Ca_DivDesc){
                .direction = CA_HORIZONTAL,
                .style     = "cf-chip",
            });
            ca_text(&(Ca_TextDesc){ .text = key_name,             .style = "cf-chip-key" });
            ca_text(&(Ca_TextDesc){ .text = suggestions[i].label, .style = "cf-chip-label" });
            if (suggestions[i].continuation_count > 0u) {
                char more[16];
                snprintf(more, sizeof(more), "+%u",
                         (unsigned int)suggestions[i].continuation_count);
                ca_text(&(Ca_TextDesc){ .text = more, .style = "cf-chip-more" });
            }
            ca_div_end(); /* cf-chip */
        }
    }

    ca_div_end(); /* cf-items */
    ca_div_end(); /* cf-panel */
}

static void sol_ui_render_settings_panel(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "settings-panel",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "settings-card",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "settings-header-row",
    });

    ca_text(&(Ca_TextDesc){
        .text = "Settings",
        .style = "settings-title",
    });

    ca_btn(&(Ca_BtnDesc){
        .text = "Close",
        .on_click = sol_ui_on_settings_close_click,
        .click_data = ui,
        .style = "settings-close-button",
    });

    ca_div_end();

    ca_text(&(Ca_TextDesc){
        .text = "Configure the leader modifier and command flow key bindings.",
        .style = "settings-help-text",
    });

    ca_text(&(Ca_TextDesc){
        .text = "Leader Modifier",
        .style = "settings-heading",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "settings-leader-row",
    });

    for (size_t i = 0u; i < 4u; ++i) {
        SolLeaderOptionContext *option = &ui->leader_options[i];
        const char *button_style = option->modifier == ui->leader_modifier
            ? "settings-leader-button settings-leader-button-active"
            : "settings-leader-button";

        ca_btn(&(Ca_BtnDesc){
            .text = option->label,
            .on_click = sol_ui_on_leader_option_click,
            .click_data = option,
            .style = button_style,
        });
    }

    ca_div_end();

    ca_text(&(Ca_TextDesc){
        .text = "Command Flow Bindings",
        .style = "settings-heading",
    });

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_HORIZONTAL,
        .style = "settings-flow-header",
    });

    ca_text(&(Ca_TextDesc){
        .text = "Command",
        .style = "settings-flow-header-label",
    });

    ca_text(&(Ca_TextDesc){
        .text = "Key",
        .style = "settings-flow-header-key",
    });

    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "settings-flow-list",
    });

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *flow = &ui->command_flows[i];
        SolFlowEditContext *ctx = &ui->flow_edit_contexts[i];

        ctx->ui = ui;
        ctx->index = i;

        char key_name[16];
        char id[48];
        sol_ui_format_key_name(sol_ui_flow_display_key(flow), key_name, sizeof(key_name));
        snprintf(id, sizeof(id), "flow-binding-%u", (unsigned int)i);

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .style = "settings-flow-row",
        });

        ca_text(&(Ca_TextDesc){
            .text = flow->label[0] != '\0' ? flow->label : flow->action,
            .style = "settings-flow-label",
        });

        ca_input(&(Ca_InputDesc){
            .text = key_name,
            .id = id,
            .on_change = sol_ui_on_binding_input_change,
            .change_data = ctx,
            .style = "settings-flow-input",
        });

        ca_div_end();
    }

    ca_div_end();

    ca_div_end();

    ca_div_end();
}

static void sol_ui_workspace_content_builder(Ca_Div *div, void *user_data)
{
    (void)div;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }

    if (ui->settings_visible) {
        sol_ui_render_settings_panel(ui);
    } else {
        sol_ui_render_workspace_tree(ui);
    }
}

static void sol_ui_overlay_builder(Ca_Div *div, void *user_data)
{
    (void)div;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }

    if (!ui->settings_visible && ui->leader_active) {
        sol_ui_render_command_flow_panel(ui);
    }
}

static void sol_ui_refresh_overlay_now(SolUISystem *ui)
{
    if (!ui || !ui->overlay_host) {
        return;
    }

    ca_div_clear(ui->overlay_host);
    sol_ui_overlay_builder(ui->overlay_host, ui);
    ca_div_end();
}

static void sol_ui_on_frame(void *user_data)
{
    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui) {
        return;
    }

    if (ui->workspace_dirty && ui->workspace_content_host) {
        ca_div_invalidate(ui->workspace_content_host);
        ui->workspace_dirty = false;
    }
    if (ui->overlay_dirty && ui->overlay_host) {
        ca_div_invalidate(ui->overlay_host);
        ui->overlay_dirty = false;
    }
}

static void sol_ui_install_title_bar_menu(SolUISystem *ui)
{
    if (!ui || !ui->primary_window) {
        return;
    }

    const Ca_MenuItemDesc settings_items[] = {
        {
            .label = "Command Flows",
            .action = sol_ui_on_settings_menu_action,
            .action_data = ui,
            .separator = false,
            .sub_items = NULL,
            .sub_item_count = 0,
        },
    };

    const Ca_MenuDesc menus[] = {
        {
            .label = "Settings",
            .items = settings_items,
            .item_count = 1,
        },
    };

    ca_window_set_title_bar_menus(ui->primary_window, menus, 1);
}

static bool sol_ui_build_layout(SolUISystem *ui)
{
    if (!ui || !ui->primary_window) {
        return false;
    }

    ca_ui_begin(ui->primary_window, &(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "app-root",
    });

    ui->workspace_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "workspace-host",
    });

    ui->workspace_content_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .style = "workspace-main-full",
    });

    ca_div_set_builder(ui->workspace_content_host, sol_ui_workspace_content_builder, ui);
    sol_ui_workspace_content_builder(ui->workspace_content_host, ui);

    ca_div_end();

    ui->overlay_host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .position = CA_POSITION_FIXED,
        .pos_x = 0.0f,
        .pos_y = 0.0f,
        .z_index = 40,
    });

    ca_div_set_builder(ui->overlay_host, sol_ui_overlay_builder, ui);
    sol_ui_overlay_builder(ui->overlay_host, ui);

    ca_div_end();

    ca_div_end();

    ca_ui_end();
    return true;
}

SolUISystem *sol_ui_system_create(Ca_Instance *instance, SolBufferSystem *buffers)
{
    if (!instance || !buffers) {
        return NULL;
    }

    SolUISystem *ui = (SolUISystem *)calloc(1u, sizeof(SolUISystem));
    if (!ui) {
        return NULL;
    }

    ui->instance = instance;
    ui->buffers = buffers;
    ui->viewport_width = SOL_UI_WINDOW_WIDTH;
    ui->viewport_height = SOL_UI_WINDOW_HEIGHT;
    ui->leader_modifier = SOL_MOD_CTRL;
    ui->workspace_dirty = true;
    ui->overlay_dirty = true;

    ui->leader_options[0].ui = ui;
    ui->leader_options[0].modifier = SOL_MOD_CTRL;
    ui->leader_options[0].label = "Ctrl";

    ui->leader_options[1].ui = ui;
    ui->leader_options[1].modifier = SOL_MOD_ALT;
    ui->leader_options[1].label = "Alt";

    ui->leader_options[2].ui = ui;
    ui->leader_options[2].modifier = SOL_MOD_SHIFT;
    ui->leader_options[2].label = "Shift";

    ui->leader_options[3].ui = ui;
    ui->leader_options[3].modifier = SOL_MOD_SUPER;
    ui->leader_options[3].label = "Super";

    ui->stylesheet = ca_css_parse(SOL_UI_MAIN_WINDOW_CSS);
    if (ui->stylesheet) {
        ca_instance_set_stylesheet(instance, ui->stylesheet);
    }

    ui->primary_window = ca_window_create(instance, &(Ca_WindowDesc){
        .title = SOL_UI_WINDOW_TITLE,
        .width = SOL_UI_WINDOW_WIDTH,
        .height = SOL_UI_WINDOW_HEIGHT,
    });

    if (!ui->primary_window) {
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        free(ui);
        return NULL;
    }

    sol_ui_install_title_bar_menu(ui);

    if (!sol_ui_build_layout(ui)) {
        ca_window_destroy(ui->primary_window);
        ui->primary_window = NULL;
        if (ui->stylesheet) {
            ca_css_destroy(ui->stylesheet);
        }
        free(ui);
        return NULL;
    }

    ca_window_set_on_frame(ui->primary_window, sol_ui_on_frame, ui);
    return ui;
}

void sol_ui_system_destroy(SolUISystem *ui)
{
    if (!ui) {
        return;
    }

    if (ui->primary_window) {
        ca_window_destroy(ui->primary_window);
        ui->primary_window = NULL;
    }

    if (ui->instance && ui->stylesheet) {
        ca_instance_set_stylesheet(ui->instance, NULL);
    }
    if (ui->stylesheet) {
        ca_css_destroy(ui->stylesheet);
        ui->stylesheet = NULL;
    }

    free(ui);
}

Ca_Window *sol_ui_system_primary_window(SolUISystem *ui)
{
    return ui ? ui->primary_window : NULL;
}

bool sol_ui_system_register_command_flow(SolUISystem *ui, const SolCommandFlowDesc *desc)
{
    if (!ui || !desc || !desc->action || !desc->callback) {
        return false;
    }

    size_t sequence_length = 0u;
    if (desc->sequence && desc->sequence_length > 0u) {
        sequence_length = desc->sequence_length;
    } else if (desc->key != SOL_KEY_UNKNOWN) {
        sequence_length = 1u;
    }

    if (sequence_length == 0u || sequence_length > SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        return false;
    }

    SolCommandFlowBinding *existing = sol_ui_find_flow_by_action(ui, desc->action);
    if (existing) {
        memset(existing->sequence, 0, sizeof(existing->sequence));
        existing->sequence_length = sequence_length;
        if (desc->sequence && desc->sequence_length > 0u) {
            for (size_t i = 0u; i < sequence_length; ++i) {
                existing->sequence[i] = sol_ui_normalize_flow_key(desc->sequence[i]);
            }
        } else {
            existing->sequence[0] = sol_ui_normalize_flow_key(desc->key);
        }

        existing->callback = desc->callback;
        existing->user_data = desc->user_data;
        sol_ui_copy_text(existing->label, sizeof(existing->label), desc->label ? desc->label : desc->action);
        sol_ui_mark_workspace_dirty(ui);
        sol_ui_mark_overlay_dirty(ui);
        return true;
    }

    if (ui->command_flow_count >= SOL_UI_MAX_COMMAND_FLOWS) {
        return false;
    }

    SolCommandFlowBinding *flow = &ui->command_flows[ui->command_flow_count++];
    memset(flow, 0, sizeof(*flow));

    flow->sequence_length = sequence_length;
    if (desc->sequence && desc->sequence_length > 0u) {
        for (size_t i = 0u; i < sequence_length; ++i) {
            flow->sequence[i] = sol_ui_normalize_flow_key(desc->sequence[i]);
        }
    } else {
        flow->sequence[0] = sol_ui_normalize_flow_key(desc->key);
    }

    flow->callback = desc->callback;
    flow->user_data = desc->user_data;

    sol_ui_copy_text(flow->action, sizeof(flow->action), desc->action);
    sol_ui_copy_text(flow->label, sizeof(flow->label), desc->label ? desc->label : desc->action);

    sol_ui_mark_workspace_dirty(ui);
    sol_ui_mark_overlay_dirty(ui);
    return true;
}

bool sol_ui_system_handle_input_event(SolUISystem *ui, const SolInputEvent *event)
{
    if (!ui || !event) {
        return false;
    }

    if (event->type != SOL_INPUT_EVENT_KEY_DOWN) {
        return false;
    }

    const SolKeyCode key = sol_ui_normalize_flow_key(event->data.key.key);

    if (sol_ui_is_leader_key(ui, key)) {
        if (event->data.key.repeated) {
            return true;
        }

        if (ui->leader_active) {
            sol_ui_close_leader_popup(ui);
        } else {
            sol_ui_open_leader_popup(ui);
        }
        return true;
    }

    if (!ui->leader_active) {
        if ((event->data.key.modifiers & ui->leader_modifier) == 0u || sol_ui_is_modifier_key(key)) {
            return false;
        }

        // Allow leader+key chord to enter flow mode and use this key as first step.
        sol_ui_open_leader_popup(ui);
    }

    if (key == SOL_KEY_ESCAPE) {
        sol_ui_close_leader_popup(ui);
        return true;
    }

    if (sol_ui_is_modifier_key(key)) {
        return true;
    }

    ui->leader_no_match = false;
    ui->leader_last_invalid_key = SOL_KEY_UNKNOWN;

    SolCommandFlowBinding *exact_match = NULL;
    bool has_deeper_path = false;
    bool has_candidate = false;

    for (size_t i = 0u; i < ui->command_flow_count; ++i) {
        SolCommandFlowBinding *flow = &ui->command_flows[i];
        if (!sol_ui_flow_matches_prefix(flow, ui->leader_prefix, ui->leader_prefix_length)) {
            continue;
        }
        if (flow->sequence_length <= ui->leader_prefix_length) {
            continue;
        }
        if (flow->sequence[ui->leader_prefix_length] != key) {
            continue;
        }

        has_candidate = true;
        if (flow->sequence_length == ui->leader_prefix_length + 1u) {
            exact_match = flow;
        } else {
            has_deeper_path = true;
        }
    }

    if (!has_candidate) {
        ui->leader_no_match = true;
        ui->leader_last_invalid_key = key;
        ui->leader_prefix_length = 0u;
        sol_ui_mark_overlay_dirty(ui);
        return true;
    }

    if (exact_match) {
        const bool handled = exact_match->callback(exact_match->action, event, exact_match->user_data);
        sol_ui_close_leader_popup(ui);
        return handled || true;
    }

    if (has_deeper_path && ui->leader_prefix_length < SOL_UI_MAX_FLOW_SEQUENCE_LEN) {
        ui->leader_prefix[ui->leader_prefix_length++] = key;
        sol_ui_mark_overlay_dirty(ui);
        return true;
    }

    sol_ui_reset_leader_prefix(ui);
    sol_ui_mark_overlay_dirty(ui);
    return true;
}

bool sol_ui_system_on_save_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    if (!user_data) {
        return false;
    }

    return true;
}

bool sol_ui_system_on_split_vertical_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }

    if (!sol_buffer_split_active(ui->buffers, SOL_BUFFER_SPLIT_VERTICAL, 0.5f, 0u, NULL)) {
        return false;
    }

    sol_ui_mark_workspace_dirty(ui);
    return true;
}

bool sol_ui_system_on_split_horizontal_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }

    if (!sol_buffer_split_active(ui->buffers, SOL_BUFFER_SPLIT_HORIZONTAL, 0.5f, 0u, NULL)) {
        return false;
    }

    sol_ui_mark_workspace_dirty(ui);
    return true;
}

bool sol_ui_system_on_focus_next_action(const char *action, const SolInputEvent *event, void *user_data)
{
    (void)action;
    (void)event;

    SolUISystem *ui = (SolUISystem *)user_data;
    if (!ui || !ui->buffers) {
        return false;
    }

    if (!sol_buffer_focus_next_leaf(ui->buffers)) {
        return false;
    }

    sol_ui_mark_workspace_dirty(ui);
    return true;
}

void sol_ui_system_on_window_close(SolUISystem *ui, const Ca_Window *window)
{
    if (!ui || !window) {
        return;
    }

    if (ui->primary_window == window) {
        ui->primary_window = NULL;
        ui->workspace_host = NULL;
        ui->workspace_content_host = NULL;
        ui->overlay_host = NULL;
    }
}

void sol_ui_system_on_window_resize(SolUISystem *ui, int width, int height)
{
    if (!ui) {
        return;
    }

    if (width > 0) {
        ui->viewport_width = width;
    }
    if (height > 0) {
        ui->viewport_height = height;
    }

    sol_ui_mark_overlay_dirty(ui);
}
