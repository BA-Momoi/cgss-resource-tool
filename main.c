#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "unpack.h"
#include "preview.h"
#include "paper.h"
#include "browse.h"
#include "cg.h"
#include "auto_updata.h"
#define Version 1.6
#define BUILD_VERIANT "db"

int main(void){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   // 强制 UTF-8 控制台，输入输出统一
    enable_vt();
    int rc = updata_main(Version);
    if(rc == -1){
        
        printf("更新失败，请关闭程序重试\n");
        fflush(stdout);
        system("pause");
    }
    if(rc == 2){
    /* 更新脚本已经在后台/新窗口跑起来了,主程序必须立刻退出 */
    printf("正在更新,程序即将关闭...\n");
    fflush(stdout);
    return 0;
    }
                       // 打开 ANSI 转义(清屏/反色), 否则界面是乱码
    /* 查找和下载已整合成一个模块(browse.c), 主菜单只留一个入口 */
    def menu[] = {
        {"1.资源查找与下载", browse_main, 0},
        {"2.解包", unpack_main, 0},
        {"3.打开Spine预览(beta)", open_spine_preview, 0},
        {"4.USM/CG解包", unpack_usm, 0},
        {"5.退出", NULL, 0},
        {"END", NULL, 0}            /* 哨兵必须最后一行 */
    };

    while(1){
        int rc = pager_pick("主菜单", menu, 0);
        if(rc == -1)                /* Esc: 继续显示菜单 */
            continue;
        if(rc == 4)                 /* "5.退出" 是第 4 项(下标从 0 数) */
            break;
        /* 选中项的 func 已经在 pager 里调用过了, 这里直接循环 */
    }

    fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
    system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
    return 0;
}
