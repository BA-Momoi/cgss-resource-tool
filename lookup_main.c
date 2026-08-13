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
#include "util.h"

#define DB_PATH "master.mdb"

// ?????
int lookup_main(void){
    sqlite3 *db = NULL;    // 游戏主库
    sqlite3 *rdb = NULL;   // 资源清单库
    const char *mp = find_manifest();
    if (GetFileAttributesA(DB_PATH) == INVALID_FILE_ATTRIBUTES){
        fprintf(stderr, "缺少 master.mdb，请把它放到程序同目录\n");
        return -1;
    }
    if (!mp){
        fprintf(stderr, "缺少 manifest_*.db（资源清单库），请先运行 check_update.exe 获取\n");
        return -1;
    }
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "打开 master.mdb 失败（%s）\n", sqlite3_errmsg(db));
        return -1;
    }
    if (sqlite3_open(mp, &rdb) != SQLITE_OK) {
        fprintf(stderr, "打开 %s 失败（%s）\n", mp, sqlite3_errmsg(rdb));
        sqlite3_close(db);
        return -1;
    }
    char buf[128];
    while (1) {
        printf("\f");
        printf("1.3D模型\t2.2DSpine小人(beta)\t3.歌曲\n");
        printf("4.歌曲动作\t5.谱面\t6.3D舞台\n");
        printf("7.卡面\t8.角色语言（文本/音频）\n");
        printf("9.BGM(beta)\t10.周年CG\n")；
        printf("0.返回上一级\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) break;   // EOF 退出
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            if (-1 == td_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }   // 内部循环，直到选"4.返回"
            break;
        case 2:
            if (-1 == spina_Search(db,rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 3:
            if (-1 == song_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 7:
            if (-1 == cardimg_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 4:
            if (-1 == action_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 5:
            if (-1 == chart_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 6:
            if (-1 == stage_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 8:
            if (-1 == voice_Search(db, rdb)) {
                fprintf(stderr, "输入错误\n");
            }
            break;
        case 9:
            printf("尚未完成\n");
            break;
        case 10:
            printf("尚未完成\n");
            break;
        case 0:
            printf("返回中...\n");
            sqlite3_close(rdb);
            sqlite3_close(db);
            return 1;   // 回到 main 初始菜单
        default:
            fprintf(stderr, "输入错误或暂未开发\n");
            break;
        }
    }
    sqlite3_close(rdb);
    sqlite3_close(db);
    return 0;
}

