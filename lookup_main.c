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
#include "paper.h"

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
    dbdef menu[]={
        {"1.3D模型",td_Search,0},
        {"2.2DSpine小人(beta)",spina_Search,0},
        {"3.歌曲",song_Search,0},
        {"4.歌曲动作",action_Search,0},
        {"5.铺面",chart_Search,0},
        {"6.3D舞台",stage_Search,0},
        {"7.卡面",cardimg_Search,0},
        {"8.角色语音（加文本）",voice_Search,0},
        {"9.BGM(beta)",NULL,0},
        {"10.CG(beta)",NULL,0},
        {"11.退出",NULL,0},
        {"END",NULL,0}          /* 哨兵必须有, pager 靠它数有几项 */
    };
    while (1) {
        int rc =pager_picks("搜索",menu,db,rdb,0);
        if(rc == -1)
            continue;
        else if(rc == 10)       /* "11.退出" 是第 10 项(下标从0数) */
            break;
    }
    sqlite3_close(rdb);
    sqlite3_close(db);
    return 0;
}

