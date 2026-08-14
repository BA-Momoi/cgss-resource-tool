#ifndef _CGSS_CG_H
#define _CGSS_CG_H
#include <windows.h>

/* 主菜单: USM/CG 解包(自定义文件/目录 + 已下载CG) */
int unpack_usm(void);

/* 解包一个目录里的所有 usm(递归), 并解配对的 acb(给 browse.c 下载完后调用) */
int unpack_cg_folder(const wchar_t *dir);

#endif
