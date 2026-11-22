#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "browse.h"
#include "spine_catalog.h"

enum { PATH_CAP = 1024, MAX_DEPTH = 24, MAX_ATLAS_BYTES = 2 * 1024 * 1024, HTTP_WORKERS = 4 };

typedef struct {
    SOCKET listener;
    int port;
    wchar_t app_dir[PATH_CAP];
    wchar_t asset_dir[PATH_CAP];
} Site;

typedef struct {
    char *text;
    size_t size, capacity;
    int failed;
} Json;

static Site site;

static int ends_with(const wchar_t *name, const wchar_t *suffix){
    size_t n = wcslen(name), s = wcslen(suffix);
    return n >= s && _wcsicmp(name + n - s, suffix) == 0;
}

static int join_path(wchar_t *out, const wchar_t *dir, const wchar_t *name){
    int n = swprintf(out, PATH_CAP, L"%ls\\%ls", dir, name);
    return n > 0 && n < PATH_CAP;
}

static int ordinary(const wchar_t *path, int directory){
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (!!(attrs & FILE_ATTRIBUTE_DIRECTORY) == directory);
}

static int file_exists(const wchar_t *dir, const wchar_t *name){
    wchar_t path[PATH_CAP];
    return join_path(path, dir, name) && ordinary(path, 0);
}

static void append(Json *json, const char *value, size_t length){
    if (json->failed) return;
    if (length > 32 * 1024 * 1024 - json->size){
        json->failed = 1;
        return;
    }
    if (json->capacity < json->size + length + 1){
        size_t capacity = json->capacity ? json->capacity * 2 : 4096;
        if (capacity < json->size + length + 1) capacity = json->size + length + 1;
        char *grown = realloc(json->text, capacity);
        if (!grown){
            json->failed = 1;
            return;
        }
        json->text = grown;
        json->capacity = capacity;
    }
    memcpy(json->text + json->size, value, length);
    json->size += length;
    json->text[json->size] = '\0';
}

static void literal(Json *json, const char *value){
    append(json, value, strlen(value));
}

static void json_string(Json *json, const wchar_t *value){
    char utf8[4 * PATH_CAP];
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8, sizeof utf8, NULL, NULL)){
        json->failed = 1;
        return;
    }
    literal(json, "\"");
    for (const char *s = utf8; *s; s++){
        if (*s == '\\' || *s == '"') literal(json, "\\");
        if ((unsigned char)*s < 32){
            char escape[7];
            snprintf(escape, sizeof escape, "\\u%04x", (unsigned char)*s);
            literal(json, escape);
        } else append(json, s, 1);
    }
    literal(json, "\"");
}

static int url_byte(unsigned char c){
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '~';
}

static void json_file(Json *json, const wchar_t *folder, const wchar_t *name){
    wchar_t rel[PATH_CAP];
    char utf8[4 * PATH_CAP];
    if (!join_path(rel, folder, name) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, rel, -1, utf8, sizeof utf8, NULL, NULL)){
        json->failed = 1;
        return;
    }
    literal(json, "{\"name\":");
    json_string(json, name);
    literal(json, ",\"url\":\"/assets");
    if (*folder) literal(json, "/");
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++){
        if (*p == '\\') literal(json, "/");
        else if (url_byte(*p)) append(json, (const char *)p, 1);
        else{
            const char hex[] = "0123456789ABCDEF";
            const char encoded[] = {'%', hex[*p >> 4], hex[*p & 15]};
            append(json, encoded, sizeof encoded);
        }
    }
    literal(json, "\"}");
}

static int find_skeleton(const wchar_t *dir, const wchar_t *stem, wchar_t *name){
    static const wchar_t *const extensions[] = {L".json", L".skel", L".skel.asset", L".skel.bytes"};
    for (size_t i = 0; i < _countof(extensions); i++){
        int n = swprintf(name, MAX_PATH, L"%ls%ls", stem, extensions[i]);
        if (n > 0 && n < MAX_PATH && file_exists(dir, name)) return 1;
    }
    return 0;
}

