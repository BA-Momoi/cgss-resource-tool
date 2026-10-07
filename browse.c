/* browse.c: 资源查找 + 下载 整合模块
 *
 * 流程(两段式):
 *   1. 找目标: 输入名称(模糊)或 id -> pager 多选要处理的对象
 *   2. 选资源: 自动拼出该对象的所有资源 -> pager 多选 -> 批量下载
 *
 * 取代原来分开的"查找"和"下载"两个菜单:
 *  - 通用/BGM/谱面/舞台/动作/模型/Spine/贴纸: 直接搜 manifest 资源名
 *  - 歌曲: 歌名模糊 或 歌曲id
 *  - 卡片: 卡名/角色名模糊 或 卡id/角色id
 *  - 语音: 卡名模糊 或 卡id
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <windows.h>
#include <conio.h>
#include "sqlite3.h"
#include "paper.h"
#include "net.h"
#include "util.h"
#include "cg.h"
#include "sticker.h"
#include "stage_map.h"

#define DB_PATH "master.mdb"
#define MAX_ITEMS 1024
#define DOWNLOAD_BATCH_SIZE 64

/* 一条可下载资源 */
typedef struct {
    char disp[128];      /* pager 里显示的名字 */
    char name[256];      /* 清单里的资源名 */
    char hash[64];
    wchar_t sub[64];     /* CGSS_DOWN 下的子目录 */
} BItem;

/* 查资源名对应的 hash, 成功返回 0 */
static int get_hash(sqlite3 *rdb, const char *name, char *hash_out, int n){
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(rdb, "SELECT hash FROM manifests WHERE name=?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW){
        snprintf(hash_out, n, "%s", (const char*)sqlite3_column_text(stmt, 0));
        rc = 0;
    }
    sqlite3_finalize(stmt);
    return rc;
}

/* 已知资源名 -> 加入待选列表(清单里没有就提示跳过) */
static int add_res(sqlite3 *rdb, BItem *items, int *n,
                   const char *name, const wchar_t *sub){
    if (*n >= MAX_ITEMS) return 0;
    for (int i = 0; i < *n; i++)
        if (strcmp(items[i].name, name) == 0 && wcscmp(items[i].sub, sub) == 0)
            return 0;
    if (get_hash(rdb, name, items[*n].hash, 64) != 0){
        printf("清单中无 %s\n", name);
        return 0;
    }
    snprintf(items[*n].name, sizeof items[*n].name, "%s", name);
    snprintf(items[*n].disp, sizeof items[*n].disp, "%s", name);
    wcscpy(items[*n].sub, sub);
    (*n)++;
    return 1;
}

/* 把待选列表转成 pager 用的 dbdef 数组(tmp 需要 n+1 项, 最后一行 END) */
static void make_menu(dbdef *tmp, BItem *items, int n){
    if (n < 0) n = 0;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    for (int i = 0; i < n; i++){
        snprintf(tmp[i].name, sizeof tmp[i].name, "%s", items[i].disp);
        tmp[i].func = NULL;
        tmp[i].state = 0;
    }
    snprintf(tmp[n].name, sizeof tmp[n].name, "END");
    tmp[n].func = NULL;
    tmp[n].state = 0;
}

/* 收集 pager 里勾选的下标, 返回个数 */
static int collect(const dbdef *tmp, int n, int *picked){
    int c = 0;
    for (int i = 0; i < n; i++)
        if (tmp[i].state) picked[c++] = i;
    return c;
}

static int is_sticker_resource(const char *name)
{
    static const char prefix[] = "spine_motion_sticker_";
    return name && strncmp(name, prefix, sizeof(prefix) - 1) == 0;
}

/* 下载一条到 wroot\sub；贴纸下载成功后立即进入统一后处理流程。 */
static int download_item(const BItem *it, const wchar_t *wroot){
    wchar_t wsub[1300];

    if (is_sticker_resource(it->name)) {
        wchar_t sticker_root[1300], raw_dir[1300];
        wchar_t spine_dir[1300], png_dir[1300];

        swprintf(sticker_root, 1300, L"%ls\\贴纸", wroot);
        swprintf(raw_dir, 1300, L"%ls\\原文件unity3d", sticker_root);
        swprintf(spine_dir, 1300, L"%ls\\spine文件", sticker_root);
        swprintf(png_dir, 1300, L"%ls\\贴纸PNG", sticker_root);
        mkdirs(raw_dir);
        mkdirs(spine_dir);
        mkdirs(png_dir);

        if (dl_one(it->name, it->hash, raw_dir) == 0) {
            sticker_unpack_file(it->name, raw_dir, spine_dir, png_dir, 0);
            return 0;
        }
        return -1;
    }

    swprintf(wsub, 1300, L"%ls\\%ls", wroot, it->sub);
    mkdirs(wsub);
    return dl_one(it->name, it->hash, wsub);
}

static int download_selected(BItem *items, const int *picked, int count,
                             const wchar_t *wroot){
    if (count <= 0) return 0;
    int succeeded = 0;
    for (int base = 0; base < count; base += DOWNLOAD_BATCH_SIZE){
        int batch_count = count - base;
        if (batch_count > DOWNLOAD_BATCH_SIZE) batch_count = DOWNLOAD_BATCH_SIZE;
        DlTask tasks[DOWNLOAD_BATCH_SIZE];
        wchar_t dirs[DOWNLOAD_BATCH_SIZE][1300];
        int stickers[DOWNLOAD_BATCH_SIZE];
        int ntasks = 0, nsticker = 0;

        for (int i = 0; i < batch_count; i++){
            int index = picked ? picked[base + i] : base + i;
            BItem *item = &items[index];
            if (is_sticker_resource(item->name)){
                stickers[nsticker++] = index;
                continue;
            }
            swprintf(dirs[ntasks], 1300, L"%ls\\%ls", wroot, item->sub);
            mkdirs(dirs[ntasks]);
            tasks[ntasks].name = item->name;
            tasks[ntasks].hash = item->hash;
            tasks[ntasks].save_dir = dirs[ntasks];
            ntasks++;
        }

        if (ntasks){
            int rc = dl_many(tasks, (size_t)ntasks);
            if (rc > 0) succeeded += rc;
        }
        for (int i = 0; i < nsticker; i++)
            if (download_item(&items[stickers[i]], wroot) == 0) succeeded++;
    }
    return succeeded;
}

/* 下面几个类别要复用"选歌曲/选卡片", 先声明(定义在后面) */
static int choose_songs(sqlite3 *db, int *ids, int max);
static int choose_cards(sqlite3 *db, int *ids, int max);
static int choose_model_cards(sqlite3 *db, int *ids, int max);

static int is_decimal_id(const char *value){
    if (!value || !value[0]) return 0;
    for (const unsigned char *p = (const unsigned char*)value; *p; p++)
        if (!isdigit(*p)) return 0;
    return 1;
}

static int parse_decimal_id(const char *value, int *out){
    if (!is_decimal_id(value)) return 0;
    int parsed = 0;
    for (const unsigned char *p = (const unsigned char*)value; *p; p++){
        int digit = *p - '0';
        if (parsed > (INT_MAX - digit) / 10) return -1;
        parsed = parsed * 10 + digit;
    }
    *out = parsed;
    return 1;
}

/* 查歌名 */
static void get_song_name(sqlite3 *db, int id, char *out, int n){
    out[0] = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name FROM music_data WHERE id=?",
                           -1, &stmt, NULL) == SQLITE_OK){
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            snprintf(out, n, "%s", (const char*)sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
    }
}

/* 建 CGSS_DOWN\<id><名字> 目录 */
static void make_dl_folder(int id, const char *name, wchar_t *wfolder, int n){
    char folder[512];
    snprintf(folder, sizeof folder, "%d%s", id, name);
    wchar_t wroot[1024], wfoldername[512];
    get_dl_root(wroot, 1024);
    utf8_to_wide(folder, wfoldername, 512);
    swprintf(wfolder, n, L"%ls\\%ls", wroot, wfoldername);
    mkdirs(wfolder);
}

