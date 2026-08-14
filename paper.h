#ifndef _PAPER_H_
#define _PAPER_H_

typedef struct TypeDef
{
    char name[128];
    void (*func)(void);
    int state;
}def;

void enble_vt(void);

int console_row(void);

int pager_pink(char *,def *,int );

#endif