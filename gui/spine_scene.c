#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>
#include <spine/spine.h>
#include <spine/extension.h>

#include "spine_scene.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_PATH_CAP 2048
#define SCENE_NAME_CAP 256
#define SCENE_ERROR_CAP 512
#define SCENE_MAX_LAYERS 16
#define SCENE_MAX_JSONS 64

typedef struct SpineLayer {
    spSkeletonData *data;
    spSkeleton *skeleton;
    spAnimationStateData *state_data;
    spAnimationState *state;
    int layer_kind;
    char name[SCENE_NAME_CAP];
    char animation[SCENE_NAME_CAP];
} SpineLayer;

typedef struct JsonCandidate {
    wchar_t path[SCENE_PATH_CAP];
    wchar_t name[SCENE_NAME_CAP];
    int score;
} JsonCandidate;

typedef struct SceneCandidate {
    wchar_t path[SCENE_PATH_CAP];
    int score;
} SceneCandidate;

struct SpineScene {
    SDL_Renderer *renderer;
    spAtlas *atlas;
    spSkeletonClipping *clipper;
    SpineLayer layers[SCENE_MAX_LAYERS];
    int layer_count;

    float min_x, min_y, max_x, max_y;
    float elapsed;
    bool loaded;

    SDL_Vertex *vertices;
    int vertices_capacity;
    int *indices;
    int indices_capacity;
    float *world_vertices;
    int world_capacity;

    char directory[SCENE_PATH_CAP];
    char scene_name[SCENE_NAME_CAP];
    char error[SCENE_ERROR_CAP];
};

static SDL_Surface *load_spine_page_surface(const char *path);

/* The 3.6 runtime expects the host application to provide these hooks. */
char *_spUtil_readFile(const char *path, int *length)
{
    SDL_IOStream *io;
    Sint64 size;
    char *data;
    size_t got;

    if (!path || !length)
        return NULL;
    *length = 0;
    io = SDL_IOFromFile(path, "rb");
    if (!io)
        return NULL;
    size = SDL_GetIOSize(io);
    if (size <= 0 || size > INT_MAX) {
        SDL_CloseIO(io);
        return NULL;
    }

    /* Leave a trailing NUL for the JSON parser, while reporting the real size. */
    data = MALLOC(char, (size_t)size + 1u);
    if (!data) {
        SDL_CloseIO(io);
        return NULL;
    }
    got = SDL_ReadIO(io, data, (size_t)size);
    SDL_CloseIO(io);
    if (got != (size_t)size) {
        FREE(data);
        return NULL;
    }
    data[size] = '\0';
    *length = (int)size;
    return data;
}

void _spAtlasPage_createTexture(spAtlasPage *page, const char *path)
{
    SDL_Renderer *renderer;
    SDL_Surface *surface;
    SDL_Texture *texture;

    if (!page || !path || !page->atlas)
        return;
    renderer = (SDL_Renderer *)page->atlas->rendererObject;
    if (!renderer)
        return;

    surface = load_spine_page_surface(path);
    if (!surface)
        return;

    texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        page->rendererObject = texture;
        page->width = surface->w;
        page->height = surface->h;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    SDL_DestroySurface(surface);
}

void _spAtlasPage_disposeTexture(spAtlasPage *page)
{
    if (!page)
        return;
    if (page->rendererObject)
        SDL_DestroyTexture((SDL_Texture *)page->rendererObject);
    page->rendererObject = NULL;
}

static void set_error(SpineScene *scene, const char *message)
{
    if (!scene)
        return;
    if (!message)
        message = "未知错误";
    snprintf(scene->error, sizeof(scene->error), "%s", message);
}

static void set_error_sdl(SpineScene *scene, const char *prefix)
{
    char message[SCENE_ERROR_CAP];
    snprintf(message, sizeof(message), "%s: %s", prefix, SDL_GetError());
    set_error(scene, message);
}

static SDL_Surface *load_rgba_surface(const char *path)
{
    SDL_Surface *loaded;
    SDL_Surface *converted;

    if (!path || path[0] == '\0')
        return NULL;
    loaded = SDL_LoadSurface(path);
    if (!loaded)
        return NULL;
    converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    return converted;
}

/* 卡面图集使用 tex.png（RGB）+ tex_A8.png（透明通道）两张页面贴图。
 * SDL/Spine 只接受一张纹理，因此在加载 atlas 页面时合成真正的 RGBA。 */
static bool make_a8_path(const char *path, char *out, size_t capacity)
{
    const char *dot;
    size_t stem_length;

    if (!path || !out || capacity == 0)
        return false;
    dot = strrchr(path, '.');
    if (!dot || _stricmp(dot, ".png") != 0)
        return false;
    stem_length = (size_t)(dot - path);
    if (stem_length >= 3 &&
        _stricmp(path + stem_length - 3, "_A8") == 0)
        return false;
    if (stem_length + strlen("_A8.png") + 1 > capacity)
        return false;
    memcpy(out, path, stem_length);
    memcpy(out + stem_length, "_A8.png", strlen("_A8.png") + 1);
    return true;
}

