#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "lookup_table.h"
#include "download.h"
#include "unpack.h"
#include "preview.h"

typedef struct  _MENU_OptionTypeDef
{
    char *String;
    void (*func)(void);
    
}MENU_OptionTypeDef;



int main(void){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   // 强制 UTF-8 控制台，输入输出统一
    MENU_OptionTypeDef menu[] = {
        {"1.数据表查找数据", lookup_main},
        {"2.数据下载并解析", dl_main},
        {"3.解包", unpack_main},
        {"4.打开Spine预览(beta)", open_spine_preview},
        {"5.usm文件解包", unpack_usm},
        {"6.自定义搜索并下载", dl_custom},
        {"7.退出", NULL},
        {"end", NULL}
    };
    char buf[32];
    
    fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
    system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
    return 0;
}
