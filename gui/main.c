/*
 * CGSS Resource Tool GUI.
 *
 * The footer icons are sampled from the game's original NGUI atlases.  The
 * existing command-line program remains the owner of CDN/download/unpack
 * operations; this window presents the resulting Spine scenes.
 */
#define SDL_MAIN_HANDLED

#ifndef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#endif
#ifndef CIMGUI_USE_SDL3
#define CIMGUI_USE_SDL3
#endif

#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cimgui.h>
#include <cimgui_impl.h>
#include "renderer3_bridge.h"
#include "spine_scene.h"
#include "voice_player.h"
#include "bgm_player.h"
#include "resource_backend.h"

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum GuiPage {
    GUI_PAGE_HOME = 0,
    GUI_PAGE_DOWNLOAD,
    GUI_PAGE_UNPACK,
    GUI_PAGE_SPINE,
    GUI_PAGE_SETTINGS
} GuiPage;

typedef struct UiTexture {
    SDL_Texture *texture;
    int width;
    int height;
} UiTexture;

typedef struct UiSlice {
    UiTexture *atlas;
    int x;
    int y;
    int width;
    int height;
} UiSlice;

typedef struct UiAssets {
    UiTexture footer;
    UiTexture home_slide;
    UiTexture menu;
    UiTexture mishiro_footer;
    UiTexture button_large;
    UiTexture button_small;
    UiTexture button_toggle;
    UiTexture button;
    UiTexture tab;
    UiTexture tab_small;
    UiTexture bg_circle;
    UiTexture bg_circle_01;
    UiTexture bg_circle_02;
    UiTexture bg_icon;
    UiTexture live_icon;
    UiTexture panel_light;
    UiTexture panel_light_wide;
    UiTexture panel_dark;
    UiTexture check_on;
    UiTexture check_off;
    UiTexture arrow_left;
    UiTexture arrow_right;
    UiTexture voice_icon;
    bool loaded;
    char root[2048];
} UiAssets;

typedef struct GuiApp {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SpineScene *spine;
    VoicePlayer *voice;
    BgmPlayer *bgm;
    ResourceBackend *resources;
    ResourceItem *resource_items;
    int resource_count;
    int resource_capacity;
    ResourceCategory resource_category;
    char resource_query[256];
    char resource_search_status[512];
    bool resource_auto_unpack;
    ResourceJobState resource_last_state;
    bool bgm_download_attempted;
    bool bgm_download_active;
    char bgm_download_status[512];
    UiAssets ui;
    char exe_dir[2048];
    bool spine_background_enabled;
    float spine_opacity;
    float spine_zoom;
    float animation_speed;
    ImFont *font_regular;
    ImFont *font_bold;
    bool show_demo;
    bool running;
    GuiPage page;
    Uint64 last_ticks;
} GuiApp;

static const char *const resource_category_names[RESOURCE_CATEGORY_COUNT] = {
    "全部资源", "BGM", "歌曲", "卡片", "角色语音", "谱面",
    "舞台", "动作", "3D 模型", "Spine / Live2D", "贴纸", "CG 影片"
};

static ImVec2_c v2(float x, float y)
{
    ImVec2_c value = {x, y};
    return value;
}

static ImVec4_c v4(float x, float y, float z, float w)
{
    ImVec4_c value = {x, y, z, w};
    return value;
}

/* The bundled Mishiro font is primarily Japanese. Keep a compact CJK range
 * in the atlas so the Chinese labels used by this tool resolve through the
 * system fallback instead of turning into tofu boxes. */
static const ImWchar *gui_cjk_ranges(void)
{
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, 0x2000, 0x206F, 0x3000, 0x30FF,
        0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0xFF00, 0xFFEF, 0
    };
    return ranges;
}

static ImU32 rgba(unsigned int r, unsigned int g, unsigned int b,
                  unsigned int a)
{
    return (ImU32)(r | (g << 8) | (b << 16) | (a << 24));
}

static void print_sdl_error(const char *where)
{
    fprintf(stderr, "%s 失败：%s\n", where, SDL_GetError());
}

static bool utf8_to_wide(const char *src, wchar_t *dst, int capacity)
{
    int n;
    if (!src || !dst || capacity <= 0)
        return false;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1,
                            dst, capacity);
    if (n <= 0)
        n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, capacity);
    return n > 0;
}

static bool wide_path_exists(const char *path)
{
    wchar_t wide[2048];
    return utf8_to_wide(path, wide, (int)(sizeof(wide) / sizeof(wide[0]))) &&
           GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

static bool load_ui_png(SDL_Renderer *renderer, const char *root,
                        const char *name, UiTexture *out)
{
    char path[2048];
    SDL_Surface *surface;
    SDL_Surface *converted;
    SDL_Texture *texture;
    if (!renderer || !root || !name || !out)
        return false;
    snprintf(path, sizeof(path), "%s\\%s", root, name);
    surface = SDL_LoadSurface(path);
    if (!surface)
        return false;
    converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    if (!converted)
        return false;
    texture = SDL_CreateTextureFromSurface(renderer, converted);
    out->width = converted->w;
    out->height = converted->h;
    SDL_DestroySurface(converted);
    if (!texture)
        return false;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    out->texture = texture;
    return true;
}

static void free_ui_assets(UiAssets *ui)
{
    if (!ui)
        return;
    if (ui->footer.texture)
        SDL_DestroyTexture(ui->footer.texture);
    if (ui->home_slide.texture)
        SDL_DestroyTexture(ui->home_slide.texture);
    if (ui->menu.texture)
        SDL_DestroyTexture(ui->menu.texture);
    if (ui->mishiro_footer.texture)
        SDL_DestroyTexture(ui->mishiro_footer.texture);
    if (ui->button_large.texture)
        SDL_DestroyTexture(ui->button_large.texture);
    if (ui->button_small.texture)
        SDL_DestroyTexture(ui->button_small.texture);
    if (ui->button_toggle.texture)
        SDL_DestroyTexture(ui->button_toggle.texture);
    if (ui->button.texture)
        SDL_DestroyTexture(ui->button.texture);
    if (ui->tab.texture)
        SDL_DestroyTexture(ui->tab.texture);
    if (ui->tab_small.texture)
        SDL_DestroyTexture(ui->tab_small.texture);
    if (ui->bg_circle.texture)
        SDL_DestroyTexture(ui->bg_circle.texture);
    if (ui->bg_circle_01.texture)
        SDL_DestroyTexture(ui->bg_circle_01.texture);
    if (ui->bg_circle_02.texture)
        SDL_DestroyTexture(ui->bg_circle_02.texture);
    if (ui->bg_icon.texture)
        SDL_DestroyTexture(ui->bg_icon.texture);
    if (ui->live_icon.texture)
        SDL_DestroyTexture(ui->live_icon.texture);
    if (ui->panel_light.texture)
        SDL_DestroyTexture(ui->panel_light.texture);
    if (ui->panel_light_wide.texture)
        SDL_DestroyTexture(ui->panel_light_wide.texture);
    if (ui->panel_dark.texture)
        SDL_DestroyTexture(ui->panel_dark.texture);
    if (ui->check_on.texture)
        SDL_DestroyTexture(ui->check_on.texture);
    if (ui->check_off.texture)
        SDL_DestroyTexture(ui->check_off.texture);
    if (ui->arrow_left.texture)
        SDL_DestroyTexture(ui->arrow_left.texture);
    if (ui->arrow_right.texture)
        SDL_DestroyTexture(ui->arrow_right.texture);
    if (ui->voice_icon.texture)
        SDL_DestroyTexture(ui->voice_icon.texture);
    memset(ui, 0, sizeof(*ui));
}

static bool load_ui_assets(GuiApp *app)
{
    const char *base;
    const char *suffixes[] = {
        "assets\\game_ui",
        "..\\assets\\game_ui",
        "..\\..\\assets\\game_ui"
    };
    char candidate[2048];
    char probe[2048];
    int i;

    memset(&app->ui, 0, sizeof(app->ui));
    base = SDL_GetBasePath();
    if (!base)
        base = ".\\";
    for (i = 0; i < (int)(sizeof(suffixes) / sizeof(suffixes[0])); ++i) {
        snprintf(candidate, sizeof(candidate), "%s\\%s", base, suffixes[i]);
        snprintf(probe, sizeof(probe), "%s\\Footer.png", candidate);
        if (!wide_path_exists(probe))
            continue;
        if (load_ui_png(app->renderer, candidate, "Footer.png",
                        &app->ui.footer) &&
            load_ui_png(app->renderer, candidate, "HomeSlideButton.png",
                        &app->ui.home_slide) &&
            load_ui_png(app->renderer, candidate, "Menu.png", &app->ui.menu)) {
            /* Mishiro uses this precomposed five-entry footer in its window. */
            load_ui_png(app->renderer, candidate, "MishiroFooter.png",
                        &app->ui.mishiro_footer);
            /* These are the reusable Mishiro chrome sheets. They are
             * optional so an older release with only the original atlases
             * still starts and falls back to ImGui controls. */
            load_ui_png(app->renderer, candidate, "button_large.png",
                        &app->ui.button_large);
            load_ui_png(app->renderer, candidate, "button_small.png",
                        &app->ui.button_small);
            load_ui_png(app->renderer, candidate, "button_toggle.png",
                        &app->ui.button_toggle);
            load_ui_png(app->renderer, candidate, "button.png",
                        &app->ui.button);
            load_ui_png(app->renderer, candidate, "tab.png", &app->ui.tab);
            load_ui_png(app->renderer, candidate, "tab_small.png",
                        &app->ui.tab_small);
            load_ui_png(app->renderer, candidate, "bg_anim_circle.png",
                        &app->ui.bg_circle);
            load_ui_png(app->renderer, candidate, "bg_anim_circle_01.png",
                        &app->ui.bg_circle_01);
            load_ui_png(app->renderer, candidate, "bg_anim_circle_02.png",
                        &app->ui.bg_circle_02);
            load_ui_png(app->renderer, candidate, "bg_anim_icon.png",
                        &app->ui.bg_icon);
            load_ui_png(app->renderer, candidate, "live_icon_857x114.png",
                        &app->ui.live_icon);
            load_ui_png(app->renderer, candidate, "panel_light.png",
                        &app->ui.panel_light);
            load_ui_png(app->renderer, candidate, "panel_light_wide.png",
                        &app->ui.panel_light_wide);
            load_ui_png(app->renderer, candidate, "panel_dark.png",
                        &app->ui.panel_dark);
            load_ui_png(app->renderer, candidate, "check_on.png",
                        &app->ui.check_on);
            load_ui_png(app->renderer, candidate, "check_off.png",
                        &app->ui.check_off);
            load_ui_png(app->renderer, candidate, "arrow_left.png",
                        &app->ui.arrow_left);
            load_ui_png(app->renderer, candidate, "arrow_right.png",
                        &app->ui.arrow_right);
            load_ui_png(app->renderer, candidate, "voice_icon.png",
                        &app->ui.voice_icon);
            app->ui.loaded = true;
            snprintf(app->ui.root, sizeof(app->ui.root), "%s", candidate);
            fprintf(stderr, "[UI] 已加载游戏原生 Atlas：%s\n", candidate);
            return true;
        }
        free_ui_assets(&app->ui);
    }
    fprintf(stderr, "[UI] 未找到 assets\\game_ui，导航将使用文字回退\n");
    return false;
}

static ImTextureRef_c texture_ref(SDL_Texture *texture)
{
    ImTextureRef_c ref;
    memset(&ref, 0, sizeof(ref));
    ref._TexID = (ImTextureID)(uintptr_t)texture;
    return ref;
}

static bool draw_ui_slice(const char *id, const UiSlice *slice, float scale,
                          bool selected)
{
    ImVec2_c size;
    ImVec2_c uv0;
    ImVec2_c uv1;
    ImVec4_c tint;
    bool clicked;
    if (!slice || !slice->atlas || !slice->atlas->texture ||
        slice->atlas->width <= 0 || slice->atlas->height <= 0)
        return false;
    size = v2((float)slice->width * scale, (float)slice->height * scale);
    uv0 = v2((float)slice->x / (float)slice->atlas->width,
             (float)slice->y / (float)slice->atlas->height);
    uv1 = v2((float)(slice->x + slice->width) /
                 (float)slice->atlas->width,
             (float)(slice->y + slice->height) /
                 (float)slice->atlas->height);
    tint = selected ? v4(1.0f, 1.0f, 1.0f, 1.0f)
                   : v4(0.84f, 0.87f, 0.92f, 1.0f);
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, v2(2.0f, 2.0f));
    clicked = igImageButton(id, texture_ref(slice->atlas->texture), size,
                            uv0, uv1, v4(0.0f, 0.0f, 0.0f, 0.0f), tint);
    igPopStyleVar(1);
    return clicked;
}

