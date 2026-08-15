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
#include <windows.h>
#include "sqlite3.h"
#include "paper.h"
#include "net.h"
#include "util.h"
#include "cg.h"

#define DB_PATH "master.mdb"
#define MAX_ITEMS 512

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
static void add_res(sqlite3 *rdb, BItem *items, int *n,
                    const char *name, const wchar_t *sub){
    if (*n >= MAX_ITEMS) return;
    if (get_hash(rdb, name, items[*n].hash, 64) != 0){
        printf("清单中无 %s\n", name);
        return;
    }
    snprintf(items[*n].name, sizeof items[*n].name, "%s", name);
    snprintf(items[*n].disp, sizeof items[*n].disp, "%s", name);
    wcscpy(items[*n].sub, sub);
    (*n)++;
}

/* 把待选列表转成 pager 用的 dbdef 数组(tmp 需要 n+1 项, 最后一行 END) */
static void make_menu(dbdef *tmp, BItem *items, int n){
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

/* 下载一条到 wroot\sub */
static void download_item(const BItem *it, const wchar_t *wroot){
    wchar_t wsub[1300];
    swprintf(wsub, 1300, L"%ls\\%ls", wroot, it->sub);
    mkdirs(wsub);
    printf("下载 %s ...\n", it->name);
    dl_one(it->name, it->hash, wsub);
}

/* 下面几个类别要复用"选歌曲/选卡片", 先声明(定义在后面) */
static int choose_songs(sqlite3 *db, int *ids, int max);
static int choose_cards(sqlite3 *db, int *ids, int max);

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
static int pick_and_download(const char *title, sqlite3 *db, sqlite3 *rdb,
                             BItem *items, int n, const wchar_t *wfolder){
    if (n == 0){ printf("没有可下载的资源\n"); return 0; }
    static dbdef tmp[MAX_ITEMS + 1];
    static int picked[MAX_ITEMS];
    make_menu(tmp, items, n);
    int rc = pager_picks(title, tmp, db, rdb, 1);
    if (rc <= 0){ if (rc == -1) printf("已取消\n"); return 0; }
    int c = collect(tmp, n, picked);
    for (int i = 0; i < c; i++)
        download_item(&items[picked[i]], wfolder);
    printf("共下载 %d 个 -> %ls\n", c, wfolder);
    return c;
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

    char sql[1000];
    if (extra && extra[0])
        snprintf(sql, sizeof sql,
            "SELECT name,hash FROM manifests WHERE name LIKE ? %s ORDER BY name LIMIT %d",
            extra, MAX_ITEMS);
    else
        snprintf(sql, sizeof sql,
            "SELECT name,hash FROM manifests WHERE name LIKE ? ORDER BY name LIMIT %d",
            MAX_ITEMS);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(rdb, sql, -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
        return;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);

    static BItem items[MAX_ITEMS];
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < MAX_ITEMS){
        snprintf(items[n].name, sizeof items[n].name, "%s",
                 (const char*)sqlite3_column_text(stmt, 0));
        snprintf(items[n].hash, sizeof items[n].hash, "%s",
                 (const char*)sqlite3_column_text(stmt, 1));
        snprintf(items[n].disp, sizeof items[n].disp, "%s", items[n].name);
        wcscpy(items[n].sub, subdir);
        n++;
    }
    sqlite3_finalize(stmt);

    if (n == 0){ printf("没有匹配的资源\n"); return; }
    printf("匹配 %d 个(Space勾选, A全选, Enter下载):\n", n);

    static dbdef tmp[MAX_ITEMS + 1];
    make_menu(tmp, items, n);
    int rc = pager_picks(title, tmp, NULL, rdb, 1);
    if (rc <= 0){ if (rc == -1) printf("已取消\n"); return; }

    static int picked[MAX_ITEMS];
    int c = collect(tmp, n, picked);
    wchar_t wroot[1024];
    get_dl_root(wroot, 1024);
    for (int i = 0; i < c; i++)
        download_item(&items[picked[i]], wroot);
    printf("共下载 %d 个 -> %ls\n", c, wroot);
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

/* 舞台: 先按歌名/id 找歌, 再列出这首歌的舞台 */
static int browse_stage(sqlite3 *db, sqlite3 *rdb){
    int ids[32];
    int nids = choose_songs(db, ids, 32);
    if (nids <= 0) return 0;
    for (int s = 0; s < nids; s++){
        int id = ids[s];
        char sname[128];
        get_song_name(db, id, sname, sizeof sname);
        printf("\n========== %d|%s 的舞台 ==========\n", id, sname);

        static BItem items[MAX_ITEMS];
        int n = 0;
        char res[256];
        sqlite3_stmt *lstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id, live_bg FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(lstmt, 1, id);
            while (sqlite3_step(lstmt) == SQLITE_ROW && n < MAX_ITEMS){
                int live_id = sqlite3_column_int(lstmt, 0);
                int live_bg = sqlite3_column_int(lstmt, 1);
                if (live_bg > 0){
                    snprintf(res, sizeof res, "3d_stage_%d.unity3d", live_bg);
                    add_res(rdb, items, &n, res, L"舞台");
                    if (n > 0)
                        snprintf(items[n-1].disp, sizeof items[n-1].disp,
                                 "live %d | 舞台 %d", live_id, live_bg);
                    snprintf(res, sizeof res, "3d_stage_%d_hq.unity3d", live_bg);
                    add_res(rdb, items, &n, res, L"舞台");
                }
            }
            sqlite3_finalize(lstmt);
        }
        wchar_t wfolder[1024];
        make_dl_folder(id, sname, wfolder, 1024);
        pick_and_download("舞台资源(空格勾选, Enter下载)", db, rdb,
                          items, n, wfolder);
    }
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
    int nids = choose_cards(db, ids, 64);
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
        printf("\n========== %d|%s 的3D模型 ==========\n", card_id, cname);

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
        pick_and_download("3D模型资源(空格勾选, Enter下载)", db, rdb,
                          items, n, wfolder);
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
        } else {
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

        /* 提取 movie_XXXX */
        char movie[64] = "";
        const char *pm = strstr(usm_name, "movie_");
        if (pm){
            const char *d = pm + 6;
            int k = 0;
            while (d[k] && isdigit((unsigned char)d[k]) && k < 60){
                movie[k] = d[k];
                k++;
            }
            movie[k] = 0;
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
            snprintf(mlike, sizeof mlike, "%%movie_%s%%.acb", movie);
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
        for (int i = 0; i < n2; i++)
            download_item(&items[i], wfolder);

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
    if (isdigit((unsigned char)buf[0])){
        int mid = atoi(buf);
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
                if (live_bg > 0){
                    snprintf(res, sizeof res, "3d_stage_%d.unity3d", live_bg);
                    add_res(rdb, items, &n, res, L"舞台");
                    snprintf(res, sizeof res, "3d_stage_%d_hq.unity3d", live_bg);
                    add_res(rdb, items, &n, res, L"舞台");
                }
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
        for (int i = 0; i < c; i++)
            download_item(&items[picked[i]], wfolder);
        printf("共下载 %d 个 -> %ls\n", c, wfolder);
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
        for (int i = 0; i < c; i++)
            download_item(&items[picked[i]], wfolder);
        printf("共下载 %d 个 -> %ls\n", c, wfolder);
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
        fprintf(stderr, "缺少 manifest_*.db（资源清单库），请先运行 check_update.exe 获取\n");
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
