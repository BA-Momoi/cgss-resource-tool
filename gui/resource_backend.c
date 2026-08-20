#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "resource_backend.h"
#include "sqlite3.h"
#include "spine_convert.h"
#include "texture_merge.h"
#include "sticker.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define RB_PATH_CAP 2048
#define RB_QUERY_VARIANT_COUNT 3
#define RB_QUERY_TEXT_CAP 512

typedef enum ResourcePostKind {
    RESOURCE_POST_NONE = 0,
    RESOURCE_POST_CARD_SPINE,
    RESOURCE_POST_STICKER
} ResourcePostKind;

typedef struct ResourceTask {
    ResourceItem item;
    wchar_t output_directory[RB_PATH_CAP];
    wchar_t output_file[RB_PATH_CAP];
    ResourcePostKind post_kind;
    int work_index;
} ResourceTask;

typedef struct RbIdSet {
    int *values;
    size_t count;
    size_t capacity;
} RbIdSet;

typedef struct RbMasterMatches {
    RbIdSet music_ids;
    RbIdSet live_ids;
    RbIdSet jacket_ids;
    RbIdSet card_ids;
    RbIdSet chara_ids;
    RbIdSet dress_ids;
} RbMasterMatches;

typedef struct RbQueryPatterns {
    char values[RB_QUERY_VARIANT_COUNT][RB_QUERY_TEXT_CAP];
    bool empty;
    bool has_non_ascii;
} RbQueryPatterns;

struct ResourceBackend {
    wchar_t exe_directory_w[RB_PATH_CAP];
    wchar_t download_root_w[RB_PATH_CAP];
    wchar_t manifest_path_w[RB_PATH_CAP];
    wchar_t master_path_w[RB_PATH_CAP];
    wchar_t assetstudio_path_w[RB_PATH_CAP];
    char manifest_path[RB_PATH_CAP];
    char master_path[RB_PATH_CAP];
    char error[512];
    char query_error[512];
    char reported_error[512];
    bool ready;

    CRITICAL_SECTION job_lock;
    HANDLE thread;
    volatile LONG cancel_requested;
    ResourceTask *tasks;
    size_t task_capacity;
    int task_count;
    bool auto_unpack;
    ResourceJobSnapshot job;
};

static bool rb_cancelled(const ResourceBackend *backend);
static int rb_copy_pattern(const wchar_t *source_directory,
                           const wchar_t *pattern,
                           const wchar_t *destination);
static void rb_clear_directory(const wchar_t *directory);

static bool rb_reserve_tasks(ResourceBackend *backend, int count)
{
    size_t needed;
    ResourceTask *tasks;

    if (!backend || count <= 0)
        return false;
    needed = (size_t)count;
    if (needed <= backend->task_capacity)
        return true;
    if (needed > SIZE_MAX / sizeof(*backend->tasks))
        return false;
    tasks = (ResourceTask *)realloc(backend->tasks,
                                    needed * sizeof(*backend->tasks));
    if (!tasks)
        return false;
    backend->tasks = tasks;
    backend->task_capacity = needed;
    return true;
}

static const char *const g_category_labels[RESOURCE_CATEGORY_COUNT] = {
    "全部资源", "BGM", "歌曲", "卡片", "角色语音", "谱面",
    "舞台", "动作", "3D 模型", "Spine", "贴纸", "CG 影片"
};

static bool rb_utf8_to_wide(const char *src, wchar_t *dst, int capacity)
{
    int length;
    if (!src || !dst || capacity <= 0)
        return false;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1,
                                 dst, capacity);
    if (length <= 0)
        length = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, capacity);
    if (length <= 0)
        dst[0] = L'\0';
    return length > 0;
}

static bool rb_wide_to_utf8(const wchar_t *src, char *dst, int capacity)
{
    int length;
    if (!src || !dst || capacity <= 0)
        return false;
    length = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, capacity,
                                 NULL, NULL);
    if (length <= 0)
        dst[0] = '\0';
    return length > 0;
}

static bool rb_query_contains_non_ascii(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    if (!text)
        return false;
    while (*cursor) {
        if (*cursor >= 0x80)
            return true;
        ++cursor;
    }
    return false;
}

static bool rb_query_add_variant(RbQueryPatterns *patterns,
                                 const char *text)
{
    char pattern[RB_QUERY_TEXT_CAP];
    int i;
    int written;
    if (!patterns || !text)
        return false;
    written = snprintf(pattern, sizeof(pattern), "%%%s%%", text);
    if (written < 0 || written >= (int)sizeof(pattern))
        return false;
    for (i = 0; i < RB_QUERY_VARIANT_COUNT; ++i) {
        if (patterns->values[i][0] &&
            strcmp(patterns->values[i], pattern) == 0)
            return true;
    }
    for (i = 0; i < RB_QUERY_VARIANT_COUNT; ++i) {
        if (!patterns->values[i][0]) {
            snprintf(patterns->values[i], sizeof(patterns->values[i]),
                     "%s", pattern);
            return true;
        }
    }
    return true;
}

static void rb_query_add_mapped_variant(RbQueryPatterns *patterns,
                                         const wchar_t *source,
                                         DWORD map_flags)
{
    wchar_t mapped[RB_QUERY_TEXT_CAP];
    char utf8[RB_QUERY_TEXT_CAP];
    int needed;
    int written;
    if (!patterns || !source || !source[0])
        return;
    needed = LCMapStringEx(LOCALE_NAME_INVARIANT, map_flags, source, -1,
                           NULL, 0, NULL, NULL, 0);
    if (needed <= 0 || needed > (int)(sizeof(mapped) / sizeof(mapped[0])))
        return;
    written = LCMapStringEx(LOCALE_NAME_INVARIANT, map_flags, source, -1,
                            mapped, (int)(sizeof(mapped) / sizeof(mapped[0])),
                            NULL, NULL, 0);
    if (written <= 0 || !rb_wide_to_utf8(mapped, utf8, sizeof(utf8)))
        return;
    rb_query_add_variant(patterns, utf8);
}

static bool rb_build_query_patterns(const char *query_utf8,
                                    RbQueryPatterns *patterns)
{
    wchar_t source[RB_QUERY_TEXT_CAP];
    int i;
    if (!patterns)
        return false;
    memset(patterns, 0, sizeof(*patterns));
    if (!query_utf8)
        query_utf8 = "";
    patterns->empty = query_utf8[0] == '\0';
    patterns->has_non_ascii = rb_query_contains_non_ascii(query_utf8);
    if (patterns->empty) {
        for (int i = 0; i < RB_QUERY_VARIANT_COUNT; ++i)
            snprintf(patterns->values[i], sizeof(patterns->values[i]), "%%%%");
        return true;
    }
    if (!rb_query_add_variant(patterns, query_utf8))
        return false;
    if (patterns->has_non_ascii &&
        rb_utf8_to_wide(query_utf8, source,
                        (int)(sizeof(source) / sizeof(source[0])))) {
        rb_query_add_mapped_variant(patterns, source,
                                    LCMAP_SIMPLIFIED_CHINESE);
        rb_query_add_mapped_variant(patterns, source,
                                    LCMAP_TRADITIONAL_CHINESE);
    }
    /* A mapped form may be identical to the original.  Never leave an
     * unused SQL LIKE parameter as an empty string: name_kana is empty for
     * some NPC rows, and "LIKE ''" would turn those into false matches. */
    for (i = 1; i < RB_QUERY_VARIANT_COUNT; ++i) {
        if (!patterns->values[i][0])
            snprintf(patterns->values[i], sizeof(patterns->values[i]),
                     "%s", patterns->values[0]);
    }
    return true;
}

static void rb_trim_trailing_slash(wchar_t *path)
{
    size_t length;
    if (!path)
        return;
    length = wcslen(path);
    while (length > 3 && (path[length - 1] == L'\\' ||
                          path[length - 1] == L'/')) {
        path[--length] = L'\0';
    }
}

static bool rb_join_path(wchar_t *dst, int capacity,
                         const wchar_t *left, const wchar_t *right)
{
    int written;
    size_t length;
    if (!dst || capacity <= 0 || !left || !right)
        return false;
    length = wcslen(left);
    written = swprintf(dst, (size_t)capacity, L"%ls%ls%ls", left,
                       length > 0 && left[length - 1] != L'\\' ? L"\\" : L"",
                       right);
    return written >= 0 && written < capacity;
}

static void rb_parent_directory(wchar_t *path)
{
    wchar_t *slash;
    if (!path)
        return;
    rb_trim_trailing_slash(path);
    slash = wcsrchr(path, L'\\');
    if (slash && slash > path + 2)
        *slash = L'\0';
}

