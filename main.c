#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "lookup_table.h"
#include "download.h"
#include "unpack.h"
#include "preview.h"
#include "paper.h"

/* ---- 还没实现的功能: 先放占位, 以后接了真功能再替换 ---- */
int unpack_usm(void){
    printf("USM 解包功能还没接进来\n");
    return 1;
}
int dl_custom(void){
    printf("自定义搜索下载功能还没接进来\n");
    return 1;
}

int main(void){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   // 强制 UTF-8 控制台，输入输出统一
    enable_vt();                   // 打开 ANSI 转义(清屏/反色), 否则界面是乱码

    /* 哨兵必须是 "END"(和 paper.c 里 strcmp 找的一致), 且放在最后 */
    def menu[] = {
        {"1.数据表查找数据", lookup_main, 0},
        {"2.数据下载并解析", dl_main, 0},
        {"3.解包", unpack_main, 0},
        {"4.打开Spine预览(beta)", open_spine_preview, 0},
        {"5.usm文件解包", unpack_usm, 0},
        {"6.自定义搜索并下载", dl_custom, 0},
        {"7.退出", NULL, 0},
        {"END", NULL, 0}            /* 小写 end 会让 pager 数出界! */
    };

    while(1){
        int rc = pager_pick("主菜单", menu, 0);
        if(rc == -1)                /* Esc: 继续显示菜单 */
            continue;
        if(rc == 6)                 /* "7.退出" 是第 6 项(下标从 0 数) */
            break;
        /* 选中项的 func 已经在 pager 里调用过了, 这里直接循环 */
    }
    fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
    system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
    return 0;
}
