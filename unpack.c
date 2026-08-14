// unpack.c: 解包菜单 + 公共解包工具
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "util.h"
#include "unpack.h"
#include "acb.h"
#include "paper.h"

void wipe_dir(const wchar_t *dir){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[1300];
        swprintf(full, 1300, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            wipe_dir(full);
            RemoveDirectoryW(full);
        } else {
            DeleteFileW(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 查找 exe 同目录 AssetStudio\AssetStudio.CLI.exe，找不到则置空（调用方提示） */

void find_assetstudio(wchar_t *out, int n){
    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    wchar_t local[1200];
    swprintf(local, 1200, L"%ls\\AssetStudio\\AssetStudio.CLI.exe", exedir);
    if (GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES){
        wcscpy(out, local);
    } else {
        out[0] = 0;
    }
}

/* 提示：表情动画都是骨骼动画；身体贴图引用缺失用 Blender 脚本修复 */

void print_gui_guide(const wchar_t *model_dir){
    char gui_u8[1200], dir_u8[1200], tex_u8[1200], sk_u8[1200];
    wchar_t gui[1200];
    /* 找 GUI 程序路径（exe 同目录 AssetStudio\AssetStudio.GUI.exe） */
    GetModuleFileNameW(NULL, gui, 1200);
    wchar_t *gp = wcsrchr(gui, L'\\');
    if (gp) *gp = 0;
    wcscat(gui, L"\\AssetStudio\\AssetStudio.GUI.exe");
    if (GetFileAttributesW(gui) == INVALID_FILE_ATTRIBUTES)
        gui[0] = 0;
    wide_to_utf8(gui, gui_u8, sizeof gui_u8);
    wide_to_utf8(model_dir, dir_u8, sizeof dir_u8);
    {
        wchar_t script[1200];
        GetModuleFileNameW(NULL, script, 1200);
        wchar_t *sp = wcsrchr(script, L'\\');
        if (sp) *sp = 0;
        wcscat(script, L"\\cgss_apply_textures.py");
        wide_to_utf8(script, tex_u8, sizeof tex_u8);
        GetModuleFileNameW(NULL, script, 1200);
        sp = wcsrchr(script, L'\\');
        if (sp) *sp = 0;
        wcscat(script, L"\\cgss_anim_to_shapekeys.py");
        wide_to_utf8(script, sk_u8, sizeof sk_u8);
    }
    printf("\n==============================================\n");
    printf("提示：\n");
    printf("  1. CLI 导出的 FBX 只有骨架+网格，没有表情动作；\n");
    printf("     要带动作的 FBX 必须用 AssetStudio GUI 全选导出：\n");
    if (gui_u8[0])
        printf("     1) 打开：%s\n", gui_u8);
    else
        printf("     1) 打开 AssetStudio GUI（未找到，请把 AssetStudio 文件夹放到程序同目录）\n");
    printf("     2) File -> Load folder 选择：%s\n", dir_u8);
    printf("     3) 资产列表选中 Animator(md_chr*_hq) 和全部 AnimationClip(an_*_face*)\n");
    printf("        （Ctrl 多选）\n");
    printf("     4) 右键 Animator -> Export selected objects (merge) + Selected AnimationClips\n");
    printf("        FBX 输出到同一目录\n");
    printf("  2. 表情动作（GUI 导出）是骨骼动画，模型本身没有形态键；\n");
    printf("     需要形态键就在 Blender 里跑 cgss_anim_to_shapekeys.py 把表情烘焙成形态键。\n");
    printf("  3. 身体（md_body）贴图引用缺失：贴图在独立 tx_body 包里，\n");
    printf("     AssetStudio 跨包解析不到，FBX 里连引用都没有。\n");
    printf("     在 Blender 里跑 cgss_apply_textures.py 即可按材质名自动贴图。\n");
    printf("Blender 脚本：\n");
    printf("  %s\n", tex_u8);
    printf("  %s\n", sk_u8);
    printf("==============================================\n\n");
}

/* 是否已解包：同目录有 <包名>.done 标记，或能找到对应 .fbx（兼容旧版导出） */

int is_done(const wchar_t *dir, const wchar_t *pkg){
    wchar_t buf[1300];
    swprintf(buf, 1300, L"%ls\\%ls.done", dir, pkg);
    if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return 1;

    wchar_t base[512];
    wcscpy(base, pkg);
    wchar_t *dot = wcsrchr(base, L'.');
    if (dot) *dot = 0;
    swprintf(buf, 1300, L"%ls\\%ls.fbx", dir, base);
    if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return 1;
    /* 3d_md_body3760_hq.unity3d 导出的 fbx 叫 md_body3760_hq.fbx */
    if (wcsncmp(base, L"3d_", 3) == 0){
        swprintf(buf, 1300, L"%ls\\%ls.fbx", dir, base + 3);
        if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}


int copy_dir(const wchar_t *outdir, const wchar_t *sub, const wchar_t *dest, const wchar_t *ext){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\%ls\\%ls", outdir, sub, ext);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t src[1300], dst[1300];
        swprintf(src, 1300, L"%ls\\%ls\\%ls", outdir, sub, fd.cFileName);
        swprintf(dst, 1300, L"%ls\\%ls", dest, fd.cFileName);
        if (CopyFileW(src, dst, FALSE)){
            char cname[300];
            wide_to_utf8(fd.cFileName, cname, sizeof cname);
            printf("  -> %s\n", cname);
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}


int unpack_main(void){
    def menu[]={
        {"1.动作解析",NULL,0},
        {"2.表情/镜头",NULL,0},
        {"3.模型解包为FBX(beta)",unpack_fbx_main,0},
        {"4.角色资源解包(卡面/背景/卡面Spina动画(beta)/3d照片/spine(beta))",unpack_resources_main,0},
        {"5.ACB文件解包",acb_main,0},
        {"6.返回",NULL,0},
        {"END",NULL,0}
    };
    while (1){
        int rc = pager_pick("解包",menu,0);
        if(rc == -1)
            continue;
        else if(rc == 5)
            break;
    }
}