static bool rb_file_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool rb_directory_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool rb_make_directories(const wchar_t *path)
{
    wchar_t buffer[RB_PATH_CAP];
    wchar_t *cursor;
    size_t length;
    if (!path || !path[0] || wcslen(path) >= RB_PATH_CAP)
        return false;
    wcscpy(buffer, path);
    rb_trim_trailing_slash(buffer);
    length = wcslen(buffer);
    for (cursor = buffer + 3; cursor < buffer + length; ++cursor) {
        if (*cursor != L'\\' && *cursor != L'/')
            continue;
        {
            wchar_t saved = *cursor;
            *cursor = L'\0';
            if (!CreateDirectoryW(buffer, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
            *cursor = saved;
        }
    }
    if (!CreateDirectoryW(buffer, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    return true;
}

static void rb_sanitize_filename(wchar_t *text)
{
    wchar_t *cursor;
    if (!text)
        return;
    for (cursor = text; *cursor; ++cursor) {
        if (*cursor < 32 || wcschr(L"<>:\"/\\|?*", *cursor))
            *cursor = L'_';
    }
}

static const char *rb_base_name(const char *path)
{
    const char *slash;
    const char *backslash;
    if (!path)
        return "";
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash))
        slash = backslash;
    return slash ? slash + 1 : path;
}

static bool rb_ends_with_ci(const char *text, const char *suffix)
{
    size_t text_length;
    size_t suffix_length;
    if (!text || !suffix)
        return false;
    text_length = strlen(text);
    suffix_length = strlen(suffix);
    if (suffix_length > text_length)
        return false;
    return _stricmp(text + text_length - suffix_length, suffix) == 0;
}

static bool rb_starts_with_ci(const char *text, const char *prefix)
{
    size_t length;
    if (!text || !prefix)
        return false;
    length = strlen(prefix);
    return _strnicmp(text, prefix, length) == 0;
}

static bool rb_id_set_append(RbIdSet *set, int value)
{
    size_t capacity;
    int *values;
    if (!set || value <= 0)
        return true;
    if (set->count < set->capacity) {
        set->values[set->count++] = value;
        return true;
    }
    capacity = set->capacity ? set->capacity * 2 : 32;
    if (capacity < set->capacity || capacity > SIZE_MAX / sizeof(*values))
        return false;
    values = (int *)realloc(set->values, capacity * sizeof(*values));
    if (!values)
        return false;
    set->values = values;
    set->capacity = capacity;
    set->values[set->count++] = value;
    return true;
}

static int rb_compare_ints(const void *left, const void *right)
{
    int a = *(const int *)left;
    int b = *(const int *)right;
    return (a > b) - (a < b);
}

static void rb_id_set_sort(RbIdSet *set)
{
    if (set && set->count > 1)
        qsort(set->values, set->count, sizeof(*set->values), rb_compare_ints);
}

static bool rb_id_set_contains(const RbIdSet *set, int value)
{
    size_t low = 0;
    size_t high;
    if (!set || value <= 0)
        return false;
    high = set->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int candidate = set->values[middle];
        if (candidate == value)
            return true;
        if (candidate < value)
            low = middle + 1;
        else
            high = middle;
    }
    return false;
}

static void rb_master_matches_clear(RbMasterMatches *matches)
{
    if (!matches)
        return;
    free(matches->music_ids.values);
    free(matches->live_ids.values);
    free(matches->jacket_ids.values);
    free(matches->card_ids.values);
    free(matches->chara_ids.values);
    free(matches->dress_ids.values);
    memset(matches, 0, sizeof(*matches));
}

static bool rb_load_id_set(sqlite3 *database, const char *sql,
                           const RbQueryPatterns *patterns, RbIdSet *set,
                           char *error, size_t error_capacity)
{
    sqlite3_stmt *statement = NULL;
    int rc;
    int i;
    if (!database || !sql || !patterns || !set)
        return false;
    rc = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK)
        goto failed;
    for (i = 0; i < RB_QUERY_VARIANT_COUNT; ++i) {
        rc = sqlite3_bind_text(statement, i + 1, patterns->values[i], -1,
                               SQLITE_TRANSIENT);
        if (rc != SQLITE_OK)
            goto failed;
    }
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(statement, 0);
        if (id > 0 && id <= INT_MAX &&
            !rb_id_set_append(set, (int)id)) {
            if (error && error_capacity > 0)
                snprintf(error, error_capacity, "搜索名称映射时内存不足");
            sqlite3_finalize(statement);
            return false;
        }
    }
    if (rc != SQLITE_DONE)
        goto failed;
    sqlite3_finalize(statement);
    rb_id_set_sort(set);
    return true;

failed:
    if (error && error_capacity > 0)
        snprintf(error, error_capacity, "读取主数据库失败：%s",
                 sqlite3_errmsg(database));
    if (statement)
        sqlite3_finalize(statement);
    return false;
}

static bool rb_load_master_matches(ResourceBackend *backend,
                                   ResourceCategory category,
                                   const RbQueryPatterns *patterns,
                                   RbMasterMatches *matches,
                                   char *error,
                                   size_t error_capacity)
{
    sqlite3 *database = NULL;
    bool need_music;
    bool need_idol;
    int rc;
    static const char music_sql[] =
        "SELECT id FROM music_data "
        "WHERE name LIKE ?1 OR name_kana LIKE ?1 OR "
        "name LIKE ?2 OR name_kana LIKE ?2 OR "
        "name LIKE ?3 OR name_kana LIKE ?3";
    static const char live_sql[] =
        "SELECT DISTINCT id FROM live_data WHERE music_data_id IN ("
        "SELECT id FROM music_data WHERE name LIKE ?1 OR name_kana LIKE ?1 OR "
        "name LIKE ?2 OR name_kana LIKE ?2 OR "
        "name LIKE ?3 OR name_kana LIKE ?3)";
    static const char jacket_sql[] =
        "SELECT DISTINCT jacket_id FROM live_data WHERE jacket_id > 0 AND "
        "music_data_id IN (SELECT id FROM music_data WHERE "
        "name LIKE ?1 OR name_kana LIKE ?1 OR name LIKE ?2 OR "
        "name_kana LIKE ?2 OR name LIKE ?3 OR name_kana LIKE ?3)";
    static const char card_sql[] =
        "SELECT DISTINCT cd.id FROM card_data AS cd "
        "WHERE cd.name LIKE ?1 OR cd.name LIKE ?2 OR cd.name LIKE ?3 OR "
        "cd.chara_id IN ("
        "SELECT chara_id FROM chara_data "
        "WHERE name LIKE ?1 OR name_kana LIKE ?1 OR "
        "name LIKE ?2 OR name_kana LIKE ?2 OR "
        "name LIKE ?3 OR name_kana LIKE ?3)";
    static const char chara_sql[] =
        "SELECT c.chara_id FROM chara_data AS c WHERE ("
        "c.name LIKE ?1 OR c.name_kana LIKE ?1 OR "
        "c.name LIKE ?2 OR c.name_kana LIKE ?2 OR "
        "c.name LIKE ?3 OR c.name_kana LIKE ?3) AND ("
        "c.base_card_id > 0 OR EXISTS (SELECT 1 FROM card_data AS linked "
        "WHERE linked.chara_id=c.chara_id)) "
        "UNION SELECT DISTINCT chara_id FROM card_data WHERE "
        "name LIKE ?1 OR name LIKE ?2 OR name LIKE ?3";
    static const char dress_sql[] =
        "SELECT DISTINCT cd.open_dress_id FROM card_data AS cd "
        "WHERE cd.open_dress_id > 0 AND (cd.name LIKE ?1 OR "
        "cd.name LIKE ?2 OR cd.name LIKE ?3 OR "
        "cd.chara_id IN (SELECT chara_id FROM chara_data "
        "WHERE name LIKE ?1 OR name_kana LIKE ?1 OR "
        "name LIKE ?2 OR name_kana LIKE ?2 OR "
        "name LIKE ?3 OR name_kana LIKE ?3)) "
        "UNION SELECT DISTINCT dd.id FROM dress_data AS dd "
        "WHERE dd.id > 0 AND dd.view_chara_id IN ("
        "SELECT chara_id FROM chara_data "
        "WHERE name LIKE ?1 OR name_kana LIKE ?1 OR "
        "name LIKE ?2 OR name_kana LIKE ?2 OR "
        "name LIKE ?3 OR name_kana LIKE ?3)";

    if (!backend || !patterns || !matches || !error || error_capacity == 0)
        return false;
    memset(matches, 0, sizeof(*matches));
    if (patterns->empty || !backend->master_path[0])
        return true;

    need_music = category == RESOURCE_CATEGORY_ALL ||
                 category == RESOURCE_CATEGORY_BGM ||
                 category == RESOURCE_CATEGORY_SONG ||
                 category == RESOURCE_CATEGORY_CHART;
    need_idol = category == RESOURCE_CATEGORY_ALL ||
                category == RESOURCE_CATEGORY_CARD ||
                category == RESOURCE_CATEGORY_VOICE ||
                category == RESOURCE_CATEGORY_MODEL ||
                category == RESOURCE_CATEGORY_SPINE;
    if (!need_music && !need_idol)
        return true;

    rc = sqlite3_open_v2(backend->master_path, &database,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        snprintf(error, error_capacity,
                 "打开主数据库失败：%s",
                 database ? sqlite3_errmsg(database) : "unknown");
        if (database)
            sqlite3_close(database);
        return false;
    }
    if (need_music &&
        !rb_load_id_set(database, music_sql, patterns, &matches->music_ids,
                        error, error_capacity))
        goto failed;
    if (need_music &&
        (!rb_load_id_set(database, live_sql, patterns, &matches->live_ids,
                         error, error_capacity) ||
         !rb_load_id_set(database, jacket_sql, patterns,
                         &matches->jacket_ids, error, error_capacity)))
        goto failed;
    if (need_idol &&
        (!rb_load_id_set(database, card_sql, patterns, &matches->card_ids,
                         error, error_capacity) ||
         !rb_load_id_set(database, chara_sql, patterns, &matches->chara_ids,
                         error, error_capacity) ||
         !rb_load_id_set(database, dress_sql, patterns, &matches->dress_ids,
                         error, error_capacity)))
        goto failed;
    sqlite3_close(database);
    return true;

failed:
    sqlite3_close(database);
    rb_master_matches_clear(matches);
    return false;
}