static UiSlice home_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 535, 6, 75, 60};
    return value;
}

static UiSlice download_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 641, 244, 69, 61};
    return value;
}

static UiSlice unpack_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 641, 184, 70, 59};
    return value;
}

static UiSlice spine_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 132, 6, 77, 60};
    return value;
}

static UiSlice settings_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 641, 69, 67, 54};
    return value;
}

static UiSlice footer_base_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 0, 225, 638, 78};
    return value;
}

static UiSlice footer_border_slice(UiAssets *ui)
{
    UiSlice value = {&ui->footer, 0, 304, 640, 80};
    return value;
}

static UiSlice footer_selected_slice(UiAssets *ui, int index)
{
    static const int rects[5][4] = {
        {132, 146, 91, 78},
        {535, 67, 76, 78},
        {132, 67, 76, 78},
        {458, 146, 76, 78},
        {0, 24, 91, 78}
    };
    UiSlice value = {&ui->footer, 0, 0, 0, 0};
    if (index < 0)
        index = 0;
    if (index > 4)
        index = 4;
    value.x = rects[index][0];
    value.y = rects[index][1];
    value.width = rects[index][2];
    value.height = rects[index][3];
    return value;
}

static UiSlice mishiro_footer_slice(UiAssets *ui, int index, bool selected)
{
    static const int x[5] = {0, 132, 248, 364, 828};
    static const int width[5] = {132, 116, 116, 116, 132};
    UiSlice value = {&ui->mishiro_footer, 0, selected ? 66 : 0, 0, 66};
    if (index < 0)
        index = 0;
    if (index > 4)
        index = 4;
    value.x = x[index];
    value.width = width[index];
    return value;
}

static void draw_slice_at(ImDrawList *draw, const UiSlice *slice,
                          ImVec2_c top_left, ImVec2_c size, ImU32 tint)
{
    ImVec2_c uv0;
    ImVec2_c uv1;
    ImVec2_c bottom_right;
    if (!draw || !slice || !slice->atlas || !slice->atlas->texture ||
        slice->atlas->width <= 0 || slice->atlas->height <= 0)
        return;
    uv0 = v2((float)slice->x / (float)slice->atlas->width,
             (float)slice->y / (float)slice->atlas->height);
    uv1 = v2((float)(slice->x + slice->width) /
                 (float)slice->atlas->width,
             (float)(slice->y + slice->height) /
                 (float)slice->atlas->height);
    bottom_right = v2(top_left.x + size.x, top_left.y + size.y);
    ImDrawList_AddImage(draw, texture_ref(slice->atlas->texture), top_left,
                        bottom_right, uv0, uv1, tint);
}

static UiSlice sheet_slice(UiTexture *texture, int x, int y, int width,
                           int height);

/* Draw one of the game's nine-slice frames without stretching its rounded
 * corners.  The source rectangles are the same sprites used by NGUI; only
 * the center/edge regions expand to fit our resizable window. */
static void draw_nine_slice(ImDrawList *draw, UiTexture *texture,
                            ImVec2_c position, ImVec2_c size,
                            int border_left, int border_right,
                            int border_top, int border_bottom, ImU32 tint)
{
    int sw;
    int sh;
    int x[4];
    int y[4];
    float dx[4];
    float dy[4];
    int i;
    int j;

    if (!draw || !texture || !texture->texture)
        return;
    sw = texture->width;
    sh = texture->height;
    if (sw <= 0 || sh <= 0 || size.x <= 0.0f || size.y <= 0.0f)
        return;
    if (border_left + border_right > sw)
        border_left = border_right = 0;
    if (border_top + border_bottom > sh)
        border_top = border_bottom = 0;
    x[0] = 0;
    x[1] = border_left;
    x[2] = sw - border_right;
    x[3] = sw;
    y[0] = 0;
    y[1] = border_top;
    y[2] = sh - border_bottom;
    y[3] = sh;
    dx[0] = position.x;
    dx[1] = position.x + (float)border_left;
    dx[2] = position.x + size.x - (float)border_right;
    dx[3] = position.x + size.x;
    dy[0] = position.y;
    dy[1] = position.y + (float)border_top;
    dy[2] = position.y + size.y - (float)border_bottom;
    dy[3] = position.y + size.y;
    for (j = 0; j < 3; ++j) {
        for (i = 0; i < 3; ++i) {
            UiSlice piece;
            ImVec2_c target;
            ImVec2_c target_size;
            if (x[i + 1] <= x[i] || y[j + 1] <= y[j] ||
                dx[i + 1] <= dx[i] || dy[j + 1] <= dy[j])
                continue;
            piece = sheet_slice(texture, x[i], y[j],
                                x[i + 1] - x[i], y[j + 1] - y[j]);
            target = v2(dx[i], dy[j]);
            target_size = v2(dx[i + 1] - dx[i], dy[j + 1] - dy[j]);
            draw_slice_at(draw, &piece, target, target_size, tint);
        }
    }
}

