// download.c: 数据下载并解析（菜单2：卡片/歌曲/按角色批量）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sqlite3.h"
#include "download.h"
#include "net.h"
#include "util.h"

#define DB_PATH "master.mdb"
#define MANIFEST_PATH "manifest_10133800.db"

typedef struct {
    char name[256];
    char hash[64];
    wchar_t sub[64];
} ResItem;

static int get_hash(sqlite3 *rdb, const char *name, char *hash_out, int n){
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(rdb, "SELECT hash FROM manifests WHERE name=?", -1, &stmt, NULL) != SQLITE_OK)
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


static void add_item(sqlite3 *rdb, ResItem *items, int *n, const char *name, const wchar_t *sub){
    if (*n >= 64) return;
    if (get_hash(rdb, name, items[*n].hash, 64) != 0){
        printf("清单中无 %s\n", name);
        return;
    }
    snprintf(items[*n].name, sizeof items[*n].name, "%s", name);
    wcscpy(items[*n].sub, sub);
    (*n)++;
}


static void download_items(ResItem *items, int n, const wchar_t *wfolder){
    for (int i = 0; i < n; i++){
        wchar_t wsub[1024];
        swprintf(wsub, 1024, L"%ls\\%ls", wfolder, items[i].sub);
        mkdirs(wsub);
        dl_one(items[i].name, items[i].hash, wsub);
    }
    printf("共 %d 个资源\n", n);
}
/* ================== 菜单2：卡片资源下载 ================== */

/* 按卡片 id 查询，成功返回 0 并回填 cname/chara_id/dress_id */

static int query_card(sqlite3 *db, int card_id, char *cname, int n, int *chara_id, int *dress_id){
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,chara_id,open_dress_id FROM card_data WHERE id=?",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, card_id);
    if (sqlite3_step(stmt) != SQLITE_ROW){
        fprintf(stderr, "没有相关卡片\n");
        sqlite3_finalize(stmt);
        return -1;
    }
    snprintf(cname, n, "%s", (const char*)sqlite3_column_text(stmt, 1));
    *chara_id = sqlite3_column_int(stmt, 2);
    *dress_id = sqlite3_column_int(stmt, 3);
    sqlite3_finalize(stmt);
    return 0;
}


static void print_card_res_menu(void){
    printf("可选资源（空格/逗号分隔数字，a=全部，0=开始下载）：\n");
    printf("1.卡面(6尺寸)\t2.背景(普通/竖版/小横版)\t3.卡面Spina动画(beta)\n");
    printf("4.3D照片(L/S)\t5.语音\t6.Spine小人(beta)\t7.3D模型\t8.台词文本\n");
}

/* 按卡片构建并下载选中的资源（卡面/背景/卡面Spina动画/3d照片/语音/spine/3d模型/台词） */

