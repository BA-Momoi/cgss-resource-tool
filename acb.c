// acb.c: ACB 音乐提取和 HCA 解码（菜单6）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>
#include <limits.h>
#include <stdint.h>
#include "acb.h"
#include "util.h"
#include "paper.h"

typedef struct {
    wchar_t folder[512];
    wchar_t acbdir[512];
    wchar_t acb[512];
    char acb_name[1024];
    char folder_name[1024];
} AcbItem;

typedef struct {
    AcbItem *items;
    int count;
    int capacity;
} AcbList;

static void get_acb2wavs(wchar_t *out, int n){
    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    wchar_t local[1200];
    swprintf(local, 1200, L"%ls\\acb2wavs.exe", exedir);
    if (GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES){
        wcscpy(out, local);
    } else {
        out[0] = 0;
    }
}

static int add_acb(AcbList *list, const wchar_t *dir, const wchar_t *filename,
                   const wchar_t *folder, const char *folder_name){
    wchar_t path[512];
    int written = swprintf(path, _countof(path), L"%ls\\%ls", dir, filename);
    if (written < 0 || (size_t)written >= _countof(path) ||
        wcslen(folder) >= _countof(list->items[0].folder)){
        fprintf(stderr, "ACB 路径过长: %ls\\%ls\n", dir, filename);
        return -1;
    }
    if (list->count == INT_MAX - 1) return -1;
    if (list->count == list->capacity){
        int capacity = list->capacity == 0 ? 64 :
            (list->capacity > (INT_MAX - 1) / 2 ? INT_MAX - 1 :
             list->capacity * 2);
        if ((size_t)capacity > SIZE_MAX / sizeof *list->items) return -1;
        AcbItem *items = realloc(list->items, (size_t)capacity * sizeof *items);
        if (!items) return -1;
        list->items = items;
        list->capacity = capacity;
    }
    AcbItem *item = &list->items[list->count];
    wcscpy(item->folder, folder);
    wcscpy(item->acbdir, dir);
    wcscpy(item->acb, path);
    wide_to_utf8(filename, item->acb_name, sizeof item->acb_name);
    snprintf(item->folder_name, sizeof item->folder_name, "%s", folder_name);
    list->count++;
    return 0;
}

static int scan_acb(const wchar_t *dir, AcbList *list,
                    const wchar_t *chara_folder, const char *chara_name){
    wchar_t pat[1200];
    int written = swprintf(pat, _countof(pat), L"%ls\\*", dir);
    if (written < 0 || (size_t)written >= _countof(pat)) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE){
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return 0;
        fprintf(stderr, "扫描 ACB 目录失败: %ls (%lu)\n", dir, error);
        return -1;
    }
    int result = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            /* Directory links can lead back into already scanned folders. */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            wchar_t sub[1200];
            written = swprintf(sub, _countof(sub), L"%ls\\%ls", dir, fd.cFileName);
            if (written < 0 || (size_t)written >= _countof(sub)){
                result = -1;
                break;
            }
            char folder_name[1024];
            wide_to_utf8(fd.cFileName, folder_name, sizeof folder_name);
            if (scan_acb(sub, list, chara_folder ? chara_folder : sub,
                         chara_name ? chara_name : folder_name) != 0){
                result = -1;
                break;
            }
            continue;
        }
        const wchar_t *dot = wcsrchr(fd.cFileName, L'.');
        if (!dot || _wcsicmp(dot, L".acb") != 0) continue;
        if (add_acb(list, dir, fd.cFileName,
                    chara_folder ? chara_folder : dir,
                    chara_name ? chara_name : "CGSS_DOWN") != 0){
            result = -1;
            break;
        }
    } while (FindNextFileW(h, &fd));
    if (result == 0 && GetLastError() != ERROR_NO_MORE_FILES){
        fprintf(stderr, "扫描 ACB 目录未完成: %ls (%lu)\n", dir, GetLastError());
        result = -1;
    }
    FindClose(h);
    return result;
}

static int compare_acb(const void *left, const void *right){
    return _wcsicmp(((const AcbItem*)left)->acb, ((const AcbItem*)right)->acb);
}

