#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "sticker.h"
#include "unpack.h"
#include "util.h"
#include "spine_convert.h"
#include "texture_merge.h"

static int sticker_id_from_resource(const char *resource_name,
                                    char *id, size_t id_size)
{
    char digits[32];
    const char *prefix = "spine_motion_sticker_";
    const char *pnum;
    const char *suffix;
    char *dot;
    char *bad;
    const char *name;

    if (!resource_name || !id || id_size == 0) return 0;

    name = base_name(resource_name);
    snprintf(id, id_size, "%s", name);
    dot = strrchr(id, '.');
    if (dot) *dot = 0;

    pnum = strstr(id, prefix);
    if (!pnum) return 0;

    suffix = pnum + strlen(prefix);
    if (!suffix[0]) return 0;

    /* Numeric entries use the historical SPMotionSticker_XXXXX name.
     * Special non-numeric entries such as *_gacha keep their resource name. */
    if (*suffix < '0' || *suffix > '9')
        return 1;

    snprintf(digits, sizeof digits, "%s", suffix);
    bad = digits;
    while (*bad >= '0' && *bad <= '9') bad++;
    *bad = 0;
    if (!digits[0]) return 0;

    snprintf(id, id_size, "SPMotionSticker_%s", digits);
    return 1;
}

static int run_assetstudio(const wchar_t *assetstudio,
                           const wchar_t *input,
                           const wchar_t *output)
{
    wchar_t cmd[3000];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;

    swprintf(cmd, 3000, L"\"%ls\" \"%ls\" \"%ls\" --game Normal",
             assetstudio, input, output);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                         &si, &pi)) {
        printf("  启动 AssetStudio.CLI 失败 err=%lu\n",
               (unsigned long)GetLastError());
        return 0;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0) {
        printf("  AssetStudio.CLI 失败，退出码 %lu\n",
               (unsigned long)exit_code);
        return 0;
    }
    return 1;
}

static int convert_sticker_skel(const wchar_t *spine_sub)
{
    wchar_t pattern[1300];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t source[1300], json[1300], v38[1300];
    size_t len;
    int ok36, ok38;

    swprintf(pattern, 1300, L"%ls\\*.skel*", spine_sub);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        FindClose(h);
        return 0;
    }

    swprintf(source, 1300, L"%ls\\%ls", spine_sub, fd.cFileName);
    wcscpy(json, source);
    len = wcslen(json);
    if (len > 11 && _wcsicmp(json + len - 11, L".skel.asset") == 0)
        wcscpy(json + len - 11, L".json");
    else if (len > 5 && _wcsicmp(json + len - 5, L".skel") == 0)
        wcscpy(json + len - 5, L".json");
    else {
        FindClose(h);
        return 0;
    }

    ok36 = (convert_skel21_to_json_w(source, json, 1.0f) == 0);
    wcscpy(v38, json);
    len = wcslen(v38);
    if (len > 5 && _wcsicmp(v38 + len - 5, L".json") == 0)
        wcscpy(v38 + len - 5, L"_v38.json");
    ok38 = (convert_skel21_to_json_v38_w(source, v38, 1.0f) == 0);

    printf("  -> %ls%s\n",
           wcsrchr(json, L'\\') ? wcsrchr(json, L'\\') + 1 : json,
           ok36 && ok38 ? " + _v38.json" :
           (ok36 ? "（3.8 失败）" : "（转换失败）"));
    FindClose(h);
    return ok36 && ok38;
}

static int find_first_file(const wchar_t *dir, const wchar_t *pattern,
                           wchar_t *path, int path_size)
{
    wchar_t search[1300];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf(search, 1300, L"%ls\\%ls", dir, pattern);
    h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        FindClose(h);
        return 0;
    }
    swprintf(path, path_size, L"%ls\\%ls", dir, fd.cFileName);
    FindClose(h);
    return 1;
}

int sticker_unpack_file(const char *resource_name,
                        const wchar_t *raw_dir,
                        const wchar_t *spine_dir,
                        const wchar_t *png_dir,
                        int work_index)
{
    char id[256];
    wchar_t wid[256];
    wchar_t raw_file[1300], spine_sub[1300], marker[1300], first_png[1300];
    wchar_t assetstudio[1200], exedir[1024], outdir[1200];
    wchar_t atlas[1300], png[1300];
    int copied = 0;

    if (!sticker_id_from_resource(resource_name, id, sizeof id))
        return 0;

    utf8_to_wide(id, wid, sizeof wid / sizeof wid[0]);
    swprintf(raw_file, 1300, L"%ls\\%hs", raw_dir, base_name(resource_name));
    swprintf(spine_sub, 1300, L"%ls\\%ls", spine_dir, wid);
    swprintf(marker, 1300, L"%ls\\done.txt", spine_sub);
    swprintf(first_png, 1300, L"%ls\\%hs_1.png", png_dir, id);

    if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(first_png) != INVALID_FILE_ATTRIBUTES) {
        return 1;
    }

    if (GetFileAttributesW(raw_file) == INVALID_FILE_ATTRIBUTES) {
        printf("  找不到贴纸资源文件: %s\n", resource_name);
        return 0;
    }

    find_assetstudio(assetstudio, sizeof assetstudio / sizeof assetstudio[0]);
    if (!assetstudio[0]) {
        printf("  找不到 AssetStudio.CLI.exe，只保留已下载文件\n");
        return 0;
    }

    GetModuleFileNameW(NULL, exedir, sizeof exedir / sizeof exedir[0]);
    {
        wchar_t *p = wcsrchr(exedir, L'\\');
        if (p) *p = 0;
    }
    swprintf(outdir, 1200, L"%ls\\AssetStudio_out\\st%03d", exedir, work_index);
    wipe_dir(outdir);
    mkdirs(outdir);
    mkdirs(spine_sub);
    mkdirs(png_dir);

    if (!run_assetstudio(assetstudio, raw_file, outdir))
        return 0;

    copied += copy_dir(outdir, L"TextAsset", spine_sub, L"*.skel*");
    copied += copy_dir(outdir, L"TextAsset", spine_sub, L"*.atlas*");
    copied += copy_dir(outdir, L"Texture2D", spine_sub, L"*.png");
    if (copied == 0) {
        printf("  AssetStudio 没有导出贴纸 Spine 文件\n");
        return 0;
    }

    convert_sticker_skel(spine_sub);

    atlas[0] = 0;
    png[0] = 0;
    find_first_file(spine_sub, L"*.atlas*", atlas, 1300);
    find_first_file(spine_sub, L"*.png", png, 1300);
    if (atlas[0] && png[0]) {
        int frames = crop_atlas_regions(atlas, png, png_dir, wid);
        printf("  -> 贴纸PNG\\%ls_1.png / _2.png（%d 帧）\n", wid, frames);
    } else {
        printf("  没找到 atlas/png，跳过裁剪\n");
    }

    {
        FILE *mf = _wfopen(marker, L"wb");
        if (mf) {
            fputs("done", mf);
            fclose(mf);
        }
    }
    return 1;
}
