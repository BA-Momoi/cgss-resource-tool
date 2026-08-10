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

// lookup_common.c: ??????
void print_res_hash(sqlite3 *rdb, const char *res_name){
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(rdb,
            "SELECT hash FROM manifests WHERE name=?",
            -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL错误: %s\n", sqlite3_errmsg(rdb));
        return;
    }
    sqlite3_bind_text(stmt, 1, res_name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("  hash=%s", (const char*)sqlite3_column_text(stmt, 0));
    } else {
        printf("  清单中未找到");
    }
    sqlite3_finalize(stmt);
}

