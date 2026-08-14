#ifndef _PAPER_H_
#define _PAPER_H_

typedef struct TypeDef
{
    char name[128];
    int (*func)(void);        /* 你的菜单函数都是 int xxx(void), 类型要对上 */
    int state;
}def;

void enable_vt(void);          /* 名字要和 paper.c 里定义的一模一样 */

int console_rows(void);

int pager_pick(const char *title, def *Def, int multi);

#endif
