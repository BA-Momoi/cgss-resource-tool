#include"sqlite3.h"
#include"data.h"
#ifndef _LOOKUP_TABLE_H
#define _LOOKUP_TABLE_H

void print_res_hash(sqlite3 *rdb, const char *res_name);
void queryaction_by_name(sqlite3 *db);
void querycardimg_by_chara(sqlite3 *db);
void querycardimg_by_name(sqlite3 *db);
int td_Search(sqlite3 *db, sqlite3 *rdb);
int spina_Search(sqlite3 *db, sqlite3 *rdb);
int song_Search(sqlite3 *db, sqlite3 *rdb);
int cardimg_Search(sqlite3 *db, sqlite3 *rdb);
int action_Search(sqlite3 *db, sqlite3 *rdb);
int chart_Search(sqlite3 *db, sqlite3 *rdb);
int voice_Search(sqlite3 *db, sqlite3 *rdb);
int stage_Search(sqlite3 *db, sqlite3 *rdb);
int lookup_main(void);

#endif