static UiSlice sheet_slice(UiTexture *texture, int x, int y, int width,
                           int height)
{
    UiSlice value = {texture, x, y, width, height};
    return value;
}

static UiSlice game_button_slice(UiAssets *ui, int kind, bool highlighted)
{
    UiTexture *texture = ui ? &ui->button_large : NULL;
    int columns = 3;
    int column = kind;
    int cell_width;
    int cell_height;
    int row;

    if (!texture || !texture->texture) {
        UiSlice empty = {NULL, 0, 0, 0, 0};
        return empty;
    }
    if (kind == 3) {
        texture = &ui->button_small;
        columns = 2;
        column = 0;
    } else if (kind == 4) {
        texture = &ui->button_small;
        columns = 2;
        column = 1;
    } else if (kind == 5) {
        texture = &ui->button;
        columns = 3;
        column = 2;
    }
    if (!texture->texture)
        return sheet_slice(NULL, 0, 0, 0, 0);
    cell_width = texture->width / columns;
    cell_height = texture->height / 2;
    row = highlighted ? 1 : 0;
    return sheet_slice(texture, column * cell_width, row * cell_height,
                       cell_width, cell_height);
}

static bool game_button_at(GuiApp *app, const char *id, const char *label,
                           ImVec2_c position, ImVec2_c size, int kind,
                           bool selected)
{
    bool clicked;
    bool hovered;
    UiSlice slice;
    ImDrawList *draw;
    ImVec2_c text_size;
    ImVec2_c text_position;
    ImVec2_c image_position;
    ImVec2_c image_size;
    ImU32 text_color;
    float scale;

    if (!app || !id || !label)
        return false;
    igSetCursorScreenPos(position);
    slice = game_button_slice(&app->ui, kind, selected);
    if (!slice.atlas || !slice.atlas->texture) {
        return igButton(label, size);
    }
    clicked = igInvisibleButton(id, size, ImGuiButtonFlags_None);
    hovered = igIsItemHovered(ImGuiHoveredFlags_None);
    draw = igGetWindowDrawList();
    scale = fminf(size.x / (float)slice.width,
                  size.y / (float)slice.height);
    image_size = v2((float)slice.width * scale,
                    (float)slice.height * scale);
    image_position = v2(position.x + (size.x - image_size.x) * 0.5f,
                         position.y + (size.y - image_size.y) * 0.5f);
    draw_slice_at(draw, &slice, image_position, image_size,
                  rgba(255, 255, 255, hovered || selected ? 255 : 238));
    text_size = igCalcTextSize(label, NULL, false, -1.0f);
    text_position = v2(position.x + (size.x - text_size.x) * 0.5f,
                        position.y + (size.y - text_size.y) * 0.5f - 1.0f);
    text_color = kind == 0 ? rgba(255, 255, 255, 255) :
                 (selected ? rgba(255, 255, 255, 255) :
                             rgba(32, 37, 48, 255));
    ImDrawList_AddText_Vec2(draw, text_position, text_color, label, NULL);
    return clicked;
}

static bool game_checkbox_at(GuiApp *app, const char *id, const char *label,
                             ImVec2_c position, bool *value, bool dark)
{
    UiTexture *texture;
    ImDrawList *draw;
    bool clicked;
    ImVec2_c text_position;
    if (!app || !id || !label || !value)
        return false;
    texture = *value ? &app->ui.check_on : &app->ui.check_off;
    igSetCursorScreenPos(position);
    clicked = igInvisibleButton(id, v2(30.0f, 30.0f),
                                ImGuiButtonFlags_None);
    if (clicked)
        *value = !*value;
    draw = igGetWindowDrawList();
    if (texture->texture) {
        UiSlice slice = sheet_slice(texture, 0, 0, texture->width,
                                     texture->height);
        draw_slice_at(draw, &slice, position, v2(30.0f, 30.0f),
                      rgba(255, 255, 255, igIsItemHovered(ImGuiHoveredFlags_None)
                                      ? 255 : 238));
    }
    text_position = v2(position.x + 38.0f, position.y + 5.0f);
    ImDrawList_AddText_Vec2(draw, text_position,
                            dark ? rgba(247, 248, 252, 255) :
                                   rgba(38, 42, 52, 255),
                            label, NULL);
    return clicked;
}

static void begin_ui_layer(const char *name, ImVec2_c position, ImVec2_c size)
{
    igSetNextWindowPos(position, ImGuiCond_Always, v2(0.0f, 0.0f));
    igSetNextWindowSize(size, ImGuiCond_Always);
    igPushStyleColor_U32(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
    igBegin(name, NULL, ImGuiWindowFlags_NoDecoration |
                          ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBackground);
}

static void end_ui_layer(void)
{
    igEnd();
    igPopStyleVar(1);
    igPopStyleColor(1);
}

static void begin_game_panel(GuiApp *app, const char *name, ImVec2_c position,
                             ImVec2_c size, bool dark)
{
    ImDrawList *draw;
    ImVec2_c bottom_right;
    ImU32 fill = dark ? rgba(33, 35, 43, 224) : rgba(249, 250, 253, 238);
    ImU32 inner = dark ? rgba(65, 67, 76, 82) : rgba(255, 255, 255, 120);
    ImU32 border = dark ? rgba(22, 24, 30, 245) : rgba(75, 80, 92, 180);
    ImU32 text = dark ? rgba(247, 248, 252, 255) : rgba(38, 42, 52, 255);
    ImU32 muted = dark ? rgba(187, 191, 204, 255) : rgba(91, 97, 110, 255);

    igSetNextWindowPos(position, ImGuiCond_Always, v2(0.0f, 0.0f));
    igSetNextWindowSize(size, ImGuiCond_Always);
    igPushStyleColor_U32(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
    igPushStyleColor_U32(ImGuiCol_Border, rgba(0, 0, 0, 0));
    igPushStyleColor_U32(ImGuiCol_Text, text);
    igPushStyleColor_U32(ImGuiCol_TextDisabled, muted);
    igPushStyleColor_U32(ImGuiCol_FrameBg,
                         dark ? rgba(28, 30, 37, 230) :
                                rgba(242, 244, 248, 238));
    igPushStyleColor_U32(ImGuiCol_FrameBgHovered,
                         dark ? rgba(50, 52, 62, 242) :
                                rgba(255, 255, 255, 250));
    igPushStyleColor_U32(ImGuiCol_FrameBgActive,
                         dark ? rgba(64, 66, 78, 250) :
                                rgba(255, 255, 255, 255));
    igPushStyleColor_U32(ImGuiCol_PopupBg,
                         dark ? rgba(31, 33, 40, 250) :
                                rgba(250, 251, 253, 252));
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 10.0f);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(16.0f, 13.0f));
    igBegin(name, NULL, ImGuiWindowFlags_NoDecoration |
                          ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings);
    draw = igGetWindowDrawList();
    bottom_right = v2(position.x + size.x, position.y + size.y);
    if (app && app->ui.panel_light.texture &&
        app->ui.panel_dark.texture) {
        UiTexture *panel = dark ? &app->ui.panel_dark :
                       (size.x >= 520.0f && app->ui.panel_light_wide.texture ?
                            &app->ui.panel_light_wide : &app->ui.panel_light);
        draw_nine_slice(draw, panel, position, size, 20, 20, 20, 20,
                        rgba(255, 255, 255, dark ? 170 : 188));
    } else {
        ImDrawList_AddRectFilled(draw, position, bottom_right, fill, 10.0f,
                                 ImDrawFlags_None);
        ImDrawList_AddRectFilled(draw,
                                 v2(position.x + 3.0f, position.y + 3.0f),
                                 v2(bottom_right.x - 3.0f, bottom_right.y - 3.0f),
                                 inner, 7.0f, ImDrawFlags_None);
        ImDrawList_AddRect(draw, position, bottom_right, border, 10.0f, 2.0f,
                           ImDrawFlags_None);
    }
    ImDrawList_AddLine(draw, v2(position.x + 16.0f, position.y + 36.0f),
                       v2(bottom_right.x - 16.0f, position.y + 36.0f),
                       dark ? rgba(255, 255, 255, 42) : rgba(40, 44, 54, 55),
                       1.0f);
}

static void end_game_panel(void)
{
    igEnd();
    igPopStyleVar(2);
    igPopStyleColor(8);
}