static const wchar_t *category_for(const wchar_t *folder, const char *kind, wchar_t *category){
    static const wchar_t discovered[] = L"已发现Spine\\";
    if (wcsncmp(folder, discovered, _countof(discovered) - 1) == 0){
        const wchar_t *begin = folder + _countof(discovered) - 1;
        const wchar_t *end = wcschr(begin, L'\\');
        size_t n = end ? (size_t)(end - begin) : wcslen(begin);
        if (n && n < MAX_PATH){
            wmemcpy(category, begin, n);
            category[n] = L'\0';
            return category;
        }
    }
    if (strcmp(kind, "card") == 0) return L"卡面动画";
    if (strcmp(kind, "petit") == 0) return L"Spine 小人";
    if (wcsstr(folder, L"loginbonus")) return L"登录奖励";
    if (wcsstr(folder, L"aniv_count") || wcsstr(folder, L"anniversary")) return L"周年";
    if (wcsstr(folder, L"carnival")) return L"嘉年华";
    if (wcsstr(folder, L"sweets")) return L"甜点小游戏";
    if (wcsstr(folder, L"spine_bus")) return L"巴士";
    if (wcsstr(folder, L"shot_") || wcsstr(folder, L"spshgrade")) return L"写真";
    if (wcsstr(folder, L"room_spine")) return L"房间";
    if (wcsstr(folder, L"spine_sprachen") || wcsstr(folder, L"spine_advent")) return L"节日";
    if (wcsstr(folder, L"spine_event") || wcsstr(folder, L"spine_tower")) return L"活动";
    if (wcsstr(folder, L"spine_lottery")) return L"抽奖";
    if (wcsstr(folder, L"spine_concert")) return L"演唱会";
    if (wcsstr(folder, L"贴纸")) return L"贴纸";
    return L"其他 Spine";
}

static int atlas_pages(const wchar_t *dir, const wchar_t *atlas,
                       wchar_t pages[][MAX_PATH], int capacity){
    wchar_t path[PATH_CAP];
    if (!join_path(path, dir, atlas) || !ordinary(path, 0)) return 0;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) || ftell(f) > MAX_ATLAS_BYTES || fseek(f, 0, SEEK_SET)){
        fclose(f);
        return 0;
    }
    char line[4 * MAX_PATH];
    int count = 0, next_page = 1;
    while (fgets(line, sizeof line, f)){
        char *begin = line;
        if (strncmp(begin, "\xef\xbb\xbf", 3) == 0) begin += 3;
        while (*begin == ' ' || *begin == '\t') begin++;
        begin[strcspn(begin, "\r\n")] = '\0';
        char *tail = begin + strlen(begin);
        while (tail > begin && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = '\0';
        if (tail == begin){
            next_page = 1;
            continue;
        }
        if (!next_page) continue;
        next_page = 0;
        if (tail - begin < 4 || _stricmp(tail - 4, ".png") != 0){
            count = 0;
            break;
        }
        char *leaf = begin;
        for (char *p = begin; *p; p++)
            if (*p == '/' || *p == '\\') leaf = p + 1;
        wchar_t name[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, leaf, -1, name, MAX_PATH) ||
            wcschr(name, L':') || !file_exists(dir, name) || count == capacity){
            count = 0;
            break;
        }
        wcscpy(pages[count++], name);
    }
    fclose(f);
    return count;
}

typedef struct {
    const wchar_t *dir, *folder, *atlas;
    wchar_t pages[24][MAX_PATH];
    int count;
} CatalogArt;

static int load_art(CatalogArt *art, const wchar_t *dir, const wchar_t *folder, const wchar_t *atlas){
    const int count = atlas_pages(dir, atlas, art->pages, _countof(art->pages));
    if (!count) return 0;
    art->dir = dir;
    art->folder = folder;
    art->atlas = atlas;
    art->count = count;
    return 1;
}