static bool rb_parse_decimal(const char *text, int *value,
                             const char **end)
{
    const unsigned char *cursor;
    unsigned long long number = 0;
    if (!text || !value || !end)
        return false;
    cursor = (const unsigned char *)text;
    if (!isdigit(*cursor))
        return false;
    do {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (number > ((unsigned long long)INT_MAX - digit) / 10)
            return false;
        number = number * 10 + digit;
        ++cursor;
    } while (isdigit(*cursor));
    if (number == 0)
        return false;
    *value = (int)number;
    *end = (const char *)cursor;
    return true;
}

/* Parse a numbered field after a known resource prefix.  Fields are separated
 * by underscores; digits embedded in an unrelated prefix or suffix are not
 * considered. */
static bool rb_number_after_prefix(const char *text, const char *prefix,
                                   int ordinal, int *value)
{
    const char *cursor;
    const char *end;
    int parsed;
    int index;
    if (!text || !prefix || ordinal < 0 || !value ||
        !rb_starts_with_ci(text, prefix))
        return false;
    cursor = text + strlen(prefix);
    for (index = 0; index <= ordinal; ++index) {
        if (index > 0) {
            if (*cursor != '_')
                return false;
            ++cursor;
        }
        if (!*cursor || !rb_parse_decimal(cursor, &parsed, &end))
            return false;
        if (index == ordinal) {
            if (*end && *end != '_' && *end != '.' && *end != '/' &&
                *end != '\\')
                return false;
            *value = parsed;
            return true;
        }
        cursor = end;
    }
    return false;
}

static bool rb_set_has_number_after_prefix(const char *text,
                                           const char *prefix,
                                           int ordinal,
                                           const RbIdSet *set)
{
    int value;
    return rb_number_after_prefix(text, prefix, ordinal, &value) &&
           rb_id_set_contains(set, value);
}

static bool rb_set_has_card_number(const char *base,
                                   const RbIdSet *card_ids)
{
    static const char *const prefixes[] = {
        "card_", "card_bg_", "card_gacha_", "card_spine_",
        "card_petit_", "card_live_", "card_cartoon_"
    };
    size_t i;
    if (!base || !card_ids)
        return false;
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (rb_set_has_number_after_prefix(base, prefixes[i], 0,
                                           card_ids))
            return true;
    }
    return false;
}

static bool rb_manifest_matches_master(const char *name,
                                       const RbMasterMatches *matches)
{
    const char *base;
    if (!name || !matches)
        return false;
    base = rb_base_name(name);

    if (rb_starts_with_ci(name, "l/song_") ||
        rb_starts_with_ci(base, "jacket_") ||
        rb_starts_with_ci(base, "musicscores_m")) {
        if (rb_starts_with_ci(name, "l/song_"))
            return rb_set_has_number_after_prefix(name, "l/song_", 0,
                                                   &matches->music_ids);
        if (rb_starts_with_ci(base, "jacket_"))
            return rb_set_has_number_after_prefix(base, "jacket_", 0,
                                                   &matches->jacket_ids);
        return rb_set_has_number_after_prefix(base, "musicscores_m", 0,
                                               &matches->live_ids);
    }

    if (rb_starts_with_ci(name, "v/card_"))
        return rb_set_has_number_after_prefix(name, "v/card_", 0,
                                               &matches->card_ids);
    if (rb_starts_with_ci(base, "idol_3d_"))
        return rb_set_has_number_after_prefix(base, "idol_3d_", 0,
                                               &matches->card_ids);
    if (rb_starts_with_ci(base, "card_"))
        return rb_set_has_card_number(base, &matches->card_ids);

    if (rb_starts_with_ci(base, "spine_sprachen_petit_chara_"))
        return rb_set_has_number_after_prefix(
            base, "spine_sprachen_petit_chara_", 0, &matches->chara_ids);
    if (rb_starts_with_ci(name, "v/chara_"))
        return rb_set_has_number_after_prefix(name, "v/chara_", 0,
                                               &matches->chara_ids);
    if (rb_starts_with_ci(name, "v/charasplit_"))
        return rb_set_has_number_after_prefix(name, "v/charasplit_", 0,
                                               &matches->chara_ids);

    if (rb_starts_with_ci(base, "3d_chara_head_"))
        return rb_set_has_number_after_prefix(base, "3d_chara_head_", 0,
                                               &matches->chara_ids) ||
               rb_set_has_number_after_prefix(base, "3d_chara_head_", 1,
                                               &matches->dress_ids);
    if (rb_starts_with_ci(base, "3d_chara_body_"))
        return rb_set_has_number_after_prefix(base, "3d_chara_body_", 0,
                                               &matches->dress_ids) ||
               rb_set_has_number_after_prefix(base, "3d_chara_body_", 1,
                                               &matches->chara_ids);
    if (rb_starts_with_ci(base, "3d_tx_head"))
        return rb_set_has_number_after_prefix(base, "3d_tx_head", 0,
                                               &matches->chara_ids);
    if (rb_starts_with_ci(base, "3d_md_body"))
        return rb_set_has_number_after_prefix(base, "3d_md_body", 0,
                                               &matches->dress_ids) ||
               rb_set_has_number_after_prefix(base, "3d_md_body", 1,
                                               &matches->chara_ids);
    if (rb_starts_with_ci(base, "3d_tx_body"))
        return rb_set_has_number_after_prefix(base, "3d_tx_body", 0,
                                               &matches->dress_ids) ||
               rb_set_has_number_after_prefix(base, "3d_tx_body", 1,
                                               &matches->chara_ids);
    return false;
}

static void rb_sql_master_match(sqlite3_context *context, int argc,
                                sqlite3_value **argv)
{
    const RbMasterMatches *matches =
        (const RbMasterMatches *)sqlite3_user_data(context);
    const unsigned char *name;
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_int(context, 0);
        return;
    }
    name = sqlite3_value_text(argv[0]);
    sqlite3_result_int(context,
                       rb_manifest_matches_master((const char *)name,
                                                  matches) ? 1 : 0);
}

static bool rb_is_spine_name(const char *name)
{
    return rb_starts_with_ci(name, "card_cartoon_") ||
           rb_starts_with_ci(name, "card_spine_") ||
           rb_starts_with_ci(name, "spine_sprachen_");
}

/* Return the decimal id after a known resource prefix, or 0 when the name
 * does not encode a card id.  Resource names can contain a directory prefix
 * (for example v/card_100001.acb), so always parse the basename. */
static int rb_card_id_from_name(const char *name)
{
    const char *base = rb_base_name(name);
    const char *p = NULL;
    char *end = NULL;
    long value;
    static const char *const prefixes[] = {
        "card_cartoon_", "card_spine_", "card_bg_", "card_",
        "idol_3d_", "v/card_"
    };
    size_t i;

    if (!base || !base[0])
        return 0;
    /* v/card_ is handled against the complete resource name. */
    if (rb_starts_with_ci(name, "v/card_"))
        p = name + strlen("v/card_");
    else {
        for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
            if (strcmp(prefixes[i], "v/card_") == 0)
                continue;
            if (rb_starts_with_ci(base, prefixes[i])) {
                p = base + strlen(prefixes[i]);
                break;
            }
        }
    }
    if (!p || !isdigit((unsigned char)*p))
        return 0;
    errno = 0;
    value = strtol(p, &end, 10);
    if (errno == ERANGE || end == p || value <= 0 || value > INT_MAX)
        return 0;
    return (int)value;
}

static void rb_scan_data_directory(ResourceBackend *backend,
                                   const wchar_t *directory,
                                   long long *best_version)
{
    wchar_t pattern[RB_PATH_CAP];
    wchar_t path[RB_PATH_CAP];
    WIN32_FIND_DATAW find_data;
    HANDLE find;

    if (!backend || !directory || !rb_directory_exists(directory))
        return;
    if (!rb_join_path(pattern, RB_PATH_CAP, directory, L"manifest_*.db"))
        return;
    find = FindFirstFileW(pattern, &find_data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            long long version = -1;
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (swscanf(find_data.cFileName, L"manifest_%lld.db", &version) == 1 &&
                version > *best_version &&
                rb_join_path(path, RB_PATH_CAP, directory,
                             find_data.cFileName)) {
                *best_version = version;
                wcscpy(backend->manifest_path_w, path);
            }
        } while (FindNextFileW(find, &find_data));
        FindClose(find);
    }

    if (!backend->master_path_w[0] &&
        rb_join_path(path, RB_PATH_CAP, directory, L"master.mdb") &&
        rb_file_exists(path)) {
        wcscpy(backend->master_path_w, path);
    }
}

static void rb_scan_sibling_tool_directory(ResourceBackend *backend,
                                           const wchar_t *directory,
                                           long long *best_version)
{
    wchar_t candidate[RB_PATH_CAP];
    if (rb_join_path(candidate, RB_PATH_CAP, directory, L"CGSS"))
        rb_scan_data_directory(backend, candidate, best_version);
    if (rb_join_path(candidate, RB_PATH_CAP, directory,
                     L"CGSS\\release\\CGSS_ResourceTool"))
        rb_scan_data_directory(backend, candidate, best_version);
}

static void rb_find_data_files(ResourceBackend *backend)
{
    wchar_t cursor[RB_PATH_CAP];
    long long best_version = -1;
    int level;

    wcscpy(cursor, backend->exe_directory_w);
    rb_trim_trailing_slash(cursor);
    for (level = 0; level < 8 && cursor[0]; ++level) {
        rb_scan_data_directory(backend, cursor, &best_version);
        rb_scan_sibling_tool_directory(backend, cursor, &best_version);
        rb_parent_directory(cursor);
    }

    if (backend->manifest_path_w[0]) {
        wchar_t manifest_dir[RB_PATH_CAP];
        wchar_t master[RB_PATH_CAP];
        wcscpy(manifest_dir, backend->manifest_path_w);
        rb_parent_directory(manifest_dir);
        if (rb_join_path(master, RB_PATH_CAP, manifest_dir, L"master.mdb") &&
            rb_file_exists(master))
            wcscpy(backend->master_path_w, master);
    }
}