static void draw_full_texture(ImDrawList *draw, const UiTexture *texture,
                              ImVec2_c position, ImVec2_c size, ImU32 tint)
{
    UiSlice full;
    if (!texture || !texture->texture)
        return;
    full = sheet_slice((UiTexture *)texture, 0, 0, texture->width,
                       texture->height);
    draw_slice_at(draw, &full, position, size, tint);
}

static void draw_fallback_stage(SDL_Renderer *renderer, int width, int height,
                                float time_s)
{
    float shift = fmodf(time_s * 8.0f, 96.0f);
    /* The fallback keeps the same bright, paper-like base as the client.  A
     * real Spine scene replaces it entirely; this is only visible before the
     * first card has been unpacked. */
    SDL_SetRenderDrawColorFloat(renderer, 0.98f, 0.985f, 0.99f, 1.0f);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(renderer, 0.48f, 0.72f, 0.82f, 0.055f);
    for (int x = -height; x < width + height; x += 72) {
        SDL_FRect stripe = {(float)x + shift, 0.0f, 18.0f, (float)height};
        SDL_RenderFillRect(renderer, &stripe);
    }
    SDL_SetRenderDrawColorFloat(renderer, 1.0f, 1.0f, 1.0f, 0.55f);
    for (int y = 28; y < height; y += 64)
        SDL_RenderLine(renderer, 0.0f, (float)y, (float)width, (float)y);
}

static void draw_background(GuiApp *app, int width, int height, float time_s)
{
    if (app->spine_background_enabled && spine_scene_is_loaded(app->spine)) {
        SDL_SetRenderDrawColorFloat(app->renderer, 0.94f, 0.97f, 0.99f, 1.0f);
        SDL_RenderClear(app->renderer);
        /* The dynamic card is the full-screen bottom layer.  The footer and
         * information surface are ImGui overlays drawn afterward. */
        spine_scene_render(app->spine, width, height,
                           app->spine_opacity, app->spine_zoom);
    } else {
        draw_fallback_stage(app->renderer, width, height, time_s);
    }
}

static void draw_game_chrome_background(GuiApp *app, float width,
                                         float height, float time_s)
{
    ImGuiViewport *viewport;
    ImDrawList *draw;
    float drift = sinf(time_s * 0.18f) * 8.0f;

    if (!app)
        return;
    viewport = igGetMainViewport();
    draw = igGetBackgroundDrawList(viewport);
    if (!draw)
        return;
    if (app->ui.bg_circle_01.texture) {
        draw_full_texture(draw, &app->ui.bg_circle_01,
                          v2(-86.0f + drift, height - 420.0f),
                          v2(520.0f, 520.0f), rgba(255, 255, 255, 48));
    }
    if (app->ui.bg_circle_02.texture) {
        draw_full_texture(draw, &app->ui.bg_circle_02,
                          v2(width - 390.0f - drift, -102.0f),
                          v2(510.0f, 510.0f), rgba(255, 255, 255, 42));
    }
    if (app->ui.bg_circle.texture) {
        draw_full_texture(draw, &app->ui.bg_circle,
                          v2(width * 0.50f - 210.0f,
                             height * 0.50f - 240.0f),
                          v2(420.0f, 420.0f), rgba(255, 255, 255, 16));
    }
    if (app->ui.bg_icon.texture) {
        draw_full_texture(draw, &app->ui.bg_icon,
                          v2(width * 0.50f - 170.0f, height * 0.50f - 126.0f),
                          v2(340.0f, 290.0f), rgba(255, 255, 255, 22));
    }
}

static bool start_home_bgm_download(GuiApp *app)
{
    ResourceItem item;

    if (!app || !app->bgm || bgm_player_source_exists(app->bgm))
        return true;
    if (app->bgm_download_active || app->bgm_download_attempted)
        return false;
    if (!app->resources || !resource_backend_is_ready(app->resources)) {
        app->bgm_download_attempted = true;
        snprintf(app->bgm_download_status,
                 sizeof(app->bgm_download_status),
                 "首页 BGM 下载不可用：%s", app->resources ?
                 resource_backend_error(app->resources) : "资源后端未创建");
        return false;
    }
    if (resource_backend_is_busy(app->resources)) {
        snprintf(app->bgm_download_status,
                 sizeof(app->bgm_download_status),
                 "等待当前下载完成后获取首页 BGM");
        return false;
    }

    memset(&item, 0, sizeof(item));
    snprintf(item.name, sizeof(item.name), "b/bgm_studio_night.acb");
    snprintf(item.hash, sizeof(item.hash),
             "f3bdf798ba05df59fa976bf568fb6e09");
    item.size = 2947040;
    item.selected = true;
    app->bgm_download_attempted = true;
    if (!resource_backend_start_download(app->resources,
                                         RESOURCE_CATEGORY_BGM,
                                         &item, 1, false)) {
        snprintf(app->bgm_download_status,
                 sizeof(app->bgm_download_status),
                 "首页 BGM 下载启动失败：%s",
                 resource_backend_error(app->resources));
        return false;
    }
    app->bgm_download_active = true;
    snprintf(app->bgm_download_status,
             sizeof(app->bgm_download_status),
             "正在下载 bgm_studio_night...");
    return true;
}

static const char *home_bgm_status(const GuiApp *app)
{
    if (!app || !app->bgm)
        return "BGM 播放器未创建";
    if (app->bgm_download_active ||
        (!bgm_player_source_exists(app->bgm) &&
         app->bgm_download_attempted && app->bgm_download_status[0]))
        return app->bgm_download_status;
    return bgm_player_status(app->bgm);
}

static void retry_home_bgm(GuiApp *app)
{
    if (!app || !app->bgm)
        return;
    if (bgm_player_source_exists(app->bgm)) {
        bgm_player_retry(app->bgm);
        return;
    }
    app->bgm_download_attempted = false;
    app->bgm_download_active = false;
    app->bgm_download_status[0] = '\0';
    start_home_bgm_download(app);
}

static void rescan_scene(GuiApp *app)
{
    if (!spine_scene_autoload(app->spine, app->exe_dir)) {
        fprintf(stderr, "[Spine] %s\n", spine_scene_error(app->spine));
    }
    if (app->voice)
        voice_player_rescan(app->voice, spine_scene_name(app->spine),
                            spine_scene_directory(app->spine));
}

static void resource_release_items(GuiApp *app)
{
    if (!app)
        return;
    free(app->resource_items);
    app->resource_items = NULL;
    app->resource_count = 0;
    app->resource_capacity = 0;
}

static bool resource_resize_items(GuiApp *app, int capacity)
{
    ResourceItem *items;

    if (!app || capacity < 0)
        return false;
    if (capacity == 0) {
        resource_release_items(app);
        return true;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(*items))
        return false;
    items = (ResourceItem *)realloc(app->resource_items,
                                    (size_t)capacity * sizeof(*items));
    if (!items)
        return false;
    app->resource_items = items;
    app->resource_capacity = capacity;
    return true;
}

static void resource_search(GuiApp *app)
{
    int count;
    int i;
    if (!app || !app->resources || !resource_backend_is_ready(app->resources))
        return;
    app->resource_search_status[0] = '\0';
    count = resource_backend_count(app->resources, app->resource_category,
                                   app->resource_query);
    if (count < 0) {
        resource_release_items(app);
        snprintf(app->resource_search_status,
                 sizeof(app->resource_search_status), "搜索失败：%s",
                 resource_backend_error(app->resources));
        return;
    }
    if (!resource_resize_items(app, count)) {
        resource_release_items(app);
        snprintf(app->resource_search_status,
                 sizeof(app->resource_search_status),
                 "搜索结果太大，无法分配内存（%d 条）", count);
        return;
    }
    if (count == 0)
        return;
    count = resource_backend_search(app->resources, app->resource_category,
                                    app->resource_query,
                                    app->resource_items, count);
    if (count < 0) {
        resource_release_items(app);
        snprintf(app->resource_search_status,
                 sizeof(app->resource_search_status), "搜索失败：%s",
                 resource_backend_error(app->resources));
        return;
    }
    app->resource_count = count;
    for (i = 0; i < app->resource_count; ++i)
        app->resource_items[i].selected = false;
}

static int resource_selected_count(const GuiApp *app)
{
    int count = 0;
    int i;
    if (!app)
        return 0;
    for (i = 0; i < app->resource_count; ++i)
        if (app->resource_items[i].selected)
            ++count;
    return count;
}

static void resource_select_all(GuiApp *app, bool selected)
{
    int i;
    if (!app)
        return;
    for (i = 0; i < app->resource_count; ++i)
        app->resource_items[i].selected = selected;
}

static int resource_build_selected(const GuiApp *app, ResourceItem *items,
                                   int capacity)
{
    int count = 0;
    int i;

    if (!app || !items || capacity <= 0)
        return 0;
    for (i = 0; i < app->resource_count && count < capacity; ++i) {
        if (!app->resource_items[i].selected)
            continue;
        items[count++] = app->resource_items[i];
    }
    return count;
}