static SDL_Surface *load_spine_page_surface(const char *path)
{
    SDL_Surface *rgb;
    SDL_Surface *a8;
    char a8_path[SCENE_PATH_CAP];
    const SDL_PixelFormatDetails *rgb_format;
    const SDL_PixelFormatDetails *a8_format;
    bool rgb_locked;
    bool a8_locked;
    int x, y;

    rgb = load_rgba_surface(path);
    if (!rgb)
        return NULL;
    if (!make_a8_path(path, a8_path, sizeof(a8_path)))
        return rgb;

    a8 = load_rgba_surface(a8_path);
    if (!a8 || a8->w != rgb->w || a8->h != rgb->h) {
        if (a8)
            SDL_DestroySurface(a8);
        return rgb;
    }
    rgb_locked = SDL_LockSurface(rgb);
    a8_locked = SDL_LockSurface(a8);
    if (!rgb_locked || !a8_locked) {
        if (a8_locked)
            SDL_UnlockSurface(a8);
        if (rgb_locked)
            SDL_UnlockSurface(rgb);
        SDL_DestroySurface(a8);
        return rgb;
    }

    rgb_format = SDL_GetPixelFormatDetails(rgb->format);
    a8_format = SDL_GetPixelFormatDetails(a8->format);
    if (rgb_format && a8_format) {
        for (y = 0; y < rgb->h; ++y) {
            Uint8 *rgb_row = (Uint8 *)rgb->pixels + y * rgb->pitch;
            const Uint8 *a8_row = (const Uint8 *)a8->pixels + y * a8->pitch;
            for (x = 0; x < rgb->w; ++x) {
                Uint32 rgb_pixel;
                Uint32 a8_pixel;
                Uint8 r, g, b, unused_alpha;
                Uint8 a8_r, a8_g, a8_b, alpha;

                memcpy(&rgb_pixel, rgb_row + x * 4, sizeof(rgb_pixel));
                memcpy(&a8_pixel, a8_row + x * 4, sizeof(a8_pixel));
                SDL_GetRGBA(rgb_pixel, rgb_format, NULL, &r, &g, &b,
                            &unused_alpha);
                SDL_GetRGBA(a8_pixel, a8_format, NULL, &a8_r, &a8_g,
                            &a8_b, &alpha);
                (void)a8_r;
                (void)a8_g;
                (void)a8_b;
                rgb_pixel = SDL_MapRGBA(rgb_format, NULL, r, g, b, alpha);
                memcpy(rgb_row + x * 4, &rgb_pixel, sizeof(rgb_pixel));
            }
        }
    }
    SDL_UnlockSurface(a8);
    SDL_UnlockSurface(rgb);
    SDL_DestroySurface(a8);
    return rgb;
}

static void copy_utf8(char *dst, size_t capacity, const char *src)
{
    if (!dst || capacity == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, capacity, "%s", src);
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

static bool wide_to_utf8(const wchar_t *src, char *dst, int capacity)
{
    int n;
    if (!src || !dst || capacity <= 0)
        return false;
    n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, capacity, NULL, NULL);
    return n > 0;
}

