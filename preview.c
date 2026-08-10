// preview.c: 打开 Spine 浏览器预览（自动补转 skel->json，再调默认浏览器打开 preview.html）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "util.h"
#include "spine_convert.h"
#include "texture_merge.h"
#include "preview.h"

typedef struct {
    wchar_t folder[1100];   /* 角色文件夹绝对路径 */
    char name[256];         /* 角色文件夹名（UTF-8） */
    int skels;              /* 该文件夹下 skel 数量 */
} PrevCard;

static int count_skels(const wchar_t *live2d){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*.skel*", live2d);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

int open_spine_preview(void){
    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;

    wchar_t html[1200];
    swprintf(html, 1200, L"%ls\\spine_preview\\preview.html", exedir);
    if (GetFileAttributesW(html) == INVALID_FILE_ATTRIBUTES){
        printf("找不到 spine_preview\\preview.html（应放在程序同目录），请先获取预览页面\n");
        return -1;
    }

    /* 扫描 CGSS_DOWN 下所有角色的 live2d 目录 */
    wchar_t wroot[1024];
    swprintf(wroot, 1024, L"%ls\\CGSS_DOWN", exedir);
    PrevCard cards[512];
    int n = 0;
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", wroot);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE){
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            /* 优先新目录：卡面Spina动画\spine 或 live2d\spine；兼容旧目录直接放的情况 */
            wchar_t live2d[1300];
            live2d[0] = 0;
            const wchar_t *cands[5];
            wchar_t a1[1300], a2[1300], a3[1300], a4[1300], a5[1300];
            swprintf(a1, 1300, L"%ls\\%ls\\卡面Spina动画\\spine", wroot, fd.cFileName);
            swprintf(a2, 1300, L"%ls\\%ls\\live2d\\spine", wroot, fd.cFileName);
            swprintf(a3, 1300, L"%ls\\%ls\\live2d", wroot, fd.cFileName);
            swprintf(a4, 1300, L"%ls\\%ls\\卡面Spina动画", wroot, fd.cFileName);
            swprintf(a5, 1300, L"%ls\\%ls\\spine", wroot, fd.cFileName);
            cands[0] = a1; cands[1] = a2; cands[2] = a3; cands[3] = a4; cands[4] = a5;
            int sk = 0;
            for (int c = 0; c < 5 && sk == 0; c++)
                sk = count_skels(cands[c]);
            /* 找到有 skel 的那个目录（上面循环只记了数量，这里再定一次） */
            for (int c = 0; c < 5; c++){
                if (count_skels(cands[c]) > 0){ wcscpy(live2d, cands[c]); break; }
            }
            if (sk > 0 && n < 512){
                swprintf(cards[n].folder, 1100, L"%ls", live2d);
                wide_to_utf8(fd.cFileName, cards[n].name, sizeof cards[n].name);
                cards[n].skels = sk;
                n++;
            }
        } while (FindNextFileW(h, &fd) && n < 512);
        FindClose(h);
    }

    if (n == 0){
        printf("CGSS_DOWN 里没有找到带 Spine(live2d) 资源的角色，请先下载并解包卡面Spina动画\n");
        return 0;
    }
    printf("有 Spine 资源的角色：\n");
    for (int i = 0; i < n; i++)
        printf("[%d] %s（%d 个 skel）\n", i + 1, cards[i].name, cards[i].skels);
    printf("选择要预览的角色（数字可多选/逗号分隔，a=全部，0=返回）：");
    char buf[256];
    if (fgets(buf, sizeof buf, stdin) == NULL) return 0;
    int *sel = (int*)malloc(sizeof(int) * n);
    if (!sel) return -1;
    int nsel = parse_multi(buf, sel, n);
    if (nsel < 0){
        nsel = 0;
        for (int i = 0; i < n; i++) sel[nsel++] = i + 1;
    }
    if (nsel == 0){ free(sel); return 0; }

    for (int s = 0; s < nsel; s++){
        PrevCard *c = &cards[sel[s] - 1];
        printf("处理 %s ...\n", c->name);
        int cn = convert_skels_in_dir(c->folder);
        printf("  %d 个 skel 已转换为 json\n", cn);
        int mn = merge_a8_textures_in_dir(c->folder);
        if (mn > 0)
            printf("  %d 张贴图已合成（3.8.75 编辑器用）\n", mn);
    }
    free(sel);

    HINSTANCE hr = ShellExecuteW(NULL, L"open", html, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hr <= 32){
        printf("打开浏览器失败（错误码 %d）\n", (int)(INT_PTR)hr);
        return -1;
    }
    printf("\n浏览器已打开 preview.html。在页面里选择：\n");
    printf("  骨架：上面角色的 spine 目录里全部 .skel（页面自动转 JSON，也支持 .json）\n");
    printf("  图集：SP3S301290_tex.atlas（改成对应卡片的文件名）\n");
    printf("  贴图：对应的 tex.png 和 tex_A8.png（A8 是透明通道，合成后无黑边）\n");
    printf("  要导入 Spine 3.8.75 编辑器：打开 *_v38.json + *_v38.atlas + *_merged.png\n");
    return 0;
}
