#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "util.h"

// util.c: 公共工具

void utf8_to_wide(const char *in, wchar_t *out, int n){
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, n);
}


void wide_to_utf8(const wchar_t *in, char *out, int n){
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, n, NULL, NULL);
}

/* ??? exe ???? acb2wavs.exe????????????? */

void get_dl_root(wchar_t *buf, int n){
    GetModuleFileNameW(NULL, buf, n);
    wchar_t *p = wcsrchr(buf, L'\\');
    if (p) *p = 0;
    wcscat(buf, L"\\CGSS_DOWN");
}

/* 递归创建目录 */

void mkdirs(const wchar_t *path){
    wchar_t tmp[1024];
    wcscpy(tmp, path);
    for (wchar_t *p = tmp + 3; *p; p++){
        if (*p == L'\\'){
            *p = 0;
            if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                printf("????? %ls err=%lu\n", tmp, (unsigned long)GetLastError());
            *p = L'\\';
        }
    }
    if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        printf("????? %ls err=%lu\n", tmp, (unsigned long)GetLastError());
}

/* 资源名去掉目录部分（l/song_1.acb -> song_1.acb） */

const char *base_name(const char *name){
    const char *p = strrchr(name, '/');
    return p ? p + 1 : name;
}

/* ================== LZ4 块解压（移植 cgss_lz4.py） ================== */


int parse_multi(const char *line, int *sel, int max){
    int n = 0;
    const char *p = line;
    while (*p){
        if (*p == 'a' || *p == 'A') return -1;
        if (*p >= '0' && *p <= '9'){
            int v = 0;
            while (*p >= '0' && *p <= '9'){ v = v * 10 + (*p - '0'); p++; }
            if (v > 0 && v <= max && n < 64) sel[n++] = v;
        } else p++;
    }

    return n;
}


int selected(const int *sel, int n, int v){
    for (int i = 0; i < n; i++)
        if (sel[i] == v) return 1;
    return 0;
}

/* 把 ResItem 列表下载到角色目录下各自子目录 */