static void dl_card_resources(sqlite3 *db, sqlite3 *rdb, int card_id, const char *cname,
                              int chara_id, int dress_id, const int *sel, int nsel){
    /* 角色目录: CGSS_DOWN\{id}{name} */
    char folder[512];
    snprintf(folder, sizeof folder, "%d%s", card_id, cname);
    wchar_t wroot[1024], wfolder[1024], wfoldername[512];
    get_dl_root(wroot, 1024);
    utf8_to_wide(folder, wfoldername, 512);
    swprintf(wfolder, 1024, L"%ls\\%ls", wroot, wfoldername);
    mkdirs(wfolder);

    ResItem items[64];
    int n = 0;
    char res[256];
    if (selected(sel, nsel, 1)){
        const char *sizes[6] = {"circle","sm","s","m","l","xl"};
        for (int i = 0; i < 6; i++){
            snprintf(res, sizeof res, "card_%d_%s.unity3d", card_id, sizes[i]);
            add_item(rdb, items, &n, res, L"卡面");
        }
    }
    if (selected(sel, nsel, 2)){
        snprintf(res, sizeof res, "card_bg_%d.unity3d", card_id);
        add_item(rdb, items, &n, res, L"背景");
        snprintf(res, sizeof res, "card_bg_%d_01.unity3d", card_id);
        add_item(rdb, items, &n, res, L"背景");
        snprintf(res, sizeof res, "card_bg_%d_s.unity3d", card_id);
        add_item(rdb, items, &n, res, L"背景");
    }
    if (selected(sel, nsel, 3)){
        snprintf(res, sizeof res, "card_cartoon_%d.unity3d", card_id);
        add_item(rdb, items, &n, res, L"卡面Spina动画");
    }
    if (selected(sel, nsel, 4)){
        snprintf(res, sizeof res, "idol_3d_%d_l.unity3d", card_id);
        add_item(rdb, items, &n, res, L"3d照片");
        snprintf(res, sizeof res, "idol_3d_%d_s.unity3d", card_id);
        add_item(rdb, items, &n, res, L"3d照片");
    }
    if (selected(sel, nsel, 5)){
        snprintf(res, sizeof res, "v/card_%d.acb", card_id);
        add_item(rdb, items, &n, res, L"语音");
    }
    if (selected(sel, nsel, 6)){
        snprintf(res, sizeof res, "card_spine_%d.unity3d", card_id);
        add_item(rdb, items, &n, res, L"spine");
        /* 共享小人骨架（SPSprachen），与卡面小人一起下载，解包时自动转 JSON */
        add_item(rdb, items, &n, "spine_sprachen_petit_chara_common.unity3d", L"spine");
    }
    if (selected(sel, nsel, 7)){
        if (dress_id > 0){
            snprintf(res, sizeof res, "3d_chara_body_%04d.unity3d", dress_id);
            add_item(rdb, items, &n, res, L"3d模型");
            /* 头部模型首选 _hq（含 M_Head/M_Cheek 网格和头部贴图），清单没有才用普通版 */
            snprintf(res, sizeof res, "3d_chara_head_%04d_%04d_hq.unity3d", chara_id, dress_id);
            if (get_hash(rdb, res, items[n].hash, 64) != 0)
                snprintf(res, sizeof res, "3d_chara_head_%04d_%04d.unity3d", chara_id, dress_id);
            add_item(rdb, items, &n, res, L"3d模型");
            snprintf(res, sizeof res, "3d_md_body%04d_hq.unity3d", dress_id);
            if (get_hash(rdb, res, items[n].hash, 64) != 0)
                snprintf(res, sizeof res, "3d_md_body%04d.unity3d", dress_id);
            add_item(rdb, items, &n, res, L"3d模型");
            const char *tx[3] = {"hq","multi","spec"};
            for (int i = 0; i < 3; i++){
                snprintf(res, sizeof res, "3d_tx_body%04d_%s.unity3d", dress_id, tx[i]);
                add_item(rdb, items, &n, res, L"3d模型");
            }
        } else {
            fprintf(stderr, "该卡没有专属服装，跳过3D模型\n");
        }
    }
    if (selected(sel, nsel, 8)){
        sqlite3_stmt *cstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT use_type, `index`, discription FROM card_comments WHERE id=? ORDER BY use_type, `index`",
                -1, &cstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(cstmt, 1, card_id);
            wchar_t wtextdir[1024], wtxt[1200];
            swprintf(wtextdir, 1024, L"%ls\\台词", wfolder);
            mkdirs(wtextdir);
            swprintf(wtxt, 1200, L"%ls\\card_%d_台词.txt", wtextdir, card_id);
            FILE *tf = _wfopen(wtxt, L"wb");
            if (tf){
                int nlines = 0;
                while (sqlite3_step(cstmt) == SQLITE_ROW){
                    fprintf(tf, "[%d-%d] %s\n",
                            sqlite3_column_int(cstmt, 0),
                            sqlite3_column_int(cstmt, 1),
                            (const char*)sqlite3_column_text(cstmt, 2));
                    nlines++;
                }
                fclose(tf);
                printf("台词已导出 %d 条 -> 台词\\card_%d_台词.txt\n", nlines, card_id);
            }
            sqlite3_finalize(cstmt);
        }
    }
    download_items(items, n, wfolder);
}

/* 按卡片 id 下载 */

