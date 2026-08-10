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

// 3D ??????
static void md_body_name(sqlite3 *rdb, int dress_id, char *out, int n){
    sqlite3_stmt *stmt = NULL;
    snprintf(out, n, "3d_md_body%04d_hq.unity3d", dress_id);
    if (sqlite3_prepare_v2(rdb, "SELECT 1 FROM manifests WHERE name=?", -1, &stmt, NULL) == SQLITE_OK){
        sqlite3_bind_text(stmt, 1, out, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_ROW)
            snprintf(out, n, "3d_md_body%04d.unity3d", dress_id);
        sqlite3_finalize(stmt);
    }
}

/* 3d_chara_head_{chara_id}_{dress_id} 同样首选 _hq（含头部网格和贴图），清单没有才用普通版 */
static void head_name(sqlite3 *rdb, int chara_id, int dress_id, char *out, int n){
    sqlite3_stmt *stmt = NULL;
    snprintf(out, n, "3d_chara_head_%04d_%04d_hq.unity3d", chara_id, dress_id);
    if (sqlite3_prepare_v2(rdb, "SELECT 1 FROM manifests WHERE name=?", -1, &stmt, NULL) == SQLITE_OK){
        sqlite3_bind_text(stmt, 1, out, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_ROW)
            snprintf(out, n, "3d_chara_head_%04d_%04d.unity3d", chara_id, dress_id);
        sqlite3_finalize(stmt);
    }
}

/* 打印一张卡的模型资源名（物理/头部/身体/贴图），每条附带 hash
   card_id 卡片id  name 卡片名  chara_id 角色id  dress_id 服装id */
static void print_model_names(sqlite3 *rdb, int card_id, const char *name, int chara_id, int dress_id){
    char res[256];
    printf("%d|%s模型名称\n", card_id, name);
    snprintf(res, sizeof res, "3d_chara_body_%04d.unity3d", dress_id);
    printf("物理：%s", res); print_res_hash(rdb, res); printf("\n");
    head_name(rdb, chara_id, dress_id, res, sizeof res);
    printf("头部：%s", res); print_res_hash(rdb, res); printf("\n");
    md_body_name(rdb, dress_id, res, sizeof res);
    printf("身体：%s", res); print_res_hash(rdb, res); printf("\n");
    const char *tx[3] = {"hq","multi","spec"};
    for(int i = 0; i < 3; i++){
        snprintf(res, sizeof res, "3d_tx_body%04d_%s.unity3d", dress_id, tx[i]);
        printf("贴图：%s", res); print_res_hash(rdb, res); printf("\n");
    }
}

/* 按卡片 id 查一张卡并打印模型资源
   只有输入非法才重新问，查完一次就返回（回上级菜单） */
static void query3d_model_by_card_id(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("请输入id：\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int id = atoi(buf);
        if (id <= 0) {
            fprintf(stderr, "输入错误\n");
            continue;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id,name,chara_id,open_dress_id FROM card_data WHERE id=?",
                -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
            return;
        }
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int dress_id = sqlite3_column_int(stmt, 3);
            if (dress_id == 0) {
                fprintf(stderr, "没有专属服装\n");
            } else {
                print_model_names(rdb,
                                  sqlite3_column_int(stmt, 0),
                                  (const char*)sqlite3_column_text(stmt, 1),
                                  sqlite3_column_int(stmt, 2), dress_id);
            }
        } else {
            fprintf(stderr, "没有相关模型\n");
        }
        sqlite3_finalize(stmt);
        return;
    }
}

/* 四级菜单：1.输入id查找模型 2.返回。选完就回三级菜单 */
static void card3d_id_menu(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    while (1) {
        printf("1.输入id查找模型\t2.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return;
        int opt = atoi(buf);
        if (opt == 1) {
            query3d_model_by_card_id(db, rdb);
            return;
        }
        if (opt == 2) return;
        fprintf(stderr, "输入错误\n");
    }
}

/* 按卡片名称模糊查询并列出 */
static void query3d_by_name(sqlite3 *db){
    char buf[128];
    printf("请输入名称（日文名）：");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    buf[strcspn(buf, "\r\n")] = 0;
    char name_utf8[256];
    gbk_to_utf8(buf, name_utf8, sizeof(name_utf8));
    char like[160];
    snprintf(like, sizeof(like), "%%%s%%", name_utf8);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,rarity,open_dress_id FROM card_data WHERE name LIKE ? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%d | %s | rarity（稀有度）=%d | dress（服饰id）=%d\n",
               sqlite3_column_int(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_int(stmt, 2),
               sqlite3_column_int(stmt, 3));
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0) fprintf(stderr, "没有找到相关数据\n");
}

/* 按角色 chara_id 查询并列出 */
static void query3d_by_chara(sqlite3 *db){
    char buf[64];
    printf("请输入角色id(chara_id)\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int chara_id = atoi(buf);
    if (chara_id <= 0) {
        fprintf(stderr, "输入错误\n");
        return;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,rarity,open_dress_id FROM card_data WHERE chara_id=? ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, chara_id);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%d | %s | rarity（稀有度）=%d | dress（服饰id）=%d\n",
               sqlite3_column_int(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_int(stmt, 2),
               sqlite3_column_int(stmt, 3));
        n++;
    }
    sqlite3_finalize(stmt);
    if (n == 0) fprintf(stderr, "输入id错误，查找为0\n");
}

/* 按服装 dress_id 查询并直接打印模型资源 */
static void query3d_by_dress(sqlite3 *db, sqlite3 *rdb){
    char buf[64];
    printf("请输入open_dress_id(服饰id)：\n");
    if (fgets(buf, sizeof buf, stdin) == NULL) return;
    int dress_id = atoi(buf);
    if (dress_id <= 0) {
        fprintf(stderr, "输入错误\n");
        return;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,name,chara_id,open_dress_id FROM card_data WHERE open_dress_id=?",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, dress_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        print_model_names(rdb,
                          sqlite3_column_int(stmt, 0),
                          (const char*)sqlite3_column_text(stmt, 1),
                          sqlite3_column_int(stmt, 2),
                          sqlite3_column_int(stmt, 3));
    } else {
        fprintf(stderr, "没有相关模型\n");
    }
    sqlite3_finalize(stmt);
}

/* 3D 模型查询菜单：选完一项自动回到本菜单，选 4 返回一级菜单 */
int td_Search(sqlite3 *db, sqlite3 *rdb){
    _setmode(_fileno(stdin), _O_BINARY);   // stdin 二进制模式，换行自己处理
    char buf[128];
    while (1) {
        printf("输入 1.卡片名称\t2.角色chara_id（角色id）\t3.角色dress_id\t4.返回\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) return -1;
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            query3d_by_name(db);
            card3d_id_menu(db, rdb);
            break;
        case 2:
            query3d_by_chara(db);
            card3d_id_menu(db, rdb);
            break;
        case 3:
            query3d_by_dress(db, rdb);
            break;
        case 4:
            printf("返回中...\n");
            return 1;   // 告诉 lookup_main 回到一级菜单
        default:
            fprintf(stderr, "输入错误\n");
            break;
        }
    }
}

