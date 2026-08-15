// acb.c: ACB 音乐提取和 HCA 解码（菜单6）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "acb.h"
#include "util.h"

typedef struct {
    wchar_t folder[512];
    wchar_t acbdir[512];
    wchar_t acb[512];
    char acb_name[256];
    char folder_name[256];
} AcbItem;

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

/* 取 exe 所在目录的 CGSS_DOWN 根路径 */

static void scan_acb(const wchar_t *dir, AcbItem *items, int *n,
                     const wchar_t *chara_folder, const char *chara_name, int depth){
    wchar_t pat[1200];
    swprintf(pat, 1200, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if (depth < 3){
                wchar_t sub[1200];
                swprintf(sub, 1200, L"%ls\\%ls", dir, fd.cFileName);
                scan_acb(sub, items, n, chara_folder, chara_name, depth + 1);
            }
            continue;
        }
        const wchar_t *dot = wcsrchr(fd.cFileName, L'.');
        if (!dot || _wcsicmp(dot, L".acb") != 0) continue;
        if (*n >= 64) break;
        wcscpy(items[*n].folder, chara_folder);
        swprintf(items[*n].acbdir, 512, L"%ls", dir);
        swprintf(items[*n].acb, 512, L"%ls\\%ls", dir, fd.cFileName);
        wide_to_utf8(fd.cFileName, items[*n].acb_name, sizeof items[*n].acb_name);
        snprintf(items[*n].folder_name, sizeof items[*n].folder_name, "%s", chara_name);
        (*n)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}


int acb_main(void){
    wchar_t wroot[1024];
    get_dl_root(wroot, 1024);


    /* 递归扫描 CGSS_DOWN\*\...\*.acb */
    AcbItem items[64];
    int n = 0;
    wchar_t pat[1200];
    swprintf(pat, 1200, L"%ls\\*", wroot);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE){
        printf("CGSS_DOWN 还没有下载内容\n");
        return 1;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        char chara_name[256];
        wide_to_utf8(fd.cFileName, chara_name, sizeof chara_name);
        wchar_t chara_dir[1200];
        swprintf(chara_dir, 1200, L"%ls\\%ls", wroot, fd.cFileName);
        scan_acb(chara_dir, items, &n, chara_dir, chara_name, 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (n == 0){
        printf("CGSS_DOWN 里没有找到 acb 文件\n");
        return 1;
    }
    for (int i = 0; i < n; i++)
        printf("[%d] %s : %s\n", i + 1, items[i].folder_name, items[i].acb_name);
    printf("选择解压（空格/逗号分隔数字，a=全部，0=返回）：");
    char buf[128];
    if (fgets(buf, sizeof buf, stdin) == NULL) return 1;
    int sel[64], nsel = parse_multi(buf, sel, n);
    if (nsel < 0){ nsel = n; for (int i = 0; i < n; i++) sel[i] = i + 1; }
    if (nsel == 0) return 1;

    for (int s = 0; s < nsel; s++){
        int i = sel[s] - 1;
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
    printf("全部完成\n");
    return 0;
}
/* ================== ?????????3-6? ================== */