/* 通用结尾: 多选 -> 批量下载到 wfolder */
static int pick_and_download_default(const char *title, sqlite3 *db,
                                     sqlite3 *rdb, BItem *items, int n,
                                     const wchar_t *wfolder, int select_all){
    if (n == 0){ printf("没有可下载的资源\n"); return 0; }
    static dbdef tmp[MAX_ITEMS + 1];
    static int picked[MAX_ITEMS];
    make_menu(tmp, items, n);
    if (select_all)
        for (int i = 0; i < n; i++) tmp[i].state = 1;
    int rc = pager_picks(title, tmp, db, rdb, 1);
    if (rc <= 0){ if (rc == -1) printf("已取消\n"); return 0; }
    int c = collect(tmp, n, picked);
    int downloaded = download_selected(items, picked, c, wfolder);
    printf("共下载 %d/%d 个 -> %ls\n", downloaded, c, wfolder);
    return c;
}

static int pick_and_download(const char *title, sqlite3 *db, sqlite3 *rdb,
                             BItem *items, int n, const wchar_t *wfolder){
    return pick_and_download_default(title, db, rdb, items, n, wfolder, 0);
}

/* ================== 通用: 搜 manifest 资源名 ================== */

static void browse_manifest(sqlite3 *rdb, const char *title,
                            const char *extra, const wchar_t *subdir){
    char buf[256];
    printf("输入关键词(留空=全部): ");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    buf[strcspn(buf, "\r\n")] = 0;

    char like[300];
    snprintf(like, sizeof like, "%%%s%%", buf);

    char where[512], sql[1000];
    snprintf(where, sizeof where, "name LIKE ? %s", extra ? extra : "");
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM manifests WHERE %s", where);
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(rdb, sql, -1, &count_stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
        return;
    }
    sqlite3_bind_text(count_stmt, 1, like, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(count_stmt);
    sqlite3_int64 total = rc == SQLITE_ROW ? sqlite3_column_int64(count_stmt, 0) : -1;
    sqlite3_finalize(count_stmt);
    if (total < 0 || total > INT_MAX){
        fprintf(stderr, "搜索结果数量无效: %s\n", sqlite3_errmsg(rdb));
        return;
    }
    if (total == 0){ printf("没有匹配的资源\n"); return; }

    snprintf(sql, sizeof sql,
             "SELECT name,hash FROM manifests WHERE %s ORDER BY name LIMIT ? OFFSET ?", where);
    sqlite3_stmt *page_stmt = NULL, *download_stmt = NULL;
    if (sqlite3_prepare_v2(rdb, sql, -1, &page_stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
        return;
    }
    /* 每次换页绑定关键词和页范围。 */
    int rows = console_rows() - 4;
    if (rows < 1) rows = 1;
    BItem *items = calloc((size_t)rows, sizeof *items);
    unsigned char *selected = calloc((size_t)total, sizeof *selected);
    if (!items || !selected){
        fprintf(stderr, "搜索结果内存分配失败\n");
        free(items);
        free(selected);
        sqlite3_finalize(page_stmt);
        return;
    }

    int sel = 0, loaded_page = -1, loaded_n = 0, chosen = 0, cancelled = 0;
    printf("\x1b[?25l");
    while (1){
        int page = sel / rows;
        if (page != loaded_page){
            sqlite3_reset(page_stmt);
            sqlite3_clear_bindings(page_stmt);
            sqlite3_bind_text(page_stmt, 1, like, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(page_stmt, 2, rows);
            sqlite3_bind_int(page_stmt, 3, page * rows);
            loaded_n = 0;
            while ((rc = sqlite3_step(page_stmt)) == SQLITE_ROW && loaded_n < rows){
                const char *name = (const char*)sqlite3_column_text(page_stmt, 0);
                const char *hash = (const char*)sqlite3_column_text(page_stmt, 1);
                snprintf(items[loaded_n].name, sizeof items[loaded_n].name, "%s", name);
                snprintf(items[loaded_n].hash, sizeof items[loaded_n].hash, "%s", hash);
                snprintf(items[loaded_n].disp, sizeof items[loaded_n].disp, "%s", name);
                wcscpy(items[loaded_n].sub, subdir);
                loaded_n++;
            }
            if (rc != SQLITE_DONE && !(rc == SQLITE_ROW && loaded_n == rows)){
                fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
                cancelled = 1;
                break;
            }
            loaded_page = page;
        }
        printf("\x1b[2J\x1b[H%s   [第%d页/%d页] 匹配%d 已选%d ↑↓选择  Space勾选  A全选/全不选  PgUp/PgDn翻页  Enter下载  Esc取消\n\n",
               title, page + 1, ((int)total - 1) / rows + 1, (int)total, chosen);
        for (int i = 0; i < loaded_n; i++){
            int index = page * rows + i;
            printf(index == sel ? "\x1b[7m%s %s\x1b[0m\n" : "%s %s\n",
                   selected[index] ? "[X]" : "[ ]", items[i].disp);
        }
        int key = _getch();
        if (key == 0xE0 || key == 0){
            key = _getch();
            switch (key){
            case 0x48: if (sel > 0) sel--; break;
            case 0x50: if (sel < total - 1) sel++; break;
            case 0x49: sel = sel > rows ? sel - rows : 0; break;
            case 0x51: sel = sel < total - rows ? sel + rows : (int)total - 1; break;
            case 0x47: sel = 0; break;
            case 0x4F: sel = (int)total - 1; break;
            }
        } else if (key == ' '){
            selected[sel] = !selected[sel];
            chosen += selected[sel] ? 1 : -1;
            if (sel < total - 1) sel++;
        } else if (key == 'a' || key == 'A'){
            int all = chosen != total;
            memset(selected, all, (size_t)total);
            chosen = all ? (int)total : 0;
        } else if (key == '\r' || key == '\n'){
            break;
        } else if (key == 27){
            cancelled = 1;
            break;
        }
    }
    printf("\x1b[?25h\x1b[2J\x1b[H");
    sqlite3_finalize(page_stmt);
    free(items);
    if (cancelled || chosen == 0){
        if (cancelled) printf("已取消\n");
        free(selected);
        return;
    }

    snprintf(sql, sizeof sql, "SELECT name,hash FROM manifests WHERE %s ORDER BY name", where);
    if (sqlite3_prepare_v2(rdb, sql, -1, &download_stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
        free(selected);
        return;
    }
    sqlite3_bind_text(download_stmt, 1, like, -1, SQLITE_TRANSIENT);
    wchar_t wroot[1024];
    get_dl_root(wroot, 1024);
    BItem batch[DOWNLOAD_BATCH_SIZE];
    int index = 0, selected_count = 0, batch_count = 0, downloaded = 0;
    while ((rc = sqlite3_step(download_stmt)) == SQLITE_ROW && index < total){
        if (selected[index] && selected_count < chosen){
            BItem *item = &batch[batch_count++];
            memset(item, 0, sizeof *item);
            snprintf(item->name, sizeof item->name, "%s",
                     (const char*)sqlite3_column_text(download_stmt, 0));
            snprintf(item->hash, sizeof item->hash, "%s",
                     (const char*)sqlite3_column_text(download_stmt, 1));
            wcscpy(item->sub, subdir);
            selected_count++;
            if (batch_count == DOWNLOAD_BATCH_SIZE){
                downloaded += download_selected(batch, NULL, batch_count, wroot);
                batch_count = 0;
            }
        }
        index++;
    }
    if (rc != SQLITE_DONE)
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
    sqlite3_finalize(download_stmt);
    if (batch_count)
        downloaded += download_selected(batch, NULL, batch_count, wroot);
    free(selected);
    printf("共下载 %d/%d 个 -> %ls\n", downloaded, selected_count, wroot);
}

/* 每个类别一个小包装(签名必须和 dbdef.func 一致) */
static int browse_all(sqlite3 *db, sqlite3 *rdb){
    (void)db;
    browse_manifest(rdb, "通用资源搜索", NULL, L"自定义");
    return 0;
}
static int browse_bgm(sqlite3 *db, sqlite3 *rdb){
    (void)db;
    browse_manifest(rdb, "BGM搜索", "AND name LIKE '%bgm%'", L"BGM");
    return 0;
}
static int browse_sticker(sqlite3 *db, sqlite3 *rdb){
    (void)db;
    browse_manifest(rdb, "贴纸搜索",
                    "AND name LIKE 'spine_motion_sticker%'", L"贴纸");
    return 0;
}

/* 谱面: 先按歌名/id 找歌, 再列出这首歌的所有谱面 */
static int browse_chart(sqlite3 *db, sqlite3 *rdb){
    int ids[32];
    int nids = choose_songs(db, ids, 32);
    if (nids <= 0) return 0;
    for (int s = 0; s < nids; s++){
        int id = ids[s];
        char sname[128];
        get_song_name(db, id, sname, sizeof sname);
        printf("\n========== %d|%s 的谱面 ==========\n", id, sname);

        static BItem items[MAX_ITEMS];
        int n = 0;
        char res[256];
        sqlite3_stmt *lstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,difficulty_1,difficulty_2,difficulty_3,difficulty_4 "
                "FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(lstmt, 1, id);
            while (sqlite3_step(lstmt) == SQLITE_ROW && n < MAX_ITEMS){
                int live_id = sqlite3_column_int(lstmt, 0);
                snprintf(res, sizeof res, "musicscores_m%d.bdb", live_id);
                add_res(rdb, items, &n, res, L"谱面");
                if (n > 0)
                    snprintf(items[n-1].disp, sizeof items[n-1].disp,
                             "live %d | diff %d/%d/%d/%d", live_id,
                             sqlite3_column_int(lstmt, 1),
                             sqlite3_column_int(lstmt, 2),
                             sqlite3_column_int(lstmt, 3),
                             sqlite3_column_int(lstmt, 4));
            }
            sqlite3_finalize(lstmt);
        }
        wchar_t wfolder[1024];
        make_dl_folder(id, sname, wfolder, 1024);
        pick_and_download("谱面资源(空格勾选, Enter下载)", db, rdb,
                          items, n, wfolder);
    }
    return 0;
}

/* Return 1 when the manifest contains the package, including an existing entry. */
static int add_stage_package(sqlite3 *rdb, BItem *items, int *n,
                             int music_id, int live_id, int source_bg,
                             int target_bg, const char *name, const char *kind){
    char hash[64];
    if (get_hash(rdb, name, hash, sizeof hash) != 0) return 0;
    for (int i = 0; i < *n; i++){
        if (strcmp(items[i].name, name) == 0 && wcscmp(items[i].sub, L"舞台") == 0){
            char ref[48];
            snprintf(ref, sizeof ref, "+%d/%d", music_id, live_id);
            int already_present = 0;
            size_t ref_len = strlen(ref);
            for (char *p = strstr(items[i].disp, ref); p;
                 p = strstr(p + ref_len, ref)){
                char before = p == items[i].disp ? '\0' : p[-1];
                char after = p[ref_len];
                if ((before == '\0' || before == ',') &&
                    (after == '\0' || after == ',')){
                    already_present = 1;
                    break;
                }
            }
            if (!already_present){
                size_t used = strlen(items[i].disp);
                if (used + strlen(ref) + 2 < sizeof items[i].disp){
                    strcat(items[i].disp, ",");
                    strcat(items[i].disp, ref);
                }
            }
            return 1;
        }
    }
    if (*n >= MAX_ITEMS) return 1;

    BItem *item = &items[*n];
    snprintf(item->name, sizeof item->name, "%s", name);
    snprintf(item->hash, sizeof item->hash, "%s", hash);
    if (source_bg == target_bg)
        snprintf(item->disp, sizeof item->disp,
                 "%d/%d bg%d %s %s", music_id, live_id, target_bg, kind, name);
    else
        snprintf(item->disp, sizeof item->disp,
                 "%d/%d bg%d>%d %s %s", music_id, live_id,
                 source_bg, target_bg, kind, name);
    wcscpy(item->sub, L"舞台");
    (*n)++;
    return 1;
}

static int add_stage_3d_packages_for_bg(sqlite3 *rdb, BItem *items, int *n,
                                        int music_id, int live_id, int bg_id){
    static const struct {
        const char *format;
        const char *kind;
    } packages[] = {
        {"3d_stage_%04d.unity3d", "3D舞台"},
        {"3d_stage_%04d_hq.unity3d", "3D舞台HQ"},
        {"3d_stage_%04d_variable.unity3d", "3D舞台变量资源"},
        {"3d_stage_%04d_variable_hq.unity3d", "3D舞台变量资源HQ"},
        {"3d_stage_%04d_variable_low.unity3d", "3D舞台变量资源Low"}
    };
    int found = 0;
    char name[256];
    for (size_t i = 0; i < sizeof packages / sizeof packages[0]; i++){
        snprintf(name, sizeof name, packages[i].format, bg_id);
        found += add_stage_package(rdb, items, n, music_id, live_id,
                                   bg_id, bg_id, name, packages[i].kind);
    }
    return found;
}

static int add_stage_2d_packages_for_bg(sqlite3 *rdb, BItem *items, int *n,
                                        int music_id, int live_id,
                                        int source_bg, int target_bg){
    static const struct {
        const char *format;
        const char *kind;
    } packages[] = {
        {"live_bg2d_bg_live_%d.unity3d", "2D舞台背景"},
        {"anime_fl_liv_2dbg_%d.unity3d", "动态2D背景"},
        {"anime_fl_liv_2dbg_%d_hq.unity3d", "动态2D背景HQ"}
    };
    int found = 0;
    char name[256];
    for (size_t i = 0; i < sizeof packages / sizeof packages[0]; i++){
        snprintf(name, sizeof name, packages[i].format, target_bg);
        found += add_stage_package(rdb, items, n, music_id, live_id,
                                   source_bg, target_bg, name, packages[i].kind);
    }
    return found;
}

/* Include conditional background substitutions recorded by the master DB. */
static int add_stage_packages_for_live(sqlite3 *db, sqlite3 *rdb,
                                       BItem *items, int *n,
                                       int music_id, int live_id, int live_bg){
    int stage_bg = 0;
    int found_3d = 0;
    if (stage_bg_for_live(live_id, &stage_bg))
        found_3d = add_stage_3d_packages_for_bg(rdb, items, n,
                                                music_id, live_id, stage_bg);

    if (live_bg <= 0) return found_3d;
    add_stage_2d_packages_for_bg(rdb, items, n, music_id, live_id,
                                 live_bg, live_bg);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT after_bg_id FROM live_bg_replace WHERE before_bg_id=? "
            "AND (include_live_id=0 OR include_live_id=?) "
            "AND (exclude_live_id=0 OR exclude_live_id<>?) ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK)
        return found_3d;
    sqlite3_bind_int(stmt, 1, live_bg);
    sqlite3_bind_int(stmt, 2, live_id);
    sqlite3_bind_int(stmt, 3, live_id);
    while (sqlite3_step(stmt) == SQLITE_ROW){
        int replacement_bg = sqlite3_column_int(stmt, 0);
        if (replacement_bg > 0 && replacement_bg != live_bg)
            add_stage_2d_packages_for_bg(rdb, items, n, music_id, live_id,
                                         live_bg, replacement_bg);
    }
    sqlite3_finalize(stmt);
    return found_3d;
}

/* Aggregate all selected songs into one de-duplicated stage package picker. */
static int browse_stage(sqlite3 *db, sqlite3 *rdb){
    int ids[32];
    int nids = choose_songs(db, ids, 32);
    if (nids <= 0) return 0;

    static BItem items[MAX_ITEMS];
    int n = 0, missing = 0;
    for (int s = 0; s < nids; s++){
        sqlite3_stmt *lstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id, live_bg FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) != SQLITE_OK){
            fprintf(stderr, "查询歌曲%d舞台失败: %s\n", ids[s], sqlite3_errmsg(db));
            continue;
        }
        sqlite3_bind_int(lstmt, 1, ids[s]);
        char song_name[128];
        get_song_name(db, ids[s], song_name, sizeof song_name);
        printf("歌曲 %d | %s\n", ids[s], song_name);
        while (sqlite3_step(lstmt) == SQLITE_ROW){
            int live_id = sqlite3_column_int(lstmt, 0);
            int live_bg = sqlite3_column_int(lstmt, 1);
            if (add_stage_packages_for_live(db, rdb, items, &n,
                                            ids[s], live_id, live_bg) == 0){
                int stage_bg = 0;
                if (!stage_map_available()){
                    printf("歌%d | live%d：缺少 stage_live_map.csv，无法读取 Master3dLive 舞台映射\n",
                           ids[s], live_id);
                } else if (!stage_bg_for_live(live_id, &stage_bg)){
                    printf("歌%d | live%d：Master3dLive 中没有对应的 3D 舞台配置\n",
                           ids[s], live_id);
                } else {
                    printf("歌%d | live%d | 3D bg%d：资源清单中没有对应的舞台 Unity 包\n",
                           ids[s], live_id, stage_bg);
                }
                missing++;
            }
            if (n >= MAX_ITEMS) break;
        }
        sqlite3_finalize(lstmt);
        if (n >= MAX_ITEMS) break;
    }
    if (n == 0){
        printf(missing ? "没有可下载的舞台包；请确认 stage_live_map.csv、master.mdb 和资源清单可用\n"
                       : "所选歌曲没有舞台记录\n");
        return 0;
    }

    wchar_t wroot[1024];
    get_dl_root(wroot, 1024);
    mkdirs(wroot);
    int three_d_count = 0;
    for (int i = 0; i < n; i++)
        if (strncmp(items[i].name, "3d_stage_", 9) == 0)
            three_d_count++;
    char title[160];
    snprintf(title, sizeof title,
             "舞台资源(%d首歌/%d个3D包/%d个2D背景包；默认全选, Enter下载)",
             nids, three_d_count, n - three_d_count);
    pick_and_download_default(title, db, rdb, items, n, wroot, 1);
    return 0;
}

/* 动作: 先按歌名/id 找歌, 再列出这首歌的动作 */
static int browse_action(sqlite3 *db, sqlite3 *rdb){
    int ids[32];
    int nids = choose_songs(db, ids, 32);
    if (nids <= 0) return 0;
    for (int s = 0; s < nids; s++){
        int id = ids[s];
        char sname[128];
        get_song_name(db, id, sname, sizeof sname);
        printf("\n========== %d|%s 的动作 ==========\n", id, sname);

        static BItem items[MAX_ITEMS];
        int n = 0;
        char like[64];
        snprintf(like, sizeof like, "3d_cutt_an_chr_son%d%%", id);
        sqlite3_stmt *mstmt = NULL;
        if (sqlite3_prepare_v2(rdb,
                "SELECT name,hash FROM manifests WHERE name LIKE ? ORDER BY name",
                -1, &mstmt, NULL) == SQLITE_OK){
            sqlite3_bind_text(mstmt, 1, like, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(mstmt) == SQLITE_ROW && n < MAX_ITEMS){
                snprintf(items[n].name, sizeof items[n].name, "%s",
                         (const char*)sqlite3_column_text(mstmt, 0));
                snprintf(items[n].hash, sizeof items[n].hash, "%s",
                         (const char*)sqlite3_column_text(mstmt, 1));
                snprintf(items[n].disp, sizeof items[n].disp, "%s", items[n].name);
                wcscpy(items[n].sub, L"动作");
                n++;
            }
            sqlite3_finalize(mstmt);
        }
        wchar_t wfolder[1024];
        make_dl_folder(id, sname, wfolder, 1024);
        pick_and_download("动作资源(空格勾选, Enter下载)", db, rdb,
                          items, n, wfolder);
    }
    return 0;
}

/* 3D模型: 按卡名/角色名/id 找卡, 再列出模型资源 */
static int browse_model(sqlite3 *db, sqlite3 *rdb){
    int ids[64];
    int nids = choose_model_cards(db, ids, 64);
    if (nids <= 0) return 0;
    for (int s = 0; s < nids; s++){
        int card_id = ids[s];
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,name,chara_id,open_dress_id FROM card_data WHERE id=?",
                -1, &stmt, NULL) != SQLITE_OK)
            continue;
        sqlite3_bind_int(stmt, 1, card_id);
        if (sqlite3_step(stmt) != SQLITE_ROW){ sqlite3_finalize(stmt); continue; }
        char cname[128];
        snprintf(cname, sizeof cname, "%s",
                 (const char*)sqlite3_column_text(stmt, 1));
        int chara_id = sqlite3_column_int(stmt, 2);
        int dress_id = sqlite3_column_int(stmt, 3);
        sqlite3_finalize(stmt);
        printf("\n========== %d|%s | chara_id=%d", card_id, cname, chara_id);
        if (dress_id > 0) printf(" | dress_id=%d", dress_id);
        printf(" 的3D模型 ==========\n");

        static BItem items[MAX_ITEMS];
        int n = 0;
        char res[256];
        if (dress_id > 0){
            snprintf(res, sizeof res, "3d_chara_body_%04d.unity3d", dress_id);
            add_res(rdb, items, &n, res, L"3D模型");
            snprintf(res, sizeof res, "3d_chara_head_%04d_%04d_hq.unity3d",
                     chara_id, dress_id);
            if (get_hash(rdb, res, items[n].hash, 64) != 0)
                snprintf(res, sizeof res, "3d_chara_head_%04d_%04d.unity3d",
                         chara_id, dress_id);
            add_res(rdb, items, &n, res, L"3D模型");
            snprintf(res, sizeof res, "3d_md_body%04d_hq.unity3d", dress_id);
            if (get_hash(rdb, res, items[n].hash, 64) != 0)
                snprintf(res, sizeof res, "3d_md_body%04d.unity3d", dress_id);
            add_res(rdb, items, &n, res, L"3D模型");
            const char *tx[3] = {"hq","multi","spec"};
            for (int i = 0; i < 3; i++){
                snprintf(res, sizeof res, "3d_tx_body%04d_%s.unity3d",
                         dress_id, tx[i]);
                add_res(rdb, items, &n, res, L"3D模型");
            }
        } else {
            printf("该卡没有专属服装, 没有3D模型\n");
        }
        wchar_t wfolder[1024];
        make_dl_folder(card_id, cname, wfolder, 1024);
        char title[256];
        if (dress_id > 0)
            snprintf(title, sizeof title,
                     "3D模型资源 [card_id=%d chara_id=%d dress_id=%d] (空格勾选, Enter下载)",
                     card_id, chara_id, dress_id);
        else
            snprintf(title, sizeof title,
                     "3D模型资源 [card_id=%d chara_id=%d] (空格勾选, Enter下载)",
                     card_id, chara_id);
        pick_and_download(title, db, rdb, items, n, wfolder);
    }
    return 0;
}

