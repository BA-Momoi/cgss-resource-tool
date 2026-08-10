// unpack_res.c: 角色资源解包（卡面/背景/卡面Spina动画/3d照片/spine -> png/数据）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "util.h"
#include "unpack.h"

#define MAX_RES_ITEMS 1024

typedef struct {
    wchar_t path[1100];
    wchar_t dir[1100];
    char name[256];
    char folder_name[256];
    char sub[64];
    int done;
} ResUnpackItem;

static void scan_res_dir(const wchar_t *chara_dir, const wchar_t *sub, const char *sub_u8,
                         ResUnpackItem *items, int *n, const char *folder_name){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\%ls\\*.unity3d", chara_dir, sub);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (*n >= MAX_RES_ITEMS) break;
        swprintf(items[*n].dir, 1100, L"%ls\\%ls", chara_dir, sub);
        swprintf(items[*n].path, 1100, L"%ls\\%ls\\%ls", chara_dir, sub, fd.cFileName);
        wide_to_utf8(fd.cFileName, items[*n].name, sizeof items[*n].name);
        snprintf(items[*n].folder_name, sizeof items[*n].folder_name, "%s", folder_name);
        snprintf(items[*n].sub, sizeof items[*n].sub, "%s", sub_u8);
        wchar_t marker[1300];
        swprintf(marker, 1300, L"%ls\\%ls\\%ls.done", chara_dir, sub, fd.cFileName);
        items[*n].done = (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES);
        (*n)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 解包单个角色资源包：导出 png/数据文件到原目录 */

static int extract_res_one(const ResUnpackItem *it, int idx){
    wchar_t exedir[1024], outdir[1200];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    swprintf(outdir, 1200, L"%ls\\AssetStudio_out\\r%03d", exedir, idx);
    wipe_dir(outdir);
    mkdirs(outdir);

    wchar_t exe[1200], cmd[3000];
    find_assetstudio(exe, 1200);
    if (!exe[0]){
        printf("找不到 AssetStudio.CLI.exe，请把 AssetStudio 文件夹放到程序同目录\n");
        return 0;
    }
    swprintf(cmd, 3000, L"\"%ls\" \"%ls\" \"%ls\" --game Normal", exe, it->path, outdir);
    printf("解包 %s\\%s ...\n", it->sub, it->name);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
        printf("启动 AssetStudio.CLI 失败 err=%lu\n", (unsigned long)GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    int n = 0;
    n += copy_dir(outdir, L"Texture2D", it->dir, L"*.png");
    n += copy_dir(outdir, L"Texture2D", it->dir, L"*.tga");
    n += copy_dir(outdir, L"Sprite", it->dir, L"*.png");
    n += copy_dir(outdir, L"TextAsset", it->dir, L"*");
    n += copy_dir(outdir, L"MonoBehaviour", it->dir, L"*.json");
    n += copy_dir(outdir, L"AudioClip", it->dir, L"*");

    wchar_t wname[256], marker[1300];
    utf8_to_wide(it->name, wname, 256);
    swprintf(marker, 1300, L"%ls\\%ls.done", it->dir, wname);
    FILE *mf = _wfopen(marker, L"wb");
    if (mf){ fputs("done", mf); fclose(mf); }

    if (n == 0) printf("  未生成可复制文件（可能包里没有图片/数据）\n");
    printf("  完成，导出 %d 个文件\n", n);
    return n;
}


int unpack_resources_main(void){
    wchar_t wroot[1024];
    GetModuleFileNameW(NULL, wroot, 1024);
    wchar_t *p = wcsrchr(wroot, L'\\');
    if (p) *p = 0;
    wcscat(wroot, L"\\CGSS_DOWN");

    ResUnpackItem *items = (ResUnpackItem*)malloc(sizeof(ResUnpackItem) * MAX_RES_ITEMS);
    if (!items){ fprintf(stderr, "内存不足\n"); return 1; }
    int n = 0;

    /* 角色目录下的子目录：卡面/背景/卡面Spina动画(兼容旧名live2d)/3d照片/spine */
    const wchar_t *subs[6] = { L"卡面", L"背景", L"卡面Spina动画", L"live2d", L"3d照片", L"spine" };
    const char *subs_u8[6] = { "卡面", "背景", "卡面Spina动画", "live2d(旧)", "3d照片", "spine" };

    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", wroot);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE){
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            char chara_name[256];
            wide_to_utf8(fd.cFileName, chara_name, sizeof chara_name);
            wchar_t chara_dir[1300];
            swprintf(chara_dir, 1300, L"%ls\\%ls", wroot, fd.cFileName);
            for (int s = 0; s < 6; s++)
                scan_res_dir(chara_dir, subs[s], subs_u8[s], items, &n, chara_name);
        } while (FindNextFileW(h, &fd) && n < MAX_RES_ITEMS);
        FindClose(h);
    }

    if (n == 0){
        printf("CGSS_DOWN 里没找到卡面/背景/卡面Spina动画/3d照片/spine 资源\n");
        free(items);
        return 1;
    }

    int ndone = 0;
    for (int i = 0; i < n; i++){
        printf("[%d] %s : %s\\%s %s\n", i + 1, items[i].folder_name, items[i].sub, items[i].name,
               items[i].done ? "[已解包]" : "[待解包]");
        if (items[i].done) ndone++;
    }
    printf("共 %d 个资源包，已解包 %d 个（a=解包全部未解包的，输编号可强制重解，0=返回）：", n, ndone);
    char buf[128];
    if (fgets(buf, sizeof buf, stdin) == NULL){ free(items); return 1; }
    int *sel = (int*)malloc(sizeof(int) * n);
    if (!sel){ free(items); return 1; }
    int nsel = parse_multi(buf, sel, n);
    if (nsel < 0){
        nsel = 0;
        for (int i = 0; i < n; i++)
            if (!items[i].done) sel[nsel++] = i + 1;
        if (nsel == 0){ printf("都已解包，无需处理\n"); free(sel); free(items); return 0; }
    }
    if (nsel == 0){ free(sel); free(items); return 1; }

    for (int s = 0; s < nsel; s++){
        extract_res_one(&items[sel[s] - 1], s);
    }
    printf("全部完成\n");
    free(sel);
    free(items);
    return 0;
}