static void rb_find_assetstudio(ResourceBackend *backend)
{
    wchar_t candidate[RB_PATH_CAP];
    wchar_t parent[RB_PATH_CAP];
    static const wchar_t *const relative[] = {
        L"AssetStudio\\AssetStudio.CLI.exe",
        L"..\\AssetStudio\\AssetStudio.CLI.exe",
        L"..\\..\\AssetStudio\\AssetStudio.CLI.exe",
        L"..\\..\\..\\CGSS\\release\\CGSS_ResourceTool\\AssetStudio\\AssetStudio.CLI.exe"
    };
    int i;
    for (i = 0; i < (int)(sizeof(relative) / sizeof(relative[0])); ++i) {
        if (!rb_join_path(candidate, RB_PATH_CAP, backend->exe_directory_w,
                          relative[i]))
            continue;
        if (rb_file_exists(candidate)) {
            wcscpy(backend->assetstudio_path_w, candidate);
            return;
        }
    }

    /* Development checkouts sometimes keep AssetStudio beside the original
     * command-line tool.  Walk a few parents without baking an absolute
     * machine-specific path into the release binary. */
    wcscpy(parent, backend->exe_directory_w);
    for (i = 0; i < 6 && parent[0]; ++i) {
        if (rb_join_path(candidate, RB_PATH_CAP, parent,
                         L"AssetStudio\\AssetStudio.CLI.exe") &&
            rb_file_exists(candidate)) {
            wcscpy(backend->assetstudio_path_w, candidate);
            return;
        }
        if (rb_join_path(candidate, RB_PATH_CAP, parent,
                         L"CGSS\\release\\CGSS_ResourceTool\\AssetStudio\\AssetStudio.CLI.exe") &&
            rb_file_exists(candidate)) {
            wcscpy(backend->assetstudio_path_w, candidate);
            return;
        }
        rb_parent_directory(parent);
    }
}

static bool rb_lookup_card_name(const ResourceBackend *backend, int card_id,
                                char *name, size_t capacity)
{
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    int rc;
    if (!backend || !backend->master_path_w[0] || card_id <= 0 ||
        !name || capacity == 0)
        return false;
    name[0] = '\0';
    rc = sqlite3_open_v2(backend->master_path,
                         &database,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                         NULL);
    if (rc != SQLITE_OK)
        goto done;
    rc = sqlite3_prepare_v2(database,
                             "SELECT name FROM card_data WHERE id=?",
                             -1, &statement, NULL);
    if (rc != SQLITE_OK)
        goto done;
    sqlite3_bind_int(statement, 1, card_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        if (text && text[0]) {
            snprintf(name, capacity, "%s", (const char *)text);
            rc = SQLITE_OK;
        }
    }
done:
    if (statement)
        sqlite3_finalize(statement);
    if (database)
        sqlite3_close(database);
    return rc == SQLITE_OK && name[0] != '\0';
}

static const wchar_t *rb_category_folder(ResourceCategory category)
{
    static const wchar_t *const folders[RESOURCE_CATEGORY_COUNT] = {
        L"自定义", L"BGM", L"歌曲", L"卡片", L"语音", L"谱面",
        L"舞台", L"动作", L"3D模型", L"Spine", L"贴纸", L"CG"
    };
    if (category < 0 || category >= RESOURCE_CATEGORY_COUNT)
        return folders[RESOURCE_CATEGORY_ALL];
    return folders[(int)category];
}

static const wchar_t *rb_resource_subfolder(const char *name,
                                            ResourceCategory category)
{
    const char *base = rb_base_name(name);
    if (rb_starts_with_ci(base, "card_cartoon_"))
        return L"卡面Spina动画";
    if (rb_starts_with_ci(base, "card_spine_") ||
        rb_starts_with_ci(base, "spine_sprachen_"))
        return L"spine";
    if (rb_starts_with_ci(base, "card_bg_"))
        return L"背景";
    if (rb_starts_with_ci(base, "card_"))
        return L"卡面";
    if (rb_starts_with_ci(name, "v/card_"))
        return L"语音";
    if (rb_starts_with_ci(base, "idol_3d_"))
        return L"3d照片";
    if (rb_starts_with_ci(base, "spine_motion_sticker_"))
        return L"原文件unity3d";
    return rb_category_folder(category);
}

const char *resource_category_label(ResourceCategory category)
{
    if (category < 0 || category >= RESOURCE_CATEGORY_COUNT)
        category = RESOURCE_CATEGORY_ALL;
    return g_category_labels[(int)category];
}

static const char *rb_category_clause(ResourceCategory category)
{
    switch (category) {
    case RESOURCE_CATEGORY_BGM:
        return "name LIKE '%bgm%'";
    case RESOURCE_CATEGORY_SONG:
        return "(name LIKE 'l/song_%' OR name LIKE 'jacket_%' "
               "OR name LIKE 'musicscores_m%')";
    case RESOURCE_CATEGORY_CARD:
        return "(name LIKE 'card_%' OR name LIKE 'idol_3d_%' "
               "OR name LIKE 'v/card_%')";
    case RESOURCE_CATEGORY_VOICE:
        return "name LIKE 'v/%'";
    case RESOURCE_CATEGORY_CHART:
        return "name LIKE 'musicscores_m%.bdb'";
    case RESOURCE_CATEGORY_STAGE:
        return "name LIKE '3d_stage_%'";
    case RESOURCE_CATEGORY_ACTION:
        return "name LIKE '3d_cutt_an_%'";
    case RESOURCE_CATEGORY_MODEL:
        return "(name LIKE '3d_chara_%' OR name LIKE '3d_md_%' "
               "OR name LIKE '3d_tx_%')";
    case RESOURCE_CATEGORY_SPINE:
        return "(name LIKE 'card_spine_%' OR name LIKE 'card_cartoon_%' "
               "OR name LIKE 'spine_sprachen_%')";
    case RESOURCE_CATEGORY_STICKER:
        return "name LIKE 'spine_motion_sticker_%'";
    case RESOURCE_CATEGORY_CG:
        return "(name LIKE '%.usm' OR name LIKE 'm/%movie%.acb' "
               "OR name LIKE 'm/%2drich%.acb')";
    case RESOURCE_CATEGORY_ALL:
    default:
        return "1=1";
    }
}

ResourceBackend *resource_backend_create(const char *exe_directory_utf8)
{
    ResourceBackend *backend;
    sqlite3 *database = NULL;
    int rc;

    backend = (ResourceBackend *)calloc(1, sizeof(*backend));
    if (!backend)
        return NULL;
    InitializeCriticalSection(&backend->job_lock);
    backend->job.state = RESOURCE_JOB_IDLE;

    if (!rb_utf8_to_wide(exe_directory_utf8 ? exe_directory_utf8 : ".",
                         backend->exe_directory_w, RB_PATH_CAP)) {
        snprintf(backend->error, sizeof(backend->error),
                 "无法读取程序目录");
        return backend;
    }
    rb_trim_trailing_slash(backend->exe_directory_w);
    rb_join_path(backend->download_root_w, RB_PATH_CAP,
                 backend->exe_directory_w, L"CGSS_DOWN");
    rb_make_directories(backend->download_root_w);
    rb_find_data_files(backend);
    rb_find_assetstudio(backend);

    if (!backend->manifest_path_w[0]) {
        snprintf(backend->error, sizeof(backend->error),
                 "找不到 manifest_*.db，请放到程序目录");
        return backend;
    }
    if (!rb_wide_to_utf8(backend->manifest_path_w, backend->manifest_path,
                         (int)sizeof(backend->manifest_path))) {
        snprintf(backend->error, sizeof(backend->error),
                 "资源清单路径过长");
        return backend;
    }
    if (backend->master_path_w[0] &&
        !rb_wide_to_utf8(backend->master_path_w, backend->master_path,
                         (int)sizeof(backend->master_path))) {
        backend->master_path[0] = '\0';
    }

    rc = sqlite3_open_v2(backend->manifest_path, &database,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        snprintf(backend->error, sizeof(backend->error),
                 "打开资源清单失败：%s",
                 database ? sqlite3_errmsg(database) : "unknown");
        if (database)
            sqlite3_close(database);
        return backend;
    }
    sqlite3_close(database);
    backend->ready = true;
    return backend;
}

bool resource_backend_is_ready(const ResourceBackend *backend)
{
    return backend && backend->ready;
}

const char *resource_backend_error(const ResourceBackend *backend)
{
    ResourceBackend *mutable_backend;
    const char *source;
    if (!backend)
        return "资源后端未创建";
    mutable_backend = (ResourceBackend *)backend;
    EnterCriticalSection(&mutable_backend->job_lock);
    source = mutable_backend->query_error[0] ? mutable_backend->query_error :
                                               mutable_backend->error;
    snprintf(mutable_backend->reported_error,
             sizeof(mutable_backend->reported_error), "%s", source);
    LeaveCriticalSection(&mutable_backend->job_lock);
    return mutable_backend->reported_error;
}

const char *resource_backend_manifest_path(const ResourceBackend *backend)
{
    return backend ? backend->manifest_path : "";
}

const char *resource_backend_master_path(const ResourceBackend *backend)
{
    return backend ? backend->master_path : "";
}

