/* Shared sticker download post-processing used by both download menus. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sticker.h"
#include "unpack.h"
#include "spine_convert.h"
#include "texture_merge.h"
#include "util.h"

static int sticker_name_parts(const char *resource_name,
                              char *asset_id, int asset_id_n,
                              wchar_t *wide_id, int wide_id_n){
    char base[256];
    const char *name = base_name(resource_name);
    snprintf(base, sizeof base, "%s", name);

    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;

    const char prefix[] = "spine_motion_sticker_";
    const char *pnum = strstr(base, prefix);
    if (pnum == base){
        char digits[64];
        const char *p = pnum + sizeof(prefix) - 1;
        size_t n = 0;
        while (p[n] >= '0' && p[n] <= '9' && n + 1 < sizeof digits)
            n++;
        if (n > 0){
            memcpy(digits, p, n);
            digits[n] = 0;
            snprintf(asset_id, asset_id_n, "SPMotionSticker_%s", digits);
        } else {
            snprintf(asset_id, asset_id_n, "%s", base);
        }
    } else {
        snprintf(asset_id, asset_id_n, "%s", base);
    }
    utf8_to_wide(asset_id, wide_id, wide_id_n);
    return asset_id[0] ? 0 : -1;
}

static int find_first_file(const wchar_t *dir, const wchar_t *pattern,
                           wchar_t *out, int out_n){
    wchar_t path[1300];
    swprintf(path, 1300, L"%ls\\%ls", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
        FindClose(h);
        return -1;
    }
    swprintf(out, out_n, L"%ls\\%ls", dir, fd.cFileName);
    FindClose(h);
    return 0;
}

int sticker_unpack_file(const char *resource_name,
                        const wchar_t *raw_dir,
                        const wchar_t *spine_dir,
                        const wchar_t *png_dir,
                        int work_index){
    char asset_id[256];
    wchar_t wide_id[256];
    if (sticker_name_parts(resource_name, asset_id, sizeof asset_id,
                           wide_id, sizeof wide_id / sizeof wide_id[0]) != 0)
        return -1;

    wchar_t raw_file[1300];
    wchar_t raw_name[512];
    utf8_to_wide(base_name(resource_name), raw_name,
                 (int)(sizeof raw_name / sizeof raw_name[0]));
    swprintf(raw_file, 1300, L"%ls\\%ls", raw_dir, raw_name);
    if (GetFileAttributesW(raw_file) == INVALID_FILE_ATTRIBUTES){
        printf("  找不到已下载文件，跳过解包: %s\n", resource_name);
        return -1;
    }

    wchar_t exe[1200] = L"";
    find_assetstudio(exe, (int)(sizeof exe / sizeof exe[0]));
    if (!exe[0]){
        printf("  找不到 AssetStudio.CLI.exe，只保留原文件: %s\n", resource_name);
        return -1;
    }

    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, (DWORD)(sizeof exedir / sizeof exedir[0]));
    wchar_t *ep = wcsrchr(exedir, L'\\');
    if (ep) *ep = 0;

    wchar_t subdir[1300], mark[1300], first_png[1300];
    swprintf(subdir, 1300, L"%ls\\%ls", spine_dir, wide_id);
    mkdirs(subdir);
    swprintf(mark, 1300, L"%ls\\done.txt", subdir);
    swprintf(first_png, 1300, L"%ls\\%hs_1.png", png_dir, asset_id);
    if (GetFileAttributesW(mark) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(first_png) != INVALID_FILE_ATTRIBUTES){
        printf("  %s 已解包\n", resource_name);
        return 0;
    }

    wchar_t outdir[1200];
    swprintf(outdir, 1200, L"%ls\\AssetStudio_out\\st%03d", exedir, work_index);
    wipe_dir(outdir);
    mkdirs(outdir);

    wchar_t cmd[3000];
    swprintf(cmd, 3000, L"\"%ls\" \"%ls\" \"%ls\" --game Normal",
             exe, raw_file, outdir);
    printf("解包 %s ...\n", resource_name);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
        printf("  启动 AssetStudio.CLI 失败 err=%lu\n",
               (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    copy_dir(outdir, L"TextAsset", subdir, L"*.skel*");
    copy_dir(outdir, L"TextAsset", subdir, L"*.atlas*");
    copy_dir(outdir, L"Texture2D", subdir, L"*.png");

    wchar_t skel[1300];
    if (find_first_file(subdir, L"*.skel*", skel, 1300) == 0){
        wchar_t json[1300];
        wcscpy(json, skel);
        size_t len = wcslen(json);
        if (len > 11 && _wcsicmp(json + len - 11, L".skel.asset") == 0)
            wcscpy(json + len - 11, L".json");
        else if (len > 5 && _wcsicmp(json + len - 5, L".skel") == 0)
            wcscpy(json + len - 5, L".json");

        int ok36 = convert_skel21_to_json_w(skel, json, 1.0f) == 0;
        wchar_t json_v38[1300];
        wcscpy(json_v38, json);
        len = wcslen(json_v38);
        if (len > 5 && _wcsicmp(json_v38 + len - 5, L".json") == 0)
            wcscpy(json_v38 + len - 5, L"_v38.json");
        int ok38 = convert_skel21_to_json_v38_w(skel, json_v38, 1.0f) == 0;
        printf("  -> %ls%s\n",
               wcsrchr(json, L'\\') ? wcsrchr(json, L'\\') + 1 : json,
               ok36 && ok38 ? " + _v38.json" :
               (ok36 ? "（3.8 失败）" : "（转换失败）"));
    } else {
        printf("  没找到 skel，跳过 JSON 转换\n");
    }

    wchar_t atlas[1300] = L"", png[1300] = L"";
    find_first_file(subdir, L"*.atlas*", atlas, 1300);
    find_first_file(subdir, L"*.png", png, 1300);
    if (atlas[0] && png[0]){
        int count = crop_atlas_regions(atlas, png, png_dir, wide_id);
        printf("  -> 贴纸PNG\\%ls_1.png / _2.png（%d 帧）\n", wide_id, count);
    } else {
        printf("  没找到 atlas/png，跳过裁剪\n");
    }

    FILE *mf = _wfopen(mark, L"wb");
    if (mf){
        fputs("done", mf);
        fclose(mf);
    }
    return 0;
}
