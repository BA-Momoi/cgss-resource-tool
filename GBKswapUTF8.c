#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#include "GBKswapUTF8.h"

/* 自动识别：输入用控制台输入代码页，输出用控制台输出代码页 */
static UINT input_cp(void) {
    UINT cp = GetConsoleCP();
    return cp == 0 ? GetACP() : cp;      /* 重定向时退回系统 ANSI */
}

static UINT output_cp(void) {
    UINT cp = GetConsoleOutputCP();
    return cp == 0 ? GetACP() : cp;
}

/* UTF-8 (db text) -> console codepage (GBK or UTF-8) */
void utf8_to_gbk(const char *in, char *out, int out_size) {
    UINT cp = output_cp();
    if (cp == CP_UTF8) {                 /* 终端就是 UTF-8，直接复制 */
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = 0;
        return;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, in, -1, NULL, 0);
    wchar_t *wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, in, -1, wbuf, wlen);
    WideCharToMultiByte(cp, 0, wbuf, -1, out, out_size, NULL, NULL);
    free(wbuf);
}

/* console codepage (GBK or UTF-8) -> UTF-8 for db queries */
void gbk_to_utf8(const char *in, char *out, int out_size) {
    UINT cp = input_cp();
    if (cp == CP_UTF8) {                 /* 输入已是 UTF-8，直接复制 */
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = 0;
        return;
    }
    int wlen = MultiByteToWideChar(cp, 0, in, -1, NULL, 0);
    wchar_t *wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(cp, 0, in, -1, wbuf, wlen);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, out_size, NULL, NULL);
    free(wbuf);
}
