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

// ????
/* ================== 5. 谱面 ================== */

/* 按歌曲 id 查谱面（live_data → musicscores_m{live_id}.bdb） */
static void querychart_by_music_id(sqlite3 *db, sqlite3 *rdb){
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

        sqlite3_stmt *lstmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,difficulty_1,difficulty_2,difficulty_3,difficulty_4 FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            return;
        }
        sqlite3_bind_int(lstmt, 1, music_id);
        int n = 0;
        while (sqlite3_step(lstmt) == SQLITE_ROW) {
            int live_id = sqlite3_column_int(lstmt, 0);
            printf("live %d | diff:%d/%d/%d/%d\n", live_id,
                   sqlite3_column_int(lstmt, 1),
                   sqlite3_column_int(lstmt, 2),
                   sqlite3_column_int(lstmt, 3),
                   sqlite3_column_int(lstmt, 4));
            if (sqlite3_column_int(lstmt, 1) == 0) {
                printf("（MV/活动变体，无谱面）\n");
                continue;
            }
            char res[256];
            snprintf(res, sizeof res, "musicscores_m%d.bdb", live_id);
            printf("谱面:%s\t", res); print_res_hash(rdb, res); printf("\n");
            n++;
        }
        sqlite3_finalize(lstmt);
        if (n == 0)
            fprintf(stderr, "没有相关 live\n");
        return;
    }
}

/* 谱面四级菜单：1.输入歌曲id查找谱面 2.返回 */
static void cardchart_id_menu(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("1.输入歌曲id查找谱面\t2.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int opt = atoi(buf);
        if (opt == 1) {
            querychart_by_music_id(db, rdb);
            return;
        }
        if (opt == 2) return;
        fprintf(stderr, "输入错误\n");
    }
}

/* 谱面查询菜单：选完一项自动回到本菜单，选 3 返回一级菜单 */
int chart_Search(sqlite3 *db, sqlite3 *rdb){
    _setmode(_fileno(stdin), _O_BINARY);   // stdin 二进制模式，换行自己处理
    char buf[128];
    while (1) {
        printf("输入 1.歌曲名\t2.歌曲id\t3.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return -1;
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            queryaction_by_name(db);   // 复用歌名列表
            cardchart_id_menu(db, rdb);
            break;
        case 2:
            querychart_by_music_id(db, rdb);
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

