// unpack_fbx.c: 模型解包为 FBX（调用 AssetStudio.CLI）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "util.h"
#include "unpack.h"

#define MAX_FBX_ITEMS 256
typedef struct {
    wchar_t path[1100];
    wchar_t dir[1100];
    char name[256];
    char folder_name[256];
    int done;
} FbxItem;

static void scan_fbx_dir(const wchar_t *dir, FbxItem *items, int *n, const char *folder_name){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*.unity3d", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (*n >= MAX_FBX_ITEMS) break;
        swprintf(items[*n].dir, 1100, L"%ls", dir);
        swprintf(items[*n].path, 1100, L"%ls\\%ls", dir, fd.cFileName);
        wide_to_utf8(fd.cFileName, items[*n].name, sizeof items[*n].name);
        snprintf(items[*n].folder_name, sizeof items[*n].folder_name, "%s", folder_name);
        items[*n].done = is_done(dir, fd.cFileName);
        (*n)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 把 outdir\sub\*.ext 复制到 dest */

static int extract_one(const FbxItem *it, int idx){
    wchar_t exedir[1024], outdir[1200];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    /* 输出目录必须 ASCII，避免日文路径传给 .NET CLI 出错 */
    swprintf(outdir, 1200, L"%ls\\AssetStudio_out\\p%03d", exedir, idx);
    wipe_dir(outdir);
    mkdirs(outdir);

    wchar_t exe[1200], cmd[3000];
    find_assetstudio(exe, 1200);
    if (!exe[0]){
        printf("找不到 AssetStudio.CLI.exe，请把 AssetStudio 文件夹放到程序同目录\n");
        return 0;
    }
    /* 不指定 --types：默认导出全部（FBX/贴图png/表情anim/布料json） */
    swprintf(cmd, 3000, L"\"%ls\" \"%ls\" \"%ls\" --game Normal", exe, it->path, outdir);
    printf("解包 %s ...\n", it->name);
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
    n += copy_dir(outdir, L"Animator", it->dir, L"*.fbx");

    wchar_t sub[1200];
    swprintf(sub, 1200, L"%ls\\贴图", it->dir);
    mkdirs(sub);
    n += copy_dir(outdir, L"Texture2D", sub, L"*.png");
    n += copy_dir(outdir, L"Texture2D", sub, L"*.tga");

    swprintf(sub, 1200, L"%ls\\表情动画", it->dir);
    mkdirs(sub);
    n += copy_dir(outdir, L"AnimationClip", sub, L"*.anim");

    swprintf(sub, 1200, L"%ls\\布料数据", it->dir);
    mkdirs(sub);
    n += copy_dir(outdir, L"MonoBehaviour", sub, L"*.json");

    /* 写已解包标记 */
    wchar_t wname[256], marker[1300];
    utf8_to_wide(it->name, wname, 256);
    swprintf(marker, 1300, L"%ls\\%ls.done", it->dir, wname);
    FILE *mf = _wfopen(marker, L"wb");
    if (mf){ fputs("done", mf); fclose(mf); }

    if (n == 0) printf("  未生成可复制文件（可能包里没有网格/贴图/动画）\n");
    printf("  完成，复制 %d 个文件\n", n);
    return n;
}


int unpack_fbx_main(void){
    /* 扫描 CGSS_DOWN\*\3d模型\*.unity3d */
    wchar_t wroot[1024];
    GetModuleFileNameW(NULL, wroot, 1024);
    wchar_t *p = wcsrchr(wroot, L'\\');
    if (p) *p = 0;
    wcscat(wroot, L"\\CGSS_DOWN");  //将wroot改为X:XXX\XXX\CGSS_DOWN

    FbxItem items[MAX_FBX_ITEMS];
    int n = 0;
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", wroot);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE){
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            char chara_name[512];
            wide_to_utf8(fd.cFileName, chara_name, sizeof chara_name);
            wchar_t mdir[1300];
            swprintf(mdir, 1300, L"%ls\\%ls\\3d模型", wroot, fd.cFileName);
            if (GetFileAttributesW(mdir) == INVALID_FILE_ATTRIBUTES) continue;
            scan_fbx_dir(mdir, items, &n, chara_name);
        } while (FindNextFileW(h, &fd) && n < MAX_FBX_ITEMS);
        FindClose(h);
    }

    if (n == 0){
        printf("CGSS_DOWN 里没找到 3d模型 文件夹，改用手动输入路径\n");
        char path[1024];
        printf("输入 .unity3d 文件路径：");
        if (fgets(path, sizeof path, stdin) == NULL) return 1;
        path[strcspn(path, "\r\n")] = 0;
        if (!path[0]) return 1;
        FbxItem it;
        memset(&it, 0, sizeof it);
        utf8_to_wide(path, it.path, 1100);
        wcscpy(it.dir, it.path);
        wchar_t *ws = wcsrchr(it.dir, L'\\');
        if (ws) *ws = 0;
        const char *bn = strrchr(path, '\\');
        snprintf(it.name, sizeof it.name, "%s", bn ? bn + 1 : path);
        snprintf(it.folder_name, sizeof it.folder_name, "手动输入");
        print_gui_guide(it.dir);
        extract_one(&it, 0);
        return 0;
    }

    print_gui_guide(items[0].dir);
    int ndone = 0;
    for (int i = 0; i < n; i++){
        printf("[%d] %s : %s %s\n", i + 1, items[i].folder_name, items[i].name,
               items[i].done ? "[已解包]" : "[待解包]");
        if (items[i].done) ndone++;
    }
    printf("共 %d 个模型包，已解包 %d 个（a=解包全部未解包的，输编号可强制重解，0=返回）：", n, ndone);
    char buf[128];
    if (fgets(buf, sizeof buf, stdin) == NULL) return 1;
    int sel[MAX_FBX_ITEMS], nsel = parse_multi(buf, sel, n);
    if (nsel < 0){
        nsel = 0;
        for (int i = 0; i < n; i++)
            if (!items[i].done) sel[nsel++] = i + 1;
        if (nsel == 0){ printf("都已解包，无需处理\n"); return 0; }
    }
    if (nsel == 0) return 1;

    for (int s = 0; s < nsel; s++){
        extract_one(&items[sel[s] - 1], s);
    }
    printf("全部完成\n");
    return 0;
}