static bool is_directory_w(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void join_wide(wchar_t *dst, int capacity, const wchar_t *left,
                      const wchar_t *right)
{
    if (!dst || capacity <= 0)
        return;
    if (!left)
        left = L"";
    if (!right)
        right = L"";
    if (left[0] == L'\0')
        swprintf(dst, capacity, L"%ls", right);
    else if (left[wcslen(left) - 1] == L'\\')
        swprintf(dst, capacity, L"%ls%ls", left, right);
    else
        swprintf(dst, capacity, L"%ls\\%ls", left, right);
}

static bool ends_with_w(const wchar_t *name, const wchar_t *suffix)
{
    size_t n, m;
    if (!name || !suffix)
        return false;
    n = wcslen(name);
    m = wcslen(suffix);
    return n >= m && _wcsicmp(name + n - m, suffix) == 0;
}

static bool contains_w(const wchar_t *name, const wchar_t *part)
{
    return name && part && wcsstr(name, part) != NULL;
}

static bool path_has_component_w(const wchar_t *path,
                                 const wchar_t *component)
{
    const wchar_t *cursor;
    size_t component_length;

    if (!path || !component || component[0] == L'\0')
        return false;
    component_length = wcslen(component);
    cursor = path;
    while (*cursor) {
        const wchar_t *start;
        size_t length;

        while (*cursor == L'\\' || *cursor == L'/')
            ++cursor;
        start = cursor;
        while (*cursor && *cursor != L'\\' && *cursor != L'/')
            ++cursor;
        length = (size_t)(cursor - start);
        if (length == component_length &&
            _wcsnicmp(start, component, component_length) == 0)
            return true;
    }
    return false;
}

static void strip_json_extension(wchar_t *name)
{
    wchar_t *dot;
    if (!name)
        return;
    dot = wcsrchr(name, L'.');
    if (dot)
        *dot = L'\0';
}

/* 动态卡面 atlas 固定为 SP3S<卡号>_tex.atlas[.asset]。
 * SPC 是 2D 小人，SPMotionSticker 是贴纸，不能作为卡面背景。 */
static bool card_atlas_stem(const wchar_t *name, wchar_t *out,
                            int capacity, int *rank_out)
{
    wchar_t stem[SCENE_NAME_CAP];
    const wchar_t *digits;
    size_t n;
    bool asset_extension = false;
    bool converted_atlas = false;
    int rank;

    if (!name || !out || capacity <= 0)
        return false;
    wcsncpy(stem, name, SCENE_NAME_CAP - 1);
    stem[SCENE_NAME_CAP - 1] = L'\0';
    n = wcslen(stem);
    if (n >= 6 && _wcsicmp(stem + n - 6, L".asset") == 0) {
        stem[n - 6] = L'\0';
        asset_extension = true;
        n -= 6;
    }
    if (n < 6 || _wcsicmp(stem + n - 6, L".atlas") != 0)
        return false;
    stem[n - 6] = L'\0';
    n -= 6;

    if (n >= 8 && _wcsicmp(stem + n - 8, L"_tex_v38") == 0) {
        stem[n - 8] = L'\0';
        converted_atlas = true;
    } else if (n >= 4 && _wcsicmp(stem + n - 4, L"_tex") == 0) {
        stem[n - 4] = L'\0';
    } else {
        return false;
    }

    if (_wcsnicmp(stem, L"SP3S", 4) != 0 || stem[4] == L'\0')
        return false;
    for (digits = stem + 4; *digits; ++digits) {
        if (*digits < L'0' || *digits > L'9')
            return false;
    }

    wcsncpy(out, stem, (size_t)capacity - 1u);
    out[capacity - 1] = L'\0';
    rank = converted_atlas ? 10 : 20;
    if (!asset_extension)
        rank += 1;
    if (rank_out)
        *rank_out = rank;
    return true;
}

static int card_json_layer_score(const wchar_t *stem,
                                 const wchar_t *card_stem)
{
    const wchar_t *suffix;
    size_t card_length;

    if (!stem || !card_stem)
        return -1;
    card_length = wcslen(card_stem);
    if (_wcsnicmp(stem, card_stem, card_length) != 0)
        return -1;
    suffix = stem + card_length;
    if (_wcsicmp(suffix, L"_bg") == 0)
        return 10;
    if (_wcsicmp(suffix, L"_eff2") == 0)
        return 20;
    if (_wcsicmp(suffix, L"_chara") == 0)
        return 30;
    if (_wcsicmp(suffix, L"_eff1") == 0)
        return 40;
    if (_wcsicmp(suffix, L"_fg") == 0)
        return 50;
    return -1;
}

static int scene_path_score(const wchar_t *path)
{
    if (path_has_component_w(path, L"贴纸") ||
        path_has_component_w(path, L"spine文件"))
        return -1;
    if (path_has_component_w(path, L"卡面Spina动画"))
        return 400;
    if (path_has_component_w(path, L"live2d"))
        return 300;
    return -1;
}

static bool directory_has_scene_files(const wchar_t *directory,
                                      wchar_t *atlas_name, int atlas_cap)
{
    wchar_t pattern[SCENE_PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE handle;
    bool has_matching_json = false;
    wchar_t best_atlas[SCENE_NAME_CAP] = L"";
    wchar_t best_stem[SCENE_NAME_CAP] = L"";
    int best_rank = -1;

    join_wide(pattern, SCENE_PATH_CAP, directory, L"*");
    handle = FindFirstFileW(pattern, &fd);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    do {
        int rank;
        wchar_t card_stem[SCENE_NAME_CAP];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (fd.cFileName[0] == L'.')
            continue;
        if (card_atlas_stem(fd.cFileName, card_stem, SCENE_NAME_CAP,
                            &rank) && rank > best_rank) {
            best_rank = rank;
            wcsncpy(best_atlas, fd.cFileName, SCENE_NAME_CAP - 1);
            best_atlas[SCENE_NAME_CAP - 1] = L'\0';
            wcsncpy(best_stem, card_stem, SCENE_NAME_CAP - 1);
            best_stem[SCENE_NAME_CAP - 1] = L'\0';
        }
    } while (FindNextFileW(handle, &fd));
    FindClose(handle);

    if (best_rank < 0)
        return false;

    join_wide(pattern, SCENE_PATH_CAP, directory, L"*");
    handle = FindFirstFileW(pattern, &fd);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    do {
        wchar_t json_stem[SCENE_NAME_CAP];
        int layer_score;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!ends_with_w(fd.cFileName, L".json") ||
            contains_w(fd.cFileName, L"_v38"))
            continue;
        wcsncpy(json_stem, fd.cFileName, SCENE_NAME_CAP - 1);
        json_stem[SCENE_NAME_CAP - 1] = L'\0';
        strip_json_extension(json_stem);
        layer_score = card_json_layer_score(json_stem, best_stem);
        if (layer_score == 30) {
            has_matching_json = true;
            break;
        }
    } while (FindNextFileW(handle, &fd));
    FindClose(handle);

    if (has_matching_json && atlas_name && atlas_cap > 0) {
        wcsncpy(atlas_name, best_atlas, (size_t)atlas_cap - 1u);
        atlas_name[atlas_cap - 1] = L'\0';
    }
    return has_matching_json;
}

static void scan_tree(const wchar_t *directory, int depth,
                      SceneCandidate *best)
{
    wchar_t pattern[SCENE_PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE handle;
    wchar_t atlas_name[SCENE_NAME_CAP];

    if (!directory || !best || depth > 8 || !is_directory_w(directory))
        return;
    if (path_has_component_w(directory, L"贴纸"))
        return;

    if (directory_has_scene_files(directory, atlas_name, SCENE_NAME_CAP)) {
        int score = scene_path_score(directory);
        if (score >= 0 && score > best->score) {
            wcsncpy(best->path, directory, SCENE_PATH_CAP - 1);
            best->path[SCENE_PATH_CAP - 1] = L'\0';
            best->score = score;
        }
    }

    join_wide(pattern, SCENE_PATH_CAP, directory, L"*");
    handle = FindFirstFileW(pattern, &fd);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    do {
        wchar_t child[SCENE_PATH_CAP];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == L'.')
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (_wcsicmp(fd.cFileName, L"原文件unity3d") == 0 ||
            _wcsicmp(fd.cFileName, L"贴纸PNG") == 0)
            continue;
        join_wide(child, SCENE_PATH_CAP, directory, fd.cFileName);
        scan_tree(child, depth + 1, best);
    } while (FindNextFileW(handle, &fd));
    FindClose(handle);
}

static void parent_directory(wchar_t *path)
{
    wchar_t *slash;
    if (!path)
        return;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    if (slash == path + 2 && path[1] == L':')
        slash[1] = L'\0';
    else
        *slash = L'\0';
}

static bool find_best_scene(const char *exe_dir_utf8, wchar_t *out,
                            int out_capacity)
{
    wchar_t cursor[SCENE_PATH_CAP];
    SceneCandidate best;
    int level;

    memset(&best, 0, sizeof(best));
    best.score = -1;
    if (!utf8_to_wide(exe_dir_utf8, cursor, SCENE_PATH_CAP))
        return false;

    for (level = 0; level < 8; ++level) {
        wchar_t root[SCENE_PATH_CAP];
        wchar_t nested[SCENE_PATH_CAP];
        join_wide(root, SCENE_PATH_CAP, cursor, L"CGSS_DOWN");
        scan_tree(root, 0, &best);
        join_wide(nested, SCENE_PATH_CAP, cursor, L"CGSS\\build\\CGSS_DOWN");
        scan_tree(nested, 0, &best);
        parent_directory(cursor);
    }

    if (best.score < 0)
        return false;
    wcsncpy(out, best.path, (size_t)out_capacity - 1u);
    out[out_capacity - 1] = L'\0';
    return true;
}

static void layer_dispose(SpineLayer *layer)
{
    if (!layer)
        return;
    if (layer->state)
        spAnimationState_dispose(layer->state);
    if (layer->skeleton)
        spSkeleton_dispose(layer->skeleton);
    if (layer->state_data)
        spAnimationStateData_dispose(layer->state_data);
    if (layer->data)
        spSkeletonData_dispose(layer->data);
    memset(layer, 0, sizeof(*layer));
}

static void scene_clear(SpineScene *scene)
{
    int i;
    if (!scene)
        return;
    for (i = 0; i < scene->layer_count; ++i)
        layer_dispose(&scene->layers[i]);
    scene->layer_count = 0;
    if (scene->clipper) {
        spSkeletonClipping_dispose(scene->clipper);
        scene->clipper = NULL;
    }
    if (scene->atlas) {
        spAtlas_dispose(scene->atlas);
        scene->atlas = NULL;
    }
    scene->loaded = false;
    scene->elapsed = 0.0f;
    scene->min_x = scene->min_y = 0.0f;
    scene->max_x = scene->max_y = 0.0f;
    scene->directory[0] = '\0';
    scene->scene_name[0] = '\0';
}

static bool ensure_world_capacity(SpineScene *scene, int count)
{
    float *next;
    int capacity;
    if (count <= scene->world_capacity)
        return true;
    capacity = scene->world_capacity > 0 ? scene->world_capacity : 64;
    while (capacity < count) {
        if (capacity > INT_MAX / 2)
            return false;
        capacity *= 2;
    }
    next = REALLOC(scene->world_vertices, float, capacity);
    if (!next)
        return false;
    scene->world_vertices = next;
    scene->world_capacity = capacity;
    return true;
}

static bool ensure_vertex_capacity(SpineScene *scene, int count)
{
    SDL_Vertex *next;
    int capacity;
    if (count <= scene->vertices_capacity)
        return true;
    capacity = scene->vertices_capacity > 0 ? scene->vertices_capacity : 64;
    while (capacity < count) {
        if (capacity > INT_MAX / 2)
            return false;
        capacity *= 2;
    }
    next = (SDL_Vertex *)realloc(scene->vertices,
                                 sizeof(SDL_Vertex) * (size_t)capacity);
    if (!next)
        return false;
    scene->vertices = next;
    scene->vertices_capacity = capacity;
    return true;
}

static bool ensure_index_capacity(SpineScene *scene, int count)
{
    int *next;
    int capacity;
    if (count <= scene->indices_capacity)
        return true;
    capacity = scene->indices_capacity > 0 ? scene->indices_capacity : 96;
    while (capacity < count) {
        if (capacity > INT_MAX / 2)
            return false;
        capacity *= 2;
    }
    next = (int *)realloc(scene->indices, sizeof(int) * (size_t)capacity);
    if (!next)
        return false;
    scene->indices = next;
    scene->indices_capacity = capacity;
    return true;
}

static void free_scratch(SpineScene *scene)
{
    if (!scene)
        return;
    free(scene->vertices);
    free(scene->indices);
    if (scene->world_vertices)
        FREE(scene->world_vertices);
    scene->vertices = NULL;
    scene->indices = NULL;
    scene->world_vertices = NULL;
    scene->vertices_capacity = 0;
    scene->indices_capacity = 0;
    scene->world_capacity = 0;
}

static int compare_json_candidates(const void *left, const void *right)
{
    const JsonCandidate *a = (const JsonCandidate *)left;
    const JsonCandidate *b = (const JsonCandidate *)right;
    if (a->score != b->score)
        return a->score - b->score;
    return _wcsicmp(a->name, b->name);
}

static bool file_has_bones_marker(const wchar_t *path)
{
    char utf8_path[SCENE_PATH_CAP];
    char *data;
    int length = 0;
    bool result;
    if (!wide_to_utf8(path, utf8_path, (int)sizeof(utf8_path)))
        return false;
    data = _spUtil_readFile(utf8_path, &length);
    if (!data)
        return false;
    result = strstr(data, "\"bones\"") != NULL &&
             strstr(data, "\"skins\"") != NULL;
    FREE(data);
    return result;
}

static int collect_json_candidates(const wchar_t *directory,
                                   const wchar_t *atlas_name,
                                   JsonCandidate *out, int capacity)
{
    wchar_t pattern[SCENE_PATH_CAP];
    wchar_t atlas_stem[SCENE_NAME_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE handle;
    int count = 0;

    if (!card_atlas_stem(atlas_name, atlas_stem, SCENE_NAME_CAP, NULL))
        return 0;

    join_wide(pattern, SCENE_PATH_CAP, directory, L"*");
    handle = FindFirstFileW(pattern, &fd);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    do {
        wchar_t stem[SCENE_NAME_CAP];
        wchar_t full_path[SCENE_PATH_CAP];
        int layer_score;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (fd.cFileName[0] == L'.' || !ends_with_w(fd.cFileName, L".json"))
            continue;
        if (contains_w(fd.cFileName, L"_v38"))
            continue;
        wcsncpy(stem, fd.cFileName, SCENE_NAME_CAP - 1);
        stem[SCENE_NAME_CAP - 1] = L'\0';
        strip_json_extension(stem);
        layer_score = card_json_layer_score(stem, atlas_stem);
        if (layer_score < 0)
            continue;

        join_wide(full_path, SCENE_PATH_CAP, directory, fd.cFileName);
        /* Material/atlas metadata JSON is not a skeleton. */
        if (!file_has_bones_marker(full_path))
            continue;
        if (count >= capacity)
            continue;
        wcsncpy(out[count].path, full_path, SCENE_PATH_CAP - 1);
        out[count].path[SCENE_PATH_CAP - 1] = L'\0';
        wcsncpy(out[count].name, stem, SCENE_NAME_CAP - 1);
        out[count].name[SCENE_NAME_CAP - 1] = L'\0';
        out[count].score = layer_score;
        count++;
    } while (FindNextFileW(handle, &fd));
    FindClose(handle);

    if (count == 0)
        return 0;
    qsort(out, (size_t)count, sizeof(out[0]), compare_json_candidates);
    return count;
}

static bool prepare_layer(SpineScene *scene, const wchar_t *json_path,
                          const wchar_t *json_name, int layer_kind)
{
    char path_utf8[SCENE_PATH_CAP];
    SpineLayer *layer;
    spSkeletonJson *parser;
    spSkeletonData *data;
    spAnimationStateData *state_data;
    spSkeleton *skeleton;
    spAnimationState *state;

    if (scene->layer_count >= SCENE_MAX_LAYERS ||
        !wide_to_utf8(json_path, path_utf8, (int)sizeof(path_utf8)))
        return false;

    parser = spSkeletonJson_create(scene->atlas);
    if (!parser) {
        set_error(scene, "创建 Spine JSON 解析器失败");
        return false;
    }
    parser->scale = 1.0f;
    data = spSkeletonJson_readSkeletonDataFile(parser, path_utf8);
    if (!data) {
        char message[SCENE_ERROR_CAP];
        snprintf(message, sizeof(message), "%s: %s", path_utf8,
                 parser->error ? parser->error : "JSON 无法解析");
        set_error(scene, message);
        spSkeletonJson_dispose(parser);
        return false;
    }
    spSkeletonJson_dispose(parser);

    skeleton = spSkeleton_create(data);
    state_data = spAnimationStateData_create(data);
    state = state_data ? spAnimationState_create(state_data) : NULL;
    if (!skeleton || !state_data || !state) {
        if (state)
            spAnimationState_dispose(state);
        if (skeleton)
            spSkeleton_dispose(skeleton);
        if (state_data)
            spAnimationStateData_dispose(state_data);
        spSkeletonData_dispose(data);
        set_error(scene, "创建 Spine 骨架或动画状态失败");
        return false;
    }

    layer = &scene->layers[scene->layer_count++];
    memset(layer, 0, sizeof(*layer));
    layer->data = data;
    layer->skeleton = skeleton;
    layer->state_data = state_data;
    layer->state = state;
    layer->layer_kind = layer_kind;
    wide_to_utf8(json_name, layer->name, (int)sizeof(layer->name));
    if (data->animationsCount > 0 && data->animations[0]) {
        spAnimationState_setAnimation(state, 0, data->animations[0], 1);
        copy_utf8(layer->animation, sizeof(layer->animation),
                  data->animations[0]->name);
    } else {
        copy_utf8(layer->animation, sizeof(layer->animation), "(无动画)");
    }
    spSkeleton_setToSetupPose(skeleton);
    spAnimationState_apply(state, skeleton);
    spSkeleton_updateWorldTransform(skeleton);
    return true;
}

static bool layer_has_drawable_attachment(const SpineLayer *layer)
{
    int i;
    if (!layer || !layer->skeleton)
        return false;
    for (i = 0; i < layer->skeleton->slotsCount; ++i) {
        spSlot *slot = layer->skeleton->slots[i];
        if (!slot || !slot->attachment)
            continue;
        if (slot->attachment->type == SP_ATTACHMENT_REGION ||
            slot->attachment->type == SP_ATTACHMENT_MESH ||
            slot->attachment->type == SP_ATTACHMENT_LINKED_MESH)
            return true;
    }
    return false;
}

static bool scene_has_card_content(const SpineScene *scene)
{
    int i;
    if (!scene)
        return false;
    for (i = 0; i < scene->layer_count; ++i) {
        const SpineLayer *layer = &scene->layers[i];
        if (strstr(layer->name, "_chara") != NULL &&
            layer_has_drawable_attachment(layer))
            return true;
    }
    return false;
}

typedef struct DrawAttachment {
    float *world;
    int world_float_count;
    float *uvs;
    const unsigned short *indices;
    int index_count;
    SDL_Texture *texture;
    const spColor *color;
} DrawAttachment;

static const unsigned short region_indices[] = {0, 1, 2, 2, 3, 0};

static bool get_draw_attachment(SpineScene *scene, spSlot *slot,
                                DrawAttachment *out)
{
    spAttachment *attachment;
    spAtlasRegion *region;

    if (!scene || !slot || !slot->attachment || !out)
        return false;
    memset(out, 0, sizeof(*out));
    attachment = slot->attachment;

    if (attachment->type == SP_ATTACHMENT_REGION) {
        spRegionAttachment *region_attachment =
            (spRegionAttachment *)attachment;
        region = (spAtlasRegion *)region_attachment->rendererObject;
        if (!region || !region->page || !region->page->rendererObject)
            return false;
        if (!ensure_world_capacity(scene, 8))
            return false;
        spRegionAttachment_computeWorldVertices(region_attachment, slot->bone,
                                                scene->world_vertices, 0, 2);
        out->world = scene->world_vertices;
        out->world_float_count = 8;
        out->uvs = region_attachment->uvs;
        out->indices = region_indices;
        out->index_count = (int)(sizeof(region_indices) /
                                 sizeof(region_indices[0]));
        out->texture = (SDL_Texture *)region->page->rendererObject;
        out->color = &region_attachment->color;
        return true;
    }

    if (attachment->type == SP_ATTACHMENT_MESH ||
        attachment->type == SP_ATTACHMENT_LINKED_MESH) {
        spMeshAttachment *mesh = (spMeshAttachment *)attachment;
        region = (spAtlasRegion *)mesh->rendererObject;
        if (!region || !region->page || !region->page->rendererObject ||
            mesh->super.worldVerticesLength <= 0 || !mesh->uvs ||
            !mesh->triangles || mesh->trianglesCount <= 0)
            return false;
        if (!ensure_world_capacity(scene, mesh->super.worldVerticesLength))
            return false;
        spVertexAttachment_computeWorldVertices(
            SUPER(mesh), slot, 0, mesh->super.worldVerticesLength,
            scene->world_vertices, 0, 2);
        out->world = scene->world_vertices;
        out->world_float_count = mesh->super.worldVerticesLength;
        out->uvs = mesh->uvs;
        out->indices = mesh->triangles;
        out->index_count = mesh->trianglesCount;
        out->texture = (SDL_Texture *)region->page->rendererObject;
        out->color = &mesh->color;
        return true;
    }
    return false;
}

static bool include_attachment_bounds(SpineScene *scene, spSlot *slot,
                                      float *min_x, float *min_y,
                                      float *max_x, float *max_y)
{
    DrawAttachment draw;
    int i;

    if (!get_draw_attachment(scene, slot, &draw))
        return false;
    for (i = 0; i < draw.world_float_count; i += 2) {
        float x = draw.world[i];
        float y = draw.world[i + 1];
        if (x < *min_x) *min_x = x;
        if (x > *max_x) *max_x = x;
        if (y < *min_y) *min_y = y;
        if (y > *max_y) *max_y = y;
    }
    return true;
}

static bool scene_calculate_bounds(SpineScene *scene)
{
    float min_x = FLT_MAX, min_y = FLT_MAX;
    float max_x = -FLT_MAX, max_y = -FLT_MAX;
    SpineLayer *background = NULL;
    bool found_base = false;
    int layer_index;

    for (layer_index = 0; layer_index < scene->layer_count; ++layer_index) {
        if (scene->layers[layer_index].layer_kind == 10) {
            background = &scene->layers[layer_index];
            break;
        }
    }
    if (!background || !background->skeleton)
        return false;

    /* The base attachment is the card frame. Animated props in the same
     * _bg skeleton (birds, ramps, particles) must not expand the viewport. */
    for (int slot_index = 0;
         slot_index < background->skeleton->slotsCount; ++slot_index) {
        spSlot *slot = background->skeleton->slots[slot_index];
        bool is_base;
        if (!slot || !slot->attachment)
            continue;
        is_base = (slot->data && slot->data->name &&
                   _stricmp(slot->data->name, "bg") == 0) ||
                  (slot->attachment->name &&
                   _stricmp(slot->attachment->name, "bg") == 0);
        if (is_base && include_attachment_bounds(scene, slot, &min_x, &min_y,
                                                  &max_x, &max_y)) {
            found_base = true;
            break;
        }
    }

    /* Older cards may not call the base slot exactly "bg". In that case use
     * only the _bg skeleton, never chara/effect layers. */
    if (!found_base) {
        for (int slot_index = 0;
             slot_index < background->skeleton->slotsCount; ++slot_index) {
            include_attachment_bounds(scene,
                                      background->skeleton->slots[slot_index],
                                      &min_x, &min_y, &max_x, &max_y);
        }
    }
    if (min_x == FLT_MAX || max_x <= min_x || max_y <= min_y)
        return false;
    scene->min_x = min_x;
    scene->min_y = min_y;
    scene->max_x = max_x;
    scene->max_y = max_y;
    return true;
}

static SDL_BlendMode blend_mode_for_slot(const spSlot *slot)
{
    if (!slot || !slot->data)
        return SDL_BLENDMODE_BLEND;
    switch (slot->data->blendMode) {
    case SP_BLEND_MODE_ADDITIVE:
        return SDL_BLENDMODE_ADD;
    case SP_BLEND_MODE_MULTIPLY:
        return SDL_BLENDMODE_MUL;
    case SP_BLEND_MODE_SCREEN:
        /* SDL's portable screen equivalent is additive for translucent FX. */
        return SDL_BLENDMODE_ADD;
    case SP_BLEND_MODE_NORMAL:
    default:
        return SDL_BLENDMODE_BLEND;
    }
}

static float clamp01(float value)
{
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

static bool render_attachment(SpineScene *scene, spSkeleton *skeleton,
                              spSlot *slot, const DrawAttachment *draw,
                              float center_x, float center_y, float origin_x,
                              float origin_y, float scale, float opacity)
{
    float r, g, b, a;
    int vertex_count;
    int i;
    float *world = draw->world;
    float *uvs = draw->uvs;
    const unsigned short *indices = draw->indices;
    int index_count = draw->index_count;

    if (!draw->texture || !draw->color || draw->world_float_count <= 0)
        return false;
    vertex_count = draw->world_float_count / 2;

    if (spSkeletonClipping_isClipping(scene->clipper)) {
        spSkeletonClipping_clipTriangles(scene->clipper, world,
                                          draw->world_float_count,
                                          (unsigned short *)indices,
                                          index_count, uvs, 2);
        world = scene->clipper->clippedVertices->items;
        uvs = scene->clipper->clippedUVs->items;
        indices = scene->clipper->clippedTriangles->items;
        vertex_count = scene->clipper->clippedVertices->size / 2;
        index_count = scene->clipper->clippedTriangles->size;
    }
    if (vertex_count <= 0 || index_count <= 0)
        return true;
    if (!ensure_vertex_capacity(scene, vertex_count) ||
        !ensure_index_capacity(scene, index_count))
        return false;

    r = skeleton->color.r * slot->color.r * draw->color->r;
    g = skeleton->color.g * slot->color.g * draw->color->g;
    b = skeleton->color.b * slot->color.b * draw->color->b;
    a = skeleton->color.a * slot->color.a * draw->color->a * opacity;
    r = clamp01(r);
    g = clamp01(g);
    b = clamp01(b);
    a = clamp01(a);

    for (i = 0; i < vertex_count; ++i) {
        SDL_Vertex *vertex = &scene->vertices[i];
        int offset = i * 2;
        vertex->position.x = origin_x + (world[offset] - center_x) * scale;
        vertex->position.y = origin_y + (world[offset + 1] - center_y) * scale;
        vertex->color.r = r;
        vertex->color.g = g;
        vertex->color.b = b;
        vertex->color.a = a;
        vertex->tex_coord.x = uvs[offset];
        vertex->tex_coord.y = uvs[offset + 1];
    }
    for (i = 0; i < index_count; ++i)
        scene->indices[i] = (int)indices[i];

    SDL_BlendMode blend_mode = blend_mode_for_slot(slot);
    SDL_SetRenderDrawBlendMode(scene->renderer, blend_mode);
    /* SDL_RenderGeometry combines the renderer and texture blend states. */
    SDL_SetTextureBlendMode(draw->texture, blend_mode);
    if (!SDL_RenderGeometry(scene->renderer, draw->texture, scene->vertices,
                            vertex_count, scene->indices, index_count)) {
        set_error_sdl(scene, "SDL_RenderGeometry 失败");
        return false;
    }
    return true;
}

static bool render_layer(SpineScene *scene, SpineLayer *layer,
                         float center_x, float center_y, float origin_x,
                         float origin_y, float scale, float opacity)
{
    int slot_index;
    for (slot_index = 0; slot_index < layer->skeleton->slotsCount;
         ++slot_index) {
        spSlot *slot = layer->skeleton->drawOrder[slot_index];
        spAttachment *attachment = slot ? slot->attachment : NULL;
        DrawAttachment draw;
        if (!attachment)
            continue;
        if (attachment->type == SP_ATTACHMENT_CLIPPING) {
            spSkeletonClipping_clipStart(scene->clipper, slot,
                                          (spClippingAttachment *)attachment);
            continue;
        }
        if (get_draw_attachment(scene, slot, &draw) &&
            !render_attachment(scene, layer->skeleton, slot, &draw,
                               center_x, center_y, origin_x, origin_y,
                               scale, opacity)) {
            spSkeletonClipping_clipEnd2(scene->clipper);
            return false;
        }
        spSkeletonClipping_clipEnd(scene->clipper, slot);
    }
    spSkeletonClipping_clipEnd2(scene->clipper);
    return true;
}

SpineScene *spine_scene_create(struct SDL_Renderer *renderer)
{
    SpineScene *scene = (SpineScene *)calloc(1, sizeof(*scene));
    if (!scene)
        return NULL;
    scene->renderer = (SDL_Renderer *)renderer;
    spBone_setYDown(1);
    return scene;
}

void spine_scene_destroy(SpineScene *scene)
{
    if (!scene)
        return;
    scene_clear(scene);
    free_scratch(scene);
    spAnimationState_disposeStatics();
    free(scene);
}

bool spine_scene_load_directory(SpineScene *scene, const char *directory_utf8)
{
    wchar_t directory[SCENE_PATH_CAP];
    wchar_t atlas_name[SCENE_NAME_CAP];
    wchar_t atlas_path[SCENE_PATH_CAP];
    JsonCandidate candidates[SCENE_MAX_JSONS];
    char atlas_path_utf8[SCENE_PATH_CAP];
    int candidate_count;
    int i;

    if (!scene || !directory_utf8 || directory_utf8[0] == '\0')
        return false;
    if (!utf8_to_wide(directory_utf8, directory, SCENE_PATH_CAP)) {
        set_error(scene, "Spine 目录路径不是有效的 UTF-8");
        return false;
    }
    if (!is_directory_w(directory)) {
        set_error(scene, "Spine 卡面目录不存在");
        return false;
    }
    if (!directory_has_scene_files(directory, atlas_name, SCENE_NAME_CAP)) {
        set_error(scene, "目录中没有同一卡号的 SP3S 动态卡面 JSON 和 atlas");
        return false;
    }
    candidate_count = collect_json_candidates(directory, atlas_name,
                                               candidates, SCENE_MAX_JSONS);
    if (candidate_count <= 0) {
        set_error(scene, "目录中没有可解析的 Spine 卡面 JSON");
        return false;
    }
    join_wide(atlas_path, SCENE_PATH_CAP, directory, atlas_name);
    if (!wide_to_utf8(atlas_path, atlas_path_utf8, (int)sizeof(atlas_path_utf8))) {
        set_error(scene, "atlas 路径转换失败");
        return false;
    }

    scene_clear(scene);
    scene->atlas = spAtlas_createFromFile(atlas_path_utf8, scene->renderer);
    if (!scene->atlas) {
        set_error_sdl(scene, "读取 Spine atlas 失败");
        return false;
    }
    {
        spAtlasPage *page;
        bool all_pages_loaded = scene->atlas->pages != NULL;
        for (page = scene->atlas->pages; page; page = page->next) {
            if (!page->rendererObject) {
                all_pages_loaded = false;
                break;
            }
        }
        if (!all_pages_loaded) {
            set_error(scene, "atlas 页面贴图加载失败，请检查 PNG 是否与 atlas 同目录");
            scene_clear(scene);
            return false;
        }
    }
    scene->clipper = spSkeletonClipping_create();
    if (!scene->clipper) {
        set_error(scene, "创建 Spine clipping 缓冲区失败");
        scene_clear(scene);
        return false;
    }

    for (i = 0; i < candidate_count; ++i) {
        if (!prepare_layer(scene, candidates[i].path, candidates[i].name,
                           candidates[i].score))
            continue;
    }
    if (scene->layer_count <= 0) {
        if (scene->error[0] == '\0')
            set_error(scene, "Spine 卡面层全部解析失败");
        scene_clear(scene);
        return false;
    }
    if (!scene_has_card_content(scene)) {
        set_error(scene, "目录不是卡面 Live2D Spine（缺少可绘制的 bg/chara 层）");
        scene_clear(scene);
        return false;
    }

    copy_utf8(scene->directory, sizeof(scene->directory), directory_utf8);
    {
        wchar_t scene_stem[SCENE_NAME_CAP];
        if (card_atlas_stem(atlas_name, scene_stem, SCENE_NAME_CAP, NULL))
            wide_to_utf8(scene_stem, scene->scene_name,
                         (int)sizeof(scene->scene_name));
        else
            copy_utf8(scene->scene_name, sizeof(scene->scene_name),
                      "动态卡面");
    }
    if (!scene_calculate_bounds(scene)) {
        set_error(scene, "动态卡面 bg 底板为空，无法确定显示范围");
        scene_clear(scene);
        return false;
    }
    scene->loaded = true;
    scene->error[0] = '\0';
    return true;
}

bool spine_scene_autoload(SpineScene *scene, const char *exe_dir_utf8)
{
    wchar_t directory[SCENE_PATH_CAP];
    char directory_utf8[SCENE_PATH_CAP];

    if (!scene || !exe_dir_utf8) {
        return false;
    }
    if (!find_best_scene(exe_dir_utf8, directory, SCENE_PATH_CAP)) {
        set_error(scene, "未找到卡面 Spine（Live2D）目录，请先解包卡面 Spina 动画");
        return false;
    }
    if (!wide_to_utf8(directory, directory_utf8, (int)sizeof(directory_utf8))) {
        set_error(scene, "卡面 Spine 目录路径转换失败");
        return false;
    }
    return spine_scene_load_directory(scene, directory_utf8);
}

void spine_scene_update(SpineScene *scene, float delta_seconds)
{
    int i;
    if (!scene || !scene->loaded)
        return;
    if (delta_seconds < 0.0f)
        delta_seconds = 0.0f;
    if (delta_seconds > 0.25f)
        delta_seconds = 0.25f;
    scene->elapsed += delta_seconds;
    for (i = 0; i < scene->layer_count; ++i) {
        SpineLayer *layer = &scene->layers[i];
        spSkeleton_update(layer->skeleton, delta_seconds);
        spAnimationState_update(layer->state, delta_seconds);
        spAnimationState_apply(layer->state, layer->skeleton);
        spSkeleton_updateWorldTransform(layer->skeleton);
    }
}

static bool intersect_render_rect(const SDL_Rect *left, const SDL_Rect *right,
                                  SDL_Rect *out)
{
    int x1 = left->x > right->x ? left->x : right->x;
    int y1 = left->y > right->y ? left->y : right->y;
    int left_x2 = left->x + left->w;
    int right_x2 = right->x + right->w;
    int left_y2 = left->y + left->h;
    int right_y2 = right->y + right->h;
    int x2 = left_x2 < right_x2 ? left_x2 : right_x2;
    int y2 = left_y2 < right_y2 ? left_y2 : right_y2;

    if (x2 <= x1 || y2 <= y1)
        return false;
    out->x = x1;
    out->y = y1;
    out->w = x2 - x1;
    out->h = y2 - y1;
    return true;
}

bool spine_scene_render(SpineScene *scene, int width, int height,
                        float opacity, float zoom)
{
    float content_width;
    float content_height;
    float scale;
    float center_x;
    float center_y;
    float origin_x;
    float origin_y;
    SDL_Rect viewport_clip;
    SDL_Rect card_clip;
    SDL_Rect active_clip;
    SDL_Rect old_clip;
    bool had_clip;
    bool render_ok = true;
    int i;

    if (!scene || !scene->loaded || width <= 0 || height <= 0)
        return false;
    opacity = clamp01(opacity);
    if (opacity <= 0.0f)
        return true;
    if (zoom < 0.05f)
        zoom = 0.05f;

    content_width = fmaxf(1.0f, scene->max_x - scene->min_x);
    content_height = fmaxf(1.0f, scene->max_y - scene->min_y);
    /* Cover the complete output.  The _bg attachment still defines the
     * card bounds, while the excess animated content is clipped at the edge. */
    scale = fmaxf((float)width / content_width,
                  (float)height / content_height) * zoom;
    center_x = (scene->min_x + scene->max_x) * 0.5f;
    center_y = (scene->min_y + scene->max_y) * 0.5f;
    origin_x = (float)width * 0.5f;
    origin_y = (float)height * 0.5f;

    viewport_clip.x = 0;
    viewport_clip.y = 0;
    viewport_clip.w = width;
    viewport_clip.h = height;
    card_clip.x = (int)floorf(origin_x +
                              (scene->min_x - center_x) * scale);
    card_clip.y = (int)floorf(origin_y +
                              (scene->min_y - center_y) * scale);
    card_clip.w = (int)ceilf(origin_x +
                             (scene->max_x - center_x) * scale) - card_clip.x;
    card_clip.h = (int)ceilf(origin_y +
                             (scene->max_y - center_y) * scale) - card_clip.y;
    if (!intersect_render_rect(&card_clip, &viewport_clip, &active_clip))
        return true;

    had_clip = SDL_RenderClipEnabled(scene->renderer);
    if (had_clip) {
        if (!SDL_GetRenderClipRect(scene->renderer, &old_clip)) {
            set_error_sdl(scene, "读取 SDL 裁剪区域失败");
            return false;
        }
        if (!intersect_render_rect(&active_clip, &old_clip, &active_clip))
            return true;
    }
    if (!SDL_SetRenderClipRect(scene->renderer, &active_clip)) {
        set_error_sdl(scene, "设置动态卡面裁剪区域失败");
        return false;
    }

    for (i = 0; i < scene->layer_count; ++i) {
        if (!render_layer(scene, &scene->layers[i], center_x, center_y,
                          origin_x, origin_y, scale, opacity)) {
            render_ok = false;
            break;
        }
    }
    if (!SDL_SetRenderClipRect(scene->renderer,
                               had_clip ? &old_clip : NULL)) {
        set_error_sdl(scene, "恢复 SDL 裁剪区域失败");
        render_ok = false;
    }
    SDL_SetRenderDrawBlendMode(scene->renderer, SDL_BLENDMODE_BLEND);
    return render_ok;
}

bool spine_scene_is_loaded(const SpineScene *scene)
{
    return scene && scene->loaded;
}

const char *spine_scene_directory(const SpineScene *scene)
{
    return scene ? scene->directory : "";
}

const char *spine_scene_name(const SpineScene *scene)
{
    return scene ? scene->scene_name : "";
}

const char *spine_scene_error(const SpineScene *scene)
{
    return scene ? scene->error : "";
}

int spine_scene_layer_count(const SpineScene *scene)
{
    return scene ? scene->layer_count : 0;
}

const char *spine_scene_layer_name(const SpineScene *scene, int index)
{
    if (!scene || index < 0 || index >= scene->layer_count)
        return "";
    return scene->layers[index].name;
}

const char *spine_scene_layer_animation(const SpineScene *scene, int index)
{
    if (!scene || index < 0 || index >= scene->layer_count)
        return "";
    return scene->layers[index].animation;
}