static void write_item(Json *json, const wchar_t *folder, const CatalogArt *art,
                       const wchar_t *stem, const char *kind, wchar_t skeletons[][MAX_PATH],
                       int count, const wchar_t *variant, wchar_t shared[][MAX_PATH], int *items){
    wchar_t name[MAX_PATH], category[MAX_PATH], title[PATH_CAP];
    wchar_t id[PATH_CAP];
    int id_length = swprintf(id, _countof(id), L"%ls#%ls#%ls", folder, stem, variant);
    if (id_length < 0 || (size_t)id_length >= _countof(id)) return;
    const wchar_t *leaf = wcsrchr(folder, L'\\');
    leaf = leaf ? leaf + 1 : folder;
    int use_leaf = strcmp(kind, "standalone") == 0 || strcmp(kind, "shared") == 0;
    if (use_leaf && *leaf)
        swprintf(title, _countof(title), L"%ls · %ls", leaf, stem);
    else{
        const wchar_t *separator = wcschr(folder, L'\\');
        if (separator)
            swprintf(title, _countof(title), L"%.*ls · %ls", (int)(separator - folder), folder, stem);
        else swprintf(title, _countof(title), L"%ls", stem);
    }
    if (*variant){
        size_t n = wcslen(title);
        swprintf(title + n, _countof(title) - n, L" · %ls", variant);
    }
    if ((*items)++) literal(json, ",");
    literal(json, "{\"id\":");
    json_string(json, id);
    literal(json, ",\"title\":");
    json_string(json, title);
    literal(json, ",\"category\":");
    json_string(json, category_for(folder, kind, category));
    literal(json, ",\"kind\":\"");
    literal(json, kind);
    literal(json, "\",\"folder\":");
    json_string(json, folder);
    literal(json, ",\"skeletons\":[");
    for (int i = 0; i < count; i++){
        if (i) literal(json, ",");
        json_file(json, folder, skeletons[i]);
    }
    literal(json, "],\"atlas\":");
    if (art) json_file(json, art->folder, art->atlas);
    else literal(json, "null");
    literal(json, ",\"pngs\":[");
    for (int i = 0; art && i < art->count; i++){
        if (i) literal(json, ",");
        json_file(json, art->folder, art->pages[i]);
        swprintf(name, MAX_PATH, L"%.*ls_A8.png", (int)wcslen(art->pages[i]) - 4, art->pages[i]);
        if (file_exists(art->dir, name)){
            literal(json, ",");
            json_file(json, art->folder, name);
        }
    }
    literal(json, "]");
    if (shared){
        literal(json, ",\"variants\":[");
        for (int i = 0; i < 2; i++){
            if (!shared[i][0]) continue;
            if (i && shared[0][0]) literal(json, ",");
            literal(json, i ? "{\"label\":\"S\",\"skeleton\":" : "{\"label\":\"N\",\"skeleton\":");
            json_file(json, folder, shared[i]);
            literal(json, "}");
        }
        literal(json, "]");
    }
    literal(json, "}");
}

static void add_item(Json *json, const wchar_t *folder, const wchar_t *dir,
                     const wchar_t *atlas, const wchar_t *stem, const char *kind,
                     wchar_t skeletons[][MAX_PATH], int count, const wchar_t *variant, int *items){
    CatalogArt art;
    if (load_art(&art, dir, folder, atlas))
        write_item(json, folder, &art, stem, kind, skeletons, count, variant, NULL, items);
}

typedef struct {
    const wchar_t *pack, *skeletons[2], *companion, *atlas;
} SharedPack;

static const SharedPack shared_packs[] = {
    {L"spine_carnival_commu_anime", {L"car_commuCharaNormal", L"car_commuCharaSmall"},
     L"spine_carnival_commu_100213", L"SPC100213.atlas"},
    {L"sweets_spine_common_chara", {L"SPACSkeltonN", L"SPACSkeltonS"},
     L"sweets_spine_common_cookidol_restaurant_001", L"SPKCStaff.atlas"}
};