static void draw_resource_download_panel(GuiApp *app, float width,
                                         float panel_height)
{
    ResourceJobSnapshot snapshot;
    int category = (int)app->resource_category;
    int selected;
    bool busy;
    float fraction = 0.0f;
    float table_height;
    float query_width;
    const char *download_label;
    bool submit_search;

    igText("资源下载");
    if (!app->resources || !resource_backend_is_ready(app->resources)) {
        igTextColored(v4(0.82f, 0.28f, 0.08f, 1.0f), "下载后端未就绪");
        igTextWrapped("%s", app->resources ?
                      resource_backend_error(app->resources) :
                      "资源后端创建失败");
        return;
    }

    /* Keep the search controls together.  The result table below is the
     * primary surface, so this row stays compact and has one search action. */
    igSetNextItemWidth(154.0f);
    if (igCombo_Str_arr("##resource_category", &category, resource_category_names,
                        RESOURCE_CATEGORY_COUNT, 8)) {
        app->resource_category = (ResourceCategory)category;
        resource_release_items(app);
        app->resource_search_status[0] = '\0';
    }
    igSameLine(0.0f, 8.0f);
    query_width = width - 520.0f;
    if (query_width < 140.0f)
        query_width = 140.0f;
    igSetNextItemWidth(query_width);
    submit_search = igInputTextWithHint(
        "##resource_query", "输入关键词，留空显示当前类别全部资源",
        app->resource_query, sizeof(app->resource_query),
        ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igSameLine(0.0f, 8.0f);
    if (igButton("搜索", v2(82.0f, 32.0f)) || submit_search)
        resource_search(app);
    igSameLine(0.0f, 8.0f);
    if (igButton("全选", v2(64.0f, 32.0f)))
        resource_select_all(app, true);
    igSameLine(0.0f, 8.0f);
    if (igButton("清空", v2(64.0f, 32.0f)))
        resource_select_all(app, false);
    igSameLine(0.0f, 8.0f);
    game_checkbox_at(app, "##auto_unpack", "自动解包",
                     igGetCursorScreenPos(), &app->resource_auto_unpack, false);

    resource_backend_snapshot(app->resources, &snapshot);
    busy = snapshot.state == RESOURCE_JOB_RUNNING;
    if (snapshot.total_items > 0 && snapshot.current_total_bytes > 0) {
        fraction = (float)snapshot.completed_items /
                   (float)snapshot.total_items;
        fraction += ((float)snapshot.current_bytes /
                     (float)snapshot.current_total_bytes) /
                    (float)snapshot.total_items;
        if (fraction > 1.0f)
            fraction = 1.0f;
    }
    if (snapshot.state != RESOURCE_JOB_IDLE) {
        igTextDisabled("%s", snapshot.status);
        if (snapshot.current_name[0])
            igText("当前：%s", snapshot.current_name);
        igProgressBar(fraction, v2(-1.0f, 16.0f), NULL);
    }

    selected = resource_selected_count(app);
    download_label = snapshot.state == RESOURCE_JOB_COMPLETED ||
                     snapshot.state == RESOURCE_JOB_FAILED ||
                     snapshot.state == RESOURCE_JOB_CANCELLED ?
                     "再次下载" : "开始下载";
    igTextDisabled("搜索结果 %d 条    已选择 %d 条", app->resource_count,
                   selected);
    if (app->resource_search_status[0])
        igTextColored(v4(0.78f, 0.24f, 0.18f, 1.0f), "%s",
                      app->resource_search_status);

    (void)panel_height;
    table_height = igGetContentRegionAvail().y - 48.0f;
    if (table_height < 150.0f)
        table_height = 150.0f;
    if (igBeginTable("##resource_results_table", 4,
                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_ScrollY |
                     ImGuiTableFlags_SizingStretchProp,
                     v2(-1.0f, table_height), 0.0f)) {
        ImGuiListClipper *clipper;
        igTableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed |
                           ImGuiTableColumnFlags_NoResize, 52.0f, 0);
        igTableSetupColumn("文件名", ImGuiTableColumnFlags_WidthStretch,
                           0.43f, 0);
        igTableSetupColumn("哈希值", ImGuiTableColumnFlags_WidthStretch,
                           0.40f, 0);
        igTableSetupColumn("文件大小", ImGuiTableColumnFlags_WidthFixed |
                           ImGuiTableColumnFlags_NoResize, 92.0f, 0);
        igTableSetupScrollFreeze(0, 1);
        igTableHeadersRow();

        clipper = ImGuiListClipper_ImGuiListClipper();
        if (clipper) {
            ImGuiListClipper_Begin(clipper, app->resource_count, 0.0f);
            while (ImGuiListClipper_Step(clipper)) {
                int i;
                for (i = clipper->DisplayStart; i < clipper->DisplayEnd; ++i) {
                    bool checked = app->resource_items[i].selected;
                    igTableNextRow(ImGuiTableRowFlags_None, 0.0f);
                    igPushID_Int(i);
                    igTableNextColumn();
                    if (igCheckbox("##select", &checked))
                        app->resource_items[i].selected = checked;
                    igTableNextColumn();
                    igText("%s", app->resource_items[i].name);
                    igTableNextColumn();
                    igTextDisabled("%s", app->resource_items[i].hash);
                    igTableNextColumn();
                    if (app->resource_items[i].size > 0)
                        igText("%lld KB", app->resource_items[i].size / 1024);
                    else
                        igTextDisabled("-");
                    igPopID();
                }
            }
            ImGuiListClipper_End(clipper);
            ImGuiListClipper_destroy(clipper);
        }
        igEndTable();
    }

    if (busy) {
        if (igButton("取消下载", v2(130.0f, 34.0f)))
            resource_backend_cancel(app->resources);
    } else {
        igBeginDisabled(selected == 0);
        if (igButton(download_label, v2(130.0f, 34.0f))) {
            ResourceItem *selected_items = NULL;
            int selected_count = 0;
            if ((size_t)selected <= SIZE_MAX / sizeof(*selected_items))
                selected_items = (ResourceItem *)malloc(
                    (size_t)selected * sizeof(*selected_items));
            if (selected_items) {
                selected_count = resource_build_selected(app, selected_items,
                                                          selected);
                if (selected_count > 0 &&
                    !resource_backend_start_download(
                        app->resources, app->resource_category,
                        selected_items, selected_count,
                        app->resource_auto_unpack))
                    snprintf(app->resource_search_status,
                             sizeof(app->resource_search_status),
                             "下载启动失败：%s",
                             resource_backend_error(app->resources));
            } else {
                snprintf(app->resource_search_status,
                         sizeof(app->resource_search_status),
                         "选中资源太多，无法分配内存");
            }
            free(selected_items);
        }
        igEndDisabled();
    }
    igSameLine(0.0f, 10.0f);
    igTextDisabled("%s", snapshot.output_directory[0] ?
                   snapshot.output_directory : "输出：程序目录\\CGSS_DOWN");
}

static float footer_scale(float width)
{
    float scale = (width - 16.0f) / 612.0f;
    if (scale > 1.0f)
        scale = 1.0f;
    if (scale < 0.55f)
        scale = 0.55f;
    return scale;
}

static float footer_reserved_height(float width)
{
    return 11.0f + 66.0f * footer_scale(width);
}

static bool footer_button(const char *id, float x, float y, float width,
                          float height, const char *tooltip)
{
    bool clicked;
    igSetCursorScreenPos(v2(x, y));
    clicked = igInvisibleButton(id, v2(width, height), ImGuiButtonFlags_None);
    if (igIsItemHovered(ImGuiHoveredFlags_None) && tooltip)
        igSetTooltip("%s", tooltip);
    return clicked;
}