static void dl_card(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    printf("请输入卡片id\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int card_id = atoi(buf);
    if (card_id <= 0){ fprintf(stderr, "输入错误\n"); return; }

    char cname[128];
    int chara_id = 0, dress_id = 0;
    if (query_card(db, card_id, cname, sizeof cname, &chara_id, &dress_id) != 0) return;
    printf("%d|%s\n", card_id, cname);

    print_card_res_menu();
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int sel[64], nsel = parse_multi(buf, sel, 8);
    if (nsel < 0){ nsel = 8; for (int i = 0; i < 8; i++) sel[i] = i + 1; }
    if (nsel == 0) return;
    dl_card_resources(db, rdb, card_id, cname, chara_id, dress_id, sel, nsel);
}

/* 按角色 chara_id 批量下载：列出角色所有卡，可多选/a 全部，资源类型只问一次 */

static void dl_chara(sqlite3 *db, sqlite3 *rdb){
    char buf[128];
    printf("请输入角色id（chara_id，输卡id也能自动识别）：\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int chara_id = atoi(buf);
    if (chara_id <= 0){ fprintf(stderr, "输入错误\n"); return; }

    /* 自动识别：输入卡id也能用——先看这个数是不是角色id（该角色有没有卡），
       没有的话再当卡id查一次，解析出真正的角色id */
    sqlite3_stmt *chk = NULL;
    int cnt = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM card_data WHERE chara_id=?", -1, &chk, NULL) == SQLITE_OK){
        sqlite3_bind_int(chk, 1, chara_id);
        if (sqlite3_step(chk) == SQLITE_ROW) cnt = sqlite3_column_int(chk, 0);
        sqlite3_finalize(chk);
    }
    if (cnt == 0){
        chk = NULL;
        if (sqlite3_prepare_v2(db, "SELECT chara_id,name FROM card_data WHERE id=?", -1, &chk, NULL) == SQLITE_OK){
            sqlite3_bind_int(chk, 1, chara_id);
            if (sqlite3_step(chk) == SQLITE_ROW){
                int real = sqlite3_column_int(chk, 0);
                printf("检测到 %d 是卡id -> 角色id %d（%s）\n", chara_id, real,
                       (const char*)sqlite3_column_text(chk, 1));
                chara_id = real;
            }
            sqlite3_finalize(chk);
        }
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,open_dress_id FROM card_data WHERE chara_id=? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, chara_id);
    int ids[128], ncards = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && ncards < 128){
        int cid = sqlite3_column_int(stmt, 0);
        ids[ncards++] = cid;
        printf("[%d] %d | %s | dress=%d\n", ncards, cid,
               (const char*)sqlite3_column_text(stmt, 1),
               sqlite3_column_int(stmt, 2));
    }
    sqlite3_finalize(stmt);
    if (ncards == 0){ fprintf(stderr, "该角色没有卡片\n"); return; }

    printf("选择要下载的卡（空格/逗号分隔数字，a=全部，0=返回）：");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int card_sel[128], ncard_sel = parse_multi(buf, card_sel, ncards);
    if (ncard_sel < 0){
        ncard_sel = ncards;
        for (int i = 0; i < ncards; i++) card_sel[i] = i + 1;
    }
    if (ncard_sel == 0) return;

    print_card_res_menu();
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int sel[64], nsel = parse_multi(buf, sel, 8);
    if (nsel < 0){ nsel = 8; for (int i = 0; i < 8; i++) sel[i] = i + 1; }
    if (nsel == 0) return;

    for (int s = 0; s < ncard_sel; s++){
        int card_id = ids[card_sel[s] - 1];
        char cname[128];
        int ch = 0, dress = 0;
        if (query_card(db, card_id, cname, sizeof cname, &ch, &dress) != 0) continue;
        printf("\n下载 %d|%s\n", card_id, cname);
        dl_card_resources(db, rdb, card_id, cname, ch, dress, sel, nsel);
    }
    printf("角色批量下载完成\n");
}

/* ================== 菜单2：歌曲资源下载 ================== */