static int sibling_path(wchar_t *out, const wchar_t *path, const wchar_t *leaf){
    const wchar_t *separator = wcsrchr(path, L'\\');
    int length = swprintf(out, PATH_CAP, L"%.*ls%ls",
                         separator ? (int)(separator - path + 1) : 0, path, leaf);
    return length > 0 && length < PATH_CAP;
}

static int find_companion(CatalogArt *art, const wchar_t *dir, const wchar_t *folder,
                          const SharedPack *pack, wchar_t *companion_dir, wchar_t *companion_folder){
    if (sibling_path(companion_dir, dir, pack->companion) && ordinary(companion_dir, 1) &&
        sibling_path(companion_folder, folder, pack->companion) &&
        load_art(art, companion_dir, companion_folder, pack->atlas)) return 1;

    static const wchar_t *const roots[] = {L"常用Spine", L"自定义"};
    for (size_t i = 0; i < _countof(roots); i++){
        wchar_t root_dir[PATH_CAP], spine_dir[PATH_CAP];
        if (!join_path(root_dir, site.asset_dir, roots[i]) || !ordinary(root_dir, 1) ||
            !join_path(spine_dir, root_dir, L"spine") || !ordinary(spine_dir, 1) ||
            !join_path(companion_dir, spine_dir, pack->companion) || !ordinary(companion_dir, 1)) continue;
        int length = swprintf(companion_folder, PATH_CAP, L"%ls\\spine\\%ls", roots[i], pack->companion);
        if (length > 0 && length < PATH_CAP && load_art(art, companion_dir, companion_folder, pack->atlas))
            return 1;
    }
    return 0;
}

static int record_shared_pack(Json *json, const wchar_t *dir, const wchar_t *folder,
                              const SharedPack *pack, int *items){
    wchar_t variants[2][MAX_PATH] = {{0}};
    for (int i = 0; i < 2; i++)
        if (!find_skeleton(dir, pack->skeletons[i], variants[i])) variants[i][0] = L'\0';
    if (!variants[0][0] && !variants[1][0]) return 0;

    CatalogArt companion;
    wchar_t companion_dir[PATH_CAP], companion_folder[PATH_CAP];
    int has_companion = find_companion(&companion, dir, folder, pack, companion_dir, companion_folder);
    for (int i = 0; i < 2; i++){
        if (!variants[i][0]) continue;
        wchar_t alias[MAX_PATH];
        CatalogArt local;
        swprintf(alias, MAX_PATH, L"%ls.atlas", pack->skeletons[i]);
        const CatalogArt *art = has_companion ? &companion : NULL;
        if (load_art(&local, dir, folder, alias) || load_art(&local, dir, folder, pack->atlas)) art = &local;
        write_item(json, folder, art, pack->skeletons[i], "shared", &variants[i], 1, L"", variants, items);
    }
    return 1;
}