/* Spine: 按卡名/角色名/id 找卡, 再列出 Spine 资源 */
static int browse_spine(sqlite3 *db, sqlite3 *rdb){
    int ids[64];
    int nids = choose_cards(db, ids, 64);
    if (nids <= 0) return 0;
    for (int s = 0; s < nids; s++){
        int card_id = ids[s];
        char cname[128] = "";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id,name FROM card_data WHERE id=?",
                               -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, card_id);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                snprintf(cname, sizeof cname, "%s",
                         (const char*)sqlite3_column_text(stmt, 1));
            sqlite3_finalize(stmt);
        }
        printf("\n========== %d|%s 的Spine ==========\n", card_id, cname);

        static BItem items[MAX_ITEMS];
        int n = 0;
        char res[256];
        snprintf(res, sizeof res, "card_spine_%d.unity3d", card_id);
        add_res(rdb, items, &n, res, L"Spine");
        add_res(rdb, items, &n, "spine_sprachen_petit_chara_common.unity3d", L"Spine");
        snprintf(res, sizeof res, "card_cartoon_%d.unity3d", card_id);
        add_res(rdb, items, &n, res, L"Spine_Live");

        wchar_t wfolder[1024];
        make_dl_folder(card_id, cname, wfolder, 1024);
        pick_and_download("Spine资源(空格勾选, Enter下载)", db, rdb,
                          items, n, wfolder);
    }
    return 0;
}

