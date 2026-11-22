#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <shellapi.h>
#include "browse.h"
#include "preview.h"
#include "spine_catalog.h"

int open_spine_preview(void){
    wchar_t app[1024], target[1200];
    DWORD length = GetModuleFileNameW(NULL, app, _countof(app));
    if (!length || length >= _countof(app)) return -1;
    wchar_t *separator = wcsrchr(app, L'\\');
    if (!separator) return -1;
    *separator = L'\0';
    const int port = start_spine_catalog(app);
    if (port > 0){
        start_default_spine_downloads();
        swprintf(target, _countof(target), L"http://127.0.0.1:%d/preview.html", port);
    } else
        swprintf(target, _countof(target), L"%ls\\spine_preview\\preview.html", app);
    if ((INT_PTR)ShellExecuteW(NULL, L"open", target, NULL, NULL, SW_SHOWNORMAL) <= 32){
        fprintf(stderr, "无法打开 Spine 预览：%ls\n", target);
        return -1;
    }
    printf("Spine 预览：%ls\n", target);
    if (port <= 0) puts("本地资源库启动失败，已打开手动预览。");
    fflush(stdout);
    return 0;
}

int run_spine_preview_cli(void){
    if (open_spine_preview() != 0) return 1;
    puts("按 Enter 关闭本地预览服务。");
    fflush(stdout);
    (void)getchar();
    return 0;
}