static void dl_song(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    printf("请输入歌曲id\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int music_id = atoi(buf);
    if (music_id <= 0){ fprintf(stderr, "输入错误\n"); return; }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name FROM music_data WHERE id=?",
            -1, &stmt, NULL) != SQLITE_OK){
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, music_id);
    if (sqlite3_step(stmt) != SQLITE_ROW){
        fprintf(stderr, "没有相关歌曲\n");
        sqlite3_finalize(stmt);
        return;
    }
    int id = sqlite3_column_int(stmt, 0);
    char sname[128];
    snprintf(sname, sizeof sname, "%s", (const char*)sqlite3_column_text(stmt, 1));
    printf("%d|%s\n", id, sname);
    sqlite3_finalize(stmt);

    char folder[512];
    snprintf(folder, sizeof folder, "%d%s", id, sname);
    wchar_t wroot[1024], wfolder[1024], wfoldername[512];
    get_dl_root(wroot, 1024);
    utf8_to_wide(folder, wfoldername, 512);
    swprintf(wfolder, 1024, L"%ls\\%ls", wroot, wfoldername);
    mkdirs(wfolder);

    printf("可选资源（空格/逗号分隔数字，a=全部，0=开始下载）：\n");
    printf("1.音频(acb)\t2.封面(jacket)\t3.动作\n");
    printf("4.谱面\t5.舞台\t6.导演包(镜头/表情/阵型)\t7.全部\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int sel[64], nsel = parse_multi(buf, sel, 7);
    if (nsel < 0){ nsel = 7; for (int i = 0; i < 7; i++) sel[i] = i + 1; }
    if (nsel == 0) return;

    ResItem items[64];
    int n = 0;
    char res[256];
    if (selected(sel, nsel, 1)){
        snprintf(res, sizeof res, "l/song_%d.acb", id);
        add_item(rdb, items, &n, res, L"acb文件");
    }
    if (selected(sel, nsel, 2)){
        /* 封面 = jacket_{jacket_id}，jacket_id 从 live_data 查 */
        sqlite3_stmt *jstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT jacket_id FROM live_data WHERE music_data_id=? AND jacket_id > 0 LIMIT 1",
                -1, &jstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(jstmt, 1, id);
            if (sqlite3_step(jstmt) == SQLITE_ROW){
                snprintf(res, sizeof res, "jacket_%d.unity3d", sqlite3_column_int(jstmt, 0));
                add_item(rdb, items, &n, res, L"封面");
            }
            sqlite3_finalize(jstmt);
        }
    }
    if (selected(sel, nsel, 3)){
        char like[64];
        snprintf(like, sizeof like, "3d_cutt_an_chr_son%d%%", id);
        sqlite3_stmt *mstmt = NULL;
        if (sqlite3_prepare_v2(rdb,
                "SELECT name,hash FROM manifests WHERE name LIKE ? ORDER BY name",
                -1, &mstmt, NULL) == SQLITE_OK){
            sqlite3_bind_text(mstmt, 1, like, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(mstmt) == SQLITE_ROW && n < 64){
                snprintf(items[n].name, sizeof items[n].name, "%s",
                         (const char*)sqlite3_column_text(mstmt, 0));
                snprintf(items[n].hash, sizeof items[n].hash, "%s",
                         (const char*)sqlite3_column_text(mstmt, 1));
                wcscpy(items[n].sub, L"动作");
                n++;
            }
            sqlite3_finalize(mstmt);
        }
    }
    if (selected(sel, nsel, 4) || selected(sel, nsel, 5)){
        sqlite3_stmt *lstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id, live_bg FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) == SQLITE_OK){
            sqlite3_bind_int(lstmt, 1, id);
            while (sqlite3_step(lstmt) == SQLITE_ROW && n < 64){
                int live_id = sqlite3_column_int(lstmt, 0);
                int live_bg = sqlite3_column_int(lstmt, 1);
                if (selected(sel, nsel, 4)){
                    snprintf(res, sizeof res, "musicscores_m%d.bdb", live_id);
                    add_item(rdb, items, &n, res, L"谱面");
                }
                if (selected(sel, nsel, 5) && live_bg > 0){
                    snprintf(res, sizeof res, "3d_stage_%d.unity3d", live_bg);
                    add_item(rdb, items, &n, res, L"舞台");
                    snprintf(res, sizeof res, "3d_stage_%d_hq.unity3d", live_bg);
                    add_item(rdb, items, &n, res, L"舞台");
                }
            }
            sqlite3_finalize(lstmt);
        }
    }
    if (selected(sel, nsel, 6)){
        char kw[128] = "";
        printf("输入导演包关键字（如 koicover，回车列出全部）：");
        if (fgets(kw, sizeof kw, stdin)) kw[strcspn(kw, "\r\n")] = 0;
        char like[256];
        if (kw[0]) snprintf(like, sizeof like, "3d_cutt_%s%%", kw);
        else snprintf(like, sizeof like, "3d_cutt_%%");
        sqlite3_stmt *mstmt = NULL;
        if (sqlite3_prepare_v2(rdb,
                "SELECT name,hash FROM manifests WHERE name LIKE ? AND name NOT LIKE '3d_cutt_an_chr%' ORDER BY name",
                -1, &mstmt, NULL) == SQLITE_OK){
            sqlite3_bind_text(mstmt, 1, like, -1, SQLITE_TRANSIENT);
            ResItem tmp[64];
            int tn = 0;
            while (sqlite3_step(mstmt) == SQLITE_ROW && tn < 64){
                snprintf(tmp[tn].name, sizeof tmp[tn].name, "%s", (const char*)sqlite3_column_text(mstmt, 0));
                snprintf(tmp[tn].hash, sizeof tmp[tn].hash, "%s", (const char*)sqlite3_column_text(mstmt, 1));
                wcscpy(tmp[tn].sub, L"导演包");
                tn++;
            }
            sqlite3_finalize(mstmt);
            if (tn == 0){
                printf("没有匹配的导演包\n");
            } else if (tn > 40){
                printf("匹配 %d 个太多，请输更具体关键字\n", tn);
            } else {
                printf("匹配 %d 个：\n", tn);
                for (int i = 0; i < tn; i++) printf("[%d] %s\n", i + 1, tmp[i].name);
                printf("选择（空格分隔数字，a=全部，0=跳过）：");
                fgets(buf, sizeof buf, stdin);
                int sel2[64], n2 = parse_multi(buf, sel2, tn);
                if (n2 < 0){ n2 = tn; for (int i = 0; i < tn; i++) sel2[i] = i + 1; }
                for (int i = 0; i < n2 && n < 64; i++){
                    items[n] = tmp[sel2[i] - 1];
                    n++;
                }
            }
        }
    }
    download_items(items, n, wfolder);
}