/* ================== CG影片: 搜 usm, 自动带上同 movie 的音频 ================== */

static int browse_cg(sqlite3 *db, sqlite3 *rdb){
    char buf[128];
    printf("输入CG关键词(如 anivcount / movie_0029, 留空=全部): ");
    if (fgets(buf, sizeof buf, stdin) == NULL) return 0;
    buf[strcspn(buf, "\r\n")] = 0;

    char like[300];
    snprintf(like, sizeof like, "%%%s%%", buf);

    static dbdef tmp[513];
    static char row_name[513][256];   /* 选中的 usm 原始资源名 */
    int n = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(rdb,
            "SELECT name FROM manifests WHERE name LIKE '%.usm' AND name LIKE ? "
            "ORDER BY name LIMIT 512",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW && n < 512){
        snprintf(row_name[n], sizeof row_name[n], "%s",
                 (const char*)sqlite3_column_text(stmt, 0));
        /* 显示名: movie_XXXX[_alt]; 2drich 则显示 2drich<id> | 歌名 */
        const char *p = strstr(row_name[n], "movie_");
        if (p){
            snprintf(tmp[n].name, sizeof tmp[n].name, "%s", p);
        } else if (strncmp(row_name[n], "m/live/high/2drich", 18) == 0){
            const char *d = row_name[n] + 18;
            char sid[16] = "";
            int k = 0;
            while (d[k] && isdigit((unsigned char)d[k]) && k < 14){
                sid[k] = d[k];
                k++;
            }
            sid[k] = 0;
            char sname[128] = "";
            if (sid[0]){
                sqlite3_stmt *sstmt = NULL;
                if (sqlite3_prepare_v2(db,
                        "SELECT name FROM music_data WHERE id=?",
                        -1, &sstmt, NULL) == SQLITE_OK){
                    sqlite3_bind_int(sstmt, 1, atoi(sid));
                    if (sqlite3_step(sstmt) == SQLITE_ROW)
                        snprintf(sname, sizeof sname, "%s",
                                 (const char*)sqlite3_column_text(sstmt, 0));
                    sqlite3_finalize(sstmt);
                }
            }
            if (sname[0])
                snprintf(tmp[n].name, sizeof tmp[n].name,
                         "2drich%s | %s", sid, sname);
            else
                snprintf(tmp[n].name, sizeof tmp[n].name, "2drich%s", sid);
        } 
        /* 针对为数不多真正的2D动画显示对应歌曲 */
        else if(strncmp(row_name[n], "m/live/high/movie", 17) == 0){
            const char *d = row_name[n] + 17;
            char sid[16] = "";
            int k = 0;
            while (d[k] && isdigit((unsigned char)d[k]) && k < 14){
                sid[k] = d[k];
                k++;
            }
            sid[k] = 0;
            char sname[128] = "";
            if (sid[0]){
                sqlite3_stmt *sstmt = NULL;
                if (sqlite3_prepare_v2(db,
                        "SELECT name FROM music_data WHERE id=?",
                        -1, &sstmt, NULL) == SQLITE_OK){
                    sqlite3_bind_int(sstmt, 1, atoi(sid));
                    if (sqlite3_step(sstmt) == SQLITE_ROW)
                        snprintf(sname, sizeof sname, "%s",
                                 (const char*)sqlite3_column_text(sstmt, 0));
                    sqlite3_finalize(sstmt);
                }
            }
            if (sname[0])
                snprintf(tmp[n].name, sizeof tmp[n].name,
                         "2dmovie%s | %s", sid, sname);
            else
                snprintf(tmp[n].name, sizeof tmp[n].name, "2dmovie%s", sid);
        }
        
        else {
            snprintf(tmp[n].name, sizeof tmp[n].name, "%s", row_name[n]);
        }
        tmp[n].func = NULL;
        tmp[n].state = 0;
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0){ printf("没有匹配的CG\n"); return 0; }

    static int picked_idx[512];
    int np = 0;
    if (n == 1){
        picked_idx[np++] = 0;
    } else {
        snprintf(tmp[n].name, sizeof tmp[n].name, "END");
        tmp[n].func = NULL;
        tmp[n].state = 0;
        int r = pager_picks("CG影片(空格勾选, Enter确认)", tmp, NULL, rdb, 1);
        if (r <= 0){ if (r == -1) printf("已取消\n"); return 0; }
        for (int i = 0; i < n; i++)
            if (tmp[i].state) picked_idx[np++] = i;
    }

    for (int s = 0; s < np; s++){
        int idx = picked_idx[s];
        const char *usm_name = row_name[idx];
        int is2dmovie = 0;
        /* 提取 movie_XXXX */
        char movie[64] = "";
        if(strstr(usm_name, "movie_")){
            const char *pm = strstr(usm_name,"movie_"); 
            if (pm){
                const char *d = pm + 6;
                int k = 0;
                while (d[k] && isdigit((unsigned char)d[k]) && k < 60){
                    movie[k] = d[k];
                    k++;
                }
                movie[k] = 0;
            }
            is2dmovie = 1;
        }else if(strstr(usm_name,"movie")){
            const char *pm = strstr(usm_name,"movie"); 
            if (pm){
                const char *d = pm + 5;
                int k = 0;
                while (d[k] && isdigit((unsigned char)d[k]) && k < 60){
                    movie[k] = d[k];
                    k++;
                }
                movie[k] = 0;
            }
            is2dmovie = 1;
        }
        printf("\n========== %s ==========\n", usm_name);

        /* 自动组合: 选中的 usm + 精确配对的音频, 不再弹第二个选择菜单 */
        static BItem items[16];
        int n2 = 0;
        char hash[64];
        if (get_hash(rdb, usm_name, hash, 64) == 0){
            snprintf(items[n2].name, sizeof items[n2].name, "%s", usm_name);
            snprintf(items[n2].hash, sizeof items[n2].hash, "%s", hash);
            snprintf(items[n2].disp, sizeof items[n2].disp, "%s", usm_name);
            wcscpy(items[n2].sub, L"影片");
            n2++;
        }
        /* 配对音频: m/AnivCount/<dir>/movie_<id>.usm
         *        -> m/bgm_anivcount_<dir>_movie_<id>.acb (同一个 dir+id) */
        char dirnum[16] = "";
        {
            const char *a = strstr(usm_name, "AnivCount/");
            if (a){
                const char *d = a + 10;
                int k = 0;
                while (d[k] && isdigit((unsigned char)d[k]) && k < 14){
                    dirnum[k] = d[k];
                    k++;
                }
                dirnum[k] = 0;
            }
        }
        if (dirnum[0] && movie[0]){
            char cand[512];
            snprintf(cand, sizeof cand, "m/bgm_anivcount_%s_movie_%s.acb",
                     dirnum, movie);
            if (get_hash(rdb, cand, hash, 64) == 0 && n2 < 16){
                snprintf(items[n2].name, sizeof items[n2].name, "%s", cand);
                snprintf(items[n2].hash, sizeof items[n2].hash, "%s", hash);
                snprintf(items[n2].disp, sizeof items[n2].disp, "%s", cand);
                wcscpy(items[n2].sub, L"音频");
                n2++;
            }
        } else if (strncmp(usm_name, "m/live/high/2drich", 18) == 0){
            /* 2D rich MV: 2drich<歌id>.usm -> l/song_<歌id>.acb */
            const char *d = usm_name + 18;
            char sid[16] = "";
            int k = 0;
            while (d[k] && isdigit((unsigned char)d[k]) && k < 14){
                sid[k] = d[k];
                k++;
            }
            sid[k] = 0;
            if (sid[0]){
                char cand[512];
                snprintf(cand, sizeof cand, "l/song_%s.acb", sid);
                if (get_hash(rdb, cand, hash, 64) == 0 && n2 < 16){
                    snprintf(items[n2].name, sizeof items[n2].name, "%s", cand);
                    snprintf(items[n2].hash, sizeof items[n2].hash, "%s", hash);
                    snprintf(items[n2].disp, sizeof items[n2].disp, "%s", cand);
                    wcscpy(items[n2].sub, L"音频");
                    n2++;
                }
            }
        }
        else if (strncmp(usm_name, "m/live/high/movie", 17) == 0) {
            /* m/live/high/movie5044.usm 这种没有下划线的情况 */
            const char *d = usm_name + 17;   /* 跳过 "m/live/high/movie" */
            char sid[16] = "";
            int k = 0;
            while (d[k] && isdigit((unsigned char)d[k]) && k < 14) {
                sid[k] = d[k];
                k++;
            }
            sid[k] = 0;
            if (sid[0]) {
                char cand[512];
                snprintf(cand, sizeof cand, "l/song_%s.acb", sid);
                if (get_hash(rdb, cand, hash, 64) == 0 && n2 < 16) {
                    snprintf(items[n2].name, sizeof items[n2].name, "%s", cand);
                    snprintf(items[n2].hash, sizeof items[n2].hash, "%s", hash);
                    snprintf(items[n2].disp, sizeof items[n2].disp, "%s", cand);
                    wcscpy(items[n2].sub, L"音频");
                    n2++;
                }
            }
        } 
        else if (movie[0]){
            /* 兜底: 同 movie id 的所有 acb */
            char mlike[128];
            if(!is2dmovie)
                snprintf(mlike, sizeof mlike, "%%movie_%s%%.acb", movie);
            else
                snprintf(mlike,sizeof mlike,"%%song_%s%%.acb",movie);
            sqlite3_stmt *mstmt = NULL;
            if (sqlite3_prepare_v2(rdb,
                    "SELECT name,hash FROM manifests WHERE name LIKE ? ORDER BY name",
                    -1, &mstmt, NULL) == SQLITE_OK){
                sqlite3_bind_text(mstmt, 1, mlike, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(mstmt) == SQLITE_ROW && n2 < 16){
                    const char *nm = (const char*)sqlite3_column_text(mstmt, 0);
                    snprintf(items[n2].name, sizeof items[n2].name, "%s", nm);
                    snprintf(items[n2].hash, sizeof items[n2].hash, "%s",
                             (const char*)sqlite3_column_text(mstmt, 1));
                    snprintf(items[n2].disp, sizeof items[n2].disp, "%s", nm);
                    wcscpy(items[n2].sub, L"音频");
                    n2++;
                }
                sqlite3_finalize(mstmt);
            }
        }
        if (n2 == 0){ printf("没有相关文件\n"); continue; }

        /* 目录: CGSS_DOWN\CG\movie_XXXX 或 CG\2drichXXXX */
        char folder[512];
        if (movie[0]){
            snprintf(folder, sizeof folder, "CG\\movie_%s", movie);
        } else {
            char b2[256];
            snprintf(b2, sizeof b2, "%s", base_name(usm_name));
            char *dot = strrchr(b2, '.');
            if (dot) *dot = 0;
            snprintf(folder, sizeof folder, "CG\\%s", b2);
        }
        wchar_t wroot[1024], wfolder[1024], wfoldername[512];
        get_dl_root(wroot, 1024);
        utf8_to_wide(folder, wfoldername, 512);
        swprintf(wfolder, 1024, L"%ls\\%ls", wroot, wfoldername);
        mkdirs(wfolder);

        /* 自动下载全部(影片 + 配对音频), 不弹选择菜单 */
        printf("自动下载 %d 个文件(影片+配对音频)...\n", n2);
        int downloaded = download_selected(items, NULL, n2, wfolder);
        printf("成功下载 %d/%d 个文件\n", downloaded, n2);

        /* 下载完问一句: 要不要直接解包并合成音频 */
        char yn[16];
        printf("是否解包成 mp4 并合成音频? (y/n): ");
        if (fgets(yn, sizeof yn, stdin) && (yn[0] == 'y' || yn[0] == 'Y'))
            unpack_cg_folder(wfolder);
    }
    return 0;
}

