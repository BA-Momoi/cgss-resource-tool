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
#include "stage_map.h"

// 3D ????
/* ================== 6. 3D舞台 ================== */

/* Resolve live_data.id through Master3dLive.id, then use Master3dLive.bg. */
static void querystage_by_music_id(sqlite3 *db, sqlite3 *rdb){
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
                "SELECT id, live_bg FROM live_data WHERE music_data_id=? ORDER BY id",
                -1, &lstmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            return;
        }
        sqlite3_bind_int(lstmt, 1, music_id);
        int n = 0;
        while (sqlite3_step(lstmt) == SQLITE_ROW) {
            int live_id = sqlite3_column_int(lstmt, 0);
            int live_bg = sqlite3_column_int(lstmt, 1);
            int stage_bg = 0;
            printf("live %d | live_bg:%d | ", live_id, live_bg);
            if (!stage_bg_for_live(live_id, &stage_bg)){
                printf("Master3dLive 中没有对应的 3D 舞台映射\n");
                n++;
                continue;
            }
            printf("Master3dLive.bg:%d\n", stage_bg);
            static const struct {
                const char *format;
                const char *label;
            } packages[] = {
                {"3d_stage_%04d.unity3d", "舞台"},
                {"3d_stage_%04d_hq.unity3d", "舞台HQ"},
                {"3d_stage_%04d_variable.unity3d", "舞台变量资源"},
                {"3d_stage_%04d_variable_hq.unity3d", "舞台变量资源HQ"},
                {"3d_stage_%04d_variable_low.unity3d", "舞台变量资源Low"}
            };
            char res[256];
            for (size_t i = 0; i < sizeof packages / sizeof packages[0]; i++){
                snprintf(res, sizeof res, packages[i].format, stage_bg);
                printf("%s:%s\t", packages[i].label, res);
                print_res_hash(rdb, res);
                printf("\n");
            }
            n++;
        }
        sqlite3_finalize(lstmt);
        if (n == 0)
            fprintf(stderr, "没有相关 live\n");
        return;
    }
}

/* 舞台四级菜单：1.输入歌曲id查找舞台 2.返回 */
static void cardstage_id_menu(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("1.输入歌曲id查找舞台\t2.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int opt = atoi(buf);
        if (opt == 1) {
            querystage_by_music_id(db, rdb);
            return;
        }
        if (opt == 2) return;
        fprintf(stderr, "输入错误\n");
    }
}

/* 舞台查询菜单：选完一项自动回到本菜单，选 3 返回一级菜单 */
int stage_Search(sqlite3 *db, sqlite3 *rdb){
    _setmode(_fileno(stdin), _O_BINARY);   // stdin 二进制模式，换行自己处理
    char buf[128];
    while (1) {
        printf("输入 1.歌曲名\t2.歌曲id\t3.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return -1;
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            queryaction_by_name(db);   // 复用歌名列表
            cardstage_id_menu(db, rdb);
            break;
        case 2:
            querystage_by_music_id(db, rdb);
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
/* 一级查找菜单 */
