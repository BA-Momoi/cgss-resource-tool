#ifndef _PAPER_H_
#define _PAPER_H_
#include "sqlite3.h"
typedef struct Typedef
{
    char name[128];
    int (*func)(void);        
    int state;
}def;

typedef struct TypeDef
{
    char name[128];
    int (*func)(sqlite3 *,sqlite3 *);
    int state;
}dbdef;

typedef struct TypeDef_Num
{
    double version;
    int state;
}versiondef;

void enable_vt(void);          /* 名字要和 paper.c 里定义的一模一样 */

int console_rows(void);

int pager_pick(const char *title, def *Def, int multi);

int pager_picks(const char *title,dbdef *Def,sqlite3 *db,sqlite3 *rdb,int multi);

int pager_pick_version(const char *title,versiondef *choice,double version,int multi);

#endif