static void rb_set_query_error(ResourceBackend *backend, const char *error)
{
    if (!backend)
        return;
    EnterCriticalSection(&backend->job_lock);
    snprintf(backend->query_error, sizeof(backend->query_error), "%s",
             error ? error : "");
    LeaveCriticalSection(&backend->job_lock);
}

static int rb_query_resources(ResourceBackend *backend,
                              ResourceCategory category,
                              const char *query_utf8,
                              ResourceItem *items,
                              int capacity,
                              bool count_only)
{
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    RbMasterMatches matches;
    RbQueryPatterns patterns;
    char sql[1024];
    char query_error[512] = "";
    int result = -1;
    int rc;
    int i;

    memset(&matches, 0, sizeof(matches));
    if (!backend || !backend->ready ||
        (!count_only && (!items || capacity <= 0)))
        return -1;
    if (!rb_build_query_patterns(query_utf8, &patterns)) {
        rb_set_query_error(backend, "搜索关键词过长");
        return -1;
    }
    if (patterns.has_non_ascii && !patterns.empty &&
        !backend->master_path[0]) {
        rb_set_query_error(backend,
                           "日文/中文名称搜索需要 master.mdb");
        return -1;
    }
    if (!rb_load_master_matches(backend, category, &patterns, &matches,
                                query_error, sizeof(query_error))) {
        rb_set_query_error(backend, query_error);
        return -1;
    }

    if (count_only) {
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM manifests WHERE %s AND "
                 "(name LIKE ?1 OR name LIKE ?2 OR name LIKE ?3 OR "
                 "rb_master_name_match(name))",
                 rb_category_clause(category));
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT name,hash,size FROM manifests WHERE %s AND "
                 "(name LIKE ?1 OR name LIKE ?2 OR name LIKE ?3 OR "
                 "rb_master_name_match(name)) "
                 "ORDER BY name LIMIT ?4", rb_category_clause(category));
    }

    rc = sqlite3_open_v2(backend->manifest_path, &database,
                          SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        snprintf(query_error, sizeof(query_error),
                 "打开资源清单失败：%s",
                 database ? sqlite3_errmsg(database) : "unknown");
        goto done;
    }
    rc = sqlite3_create_function_v2(database, "rb_master_name_match", 1,
                                    SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                    &matches, rb_sql_master_match,
                                    NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        snprintf(query_error, sizeof(query_error), "SQL 错误：%s",
                 sqlite3_errmsg(database));
        goto done;
    }
    rc = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK) {
        snprintf(query_error, sizeof(query_error), "SQL 错误：%s",
                  sqlite3_errmsg(database));
        goto done;
    }
    for (i = 0; i < RB_QUERY_VARIANT_COUNT; ++i) {
        rc = sqlite3_bind_text(statement, i + 1, patterns.values[i], -1,
                               SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            snprintf(query_error, sizeof(query_error), "绑定搜索参数失败：%s",
                     sqlite3_errmsg(database));
            goto done;
        }
    }
    if (count_only) {
        if (sqlite3_step(statement) == SQLITE_ROW) {
            sqlite3_int64 value = sqlite3_column_int64(statement, 0);
            if (value >= 0 && value <= INT_MAX)
                result = (int)value;
            else
                snprintf(query_error, sizeof(query_error),
                         "资源数量超出接口限制");
        } else {
            snprintf(query_error, sizeof(query_error),
                     "读取资源数量失败：%s", sqlite3_errmsg(database));
        }
    } else {
        int count = 0;
        rc = sqlite3_bind_int(statement, 4, capacity);
        if (rc != SQLITE_OK) {
            snprintf(query_error, sizeof(query_error), "绑定结果数量失败：%s",
                     sqlite3_errmsg(database));
            goto done;
        }
        while (count < capacity &&
               (rc = sqlite3_step(statement)) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(statement, 0);
            const unsigned char *hash = sqlite3_column_text(statement, 1);
            memset(&items[count], 0, sizeof(items[count]));
            snprintf(items[count].name, sizeof(items[count].name), "%s",
                     name ? (const char *)name : "");
            snprintf(items[count].hash, sizeof(items[count].hash), "%s",
                     hash ? (const char *)hash : "");
            items[count].size = sqlite3_column_int64(statement, 2);
            ++count;
        }
        if (rc == SQLITE_ROW || rc == SQLITE_DONE)
            result = count;
        else
            snprintf(query_error, sizeof(query_error),
                     "读取搜索结果失败：%s", sqlite3_errmsg(database));
    }
done:
    if (statement)
        sqlite3_finalize(statement);
    if (database)
        sqlite3_close(database);
    rb_master_matches_clear(&matches);
    if (result >= 0)
        rb_set_query_error(backend, "");
    else
        rb_set_query_error(backend, query_error[0] ? query_error :
                                                "资源搜索失败");
    return result;
}

int resource_backend_count(ResourceBackend *backend,
                           ResourceCategory category,
                           const char *query_utf8)
{
    return rb_query_resources(backend, category, query_utf8,
                              NULL, 0, true);
}

int resource_backend_search(ResourceBackend *backend,
                            ResourceCategory category,
                            const char *query_utf8,
                            ResourceItem *items,
                            int capacity)
{
    return rb_query_resources(backend, category, query_utf8,
                              items, capacity, false);
}

static void rb_set_status(ResourceBackend *backend, const char *status)
{
    EnterCriticalSection(&backend->job_lock);
    snprintf(backend->job.status, sizeof(backend->job.status), "%s",
             status ? status : "");
    LeaveCriticalSection(&backend->job_lock);
}

static void rb_set_error(ResourceBackend *backend, const char *error)
{
    EnterCriticalSection(&backend->job_lock);
    backend->query_error[0] = '\0';
    snprintf(backend->error, sizeof(backend->error), "%s",
             error ? error : "未知错误");
    snprintf(backend->job.status, sizeof(backend->job.status), "%s",
             error ? error : "未知错误");
    LeaveCriticalSection(&backend->job_lock);
}

static bool rb_has_error(ResourceBackend *backend)
{
    bool result;
    EnterCriticalSection(&backend->job_lock);
    result = backend->error[0] != '\0';
    LeaveCriticalSection(&backend->job_lock);
    return result;
}

static bool rb_cancelled(const ResourceBackend *backend)
{
    return InterlockedCompareExchange((volatile LONG *)&backend->cancel_requested,
                                       0, 0) != 0;
}

static void rb_begin_task(ResourceBackend *backend, const ResourceTask *task,
                          int index)
{
    EnterCriticalSection(&backend->job_lock);
    snprintf(backend->job.current_name, sizeof(backend->job.current_name),
             "%s", task->item.name);
    backend->job.current_bytes = 0;
    backend->job.current_total_bytes =
        task->item.size > 0 ? (unsigned long long)task->item.size : 0;
    backend->job.completed_items = index;
    snprintf(backend->job.status, sizeof(backend->job.status), "准备 %s",
             task->item.name);
    LeaveCriticalSection(&backend->job_lock);
}

static void rb_update_bytes(ResourceBackend *backend,
                            unsigned long long completed_bytes,
                            unsigned long long current,
                            unsigned long long total)
{
    EnterCriticalSection(&backend->job_lock);
    backend->job.downloaded_bytes = completed_bytes + current;
    backend->job.current_bytes = current;
    backend->job.current_total_bytes = total;
    LeaveCriticalSection(&backend->job_lock);
}

static bool rb_read_file(const wchar_t *path, unsigned char **data,
                         size_t *length)
{
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *buffer;
    DWORD read_bytes = 0;
    bool ok;

    if (!path || !data || !length)
        return false;
    *data = NULL;
    *length = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (unsigned long long)size.QuadPart > SIZE_MAX ||
        (unsigned long long)size.QuadPart > UINT32_MAX) {
        CloseHandle(file);
        return false;
    }
    buffer = (unsigned char *)malloc((size_t)size.QuadPart + 1u);
    if (!buffer) {
        CloseHandle(file);
        return false;
    }
    ok = ReadFile(file, buffer, (DWORD)size.QuadPart, &read_bytes, NULL) != 0 &&
         (unsigned long long)read_bytes == (unsigned long long)size.QuadPart;
    CloseHandle(file);
    if (!ok) {
        free(buffer);
        return false;
    }
    *data = buffer;
    *length = (size_t)size.QuadPart;
    return true;
}

static bool rb_write_file(const wchar_t *path, const unsigned char *data,
                          size_t length)
{
    HANDLE file;
    DWORD written = 0;
    bool ok;
    if (length > UINT32_MAX)
        return false;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    ok = WriteFile(file, data, (DWORD)length, &written, NULL) != 0 &&
         (size_t)written == length;
    CloseHandle(file);
    return ok;
}