/* ================== 菜单2主入口 ================== */


int dl_main(void){
    sqlite3 *db = NULL, *rdb = NULL;
    if (GetFileAttributesA(DB_PATH) == INVALID_FILE_ATTRIBUTES){
        fprintf(stderr, "缺少 master.mdb，请把它放到程序同目录\n");
        return -1;
    }
    if (GetFileAttributesA(MANIFEST_PATH) == INVALID_FILE_ATTRIBUTES){
        fprintf(stderr, "缺少 manifest_10133800.db，请把它放到程序同目录\n");
        return -1;
    }
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK){
        fprintf(stderr, "打开 master.mdb 失败（%s）\n", sqlite3_errmsg(db));
        return -1;
    }
    if (sqlite3_open(MANIFEST_PATH, &rdb) != SQLITE_OK){
        fprintf(stderr, "打开 manifest_10133800.db 失败（%s）\n", sqlite3_errmsg(rdb));
        sqlite3_close(db);
        return -1;
    }
    char buf[128];
    while (1){
        printf("下载类型 1.卡片资源\t2.歌曲资源\t3.按角色批量\t4.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) break;
        int opt = atoi(buf);
        switch (opt){
        case 1: dl_card(db, rdb); break;
        case 2: dl_song(db, rdb); break;
        case 3: dl_chara(db, rdb); break;
        case 4:
            printf("返回中...\n");
            sqlite3_close(rdb);
            sqlite3_close(db);
            return 1;
        default:
            fprintf(stderr, "输入错误\n");
            break;
        }
    }
    sqlite3_close(rdb);
    sqlite3_close(db);
    return 0;
}
/* ================== 菜单6：ACB音乐提取和HCA解码 ================== */

typedef struct {
    wchar_t folder[512];
    wchar_t acbdir[512];
    wchar_t acb[512];
    char acb_name[256];
    char folder_name[256];
} AcbItem;


/* ??? .acb?dir ?????chara_folder ????? */
