// lookup ??????
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#include "data.h"
#include "sqlite3.h"
#include "lookup_table.h"
#include "GBKswapUTF8.h"

// ??????
/* ================== 4. 歌曲动作 ================== */

/* 按歌曲 id 查动作资源（清单前缀 3d_cutt_an_chr_son{music}%） */
static void queryaction_by_music_id(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("请输入歌曲id\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int music_id = atoi(buf);
        if (music_id <= 0) {
            fprintf(stderr, "输入错误\n");
            continue;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,name FROM music_data WHERE id=?",
                -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            continue;
        }
        sqlite3_bind_int(stmt, 1, music_id);
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            fprintf(stderr, "没有相关歌曲\n");
            sqlite3_finalize(stmt);
            return;
        }
        printf("%d|%s\n", sqlite3_column_int(stmt, 0), sqlite3_column_text(stmt, 1));
        sqlite3_finalize(stmt);

        char like[64];
        snprintf(like, sizeof like, "3d_cutt_an_chr_son%d%%", music_id);
        sqlite3_stmt *mstmt = NULL;
        if (sqlite3_prepare_v2(rdb,
                "SELECT name FROM manifests WHERE name LIKE ? ORDER BY name",
                -1, &mstmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
            return;
        }
        sqlite3_bind_text(mstmt, 1, like, -1, SQLITE_TRANSIENT);
        int n = 0;
        while (sqlite3_step(mstmt) == SQLITE_ROW) {
            const char *res = (const char*)sqlite3_column_text(mstmt, 0);
            printf("动作:%s\t", res); print_res_hash(rdb, res); printf("\n");
            n++;
        }
        sqlite3_finalize(mstmt);
        if (n == 0)
            fprintf(stderr, "清单中未找到该曲动作\n");
        return;
    }
}

/* 动作四级菜单：1.输入歌曲id查找动作 2.返回 */
static void cardaction_id_menu(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("1.输入歌曲id查找动作\t2.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int opt = atoi(buf);
        if (opt == 1) {
            queryaction_by_music_id(db, rdb);
            return;
        }
        if (opt == 2) return;
        fprintf(stderr, "输入错误\n");
    }
}

/* 按歌名模糊查询并列出 */
void queryaction_by_name(sqlite3 *db){
    char buf[128];
    printf("请输入歌曲名：\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    buf[strcspn(buf, "\r\n")] = 0;
    char name_utf8[128];
    gbk_to_utf8(buf, name_utf8, sizeof name_utf8);
    char like[256];
    snprintf(like, sizeof like, "%%%s%%", name_utf8);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,bpm,composer FROM music_data WHERE name LIKE ? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误:%s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("| id = %d | name = %s | BPM = %d | 作曲 = %s |\n",
               sqlite3_column_int(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_int(stmt, 2),
               sqlite3_column_text(stmt, 3));
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0)
        fprintf(stderr, "没有找到相关歌曲\n");
}

/* 动作查询菜单：选完一项自动回到本菜单，选 3 返回一级菜单 */
int action_Search(sqlite3 *db, sqlite3 *rdb){
    _setmode(_fileno(stdin), _O_BINARY);   // stdin 二进制模式，换行自己处理
    char buf[128];
    while (1) {
        printf("输入 1.歌曲名\t2.歌曲id\t3.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return -1;
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            queryaction_by_name(db);
            cardaction_id_menu(db, rdb);
            break;
        case 2:
            queryaction_by_music_id(db, rdb);
            break;
        case 3:
            printf("返回中...\n");
            return 1;   // 告诉 lookup_main 回到一级菜单
        default:
            fprintf(stderr, "输入错误\n");
            break;
        }
    }
}