static void record_atlas(Json *json, const wchar_t *dir, const wchar_t *folder,
                         const wchar_t *name, int *items){
    wchar_t stem[MAX_PATH], skeletons[5][MAX_PATH];
    size_t len = wcslen(name);
    if (len >= MAX_PATH) return;
    if (ends_with(name, L".atlas.asset")){
        wchar_t alias[MAX_PATH];
        wcscpy(alias, name);
        alias[len - 6] = L'\0';
        if (file_exists(dir, alias)) return;
        len -= 12;
    } else if (ends_with(name, L".atlas")) len -= 6;
    else return;
    if (!len) return;
    wmemcpy(stem, name, len);
    stem[len] = L'\0';
    if (ends_with(stem, L"_v38")) return;
    if (_wcsnicmp(stem, L"SP3S", 4) == 0 && ends_with(stem, L"_tex")){
        static const wchar_t *const layers[] = {L"bg", L"eff2", L"chara", L"eff1", L"fg"};
        wchar_t layer[MAX_PATH];
        size_t id = len - 4;
        int count = 0;
        for (size_t i = 0; i < _countof(layers); i++){
            int n = swprintf(layer, MAX_PATH, L"%.*ls_%ls", (int)id, stem, layers[i]);
            if (n > 0 && n < MAX_PATH && find_skeleton(dir, layer, skeletons[count]))
                count++;
        }
        if (count) add_item(json, folder, dir, name, stem, "card", skeletons, count, L"", items);
        return;
    }
    if (find_skeleton(dir, stem, skeletons[0])){
        add_item(json, folder, dir, name, stem, "standalone", skeletons, 1, L"", items);
        return;
    }
    if (_wcsnicmp(stem, L"SPC", 3) != 0) return;
    if (find_skeleton(dir, L"SPSprachen_s_merged", skeletons[0]) ||
        find_skeleton(dir, L"SPSprachen_s", skeletons[0]))
        add_item(json, folder, dir, name, stem, "petit", skeletons, 1, L"S", items);
    if (find_skeleton(dir, L"SPSprachen_N_merged", skeletons[0]) ||
        find_skeleton(dir, L"SPSprachen_N", skeletons[0]))
        add_item(json, folder, dir, name, stem, "petit", skeletons, 1, L"N", items);
}

static void scan_catalog(Json *json, const wchar_t *dir, const wchar_t *folder, int depth, int *items){
    wchar_t pattern[PATH_CAP], path[PATH_CAP], rel[PATH_CAP];
    if (depth > MAX_DEPTH || !ordinary(dir, 1) || !join_path(pattern, dir, L"*")) return;
    const wchar_t *leaf = wcsrchr(folder, L'\\');
    leaf = leaf ? leaf + 1 : folder;
    for (size_t i = 0; i < _countof(shared_packs); i++){
        const SharedPack *pack = &shared_packs[i];
        if (_wcsicmp(leaf, pack->pack) == 0 && record_shared_pack(json, dir, folder, pack, items)) return;
    }
    WIN32_FIND_DATAW info;
    HANDLE search = FindFirstFileW(pattern, &info);
    if (search == INVALID_HANDLE_VALUE) return;
    do{
        const wchar_t *name = info.cFileName;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT || name[0] == L'.') continue;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if (wcsncmp(name, L"模板示例", 4) == 0 || !join_path(path, dir, name)) continue;
            if (*folder){
                if (!join_path(rel, folder, name)) continue;
            } else wcscpy(rel, name);
            scan_catalog(json, path, rel, depth + 1, items);
        } else record_atlas(json, dir, folder, name, items);
    } while (!json->failed && FindNextFileW(search, &info));
    FindClose(search);
}

static int send_all(SOCKET client, const char *data, size_t length){
    while (length){
        int step = length > 65536 ? 65536 : (int)length;
        int count = send(client, data, step, 0);
        if (count <= 0) return 0;
        data += count;
        length -= (size_t)count;
    }
    return 1;
}

static int headers(SOCKET client, const char *status, const char *type, unsigned long long length){
    char data[512];
    int n = snprintf(data, sizeof data,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        status, type, length);
    return n > 0 && (size_t)n < sizeof data && send_all(client, data, n);
}

static void status_only(SOCKET client, const char *status){
    headers(client, status, "text/plain; charset=utf-8", 0);
}

static void send_catalog(SOCKET client){
    Json json = {0};
    int items = 0;
    literal(&json, default_spine_downloads_running() ?
            "{\"loadingDefaults\":true,\"items\":[" : "{\"loadingDefaults\":false,\"items\":[");
    scan_catalog(&json, site.asset_dir, L"", 0, &items);
    literal(&json, "]}");
    if (json.failed) status_only(client, "500 Internal Server Error");
    else if (headers(client, "200 OK", "application/json; charset=utf-8", json.size))
        send_all(client, json.text, json.size);
    free(json.text);
}