static void draw_footer(GuiApp *app, float width, float height)
{
    static const float centers[5] = {66.0f, 190.0f, 306.0f, 422.0f, 546.0f};
    static const char *ids[5] = {
        "##nav_home", "##nav_idol", "##nav_commu", "##nav_live", "##nav_menu"
    };
    static const char *tips[5] = {
        "主页", "资源入口", "解包入口", "Spine 动态卡面", "设置"
    };
    static const GuiPage pages[5] = {
        GUI_PAGE_HOME, GUI_PAGE_DOWNLOAD, GUI_PAGE_UNPACK,
        GUI_PAGE_SPINE, GUI_PAGE_SETTINGS
    };
    UiSlice icons[5] = {
        home_slice(&app->ui), download_slice(&app->ui), unpack_slice(&app->ui),
        spine_slice(&app->ui), settings_slice(&app->ui)
    };
    float scale = footer_scale(width);
    bool has_mishiro_footer = app->ui.mishiro_footer.texture != NULL;
    float frame_width = (has_mishiro_footer ? 612.0f : 640.0f) * scale;
    float frame_height = (has_mishiro_footer ? 66.0f : 80.0f) * scale;
    float frame_x = (width - frame_width) * 0.5f;
    float frame_y = height - frame_height - 5.0f;
    ImDrawList *draw;
    bool open = true;

    /* This window is only an input/draw layer; the stage remains visible below. */
    igSetNextWindowPos(v2(0.0f, frame_y - 4.0f), ImGuiCond_Always,
                       v2(0.0f, 0.0f));
    igSetNextWindowSize(v2(width, frame_height + 12.0f), ImGuiCond_Always);
    igSetNextWindowBgAlpha(0.0f);
    igBegin("##game_footer", &open, ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBackground);
    draw = igGetWindowDrawList();
    if (has_mishiro_footer) {
        float item_x = frame_x;
        for (int i = 0; i < 5; ++i) {
            UiSlice item = mishiro_footer_slice(&app->ui, i,
                                                 app->page == pages[i]);
            float item_width = (float)item.width * scale;
            draw_slice_at(draw, &item, v2(item_x, frame_y),
                          v2(item_width, frame_height),
                          rgba(255, 255, 255, 255));
            if (footer_button(ids[i], item_x, frame_y, item_width,
                              frame_height, tips[i]))
                app->page = pages[i];
            item_x += item_width;
        }
    } else if (app->ui.loaded) {
        UiSlice base = footer_base_slice(&app->ui);
        UiSlice border = footer_border_slice(&app->ui);
        draw_slice_at(draw, &base, v2(frame_x + scale, frame_y + scale),
                      v2(638.0f * scale, 78.0f * scale), rgba(255, 255, 255, 255));
        for (int i = 0; i < 5; ++i) {
            if (app->page == pages[i]) {
                UiSlice selected = footer_selected_slice(&app->ui, i);
                float selected_w = (float)selected.width * scale;
                float selected_h = (float)selected.height * scale;
                draw_slice_at(draw, &selected,
                              v2(frame_x + centers[i] * scale - selected_w * 0.5f,
                                 frame_y + (80.0f * scale - selected_h) * 0.5f),
                              v2(selected_w, selected_h),
                              rgba(255, 255, 255, 255));
            }
        }
        draw_slice_at(draw, &border, v2(frame_x, frame_y),
                      v2(frame_width, frame_height), rgba(255, 255, 255, 255));
        for (int i = 0; i < 5; ++i) {
            float icon_w = (float)icons[i].width * scale;
            float icon_h = (float)icons[i].height * scale;
            draw_slice_at(draw, &icons[i],
                          v2(frame_x + centers[i] * scale - icon_w * 0.5f,
                             frame_y + (80.0f * scale - icon_h) * 0.5f),
                          v2(icon_w, icon_h), rgba(255, 255, 255, 255));
        }
        for (int i = 0; i < 5; ++i) {
            float hit_width = 104.0f * scale;
            if (footer_button(ids[i], frame_x + centers[i] * scale - hit_width * 0.5f,
                              frame_y + 2.0f * scale, hit_width,
                              76.0f * scale, tips[i]))
                app->page = pages[i];
        }
    } else {
        igSetCursorScreenPos(v2(frame_x, frame_y + 18.0f));
        if (igButton("主页", v2(94.0f, 42.0f))) app->page = GUI_PAGE_HOME;
        igSameLine(0.0f, 6.0f);
        if (igButton("下载", v2(94.0f, 42.0f))) app->page = GUI_PAGE_DOWNLOAD;
        igSameLine(0.0f, 6.0f);
        if (igButton("解包", v2(94.0f, 42.0f))) app->page = GUI_PAGE_UNPACK;
        igSameLine(0.0f, 6.0f);
        if (igButton("Spine", v2(94.0f, 42.0f))) app->page = GUI_PAGE_SPINE;
        igSameLine(0.0f, 6.0f);
        if (igButton("设置", v2(94.0f, 42.0f))) app->page = GUI_PAGE_SETTINGS;
    }
    igEnd();
}

static void draw_home_workspace(GuiApp *app, float width, float stage_height)
{
    /* The home screen is intentionally unobstructed: the Spine stage and the
     * native footer are the only visible UI surfaces. */
    (void)app;
    (void)width;
    (void)stage_height;
}

/* One translucent game-like content surface. The Spine scene is deliberately
 * left visible through it; individual pages only change the controls inside. */
static void draw_main_panel(GuiApp *app, float width, float stage_height)
{
    float work_y = 28.0f;
    float work_height = stage_height - work_y - 12.0f;
    float work_width = width - 56.0f;
    bool enabled;
    float volume;

    if (work_height < 180.0f)
        work_height = 180.0f;
    if (app->page == GUI_PAGE_HOME) {
        draw_home_workspace(app, width, stage_height);
        return;
    }

    if (app->page == GUI_PAGE_DOWNLOAD) {
        float surface_x = 44.0f;
        float surface_width = width - 88.0f;
        float surface_y = 100.0f;
        float surface_height = stage_height - surface_y - 12.0f;
        if (surface_height < 220.0f)
            surface_height = 220.0f;
        if (surface_width < 420.0f) {
            surface_x = 24.0f;
            surface_width = width - 48.0f;
        }
        /* The download page has one purposeful surface: a compact command
         * row above a large, virtualized manifest table.  Progress and
         * output stay in the same surface so the list gets most of the room. */
        begin_game_panel(app, "##download_surface",
                         v2(surface_x, surface_y),
                         v2(surface_width, surface_height), false);
        draw_resource_download_panel(app, surface_width - 32.0f,
                                     surface_height - 22.0f);
        end_game_panel();
        return;
    }

    if (app->page == GUI_PAGE_UNPACK) {
        float left = work_width * 0.58f;
        begin_game_panel(app, "##unpack_workspace", v2(28.0f, work_y),
                         v2(left, work_height), true);
        igText("解包管理");
        igTextDisabled("AssetStudio、LZ4 与 Spine 转换共用主工程实现");
        igSeparator();
        if (spine_scene_is_loaded(app->spine)) {
            igTextColored(v4(0.06f, 0.62f, 0.40f, 1.0f), "当前卡面");
            igTextWrapped("%s", spine_scene_directory(app->spine));
            igTextDisabled("%d 个图层", spine_scene_layer_count(app->spine));
        } else {
            igTextColored(v4(0.86f, 0.40f, 0.08f, 1.0f), "没有可显示的卡面目录");
        }
        igSpacing();
        if (game_button_at(app, "##unpack_rescan", "重新载入卡面",
                           igGetCursorScreenPos(), v2(170.0f, 42.0f),
                           0, false))
            rescan_scene(app);
        end_game_panel();

        begin_game_panel(app, "##unpack_actions", v2(44.0f + left, work_y),
                         v2(work_width - left - 16.0f, work_height), false);
        igText("处理状态");
        igTextDisabled("下载页勾选自动解包后，完成的资源会进入这里");
        igSeparator();
        if (app->resources) {
            ResourceJobSnapshot snapshot;
            resource_backend_snapshot(app->resources, &snapshot);
            igTextWrapped("%s", snapshot.status[0] ? snapshot.status :
                          "等待任务");
            if (snapshot.current_name[0])
                igTextWrapped("当前：%s", snapshot.current_name);
            if (snapshot.output_directory[0]) {
                igTextDisabled("输出目录");
                igTextWrapped("%s", snapshot.output_directory);
            }
        }
        end_game_panel();
        return;
    }

    if (app->page == GUI_PAGE_SPINE) {
        float left = work_width * 0.44f;
        begin_game_panel(app, "##spine_info", v2(28.0f, work_y),
                         v2(left, work_height), true);
        igText("Spine 舞台");
        igTextDisabled("背景范围以 bg 底板为准，动态越界自然裁切");
        igSeparator();
        if (spine_scene_is_loaded(app->spine)) {
            igTextColored(v4(0.06f, 0.62f, 0.40f, 1.0f), "%s",
                          spine_scene_name(app->spine));
            igTextDisabled("图层 %d", spine_scene_layer_count(app->spine));
            for (int i = 0; i < spine_scene_layer_count(app->spine); ++i)
                igBulletText("%s", spine_scene_layer_name(app->spine, i));
        } else {
            igTextColored(v4(0.86f, 0.40f, 0.08f, 1.0f), "未找到卡面 Spine");
            igTextDisabled("%s", spine_scene_error(app->spine));
        }
        if (game_button_at(app, "##spine_rescan", "重新扫描",
                           v2(46.0f, work_y + work_height - 58.0f),
                           v2(150.0f, 40.0f), 0, false))
            rescan_scene(app);
        end_game_panel();

        begin_game_panel(app, "##spine_options", v2(44.0f + left, work_y),
                         v2(work_width - left - 16.0f, work_height), false);
        igText("舞台控制");
        igTextDisabled("Live2D / Spine 动画始终位于最底层");
        igSeparator();
        game_checkbox_at(app, "##spine_enabled", "启用动态背景",
                         igGetCursorScreenPos(),
                         &app->spine_background_enabled, false);
        igSliderFloat("不透明度", &app->spine_opacity, 0.10f, 1.0f,
                      "%.2f", 0);
        igSliderFloat("缩放", &app->spine_zoom, 0.50f, 1.80f, "%.2f", 0);
        igSliderFloat("动画速度", &app->animation_speed, 0.0f, 2.0f,
                      "%.2f", 0);
        end_game_panel();
        return;
    }

    /* Settings uses two work surfaces so audio controls do not visually merge
     * with the stage controls. */
    {
        float left = work_width * 0.52f;
        bool bgm_enabled = app->bgm && bgm_player_is_enabled(app->bgm);
        float bgm_volume = app->bgm ? bgm_player_volume(app->bgm) : 0.0f;
        enabled = app->voice && voice_player_is_enabled(app->voice);
        volume = app->voice ? voice_player_volume(app->voice) : 0.0f;
        begin_game_panel(app, "##settings_display", v2(28.0f, work_y),
                         v2(left, work_height), false);
        igText("显示设置");
        igTextDisabled("调整卡面在游戏主界面中的表现");
        igSeparator();
        game_checkbox_at(app, "##settings_spine_enabled", "启用 Spine 背景",
                         igGetCursorScreenPos(),
                         &app->spine_background_enabled, false);
        igSliderFloat("不透明度", &app->spine_opacity, 0.10f, 1.0f,
                      "%.2f", 0);
        igSliderFloat("缩放", &app->spine_zoom, 0.50f, 1.80f, "%.2f", 0);
        igSliderFloat("动画速度", &app->animation_speed, 0.0f, 2.0f,
                      "%.2f", 0);
        end_game_panel();

        begin_game_panel(app, "##settings_audio", v2(44.0f + left, work_y),
                         v2(work_width - left - 16.0f, work_height), true);
        igText("声音");
        igTextDisabled("首页 BGM 与角色随机语音");
        igSeparator();
        if (game_checkbox_at(app, "##settings_bgm", "主页播放 bgm_studio_night",
                             igGetCursorScreenPos(), &bgm_enabled, true) && app->bgm)
            bgm_player_set_enabled(app->bgm, bgm_enabled);
        if (igSliderFloat("BGM 音量", &bgm_volume, 0.0f, 1.0f, "%.2f", 0) &&
            app->bgm)
            bgm_player_set_volume(app->bgm, bgm_volume);
        igTextDisabled("BGM：%s", home_bgm_status(app));
        if (app->bgm && !bgm_player_is_ready(app->bgm) &&
            !app->bgm_download_active &&
            game_button_at(app, "##retry_bgm", "重试 BGM",
                           v2(60.0f, work_y + 170.0f), v2(130.0f, 40.0f),
                           0, false))
            retry_home_bgm(app);
        igSeparator();
        if (game_checkbox_at(app, "##settings_voice", "随机播放角色语音",
                             igGetCursorScreenPos(), &enabled, true) && app->voice)
            voice_player_set_enabled(app->voice, enabled);
        if (igSliderFloat("语音音量", &volume, 0.0f, 1.0f, "%.2f", 0) &&
            app->voice)
            voice_player_set_volume(app->voice, volume);
        igTextDisabled("WAV %d", app->voice ? voice_player_file_count(app->voice) : 0);
        if (game_button_at(app, "##toggle_demo", "调试窗口",
                           v2(60.0f, work_y + work_height - 58.0f),
                           v2(130.0f, 40.0f), 1, false))
            app->show_demo = !app->show_demo;
        end_game_panel();
    }
}