int acb_main(void){
    wchar_t wroot[1024];
    get_dl_root(wroot, 1024);


    AcbList list = {0};
    if (scan_acb(wroot, &list, NULL, NULL) != 0){
        fprintf(stderr, "ACB 扫描失败，未显示不完整列表\n");
        free(list.items);
        return 1;
    }
    AcbItem *items = list.items;
    int n = list.count;
    if (n == 0){
        printf("CGSS_DOWN 里没有找到 acb 文件\n");
        free(items);
        return 1;
    }
    qsort(items, (size_t)n, sizeof *items, compare_acb);
    if ((size_t)n + 1 > SIZE_MAX / sizeof(dbdef)){
        free(items);
        return 1;
    }
    dbdef *menu = calloc((size_t)n + 1, sizeof *menu);
    if (!menu){
        fprintf(stderr, "ACB 列表内存分配失败\n");
        free(items);
        return 1;
    }
    for (int i = 0; i < n; i++)
        snprintf(menu[i].name, sizeof menu[i].name, "%s : %s",
                 items[i].acb_name, items[i].folder_name);
    strcpy(menu[n].name, "END");
    char title[128];
    snprintf(title, sizeof title, "ACB文件解包（共%d个）", n);
    if (pager_picks(title, menu, NULL, NULL, 1) <= 0){
        free(menu);
        free(items);
        return 1;
    }

    for (int i = 0; i < n; i++){
        if (!menu[i].state) continue;
        printf("解码 %s ...\n", items[i].acb_name);
        wchar_t cmd[2048];
        wchar_t wacb2wavs[512];
        get_acb2wavs(wacb2wavs, 512);
        if (!wacb2wavs[0]){
            printf("  找不到 acb2wavs.exe，请把它放到程序同目录\n");
            continue;
        }
        swprintf(cmd, 2048, L"\"%ls\" \"%ls\"", wacb2wavs, items[i].acb);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            printf("??acb2wavs?? err=%lu\n", (unsigned long)GetLastError());
        }

        /* 音频目录 */
        wchar_t adir[1024];
        swprintf(adir, 1024, L"%ls\\音频", items[i].folder);
        mkdirs(adir);
        /* 目标名 = acb 文件名去扩展名 */
        char outname[256];
        snprintf(outname, sizeof outname, "%s", items[i].acb_name);
        char *dot = strrchr(outname, '.');
        if (dot) *dot = 0;
        /* 移动全部解码出的 wav */
        wchar_t wacbname[256];
        utf8_to_wide(outname, wacbname, 256);   /* ????????? */
        wchar_t wdir[1200], wpat[1200];
        swprintf(wdir, 1200, L"%ls\\_acb_%ls.acb\\internal", items[i].acbdir, wacbname);
        swprintf(wpat, 1200, L"%ls\\*.wav", wdir);
        WIN32_FIND_DATAW wfd;
        HANDLE wh = FindFirstFileW(wpat, &wfd);
        if (wh == INVALID_HANDLE_VALUE){
            printf("  未找到解码wav（请检查 acb2wavs 是否成功）\n");
        } else {
            int wcount = 0;
            do { if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) wcount++; }
            while (FindNextFileW(wh, &wfd));
            FindClose(wh);
            wh = FindFirstFileW(wpat, &wfd);
            do {
                if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t src[1200], dst[1200];
                swprintf(src, 1200, L"%ls\\%ls", wdir, wfd.cFileName);
                if (wcount == 1){
                    swprintf(dst, 1200, L"%ls\\%hs.wav", adir, outname);
                } else {
                    swprintf(dst, 1200, L"%ls\\%hs_%ls", adir, outname, wfd.cFileName);
                }
                if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING)) {
                    wchar_t *base = wcsrchr(dst, L'\\');
                    wprintf(L"  -> 音频\\%ls\n", base ? (base + 1) : dst);
                }
            } while (FindNextFileW(wh, &wfd));
            FindClose(wh);
        }
        /* 封面: 角色文件夹\封面\* -> 音频\ */
        wchar_t cpat[1200];
        swprintf(cpat, 1200, L"%ls\\封面\\*", items[i].folder);
        WIN32_FIND_DATAW cfd;
        HANDLE ch = FindFirstFileW(cpat, &cfd);
        if (ch != INVALID_HANDLE_VALUE){
            do {
                if (cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t src[1200], dst[1200];
                swprintf(src, 1200, L"%ls\\封面\\%ls", items[i].folder, cfd.cFileName);
                swprintf(dst, 1200, L"%ls\\%ls", adir, cfd.cFileName);
                CopyFileW(src, dst, FALSE);
                printf("  封面 -> %ls\n", cfd.cFileName);
            } while (FindNextFileW(ch, &cfd));
            FindClose(ch);
        }
        /* 歌词: 角色文件夹\*.lrc -> 音频\ */
        wchar_t lpat[1200];
        swprintf(lpat, 1200, L"%ls\\*.lrc", items[i].folder);
        WIN32_FIND_DATAW lfd;
        HANDLE lh = FindFirstFileW(lpat, &lfd);
        if (lh != INVALID_HANDLE_VALUE){
            do {
                if (lfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t src[1200], dst[1200];
                swprintf(src, 1200, L"%ls\\%ls", items[i].folder, lfd.cFileName);
                swprintf(dst, 1200, L"%ls\\%ls", adir, lfd.cFileName);
                CopyFileW(src, dst, FALSE);
                printf("  歌词 -> %ls\n", lfd.cFileName);
            } while (FindNextFileW(lh, &lfd));
            FindClose(lh);
        }
    }
    free(menu);
    free(items);
    printf("全部完成\n");
    return 0;
}
/* ================== ?????????3-6? ================== */