static int hex_digit(unsigned char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int decode_relative(const char *uri, wchar_t *out){
    char utf8[4 * PATH_CAP];
    size_t n = 0;
    while (*uri){
        unsigned char c = *uri++;
        if (c == '%'){
            if (!uri[0] || !uri[1]) return 0;
            int a = hex_digit(uri[0]), b = hex_digit(uri[1]);
            if (a < 0 || b < 0) return 0;
            c = (unsigned char)((a << 4) | b);
            uri += 2;
        }
        if (c < 32 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || n == sizeof utf8 - 1) return 0;
        utf8[n++] = c == '/' ? '\\' : (char)c;
    }
    utf8[n] = '\0';
    return n && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, PATH_CAP);
}

static int safe_components(const wchar_t *base, const wchar_t *relative){
    wchar_t path[PATH_CAP];
    if (!ordinary(base, 1) || !join_path(path, base, relative)) return 0;
    wchar_t *segment = path + wcslen(base) + 1;
    for (;;){
        wchar_t *next = wcschr(segment, L'\\');
        if (next) *next = L'\0';
        size_t n = wcslen(segment);
        if (!n || segment[0] == L'.' || segment[n - 1] == L'.' || segment[n - 1] == L' ' ||
            !ordinary(path, next != NULL)) return 0;
        if (!next) return 1;
        *next = L'\\';
        segment = next + 1;
    }
}