static void set_style(void)
{
    igStyleColorsLight(NULL);
    ImGuiStyle *style = igGetStyle();
    style->WindowRounding = 10.0f;
    style->ChildRounding = 8.0f;
    style->FrameRounding = 6.0f;
    style->PopupRounding = 8.0f;
    style->WindowBorderSize = 0.0f;
    style->ChildBorderSize = 1.0f;
    style->FrameBorderSize = 1.0f;
    style->WindowPadding = v2(16.0f, 13.0f);
    style->FramePadding = v2(9.0f, 6.0f);
    style->ItemSpacing = v2(8.0f, 7.0f);
    style->Colors[ImGuiCol_Text] = v4(0.96f, 0.97f, 1.0f, 1.0f);
    style->Colors[ImGuiCol_TextDisabled] = v4(0.72f, 0.75f, 0.82f, 1.0f);
    style->Colors[ImGuiCol_WindowBg] = v4(0.0f, 0.0f, 0.0f, 0.0f);
    style->Colors[ImGuiCol_ChildBg] = v4(0.92f, 0.94f, 0.97f, 0.84f);
    style->Colors[ImGuiCol_Border] = v4(0.08f, 0.09f, 0.12f, 0.82f);
    style->Colors[ImGuiCol_FrameBg] = v4(0.10f, 0.12f, 0.16f, 0.92f);
    style->Colors[ImGuiCol_FrameBgHovered] = v4(0.21f, 0.16f, 0.24f, 0.96f);
    style->Colors[ImGuiCol_FrameBgActive] = v4(0.44f, 0.12f, 0.34f, 0.98f);
    style->Colors[ImGuiCol_Button] = v4(0.96f, 0.26f, 0.66f, 1.0f);
    style->Colors[ImGuiCol_ButtonHovered] = v4(1.0f, 0.42f, 0.76f, 1.0f);
    style->Colors[ImGuiCol_ButtonActive] = v4(0.78f, 0.10f, 0.48f, 1.0f);
    style->Colors[ImGuiCol_CheckMark] = v4(1.0f, 0.30f, 0.68f, 1.0f);
    style->Colors[ImGuiCol_SliderGrab] = v4(0.96f, 0.30f, 0.70f, 1.0f);
    style->Colors[ImGuiCol_SliderGrabActive] = v4(1.0f, 0.52f, 0.82f, 1.0f);
    style->Colors[ImGuiCol_ScrollbarBg] = v4(0.05f, 0.06f, 0.08f, 0.60f);
    style->Colors[ImGuiCol_ScrollbarGrab] = v4(0.70f, 0.20f, 0.54f, 0.85f);
    style->Colors[ImGuiCol_Header] = v4(0.52f, 0.15f, 0.43f, 0.85f);
    style->Colors[ImGuiCol_HeaderHovered] = v4(0.82f, 0.24f, 0.62f, 0.92f);
    style->Colors[ImGuiCol_HeaderActive] = v4(0.96f, 0.30f, 0.70f, 0.98f);
    style->Colors[ImGuiCol_TableRowBg] = v4(0.96f, 0.97f, 0.99f, 0.42f);
    style->Colors[ImGuiCol_TableRowBgAlt] = v4(0.82f, 0.85f, 0.90f, 0.32f);
    style->Colors[ImGuiCol_TableHeaderBg] = v4(0.64f, 0.67f, 0.73f, 0.54f);
}

