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

// 2D Spine ????
/* ================== 2D Spine 小人 ================== */

/* 按卡片 id 查一张卡并打印 Spina 资源（card_spine_{卡片id}.unity3d） */
static void queryspina_by_card_id(sqlite3 *db,sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("请输入id\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int card_id = atoi(buf);
        if (card_id <= 0) {
            fprintf(stderr, "输入错误\n");
            continue;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,name FROM card_data WHERE id=?",
                -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            continue;
        }
        sqlite3_bind_int(stmt, 1, card_id);
        char res[256];
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("%d|%s\n", sqlite3_column_int(stmt, 0), sqlite3_column_text(stmt, 1));
            snprintf(res,sizeof res,"card_spine_%d.unity3d",sqlite3_column_int(stmt,0));
            printf("Spine:%s\t",res);print_res_hash(rdb,res); printf("\n");
            snprintf(res, sizeof res, "card_live_%d.unity3d", sqlite3_column_int(stmt, 0));
            printf("Spine_Live:%s\t", res); print_res_hash(rdb, res); printf("\n");
            
        } else {
            fprintf(stderr, "没有相关卡片\n");
        }
        sqlite3_finalize(stmt);
        return;
    }
}

/* Spina 查找第四级菜单：1.输入id查找Spina资源 2.返回 */
static void cardspina_id_menu(sqlite3 *db,sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("1.输入id查找Spine资源\t2.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int opt = atoi(buf);
        if (opt == 1) {
            queryspina_by_card_id(db,rdb);
            return;
        }
        if (opt == 2) return;
        fprintf(stderr, "输入错误\n");
    }
}

/* Spina 小人名字模糊查询并列出 */
static void queryspina_by_name(sqlite3 *db){
    char buf[128];
    printf("请输入角色名（日文名）：\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    buf[strcspn(buf, "\r\n")] = 0;
    char name_utf8[128];
    gbk_to_utf8(buf, name_utf8, sizeof name_utf8);
    char like[256];
    snprintf(like, sizeof like, "%%%s%%", name_utf8);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,open_dress_id FROM card_data WHERE name LIKE ? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误:%s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    int n = 0;  //记录输出次数
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("| id = %d | name = %s | dress = %d |\n",
               sqlite3_column_int(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_int(stmt, 2));
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0)
        fprintf(stderr, "没有找到相关资源\n");
}

/* Spina 按角色 chara_id 查询并列出 */
static void queryspina_by_chara(sqlite3 *db){
    char buf[64];
    while (1)
    {
            printf("请输入角色id（chara_id）：\n");
         if (fgets(buf, sizeof buf, stdin) == NULL) return;
         int chara_id = atoi(buf);
         if (chara_id <= 0) {
            fprintf(stderr, "输入错误\n");
            continue;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,name,open_dress_id FROM card_data WHERE chara_id=? ORDER BY id",
                -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            return;
        }
        sqlite3_bind_int(stmt, 1, chara_id);
        int n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("| id = %d | name = %s | dress = %d |\n",
                   sqlite3_column_int(stmt, 0),
                   sqlite3_column_text(stmt, 1),
                   sqlite3_column_int(stmt, 2));
            n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0)
        fprintf(stderr, "没有找到相关资源\n");
    }
}

/* Spina 按服装 dress_id 查询并列出 */
static void queryspina_by_dress(sqlite3 *db){
    char buf[64];
    printf("请输入open_dress_id（服饰id）：\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int dress_id = atoi(buf);
    if (dress_id <= 0) {
        fprintf(stderr, "输入错误\n");
        return;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,open_dress_id FROM card_data WHERE open_dress_id=? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, dress_id);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("| id = %d | name = %s | dress = %d |\n",
               sqlite3_column_int(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_int(stmt, 2));
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0)
        fprintf(stderr, "没有找到相关资源\n");
}

/* Spine 模型查询菜单：选完一项自动回到本菜单，选 4 返回一级菜单 */
int spina_Search(sqlite3 *db,sqlite3 *rdb){
    _setmode(_fileno(stdin), _O_BINARY);   // stdin 二进制模式，换行自己处理
    char buf[128];
    while (1) {
        printf("输入 1.卡片名称\t2.角色id（chara_id）\t3.角色dress_id\t4.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return -1;
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            queryspina_by_name(db);
            cardspina_id_menu(db,rdb);
            break;
        case 2:
            queryspina_by_chara(db);
            cardspina_id_menu(db,rdb);
            break;
        case 3:
            queryspina_by_dress(db);
            cardspina_id_menu(db,rdb);
            break;
        case 4:
            printf("返回中...\n");
            return 1;   // 告诉 lookup_main 回到一级菜单
        default:
            fprintf(stderr, "输入错误\n");
            break;
        }
    }
    return 0;
}