/* CGSS resources use a 16-byte header followed by an LZ4 block. */
static bool rb_lz4_decompress(const ResourceBackend *backend,
                              const unsigned char *raw, size_t raw_length,
                              unsigned char **out, size_t *out_length)
{
    size_t input = 16;
    size_t output = 0;
    size_t expected;
    unsigned char *decoded;

    if (!raw || raw_length < 16 || !out || !out_length)
        return false;
    expected = (size_t)raw[4] | ((size_t)raw[5] << 8) |
               ((size_t)raw[6] << 16) | ((size_t)raw[7] << 24);
    if (expected == 0 || expected > (size_t)512 * 1024 * 1024)
        return false;
    decoded = (unsigned char *)malloc(expected);
    if (!decoded)
        return false;
    while (input < raw_length) {
        unsigned int token;
        size_t literal_length;
        size_t match_length;
        size_t offset;
        size_t match_start;
        if ((backend && rb_cancelled(backend)) ||
            output > expected || input >= raw_length) {
            free(decoded);
            return false;
        }
        token = raw[input++];
        literal_length = (size_t)(token >> 4);
        if (literal_length == 15) {
            unsigned int value;
            do {
                if (input >= raw_length) {
                    free(decoded);
                    return false;
                }
                value = raw[input++];
                literal_length += value;
            } while (value == 255);
        }
        if (literal_length > raw_length - input ||
            literal_length > expected - output) {
            free(decoded);
            return false;
        }
        memcpy(decoded + output, raw + input, literal_length);
        input += literal_length;
        output += literal_length;
        if (input == raw_length)
            break;
        if (raw_length - input < 2) {
            free(decoded);
            return false;
        }
        offset = (size_t)raw[input] | ((size_t)raw[input + 1] << 8);
        input += 2;
        if (offset == 0 || offset > output) {
            free(decoded);
            return false;
        }
        match_length = (size_t)(token & 0x0f) + 4;
        if ((token & 0x0f) == 15) {
            unsigned int value;
            do {
                if (input >= raw_length) {
                    free(decoded);
                    return false;
                }
                value = raw[input++];
                match_length += value;
            } while (value == 255);
        }
        if (match_length > expected - output) {
            free(decoded);
            return false;
        }
        match_start = output - offset;
        for (size_t i = 0; i < match_length; ++i)
        {
            if (backend && (i % 4096u) == 0u && rb_cancelled(backend)) {
                free(decoded);
                return false;
            }
            decoded[output++] = decoded[match_start + i];
        }
    }
    if (output != expected) {
        free(decoded);
        return false;
    }
    *out = decoded;
    *out_length = output;
    return true;
}

static const wchar_t *rb_resource_category(const char *name)
{
    if (rb_ends_with_ci(name, ".acb"))
        return L"Sound";
    if (rb_ends_with_ci(name, ".usm"))
        return L"Movie";
    if (rb_ends_with_ci(name, ".bdb"))
        return L"Generic";
    return L"AssetBundles";
}

static bool rb_http_download(ResourceBackend *backend, ResourceTask *task,
                             unsigned long long completed_bytes)
{
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    wchar_t path[1024];
    wchar_t url_hash[80];
    wchar_t save_part[RB_PATH_CAP];
    const char *base;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    wchar_t content_length[64];
    DWORD content_size = sizeof(content_length);
    HANDLE output = INVALID_HANDLE_VALUE;
    bool ok = false;
    unsigned long long total = 0;
    unsigned long long received = 0;
    const wchar_t *category;

    base = rb_base_name(task->item.name);
    if (!rb_utf8_to_wide(base, path,
                         (int)(sizeof(path) / sizeof(path[0])))) {
        rb_set_error(backend, "资源文件名无效");
        return false;
    }
    rb_sanitize_filename(path);
    if (!rb_join_path(task->output_file, RB_PATH_CAP,
                      task->output_directory, path)) {
        rb_set_error(backend, "资源输出路径过长");
        return false;
    }
    swprintf(save_part, RB_PATH_CAP, L"%ls.part", task->output_file);
    rb_make_directories(task->output_directory);

    /* A previous successful run is a valid cache hit.  Keep the file and
     * still run the optional post-processing stage in the worker. */
    if (rb_file_exists(task->output_file)) {
        HANDLE existing = CreateFileW(task->output_file, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER existing_size;
        if (existing != INVALID_HANDLE_VALUE &&
            GetFileSizeEx(existing, &existing_size) && existing_size.QuadPart > 0) {
            unsigned long long cached = task->item.size > 0 ?
                (unsigned long long)task->item.size :
                (unsigned long long)existing_size.QuadPart;
            CloseHandle(existing);
            rb_update_bytes(backend, completed_bytes, cached,
                            task->item.size > 0 ?
                                (unsigned long long)task->item.size : cached);
            rb_set_status(backend, "资源已存在，跳过下载");
            return true;
        }
        if (existing != INVALID_HANDLE_VALUE)
            CloseHandle(existing);
        DeleteFileW(task->output_file);
    }

    if (strlen(task->item.hash) != 32) {
        rb_set_error(backend, "资源 hash 无效");
        return false;
    }
    for (const char *cursor = task->item.hash; *cursor; ++cursor) {
        if (!isxdigit((unsigned char)*cursor)) {
            rb_set_error(backend, "资源 hash 无效");
            return false;
        }
    }
    rb_utf8_to_wide(task->item.hash, url_hash,
                   (int)(sizeof(url_hash) / sizeof(url_hash[0])));
    category = rb_resource_category(task->item.name);
    swprintf(path, sizeof(path) / sizeof(path[0]),
             L"/dl/resources/%ls/%.2ls/%ls", category, url_hash,
             url_hash);

    session = WinHttpOpen(L"CGSS-DL/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        rb_set_error(backend, "WinHTTP 初始化失败");
        goto cleanup;
    }
    {
        /* Keep cancellation responsive even when the CDN is unreachable. */
        WinHttpSetTimeouts(session, 10000, 10000, 30000, 10000);
    }
    connection = WinHttpConnect(session,
                                L"asset-starlight-stage.akamaized.net",
                                INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        rb_set_error(backend, "连接资源服务器失败");
        goto cleanup;
    }
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request) {
        rb_set_error(backend, "创建下载请求失败");
        goto cleanup;
    }
    WinHttpAddRequestHeaders(
        request,
        L"User-Agent: UnityPlayer/2022.3.56f1 (UnityWebRequest/1.0, libcurl/8.10.1-DEV)\r\n"
        L"X-Unity-Version: 2022.3.56f1",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        rb_set_error(backend, "下载请求失败");
        goto cleanup;
    }
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        char message[128];
        snprintf(message, sizeof(message), "服务器返回 HTTP %lu",
                 (unsigned long)status);
        rb_set_error(backend, message);
        goto cleanup;
    }
    content_length[0] = L'\0';
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, content_length,
                            &content_size, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(content_length, NULL, 10);
    if (total == 0 && task->item.size > 0)
        total = (unsigned long long)task->item.size;
    rb_update_bytes(backend, completed_bytes, 0, total);
    output = CreateFileW(save_part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        rb_set_error(backend, "无法创建下载文件");
        goto cleanup;
    }
    for (;;) {
        DWORD available = 0;
        DWORD read_bytes = 0;
        unsigned char buffer[128 * 1024];
        if (rb_cancelled(backend))
            goto cleanup;
        if (!WinHttpQueryDataAvailable(request, &available))
            goto cleanup;
        if (available == 0) {
            ok = true;
            break;
        }
        if (available > sizeof(buffer))
            available = sizeof(buffer);
        if (!WinHttpReadData(request, buffer, available, &read_bytes) ||
            read_bytes == 0)
            goto cleanup;
        {
            DWORD written = 0;
            if (!WriteFile(output, buffer, read_bytes, &written, NULL) ||
                written != read_bytes)
                goto cleanup;
        }
        received += read_bytes;
        rb_update_bytes(backend, completed_bytes, received, total);
    }

cleanup:
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    if (session)
        WinHttpCloseHandle(session);
    if (!ok) {
        DeleteFileW(save_part);
        if (rb_cancelled(backend))
            rb_set_status(backend, "正在取消下载...");
        else if (!rb_has_error(backend))
            rb_set_error(backend, "下载过程中断");
        return false;
    }
    if (total > 0 && received != total) {
        DeleteFileW(save_part);
        rb_set_error(backend, "下载大小校验失败");
        return false;
    }
    if (!MoveFileExW(save_part, task->output_file,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(save_part);
        rb_set_error(backend, "保存下载文件失败");
        return false;
    }
    return true;
}

static bool rb_unpack_lz4_file(ResourceBackend *backend, ResourceTask *task)
{
    unsigned char *raw = NULL;
    unsigned char *decoded = NULL;
    size_t raw_length = 0;
    size_t decoded_length = 0;
    bool ok = false;
    if (!rb_ends_with_ci(task->item.name, ".unity3d"))
        return true;
    if (rb_cancelled(backend))
        return false;
    rb_set_status(backend, "正在解压 Unity3D LZ4...");
    if (!rb_read_file(task->output_file, &raw, &raw_length))
        return false;
    /* A cache hit may already contain the post-LZ4 UnityFS payload. */
    if (raw_length >= 7 && memcmp(raw, "UnityFS", 7) == 0) {
        ok = true;
    } else if (raw_length >= 16 && rb_lz4_decompress(backend, raw, raw_length,
                                                       &decoded, &decoded_length)) {
        ok = !rb_cancelled(backend) &&
             rb_write_file(task->output_file, decoded, decoded_length);
        free(decoded);
    }
    free(raw);
    if (!ok && !rb_cancelled(backend))
        rb_set_error(backend, "Unity3D LZ4 解包失败");
    return ok;
}