/* ================== 第一级选择: 通用"查对象" ================== */

/* 把查询结果(id,name)列出多选, 选中的 id 写入 out, 返回个数
 * 只有一条结果时自动选中, 不用按空格 */
static int pick_rows(sqlite3_stmt *stmt, int *out, int max){
    static int row_ids[512];
    static dbdef tmp[513];
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < 512){
        row_ids[n] = sqlite3_column_int(stmt, 0);
        snprintf(tmp[n].name, sizeof tmp[n].name, "%d | %s", row_ids[n],
                 (const char*)sqlite3_column_text(stmt, 1));
        tmp[n].func = NULL;
        tmp[n].state = 0;
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0){ printf("没有匹配的记录\n"); return 0; }
    if (n == 1){ out[0] = row_ids[0]; return 1; }

    snprintf(tmp[n].name, sizeof tmp[n].name, "END");
    tmp[n].func = NULL;
    tmp[n].state = 0;
    int rc = pager_picks("搜索结果(空格勾选, Enter确认)", tmp, NULL, NULL, 1);
    if (rc <= 0) return 0;
    int c = 0;
    for (int i = 0; i < n && c < max; i++)
        if (tmp[i].state) out[c++] = row_ids[i];
    return c;
}

/* 选歌曲: 输入歌名(模糊)或歌曲id */
static int choose_songs(sqlite3 *db, int *ids, int max){
    char buf[128];
    printf("输入歌曲名(模糊)或歌曲id: ");
    if (fgets(buf, sizeof buf, stdin) == NULL) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if (!buf[0]) return 0;

    sqlite3_stmt *stmt = NULL;
    int mid = 0;
    int numeric = parse_decimal_id(buf, &mid);
    if (numeric != 0){
        if (numeric < 0){
            printf("歌曲ID超出有效范围\n");
            return 0;
        }
        if (sqlite3_prepare_v2(db, "SELECT id,name FROM music_data WHERE id=?",
                               -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, mid);
            return pick_rows(stmt, ids, max);
        }
        return 0;
    }
    char like[256];
    snprintf(like, sizeof like, "%%%s%%", buf);
    if (sqlite3_prepare_v2(db,
            "SELECT id,name FROM music_data WHERE name LIKE ? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    return pick_rows(stmt, ids, max);
}

/* 选卡片: 输入卡名/角色名(模糊)或 卡id/角色id */
static int choose_cards(sqlite3 *db, int *ids, int max){
    char buf[128];
    printf("输入卡名/角色名(模糊)或id: ");
    if (fgets(buf, sizeof buf, stdin) == NULL) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if (!buf[0]) return 0;

    sqlite3_stmt *stmt = NULL;
    if (isdigit((unsigned char)buf[0])){
        int nid = atoi(buf);
        /* 先按卡id查, 查不到再按角色id查 */
        if (sqlite3_prepare_v2(db, "SELECT id,name FROM card_data WHERE id=?",
                               -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, nid);
            int r = pick_rows(stmt, ids, max);
            if (r > 0) return r;
        }
        if (sqlite3_prepare_v2(db,
                "SELECT c.id,c.name FROM card_data c WHERE c.chara_id=? ORDER BY c.id",
                -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, nid);
            return pick_rows(stmt, ids, max);
        }
        return 0;
    }
    /* 卡名 或 角色名 模糊匹配 */
    char like[256];
    snprintf(like, sizeof like, "%%%s%%", buf);
    if (sqlite3_prepare_v2(db,
            "SELECT c.id,c.name FROM card_data c WHERE c.name LIKE ? "
            "OR c.chara_id IN (SELECT chara_id FROM chara_data WHERE name LIKE ?) "
            "ORDER BY c.id",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    return pick_rows(stmt, ids, max);
}

/* 3D模型搜索结果需要在卡片行中显示角色ID及可用的服装ID。 */
static int pick_model_rows(sqlite3_stmt *stmt, int *out, int max){
    static int row_ids[512];
    static dbdef tmp[513];
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < 512){
        int card_id = sqlite3_column_int(stmt, 0);
        int chara_id = sqlite3_column_int(stmt, 2);
        int dress_id = sqlite3_column_int(stmt, 3);
        const char *name = (const char*)sqlite3_column_text(stmt, 1);
        row_ids[n] = card_id;
        if (dress_id > 0)
            snprintf(tmp[n].name, sizeof tmp[n].name,
                     "%d | chara_id=%d | dress_id=%d | %s",
                     card_id, chara_id, dress_id, name ? name : "");
        else
            snprintf(tmp[n].name, sizeof tmp[n].name,
                     "%d | chara_id=%d | %s", card_id, chara_id,
                     name ? name : "");
        tmp[n].func = NULL;
        tmp[n].state = 0;
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0){ printf("没有匹配的记录\n"); return 0; }
    if (n == 1){ out[0] = row_ids[0]; return 1; }

    snprintf(tmp[n].name, sizeof tmp[n].name, "END");
    tmp[n].func = NULL;
    tmp[n].state = 0;
    int rc = pager_picks("3D模型搜索结果(空格勾选, Enter确认)",
                         tmp, NULL, NULL, 1);
    if (rc <= 0) return 0;
    int c = 0;
    for (int i = 0; i < n && c < max; i++)
        if (tmp[i].state) out[c++] = row_ids[i];
    return c;
}

static int read_model_search_term(char *buf, size_t cap){
    if (!buf || cap < 2) return 0;

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)){
        wchar_t wide[256];
        DWORD count = 0;
        if (!ReadConsoleW(input, wide, (DWORD)(sizeof wide / sizeof wide[0] - 1),
                          &count, NULL))
            return 0;
        while (count > 0 && (wide[count - 1] == L'\r' || wide[count - 1] == L'\n'))
            count--;
        int bytes = count == 0 ? 0 : WideCharToMultiByte(
            CP_UTF8, 0, wide, (int)count, buf, (int)cap - 1, NULL, NULL);
        if (count > 0 && bytes <= 0) return 0;
        buf[bytes] = 0;
        return 1;
    }

    if (!fgets(buf, (int)cap, stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

/* 选3D模型卡片: 沿用通用卡片的卡名/角色名/卡id/角色id匹配规则。 */
static int choose_model_cards(sqlite3 *db, int *ids, int max){
    char buf[128];
    printf("输入卡名/角色名(模糊)或id: ");
    if (!read_model_search_term(buf, sizeof buf)) return 0;
    if (!buf[0]) return 0;

    sqlite3_stmt *stmt = NULL;
    int nid = 0;
    int numeric = parse_decimal_id(buf, &nid);
    if (numeric != 0){
        if (numeric < 0){
            printf("卡片/角色ID超出有效范围\n");
            return 0;
        }
        if (sqlite3_prepare_v2(db,
                "SELECT id,name,chara_id,open_dress_id FROM card_data WHERE id=?",
                -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, nid);
            int r = pick_model_rows(stmt, ids, max);
            if (r > 0) return r;
        }
        if (sqlite3_prepare_v2(db,
                "SELECT id,name,chara_id,open_dress_id FROM card_data "
                "WHERE chara_id=? ORDER BY id",
                -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, nid);
            return pick_model_rows(stmt, ids, max);
        }
        return 0;
    }

    char like[256];
    snprintf(like, sizeof like, "%%%s%%", buf);
    if (sqlite3_prepare_v2(db,
            "SELECT c.id,c.name,c.chara_id,c.open_dress_id FROM card_data c "
            "WHERE c.name LIKE ? "
            "OR c.chara_id IN (SELECT chara_id FROM chara_data WHERE name LIKE ?) "
            "ORDER BY c.id",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
    return pick_model_rows(stmt, ids, max);
}

/* ================== 歌曲 ================== */

static int browse_song(sqlite3 *db, sqlite3 *rdb){
    int ids[32];
    int nids = choose_songs(db, ids, 32);
    if (nids <= 0) return 0;

    for (int s = 0; s < nids; s++){
        int id = ids[s];
        char sname[128] = "";
        sqlite3_stmt *nstmt = NULL;
        if (sqlite3_prepare_v2(db, "SELECT name FROM music_data WHERE id=?",
                               -1, &nstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(nstmt, 1, id);
            if (sqlite3_step(nstmt) == SQLITE_ROW)
                snprintf(sname, sizeof sname, "%s",
                         (const char*)sqlite3_column_text(nstmt, 0));
            sqlite3_finalize(nstmt);
        }
        printf("\n========== %d|%s ==========\n", id, sname);

        static BItem items[MAX_ITEMS];
        int n = 0;
        char res[256];

        /* 音频 */
        snprintf(res, sizeof res, "l/song_%d.acb", id);
        add_res(rdb, items, &n, res, L"acb文件");
        /* 封面 */
        sqlite3_stmt *jstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT jacket_id FROM live_data WHERE music_data_id=? AND jacket_id > 0 LIMIT 1",
                -1, &jstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(jstmt, 1, id);
            if (sqlite3_step(jstmt) == SQLITE_ROW){
                snprintf(res, sizeof res, "jacket_%d.unity3d",
                         sqlite3_column_int(jstmt, 0));
                add_res(rdb, items, &n, res, L"封面");
            }
            sqlite3_finalize(jstmt);
        }
        /* 谱面 + 舞台 */
        sqlite3_stmt *lstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id, live_bg FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(lstmt, 1, id);
            while (sqlite3_step(lstmt) == SQLITE_ROW && n < MAX_ITEMS){
                int live_id = sqlite3_column_int(lstmt, 0);
                int live_bg = sqlite3_column_int(lstmt, 1);
                snprintf(res, sizeof res, "musicscores_m%d.bdb", live_id);
                add_res(rdb, items, &n, res, L"谱面");
                add_stage_packages_for_live(db, rdb, items, &n,
                                            id, live_id, live_bg);
            }
            sqlite3_finalize(lstmt);
        }
        /* 动作 */
        char like[64];
        snprintf(like, sizeof like, "3d_cutt_an_chr_son%d%%", id);
        sqlite3_stmt *mstmt = NULL;
        if (sqlite3_prepare_v2(rdb,
                "SELECT name,hash FROM manifests WHERE name LIKE ? ORDER BY name",
                -1, &mstmt, NULL) == SQLITE_OK){
            sqlite3_bind_text(mstmt, 1, like, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(mstmt) == SQLITE_ROW && n < MAX_ITEMS){
                snprintf(items[n].name, sizeof items[n].name, "%s",
                         (const char*)sqlite3_column_text(mstmt, 0));
                snprintf(items[n].hash, sizeof items[n].hash, "%s",
                         (const char*)sqlite3_column_text(mstmt, 1));
                snprintf(items[n].disp, sizeof items[n].disp, "%s", items[n].name);
                wcscpy(items[n].sub, L"动作");
                n++;
            }
            sqlite3_finalize(mstmt);
        }

        if (n == 0){ printf("没有可下载的资源\n"); continue; }
        static dbdef tmp[MAX_ITEMS + 1];
        static int picked[MAX_ITEMS];
        make_menu(tmp, items, n);
        int rc = pager_picks("歌曲资源(空格勾选, Enter下载)", tmp, db, rdb, 1);
        if (rc <= 0){ if (rc == -1) printf("已取消\n"); continue; }

        int c = collect(tmp, n, picked);
        char folder[512];
        snprintf(folder, sizeof folder, "%d%s", id, sname);
        wchar_t wroot[1024], wfolder[1024], wfoldername[512];
        get_dl_root(wroot, 1024);
        utf8_to_wide(folder, wfoldername, 512);
        swprintf(wfolder, 1024, L"%ls\\%ls", wroot, wfoldername);
        mkdirs(wfolder);
        int downloaded = download_selected(items, picked, c, wfolder);
        printf("共下载 %d/%d 个 -> %ls\n", downloaded, c, wfolder);
    }
    return 0;
}

/* ================== 卡片 ================== */

static int browse_card(sqlite3 *db, sqlite3 *rdb){
    int ids[64];
    int nids = choose_cards(db, ids, 64);
    if (nids <= 0) return 0;

    for (int s = 0; s < nids; s++){
        int card_id = ids[s];
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,name,chara_id,open_dress_id FROM card_data WHERE id=?",
                -1, &stmt, NULL) != SQLITE_OK){
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            continue;
        }
        sqlite3_bind_int(stmt, 1, card_id);
        if (sqlite3_step(stmt) != SQLITE_ROW){
            sqlite3_finalize(stmt);
            continue;
        }
        char cname[128];
        snprintf(cname, sizeof cname, "%s",
                 (const char*)sqlite3_column_text(stmt, 1));
        int chara_id = sqlite3_column_int(stmt, 2);
        int dress_id = sqlite3_column_int(stmt, 3);
        sqlite3_finalize(stmt);
        printf("\n========== %d|%s ==========\n", card_id, cname);

        static BItem items[MAX_ITEMS];
        int n = 0;
        char res[256];
        const char *sizes[6] = {"circle","sm","s","m","l","xl"};
        for (int i = 0; i < 6; i++){
            snprintf(res, sizeof res, "card_%d_%s.unity3d", card_id, sizes[i]);
            add_res(rdb, items, &n, res, L"卡面");
        }
        snprintf(res, sizeof res, "card_bg_%d.unity3d", card_id);
        add_res(rdb, items, &n, res, L"背景");
        snprintf(res, sizeof res, "card_bg_%d_01.unity3d", card_id);
        add_res(rdb, items, &n, res, L"背景");
        snprintf(res, sizeof res, "card_bg_%d_s.unity3d", card_id);
        add_res(rdb, items, &n, res, L"背景");
        snprintf(res, sizeof res, "card_cartoon_%d.unity3d", card_id);
        add_res(rdb, items, &n, res, L"Live2D");
        snprintf(res, sizeof res, "idol_3d_%d_l.unity3d", card_id);
        add_res(rdb, items, &n, res, L"3d照片");
        snprintf(res, sizeof res, "idol_3d_%d_s.unity3d", card_id);
        add_res(rdb, items, &n, res, L"3d照片");
        snprintf(res, sizeof res, "v/card_%d.acb", card_id);
        add_res(rdb, items, &n, res, L"语音");
        snprintf(res, sizeof res, "card_spine_%d.unity3d", card_id);
        add_res(rdb, items, &n, res, L"Spine");
        add_res(rdb, items, &n, "spine_sprachen_petit_chara_common.unity3d", L"Spine");
        if (dress_id > 0 && n < MAX_ITEMS){
            snprintf(res, sizeof res, "3d_chara_body_%04d.unity3d", dress_id);
            add_res(rdb, items, &n, res, L"3D模型");
            snprintf(res, sizeof res, "3d_chara_head_%04d_%04d_hq.unity3d",
                     chara_id, dress_id);
            if (get_hash(rdb, res, items[n].hash, 64) != 0)
                snprintf(res, sizeof res, "3d_chara_head_%04d_%04d.unity3d",
                         chara_id, dress_id);
            add_res(rdb, items, &n, res, L"3D模型");
            snprintf(res, sizeof res, "3d_md_body%04d_hq.unity3d", dress_id);
            if (get_hash(rdb, res, items[n].hash, 64) != 0)
                snprintf(res, sizeof res, "3d_md_body%04d.unity3d", dress_id);
            add_res(rdb, items, &n, res, L"3D模型");
            const char *tx[3] = {"hq","multi","spec"};
            for (int i = 0; i < 3; i++){
                snprintf(res, sizeof res, "3d_tx_body%04d_%s.unity3d", dress_id, tx[i]);
                add_res(rdb, items, &n, res, L"3D模型");
            }
        }

        if (n == 0){ printf("没有可下载的资源\n"); continue; }
        static dbdef tmp[MAX_ITEMS + 1];
        static int picked[MAX_ITEMS];
        make_menu(tmp, items, n);
        int rc = pager_picks("卡片资源(空格勾选, Enter下载)", tmp, db, rdb, 1);
        if (rc <= 0){ if (rc == -1) printf("已取消\n"); continue; }

        int c = collect(tmp, n, picked);
        char folder[512];
        snprintf(folder, sizeof folder, "%d%s", card_id, cname);
        wchar_t wroot[1024], wfolder[1024], wfoldername[512];
        get_dl_root(wroot, 1024);
        utf8_to_wide(folder, wfoldername, 512);
        swprintf(wfolder, 1024, L"%ls\\%ls", wroot, wfoldername);
        mkdirs(wfolder);
        int downloaded = download_selected(items, picked, c, wfolder);
        printf("共下载 %d/%d 个 -> %ls\n", downloaded, c, wfolder);
    }
    return 0;
}

/* ================== 语音 ================== */

static int browse_voice(sqlite3 *db, sqlite3 *rdb){
    int ids[64];
    int nids = choose_cards(db, ids, 64);   /* 复用卡片的查找逻辑 */
    if (nids <= 0) return 0;

    for (int s = 0; s < nids; s++){
        int card_id = ids[s];
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id,name FROM card_data WHERE id=?",
                               -1, &stmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(stmt, 1, card_id);
            if (sqlite3_step(stmt) != SQLITE_ROW){
                sqlite3_finalize(stmt);
                continue;
            }
            char cname[128];
            snprintf(cname, sizeof cname, "%s",
                     (const char*)sqlite3_column_text(stmt, 1));
            sqlite3_finalize(stmt);

            static BItem items[8];
            int n = 0;
            char res[256];
            snprintf(res, sizeof res, "v/card_%d.acb", card_id);
            add_res(rdb, items, &n, res, L"语音");
            if (n == 0) continue;

            char folder[512];
            snprintf(folder, sizeof folder, "%d%s", card_id, cname);
            wchar_t wroot[1024], wfolder[1024], wfoldername[512];
            get_dl_root(wroot, 1024);
            utf8_to_wide(folder, wfoldername, 512);
            swprintf(wfolder, 1024, L"%ls\\%ls", wroot, wfoldername);
            mkdirs(wfolder);
            download_item(&items[0], wfolder);
            printf("语音已下载 -> %ls\n", wfolder);
        }
    }
    return 0;
}

/* ================== 模块入口 ================== */

int browse_main(void){
    sqlite3 *db = NULL, *rdb = NULL;
    const char *mp = find_manifest();
    if (GetFileAttributesA(DB_PATH) == INVALID_FILE_ATTRIBUTES){
        fprintf(stderr, "缺少 master.mdb，请把它放到程序同目录\n");
        return -1;
    }
    if (!mp){
        fprintf(stderr, "缺少 manifest_*.db（资源清单库）；联网同步失败且本地没有可用清单\n");
        return -1;
    }
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK){
        fprintf(stderr, "打开 master.mdb 失败（%s）\n", sqlite3_errmsg(db));
        return -1;
    }
    if (sqlite3_open(mp, &rdb) != SQLITE_OK){
        fprintf(stderr, "打开 %s 失败（%s）\n", mp, sqlite3_errmsg(rdb));
        sqlite3_close(db);
        return -1;
    }

    dbdef menu[] = {
        {"1.自由搜索(按资源名)", browse_all, 0},
        {"2.BGM", browse_bgm, 0},
        {"3.歌曲(名/id)", browse_song, 0},
        {"4.卡片(名/角色名/id)", browse_card, 0},
        {"5.角色语音(名/id)", browse_voice, 0},
        {"6.谱面(歌名/id)", browse_chart, 0},
        {"7.舞台(歌名/id)", browse_stage, 0},
        {"8.动作(歌名/id)", browse_action, 0},
        {"9.3D模型(卡名/角色名/id)", browse_model, 0},
        {"10.Spine小人(卡名/角色名/id)", browse_spine, 0},
        {"11.贴纸", browse_sticker, 0},
        {"12.CG影片(关键词)", browse_cg, 0},
        {"13.返回", NULL, 0},
        {"END", NULL, 0}
    };
    while (1){
        int rc = pager_picks("资源查找与下载", menu, db, rdb, 0);
        if (rc == -1)
            continue;
        if (rc == 12)
            break;
    }
    sqlite3_close(rdb);
    sqlite3_close(db);
    return 0;
}
