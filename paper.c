#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<windows.h>
#include<conio.h>
#include"paper.h"
HANDLE hOut;

void enable_vt(void){
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)){
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleOutputCP(CP_UTF8);   /* 和你 main.c 一样: 输出强制 UTF-8 */
}

int console_rows(void){
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hOut, &info)){
        return info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    return 24;
}