static int same_root(HANDLE file, const wchar_t *base){
    HANDLE root = CreateFileW(base, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (root == INVALID_HANDLE_VALUE) return 0;
    wchar_t canonical[PATH_CAP + 16], actual[PATH_CAP + 16];
    DWORD length = GetFinalPathNameByHandleW(root, canonical, _countof(canonical), FILE_NAME_NORMALIZED);
    DWORD n = GetFinalPathNameByHandleW(file, actual, _countof(actual), FILE_NAME_NORMALIZED);
    CloseHandle(root);
    return length && length < _countof(canonical) && n > length && n < _countof(actual) &&
           _wcsnicmp(actual, canonical, length) == 0 && actual[length] == L'\\';
}

static void send_file(SOCKET client, const wchar_t *base, const wchar_t *relative, const char *type){
    wchar_t path[PATH_CAP];
    if (!safe_components(base, relative) || !join_path(path, base, relative)){
        status_only(client, "404 Not Found");
        return;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER size;
    BY_HANDLE_FILE_INFORMATION info;
    if (file == INVALID_HANDLE_VALUE || GetFileType(file) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !same_root(file, base) || !GetFileSizeEx(file, &size) || size.QuadPart < 0){
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        status_only(client, "404 Not Found");
        return;
    }
    if (headers(client, "200 OK", type, (unsigned long long)size.QuadPart)){
        char bytes[65536];
        DWORD n;
        while (ReadFile(file, bytes, sizeof bytes, &n, NULL) && n)
            if (!send_all(client, bytes, n)) break;
    }
    CloseHandle(file);
}

static int valid_host(char *headers_start){
    char wanted[48];
    snprintf(wanted, sizeof wanted, "127.0.0.1:%d", site.port);
    for (char *line = headers_start; line && *line; ){
        char *end = strstr(line, "\r\n");
        if (!end || end == line) break;
        if (_strnicmp(line, "Host:", 5) == 0){
            line += 5;
            while (line < end && (*line == ' ' || *line == '\t')) line++;
            return (size_t)(end - line) == strlen(wanted) && memcmp(line, wanted, end - line) == 0;
        }
        line = end + 2;
    }
    return 0;
}

static void handle_request(SOCKET client){
    DWORD timeout = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
    char request[8192];
    int used = 0;
    do{
        int n = recv(client, request + used, sizeof request - used - 1, 0);
        if (n <= 0) return;
        used += n;
        request[used] = '\0';
    } while (used < (int)sizeof request - 1 && !strstr(request, "\r\n\r\n"));
    char *headers_start = strstr(request, "\r\n");
    if (strncmp(request, "GET ", 4) || !headers_start || !strstr(headers_start, "\r\n\r\n") ||
        !valid_host(headers_start + 2)){
        status_only(client, "400 Bad Request");
        return;
    }
    *headers_start = '\0';
    char *uri = request + 4;
    char *end = strchr(uri, ' ');
    if (!end || strcmp(end + 1, "HTTP/1.1")){
        status_only(client, "400 Bad Request");
        return;
    }
    *end = '\0';
    char *query = strchr(uri, '?');
    if (query) *query = '\0';
    if (strcmp(uri, "/api/catalog.json") == 0){
        send_catalog(client);
        return;
    }
    if (strncmp(uri, "/assets/", 8) == 0){
        wchar_t rel[PATH_CAP];
        if (decode_relative(uri + 8, rel)){
            const char *type = NULL;
            if (ends_with(rel, L".png")) type = "image/png";
            else if (ends_with(rel, L".json")) type = "application/json; charset=utf-8";
            else if (ends_with(rel, L".atlas") || ends_with(rel, L".atlas.asset")) type = "text/plain; charset=utf-8";
            else if (ends_with(rel, L".skel") || ends_with(rel, L".skel.asset") || ends_with(rel, L".skel.bytes"))
                type = "application/octet-stream";
            if (type){
                send_file(client, site.asset_dir, rel, type);
                return;
            }
        }
        status_only(client, "404 Not Found");
        return;
    }
    static const struct { const char *uri; const wchar_t *name; const char *type; } files[] = {
        {"/preview.html", L"preview.html", "text/html; charset=utf-8"},
        {"/library.js", L"library.js", "application/javascript; charset=utf-8"},
        {"/library.css", L"library.css", "text/css; charset=utf-8"},
        {"/spine-core.js", L"spine-core.js", "application/javascript; charset=utf-8"},
        {"/spine-canvas.js", L"spine-canvas.js", "application/javascript; charset=utf-8"},
        {"/spine-webgl.js", L"spine-webgl.js", "application/javascript; charset=utf-8"},
        {"/cgss_skel_parser.js", L"cgss_skel_parser.js", "application/javascript; charset=utf-8"},
        {"/mp4-muxer.min.js", L"mp4-muxer.min.js", "application/javascript; charset=utf-8"},
        {"/gifenc.js", L"gifenc.js", "application/javascript; charset=utf-8"},
        {"/gif-export.js", L"gif-export.js", "application/javascript; charset=utf-8"}
    };
    if (strcmp(uri, "/") == 0) uri = "/preview.html";
    for (size_t i = 0; i < _countof(files); i++){
        if (strcmp(uri, files[i].uri) == 0){
            send_file(client, site.app_dir, files[i].name, files[i].type);
            return;
        }
    }
    status_only(client, "404 Not Found");
}

static DWORD WINAPI serve(LPVOID ignored){
    (void)ignored;
    for (;;){
        SOCKET client = accept(site.listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        handle_request(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
    return 0;
}

int start_spine_catalog(const wchar_t *app_dir){
    if (site.port) return site.port;
    if (!join_path(site.app_dir, app_dir, L"spine_preview") ||
        !join_path(site.asset_dir, app_dir, L"CGSS_DOWN") || !ordinary(site.app_dir, 1)) return -1;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return -1;
    site.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (site.listener == INVALID_SOCKET){
        WSACleanup();
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof addr;
    if (bind(site.listener, (const struct sockaddr *)&addr, sizeof addr) == 0 &&
        getsockname(site.listener, (struct sockaddr *)&addr, &len) == 0 &&
        listen(site.listener, 16) == 0){
        site.port = ntohs(addr.sin_port);
        int workers = 0;
        for (; workers < HTTP_WORKERS; workers++){
            HANDLE thread = CreateThread(NULL, 0, serve, NULL, 0, NULL);
            if (!thread) break;
            CloseHandle(thread);
        }
        if (workers) return site.port;
        site.port = 0;
    }
    closesocket(site.listener);
    WSACleanup();
    return -1;
}