static int rb_copy_pattern(const wchar_t *source_directory,
                           const wchar_t *pattern,
                           const wchar_t *destination)
{
    wchar_t search[RB_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int count = 0;
    if (!rb_join_path(search, RB_PATH_CAP, source_directory, pattern))
        return 0;
    find = FindFirstFileW(search, &data);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    rb_make_directories(destination);
    do {
        wchar_t source[RB_PATH_CAP];
        wchar_t target[RB_PATH_CAP];
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!rb_join_path(source, RB_PATH_CAP, source_directory,
                          data.cFileName) ||
            !rb_join_path(target, RB_PATH_CAP, destination, data.cFileName))
            continue;
        if (CopyFileW(source, target, FALSE) ||
            GetLastError() == ERROR_FILE_EXISTS)
            ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

static void rb_clear_directory(const wchar_t *directory)
{
    wchar_t search[RB_PATH_CAP];
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (!directory || !rb_directory_exists(directory) ||
        !rb_join_path(search, RB_PATH_CAP, directory, L"*"))
        return;
    find = FindFirstFileW(search, &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        wchar_t child[RB_PATH_CAP];
        if (_wcsicmp(data.cFileName, L".") == 0 ||
            _wcsicmp(data.cFileName, L"..") == 0 ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (!rb_join_path(child, RB_PATH_CAP, directory, data.cFileName))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            rb_clear_directory(child);
            RemoveDirectoryW(child);
        } else {
            DeleteFileW(child);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static bool rb_run_assetstudio(ResourceBackend *backend, ResourceTask *task)
{
    wchar_t output_root[RB_PATH_CAP];
    wchar_t marker[RB_PATH_CAP];
    wchar_t asset_subdir[RB_PATH_CAP];
    wchar_t command[RB_PATH_CAP * 3 + 128];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;
    DWORD wait_result;
    int copied;
    bool output_created = false;
    bool wait_failed = false;
    bool success = false;

    if (task->post_kind == RESOURCE_POST_NONE)
        return true;
    if (!backend->assetstudio_path_w[0]) {
        rb_set_error(backend, "找不到 AssetStudio\\AssetStudio.CLI.exe");
        return false;
    }
    if (rb_cancelled(backend))
        return false;
    if (swprintf(marker, RB_PATH_CAP, L"%ls.done", task->output_file) < 0) {
        rb_set_error(backend, "解包标记路径过长");
        return false;
    }
    if (rb_file_exists(marker)) {
        rb_set_status(backend, "Spine 资源已解包");
        return true;
    }
    rb_set_status(backend, "AssetStudio 正在解包...");
    swprintf(output_root, RB_PATH_CAP,
             L"%ls\\AssetStudio_out\\gui_%lu_%d",
             backend->exe_directory_w, (unsigned long)GetTickCount(),
             task->work_index);
    if (!rb_make_directories(output_root)) {
        rb_set_error(backend, "无法创建 AssetStudio 临时目录");
        return false;
    }
    output_created = true;
    rb_clear_directory(output_root);
    if (swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"\"%ls\" \"%ls\" \"%ls\" --game Normal",
                 backend->assetstudio_path_w, task->output_file,
                 output_root) < 0) {
        rb_set_error(backend, "AssetStudio 命令行过长");
        goto cleanup;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, backend->exe_directory_w, &startup, &process)) {
        rb_set_error(backend, "启动 AssetStudio 失败");
        goto cleanup;
    }
    for (;;) {
        wait_result = WaitForSingleObject(process.hProcess, 100);
        if (wait_result == WAIT_OBJECT_0)
            break;
        if (wait_result == WAIT_TIMEOUT && !rb_cancelled(backend))
            continue;
        if (wait_result != WAIT_TIMEOUT)
            wait_failed = true;
        if (rb_cancelled(backend) || wait_failed) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (rb_cancelled(backend) || wait_failed || exit_code != 0) {
        if (wait_failed)
            rb_set_error(backend, "等待 AssetStudio 失败");
        else if (!rb_cancelled(backend))
            rb_set_error(backend, "AssetStudio 解包失败");
        goto cleanup;
    }
    copied = 0;
    if (rb_join_path(asset_subdir, RB_PATH_CAP, output_root, L"TextAsset")) {
        copied += rb_copy_pattern(asset_subdir, L"*.skel*",
                                  task->output_directory);
        copied += rb_copy_pattern(asset_subdir, L"*.atlas*",
                                  task->output_directory);
    }
    if (rb_join_path(asset_subdir, RB_PATH_CAP, output_root, L"Texture2D")) {
        copied += rb_copy_pattern(asset_subdir, L"*.png",
                                  task->output_directory);
        copied += rb_copy_pattern(asset_subdir, L"*.tga",
                                  task->output_directory);
    }
    if (rb_join_path(asset_subdir, RB_PATH_CAP, output_root, L"Sprite"))
        copied += rb_copy_pattern(asset_subdir, L"*.png",
                                  task->output_directory);
    if (rb_join_path(asset_subdir, RB_PATH_CAP, output_root,
                     L"MonoBehaviour"))
        copied += rb_copy_pattern(asset_subdir, L"*.json",
                                  task->output_directory);
    if (copied == 0) {
        rb_set_error(backend, "AssetStudio 未导出 Spine 文件");
        goto cleanup;
    }
    if (task->post_kind == RESOURCE_POST_CARD_SPINE) {
        rb_set_status(backend, "转换 Spine 数据...");
        convert_skels_in_dir(task->output_directory);
        if (rb_cancelled(backend))
            goto cleanup;
        rb_set_status(backend, "合成 Spine 透明度...");
        merge_a8_textures_in_dir(task->output_directory);
    }
    if (rb_cancelled(backend))
        goto cleanup;
    {
        static const char done[] = "done";
        rb_write_file(marker, (const unsigned char *)done,
                      sizeof(done) - 1u);
    }
    success = true;

cleanup:
    /* Do not leave gui_XXXX directories behind when AssetStudio fails or is
     * cancelled.  Successful runs are cleaned up as well after copying. */
    if (output_created) {
        rb_clear_directory(output_root);
        RemoveDirectoryW(output_root);
    }
    return success;
}

/* sticker.c is shared with the command-line tool.  Its small unpack helpers
 * are supplied here so the GUI does not have to pull in the interactive
 * unpack.c menu and its unrelated dependencies. */
void find_assetstudio(wchar_t *out, int n)
{
    wchar_t module_path[RB_PATH_CAP];
    wchar_t module_dir[RB_PATH_CAP];
    wchar_t parent[RB_PATH_CAP];
    wchar_t candidate[RB_PATH_CAP];
    static const wchar_t *const relative[] = {
        L"AssetStudio\\AssetStudio.CLI.exe",
        L"..\\AssetStudio\\AssetStudio.CLI.exe",
        L"..\\..\\AssetStudio\\AssetStudio.CLI.exe",
        L"..\\..\\..\\CGSS\\release\\CGSS_ResourceTool\\AssetStudio\\AssetStudio.CLI.exe"
    };
    DWORD length;
    int i;

    if (!out || n <= 0)
        return;
    out[0] = L'\0';
    length = GetModuleFileNameW(NULL, module_path,
                                (DWORD)(sizeof(module_path) /
                                        sizeof(module_path[0])));
    if (length == 0 || length >= sizeof(module_path) / sizeof(module_path[0]))
        return;
    module_path[length] = L'\0';
    wcscpy(module_dir, module_path);
    rb_parent_directory(module_dir);
    for (i = 0; i < (int)(sizeof(relative) / sizeof(relative[0])); ++i) {
        if (rb_join_path(candidate, RB_PATH_CAP, module_dir, relative[i]) &&
            rb_file_exists(candidate)) {
            wcsncpy(out, candidate, (size_t)n - 1u);
            out[n - 1] = L'\0';
            return;
        }
    }
    wcscpy(parent, module_dir);
    for (i = 0; i < 6 && parent[0]; ++i) {
        if (rb_join_path(candidate, RB_PATH_CAP, parent,
                         L"AssetStudio\\AssetStudio.CLI.exe") &&
            rb_file_exists(candidate)) {
            wcsncpy(out, candidate, (size_t)n - 1u);
            out[n - 1] = L'\0';
            return;
        }
        if (rb_join_path(candidate, RB_PATH_CAP, parent,
                         L"CGSS\\release\\CGSS_ResourceTool\\AssetStudio\\AssetStudio.CLI.exe") &&
            rb_file_exists(candidate)) {
            wcsncpy(out, candidate, (size_t)n - 1u);
            out[n - 1] = L'\0';
            return;
        }
        rb_parent_directory(parent);
    }
}

void wipe_dir(const wchar_t *directory)
{
    rb_clear_directory(directory);
}

int copy_dir(const wchar_t *outdir, const wchar_t *sub,
             const wchar_t *destination, const wchar_t *pattern)
{
    wchar_t source_directory[RB_PATH_CAP];
    if (!rb_join_path(source_directory, RB_PATH_CAP, outdir, sub))
        return 0;
    return rb_copy_pattern(source_directory, pattern, destination);
}

static bool rb_unpack_sticker(ResourceBackend *backend, ResourceTask *task)
{
    wchar_t sticker_root[RB_PATH_CAP];
    wchar_t spine_directory[RB_PATH_CAP];
    wchar_t png_directory[RB_PATH_CAP];
    wchar_t temporary_root[RB_PATH_CAP];
    int result;

    if (!rb_join_path(sticker_root, RB_PATH_CAP,
                      backend->download_root_w, L"贴纸") ||
        !rb_join_path(spine_directory, RB_PATH_CAP,
                      sticker_root, L"spine文件") ||
        !rb_join_path(png_directory, RB_PATH_CAP,
                      sticker_root, L"贴纸PNG")) {
        rb_set_error(backend, "贴纸输出路径过长");
        return false;
    }
    if (!rb_make_directories(task->output_directory) ||
        !rb_make_directories(spine_directory) ||
        !rb_make_directories(png_directory)) {
        rb_set_error(backend, "无法创建贴纸输出目录");
        return false;
    }
    if (rb_cancelled(backend))
        return false;
    rb_set_status(backend, "正在解包贴纸 Spine...");
    result = sticker_unpack_file(task->item.name,
                                 task->output_directory,
                                 spine_directory,
                                 png_directory,
                                 task->work_index);

    /* sticker.c predates the GUI and does not own temporary-output cleanup. */
    if (swprintf(temporary_root, RB_PATH_CAP,
                 L"%ls\\AssetStudio_out\\st%03d",
                 backend->exe_directory_w, task->work_index) >= 0) {
        rb_clear_directory(temporary_root);
        RemoveDirectoryW(temporary_root);
    }
    if (!result) {
        if (!rb_cancelled(backend))
            rb_set_error(backend, "贴纸 Spine 解包失败");
        return false;
    }
    return !rb_cancelled(backend);
}

static ResourcePostKind rb_post_kind(ResourceCategory category,
                                      const char *name)
{
    (void)category;
    if (rb_ends_with_ci(name, ".unity3d") &&
        rb_starts_with_ci(rb_base_name(name), "spine_motion_sticker_"))
        return RESOURCE_POST_STICKER;
    if (rb_ends_with_ci(name, ".unity3d") && rb_is_spine_name(rb_base_name(name)))
        return RESOURCE_POST_CARD_SPINE;
    return RESOURCE_POST_NONE;
}

static bool rb_make_card_output(ResourceBackend *backend,
                                 ResourceCategory category,
                                 ResourceTask *task)
{
    char card_name[256];
    wchar_t card_name_w[512];
    wchar_t card_root[RB_PATH_CAP];
    wchar_t folder[128];
    int card_id;

    card_id = rb_card_id_from_name(task->item.name);
    if (card_id <= 0 || !rb_lookup_card_name(backend, card_id,
                                               card_name, sizeof(card_name)))
        return false;
    if (!rb_utf8_to_wide(card_name, card_name_w,
                         (int)(sizeof(card_name_w) / sizeof(card_name_w[0]))))
        return false;
    rb_sanitize_filename(card_name_w);
    if (swprintf(card_root, RB_PATH_CAP, L"%ls\\%d%ls",
                 backend->download_root_w, card_id, card_name_w) < 0)
        return false;
    folder[0] = L'\0';
    wcsncpy(folder, rb_resource_subfolder(task->item.name, category),
            (sizeof(folder) / sizeof(folder[0])) - 1u);
    folder[(sizeof(folder) / sizeof(folder[0])) - 1u] = L'\0';
    return rb_join_path(task->output_directory, RB_PATH_CAP, card_root,
                        folder);
}

static void rb_choose_output(ResourceBackend *backend, ResourceCategory category,
                             ResourceTask *task)
{
    const wchar_t *folder;
    wchar_t category_root[RB_PATH_CAP];
    if (rb_make_card_output(backend, category, task))
        return;
    folder = rb_resource_subfolder(task->item.name, category);
    if (rb_starts_with_ci(rb_base_name(task->item.name),
                          "spine_motion_sticker_") &&
        rb_join_path(category_root, RB_PATH_CAP,
                     backend->download_root_w, L"贴纸") &&
        rb_join_path(task->output_directory, RB_PATH_CAP,
                     category_root, folder))
        return;
    if (!rb_join_path(task->output_directory, RB_PATH_CAP,
                      backend->download_root_w, folder))
        task->output_directory[0] = L'\0';
}

static DWORD WINAPI rb_worker(void *argument)
{
    ResourceBackend *backend = (ResourceBackend *)argument;
    int completed = 0;
    int i;
    bool failed = false;
    unsigned long long completed_bytes = 0;

    for (i = 0; i < backend->task_count; ++i) {
        ResourceTask *task = &backend->tasks[i];
        if (rb_cancelled(backend))
            break;
        rb_begin_task(backend, task, i);
        EnterCriticalSection(&backend->job_lock);
        backend->job.downloaded_bytes = completed_bytes;
        LeaveCriticalSection(&backend->job_lock);
        if (!rb_http_download(backend, task, completed_bytes) ||
            !rb_unpack_lz4_file(backend, task) ||
            (backend->auto_unpack &&
             (task->post_kind == RESOURCE_POST_STICKER ?
                  !rb_unpack_sticker(backend, task) :
                  !rb_run_assetstudio(backend, task)))) {
            failed = true;
            if (!rb_cancelled(backend))
                break;
            break;
        }
        ++completed;
        EnterCriticalSection(&backend->job_lock);
        completed_bytes = backend->job.downloaded_bytes;
        backend->job.completed_items = completed;
        LeaveCriticalSection(&backend->job_lock);
        rb_set_status(backend, "当前资源完成");
    }

    if (rb_cancelled(backend))
        rb_set_status(backend, "已取消");
    else if (failed && !rb_has_error(backend))
        rb_set_status(backend, "下载失败");
    else if (!failed)
        rb_set_status(backend, "全部下载完成");
    /* Publish a terminal state only after all worker writes are finished. */
    EnterCriticalSection(&backend->job_lock);
    if (rb_cancelled(backend))
        backend->job.state = RESOURCE_JOB_CANCELLED;
    else if (failed)
        backend->job.state = RESOURCE_JOB_FAILED;
    else
        backend->job.state = RESOURCE_JOB_COMPLETED;
    backend->job.completed_items = completed;
    LeaveCriticalSection(&backend->job_lock);
    return 0;
}

bool resource_backend_start_download(ResourceBackend *backend,
                                     ResourceCategory category,
                                     const ResourceItem *items,
                                     int count,
                                     bool auto_unpack)
{
    HANDLE previous_thread = NULL;
    int i;
    if (!backend || !backend->ready || !items || count <= 0 ||
        resource_backend_is_busy(backend))
        return false;

    /* A worker publishes its terminal state just before returning.  Wait for
     * the old thread outside the critical section before reusing its task
     * buffer; otherwise a quick second download could race a still-running
     * worker during realloc/memset. */
    EnterCriticalSection(&backend->job_lock);
    if (backend->job.state == RESOURCE_JOB_RUNNING) {
        LeaveCriticalSection(&backend->job_lock);
        return false;
    }
    previous_thread = backend->thread;
    backend->thread = NULL;
    LeaveCriticalSection(&backend->job_lock);
    if (previous_thread) {
        WaitForSingleObject(previous_thread, INFINITE);
        CloseHandle(previous_thread);
    }

    EnterCriticalSection(&backend->job_lock);
    backend->query_error[0] = '\0';
    if (!rb_reserve_tasks(backend, count)) {
        snprintf(backend->error, sizeof(backend->error),
                 "无法分配下载任务队列");
        backend->job.state = RESOURCE_JOB_FAILED;
        LeaveCriticalSection(&backend->job_lock);
        return false;
    }
    memset(backend->tasks, 0, (size_t)count * sizeof(*backend->tasks));
    backend->task_count = 0;
    backend->auto_unpack = auto_unpack;
    for (i = 0; i < count; ++i) {
        backend->tasks[i].item = items[i];
        backend->tasks[i].post_kind = rb_post_kind(category, items[i].name);
        rb_choose_output(backend, category, &backend->tasks[i]);
        if (!backend->tasks[i].output_directory[0]) {
            snprintf(backend->error, sizeof(backend->error),
                     "资源输出路径无效：%s", items[i].name);
            backend->job.state = RESOURCE_JOB_FAILED;
            LeaveCriticalSection(&backend->job_lock);
            return false;
        }
        backend->tasks[i].work_index = i;
    }
    backend->task_count = count;
    InterlockedExchange(&backend->cancel_requested, 0);
    backend->error[0] = '\0';
    memset(&backend->job, 0, sizeof(backend->job));
    backend->job.state = RESOURCE_JOB_RUNNING;
    backend->job.total_items = count;
    rb_wide_to_utf8(backend->tasks[0].output_directory,
                    backend->job.output_directory,
                    (int)sizeof(backend->job.output_directory));
    snprintf(backend->job.status, sizeof(backend->job.status), "开始下载");
    backend->thread = CreateThread(NULL, 0, rb_worker, backend, 0, NULL);
    if (!backend->thread) {
        backend->job.state = RESOURCE_JOB_FAILED;
        snprintf(backend->error, sizeof(backend->error), "创建下载线程失败");
        LeaveCriticalSection(&backend->job_lock);
        return false;
    }
    LeaveCriticalSection(&backend->job_lock);
    return true;
}

void resource_backend_cancel(ResourceBackend *backend)
{
    if (backend) {
        InterlockedExchange(&backend->cancel_requested, 1);
        EnterCriticalSection(&backend->job_lock);
        if (backend->job.state == RESOURCE_JOB_RUNNING)
            snprintf(backend->job.status, sizeof(backend->job.status),
                     "正在取消...");
        LeaveCriticalSection(&backend->job_lock);
    }
}

void resource_backend_snapshot(ResourceBackend *backend,
                               ResourceJobSnapshot *snapshot)
{
    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!backend)
        return;
    EnterCriticalSection(&backend->job_lock);
    *snapshot = backend->job;
    LeaveCriticalSection(&backend->job_lock);
}

bool resource_backend_is_busy(ResourceBackend *backend)
{
    ResourceJobState state;
    if (!backend)
        return false;
    EnterCriticalSection(&backend->job_lock);
    state = backend->job.state;
    LeaveCriticalSection(&backend->job_lock);
    return state == RESOURCE_JOB_RUNNING;
}

void resource_backend_destroy(ResourceBackend *backend)
{
    if (!backend)
        return;
    resource_backend_cancel(backend);
    if (backend->thread) {
        WaitForSingleObject(backend->thread, INFINITE);
        CloseHandle(backend->thread);
        backend->thread = NULL;
    }
    free(backend->tasks);
    DeleteCriticalSection(&backend->job_lock);
    free(backend);
}
