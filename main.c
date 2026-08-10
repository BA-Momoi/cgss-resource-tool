#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "lookup_table.h"
#include "download.h"
#include "unpack.h"
#include "preview.h"

int main(void){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);   // 强制 UTF-8 控制台，输入输出统一
    char buf[32];
    while (1) {
        printf("1.数据表查找数据\n2.数据下载并解析\n3.解包\n4.打开Spine预览\n");
        if (fgets(buf, sizeof buf, stdin) == NULL) break;   // EOF 退出
        int opt = atoi(buf);
        switch (opt) {
        case 1:
            lookup_main();
            break;
        case 2:
            dl_main();
            break;
        case 3:
            unpack_main();
            break;
        case 4:
            open_spine_preview();
            break;
        default:
            fprintf(stderr, "输入错误\n");
            break;
        }
    }
    fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
    system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
    return 0;
}