static bool setup_app(GuiApp *app)
{
    const char *base;
    int window_width = 1180;
    int window_height = 700;
    SDL_Rect usable_bounds;
    memset(app, 0, sizeof(*app));
    app->page = GUI_PAGE_HOME;
    app->spine_background_enabled = true;
    app->spine_opacity = 1.0f;
    app->spine_zoom = 1.0f;
    app->animation_speed = 1.0f;
    app->running = true;
    /* Let Windows render the native IME composition/candidate windows.
     * SDL requires this hint before initialization; setting it afterward can
     * prevent Japanese and Chinese IMEs from opening their candidate UI. */
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "none");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        print_sdl_error("SDL_Init");
        return false;
    }
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable_bounds)) {
        if (usable_bounds.w > 96 && window_width > usable_bounds.w - 48)
            window_width = usable_bounds.w - 48;
        if (usable_bounds.h > 96 && window_height > usable_bounds.h - 48)
            window_height = usable_bounds.h - 48;
    }
    if (!SDL_CreateWindowAndRenderer("CGSS",
                                     window_width, window_height,
                                     SDL_WINDOW_RESIZABLE |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &app->window, &app->renderer)) {
        print_sdl_error("SDL_CreateWindowAndRenderer");
        SDL_Quit();
        return false;
    }
    SDL_SetWindowMinimumSize(app->window, 900, 600);
    SDL_SetWindowPosition(app->window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
    base = SDL_GetBasePath();
    snprintf(app->exe_dir, sizeof(app->exe_dir), "%s", base ? base : ".");
    app->resources = resource_backend_create(app->exe_dir);
    app->resource_category = RESOURCE_CATEGORY_SPINE;
    app->resource_auto_unpack = true;
    app->resource_last_state = RESOURCE_JOB_IDLE;
    if (!app->resources || !resource_backend_is_ready(app->resources))
        fprintf(stderr, "[Resources] %s\n", app->resources ?
                resource_backend_error(app->resources) : "创建资源后端失败");
    else
        fprintf(stderr, "[Resources] 清单已加载：%s\n",
                resource_backend_manifest_path(app->resources));
    app->spine = spine_scene_create(app->renderer);
    if (!app->spine) {
        fprintf(stderr, "创建 Spine 场景失败\n");
        if (app->resources)
            resource_backend_destroy(app->resources);
        SDL_DestroyRenderer(app->renderer);
        SDL_DestroyWindow(app->window);
        SDL_Quit();
        return false;
    }
    if (!spine_scene_autoload(app->spine, app->exe_dir))
        fprintf(stderr, "[Spine] 未自动加载卡面：%s\n",
                spine_scene_error(app->spine));
    else
        fprintf(stderr, "[Spine] 已加载：%s（%d 层）\n",
                spine_scene_directory(app->spine),
                spine_scene_layer_count(app->spine));
    load_ui_assets(app);
    app->bgm = bgm_player_create(app->exe_dir);
    app->voice = voice_player_init();
    if (app->voice)
        voice_player_rescan(app->voice, spine_scene_name(app->spine),
                            spine_scene_directory(app->spine));
    start_home_bgm_download(app);

    igCreateContext(NULL);
    ImGuiIO *io = igGetIO_Nil();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    {
        char regular_font[2048];
        char bold_font[2048];
        ImFont *font;
        snprintf(regular_font, sizeof(regular_font), "%s\\FRM.otf",
                 app->ui.root);
        snprintf(bold_font, sizeof(bold_font), "%s\\FRB.otf", app->ui.root);
        app->font_regular = NULL;
        app->font_bold = NULL;
        if (wide_path_exists(regular_font))
            app->font_regular = ImFontAtlas_AddFontFromFileTTF(
                io->Fonts, regular_font, 18.0f, NULL, gui_cjk_ranges());
        if (wide_path_exists(bold_font))
            app->font_bold = ImFontAtlas_AddFontFromFileTTF(
                io->Fonts, bold_font, 18.0f, NULL, gui_cjk_ranges());
        /* Keep a Windows fallback for labels not covered by the game font. */
        font = ImFontAtlas_AddFontFromFileTTF(
            io->Fonts, "C:/Windows/Fonts/msyh.ttc", 18.0f, NULL,
            gui_cjk_ranges());
        /* Chinese labels are the tool's primary language. The Windows font
         * remains the default because the game OTFs intentionally contain
         * Japanese/Latin glyphs only; their slices are still used by native
         * button art and can be selected for English headings later. */
        if (font)
            io->FontDefault = font;
        else if (app->font_regular)
            io->FontDefault = app->font_regular;
    }
    set_style();
    if (!ImGui_ImplSDL3_InitForSDLRenderer(app->window, app->renderer) ||
        !cgss_renderer3_init(app->renderer)) {
        fprintf(stderr, "ImGui SDL 后端初始化失败：%s\n", SDL_GetError());
        igDestroyContext(NULL);
        if (app->voice)
            voice_player_shutdown(app->voice);
        if (app->bgm)
            bgm_player_destroy(app->bgm);
        if (app->resources)
            resource_backend_destroy(app->resources);
        free_ui_assets(&app->ui);
        spine_scene_destroy(app->spine);
        SDL_DestroyRenderer(app->renderer);
        SDL_DestroyWindow(app->window);
        SDL_Quit();
        return false;
    }
    return true;
}

static void shutdown_app(GuiApp *app)
{
    cgss_renderer3_shutdown();
    ImGui_ImplSDL3_Shutdown();
    igDestroyContext(NULL);
    if (app->voice)
        voice_player_shutdown(app->voice);
    if (app->bgm)
        bgm_player_destroy(app->bgm);
    if (app->resources)
        resource_backend_destroy(app->resources);
    free(app->resource_items);
    app->resource_items = NULL;
    app->resource_count = 0;
    app->resource_capacity = 0;
    free_ui_assets(&app->ui);
    spine_scene_destroy(app->spine);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}

int main(int argc, char **argv)
{
    GuiApp app;
    (void)argc;
    (void)argv;
    /* Narrow strings in diagnostics are UTF-8.  Set the Windows console
     * code page when the GUI is launched from a terminal; this is harmless
     * when it is launched normally without a console. */
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    SDL_SetMainReady();
    if (!setup_app(&app))
        return 1;
    while (app.running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGuiIO *io;
            ImGui_ImplSDL3_ProcessEvent(&event);
            io = igGetIO_Nil();
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                bool text_input_active = io && io->WantTextInput;
                bool ctrl_shortcut = (event.key.mod & SDL_KMOD_CTRL) != 0;
                /* Page shortcuts are deliberately chorded. Plain digits must
                 * remain available to resource names and IME composition. */
                if (!text_input_active && ctrl_shortcut &&
                    event.key.key >= SDLK_1 && event.key.key <= SDLK_5) {
                    app.page = (GuiPage)(event.key.key - SDLK_1);
                } else if (!text_input_active &&
                           event.key.key == SDLK_ESCAPE && app.resources &&
                           resource_backend_is_busy(app.resources)) {
                    resource_backend_cancel(app.resources);
                }
            }
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(app.window)))
                app.running = false;
        }
        Uint64 now = SDL_GetTicks();
        float delta = 0.0f;
        if (app.last_ticks != 0 && now >= app.last_ticks)
            delta = (float)(now - app.last_ticks) / 1000.0f;
        app.last_ticks = now;
        spine_scene_update(app.spine, delta * app.animation_speed);
        if (app.voice)
            voice_player_update(app.voice, now);
        if (app.bgm)
            bgm_player_update(app.bgm, app.page == GUI_PAGE_HOME);
        if (app.bgm && !bgm_player_source_exists(app.bgm) &&
            !app.bgm_download_attempted)
            start_home_bgm_download(&app);
        if (app.resources) {
            ResourceJobSnapshot resource_snapshot;
            bool completed_home_bgm = false;
            resource_backend_snapshot(app.resources, &resource_snapshot);
            if (app.bgm_download_active) {
                snprintf(app.bgm_download_status,
                         sizeof(app.bgm_download_status), "%s",
                         resource_snapshot.status[0] ? resource_snapshot.status :
                         "正在下载 bgm_studio_night...");
                if (resource_snapshot.state == RESOURCE_JOB_COMPLETED ||
                    resource_snapshot.state == RESOURCE_JOB_FAILED ||
                    resource_snapshot.state == RESOURCE_JOB_CANCELLED) {
                    completed_home_bgm = true;
                    app.bgm_download_active = false;
                    if (resource_snapshot.state == RESOURCE_JOB_COMPLETED)
                        snprintf(app.bgm_download_status,
                                 sizeof(app.bgm_download_status),
                                 "首页 BGM 下载完成，准备解码");
                }
            }
            if (!completed_home_bgm &&
                app.resource_last_state == RESOURCE_JOB_RUNNING &&
                resource_snapshot.state == RESOURCE_JOB_COMPLETED)
                rescan_scene(&app);
            app.resource_last_state = resource_snapshot.state;
        }

        int pixel_w = 0;
        int pixel_h = 0;
        SDL_GetRenderOutputSize(app.renderer, &pixel_w, &pixel_h);
        draw_background(&app, pixel_w, pixel_h, (float)now / 1000.0f);
        cgss_renderer3_new_frame();
        ImGui_ImplSDL3_NewFrame();
        igNewFrame();
        ImGuiViewport *viewport = igGetMainViewport();
        float width = viewport ? viewport->Size.x : 1280.0f;
        float height = viewport ? viewport->Size.y : 760.0f;
        float stage_height = height - footer_reserved_height(width);
        if (stage_height < 1.0f)
            stage_height = height;
        draw_game_chrome_background(&app, width, height,
                                    (float)now / 1000.0f);
        draw_main_panel(&app, width, stage_height);
        draw_footer(&app, width, height);
        if (app.show_demo) {
            bool open = true;
            igShowDemoWindow(&open);
            if (!open)
                app.show_demo = false;
        }
        igRender();
        cgss_renderer3_render(igGetDrawData(), app.renderer);
        SDL_RenderPresent(app.renderer);
        SDL_Delay(1);
    }
    shutdown_app(&app);
    return 0;
}
