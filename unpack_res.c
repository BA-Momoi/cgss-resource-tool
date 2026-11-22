// unpack_res.c: 角色资源解包（卡面/背景/卡面Spina动画/3d照片/spine -> png/数据）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "util.h"
#include "unpack.h"
#include "spine_convert.h"
#include "texture_merge.h"
#include "cg.h"

#define MAX_RES_ITEMS 1024
#define RES_UNPACK_WORKERS 2
#define RES_PROBE_WORKERS 4

typedef enum {
    RES_FLAT,
    RES_CARD_SPINE,
    RES_SHARED_SPINE,
    RES_STANDALONE_SPINE,
    RES_STANDALONE_UNITY
} ResOutput;

typedef enum { RES_SCAN_KNOWN, RES_SCAN_IDENTIFY, RES_SCAN_SPINE_ONLY } ResScan;

typedef struct {
    wchar_t path[1100];
    wchar_t dir[1100];
    char name[256];
    char folder_name[256];
    char sub[64];
    int done;
    ResOutput output;
    FILETIME source_time;
} ResUnpackItem;

static const wchar_t *resource_subs[] = {
    L"卡面", L"背景", L"卡面Spina动画", L"live2d", L"3d照片", L"spine"
};
static const char *resource_subs_u8[] = {
    "卡面", "背景", "卡面Spina动画", "live2d(旧)", "3d照片", "spine"
};

static void res_output_dir(const ResUnpackItem *it, wchar_t *dir, size_t capacity){
    if (it->output == RES_STANDALONE_SPINE || it->output == RES_STANDALONE_UNITY){
        const wchar_t *file = wcsrchr(it->path, L'\\') + 1;
        const wchar_t *ext = wcsrchr(file, L'.');
        const wchar_t *category = it->output == RES_STANDALONE_SPINE ? L"spine" : L"导出";
        swprintf(dir, capacity, L"%ls\\%ls\\%.*ls", it->dir, category, (int)(ext - file), file);
    } else {
        swprintf(dir, capacity, L"%ls%ls", it->dir, it->output == RES_CARD_SPINE ? L"\\spine" : L"");
    }
}

static void res_done_path(const ResUnpackItem *it, wchar_t *path, size_t capacity){
    if (it->output == RES_STANDALONE_SPINE || it->output == RES_STANDALONE_UNITY){
        wchar_t dir[1300];
        res_output_dir(it, dir, sizeof dir / sizeof dir[0]);
        swprintf(path, capacity, L"%ls\\.done", dir);
    } else {
        swprintf(path, capacity, L"%ls.done", it->path);
    }
}

static void mark_res_kind(ResUnpackItem *it, int standalone){
    if (standalone){
        it->output = RES_STANDALONE_SPINE;
        snprintf(it->sub, sizeof it->sub, "Spine");
    }
    wchar_t marker[1300];
    res_done_path(it, marker, sizeof marker / sizeof marker[0]);
    WIN32_FILE_ATTRIBUTE_DATA done;
    it->done = GetFileAttributesExW(marker, GetFileExInfoStandard, &done) &&
               (!standalone || CompareFileTime(&done.ftLastWriteTime, &it->source_time) >= 0);
}

static int read_spine_index(const wchar_t *path, const WIN32_FILE_ATTRIBUTE_DATA *input);

typedef struct {
    ResUnpackItem *items;
    int *pending;
    int count;
    volatile LONG next;
} ResProbeQueue;

static DWORD WINAPI res_probe_worker(LPVOID param){
    ResProbeQueue *queue = (ResProbeQueue *)param;
    for (;;){
        LONG next = InterlockedIncrement(&queue->next) - 1;
        if (next >= queue->count) break;
        ResUnpackItem *it = &queue->items[queue->pending[next]];
        mark_res_kind(it, probe_spine_bundle(it->path) == 1);
    }
    return 0;
}

static void identify_spine_items(ResUnpackItem *items, int start, int end){
    int pending[MAX_RES_ITEMS];
    ResProbeQueue queue = {items, pending, 0, 0};
    for (int i = start; i < end; i++){
        WIN32_FILE_ATTRIBUTE_DATA input;
        int found = GetFileAttributesExW(items[i].path, GetFileExInfoStandard, &input)
            ? read_spine_index(items[i].path, &input) : -1;
        if (found < 0) pending[queue.count++] = i;
        else mark_res_kind(&items[i], found == 1);
    }
    HANDLE workers[RES_PROBE_WORKERS - 1];
    int started = 0;
    for (; started < RES_PROBE_WORKERS - 1 && started + 1 < queue.count; started++){
        workers[started] = CreateThread(NULL, 0, res_probe_worker, &queue, 0, NULL);
        if (!workers[started]) break;
    }
    if (queue.count > 0) res_probe_worker(&queue);
    for (int i = 0; i < started; i++){
        WaitForSingleObject(workers[i], INFINITE);
        CloseHandle(workers[i]);
    }
}

static void finish_spine_scan(ResUnpackItem *items, int start, int *n, ResScan scan){
    if (scan == RES_SCAN_KNOWN) return;
    identify_spine_items(items, start, *n);
    if (scan != RES_SCAN_SPINE_ONLY) return;
    int kept = start;
    for (int i = start; i < *n; i++){
        if (items[i].output != RES_STANDALONE_SPINE) continue;
        if (kept != i) items[kept] = items[i];
        kept++;
    }
    *n = kept;
}

static void scan_res_files(const wchar_t *res_dir, const char *sub_u8,
                           ResUnpackItem *items, int *n, const char *folder_name,
                           ResOutput output, ResScan scan){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*.unity3d", res_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int start = *n;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (*n == MAX_RES_ITEMS){
            if (scan != RES_SCAN_SPINE_ONLY) break;
            finish_spine_scan(items, start, n, scan);
            start = *n;
            if (*n == MAX_RES_ITEMS) break;
        }
        ResUnpackItem *item = &items[*n];
        if (swprintf(item->path, 1100, L"%ls\\%ls", res_dir, fd.cFileName) < 0) continue;
        swprintf(item->dir, 1100, L"%ls", res_dir);
        wide_to_utf8(fd.cFileName, item->name, sizeof item->name);
        snprintf(item->folder_name, sizeof item->folder_name, "%s", folder_name);
        snprintf(item->sub, sizeof item->sub, "%s", sub_u8);
        item->output = output;
        item->source_time = fd.ftLastWriteTime;
        if (scan == RES_SCAN_KNOWN) mark_res_kind(item, 0);
        (*n)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    finish_spine_scan(items, start, n, scan);
}

static void scan_res_dir(const wchar_t *chara_dir, const wchar_t *sub, const char *sub_u8,
                           ResUnpackItem *items, int *n, const char *folder_name, ResOutput output){
    wchar_t res_dir[1300];
    swprintf(res_dir, 1300, L"%ls\\%ls", chara_dir, sub);
    scan_res_files(res_dir, sub_u8, items, n, folder_name, output, RES_SCAN_KNOWN);
}

/* 把共享骨架包自带的模板皮肤（卯月/杏示例）挪到子文件夹，避免和卡自己的小人混在一起 */
static void move_shared_template_samples(const wchar_t *dir){
    /* 需要挪走的文件前缀（共享骨架包 SPSprachen_N/s 的贴图与图集；骨架本体保留） */
    static const wchar_t *samples[] = {
        L"SPSprachen_N.png", L"SPSprachen_N.atlas", L"SPSprachen_N.atlas.asset",
        L"SPSprachen_N.atlas.atlas", L"SPSprachen_N_Atlas.json", L"SPSprachen_N_SkeletonData.json",
        L"SPSprachen_N.skel", L"SPSprachen_N.skel.asset", L"SPSprachen_N.json", L"SPSprachen_N_v38.json",
        L"SPSprachen_s.png", L"SPSprachen_s.atlas", L"SPSprachen_s.atlas.asset",
        L"SPSprachen_s.atlas.atlas", L"SPSprachen_s_Atlas.json", L"SPSprachen_s_SkeletonData.json",
    };
    wchar_t subdir[1300];
    swprintf(subdir, 1300, L"%ls\\模板示例(卯月杏)", dir);
    mkdirs(subdir);
    int moved = 0;
    for (int i = 0; i < (int)(sizeof samples / sizeof samples[0]); i++){
        wchar_t src[1300], dst[1300];
        swprintf(src, 1300, L"%ls\\%ls", dir, samples[i]);
        if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) continue;
        swprintf(dst, 1300, L"%ls\\%ls", subdir, samples[i]);
        if (MoveFileW(src, dst)) moved++;
    }
    /* N 骨架是旧卡小人用的（旧卡用 s 会"大头"），在 spine 根目录也保留一份方便使用 */
    static const wchar_t *keepN[] = {
        L"SPSprachen_N.skel", L"SPSprachen_N.skel.asset",
        L"SPSprachen_N.json", L"SPSprachen_N_v38.json"
    };
    for (int i = 0; i < (int)(sizeof keepN / sizeof keepN[0]); i++){
        wchar_t src[1300], dst[1300];
        swprintf(src, 1300, L"%ls\\%ls", subdir, keepN[i]);
        swprintf(dst, 1300, L"%ls\\%ls", dir, keepN[i]);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(src, dst, FALSE);
    }
    if (moved > 0)
        printf("  已把共享骨架的模板示例(卯月/杏)移到 模板示例(卯月杏)\\ 子文件夹\n");
}

/* 解包单个角色资源包：导出 png/数据文件到原目录 */

static void make_res_outdir(int idx, wchar_t *outdir, size_t outdir_size){
    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    swprintf(outdir, outdir_size, L"%ls\\AssetStudio_out\\r%03d", exedir, idx);
}

static int run_assetstudio(const wchar_t *input, const wchar_t *outdir, HANDLE log){
    wchar_t exe[1200], cmd[4096];
    find_assetstudio(exe, 1200);
    if (!exe[0]){
        if (!log) printf("找不到 AssetStudio.CLI.exe，请把 AssetStudio 文件夹放到程序同目录\n");
        return -1;
    }
    if (swprintf(cmd, 4096, L"\"%ls\" \"%ls\" \"%ls\" --game Normal%ls", exe, input, outdir,
                 log ? L" --types TextAsset --logger_flags Info" : L"") < 0) return -1;
    STARTUPINFOEXW si = {0};
    PROCESS_INFORMATION pi;
    DWORD flags = 0;
    si.StartupInfo.cb = sizeof si.StartupInfo;
    if (log){
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        si.lpAttributeList = malloc(size);
        if (!si.lpAttributeList) return -1;
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)){
            free(si.lpAttributeList);
            return -1;
        }
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       &log, sizeof log, NULL, NULL)){
            DeleteProcThreadAttributeList(si.lpAttributeList);
            free(si.lpAttributeList);
            return -1;
        }
        flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = log;
        si.StartupInfo.hStdOutput = log;
        si.StartupInfo.hStdError = log;
    }
    BOOL created = CreateProcessW(exe, cmd, NULL, NULL, log != NULL, flags, NULL, NULL,
                                  &si.StartupInfo, &pi);
    if (si.lpAttributeList){
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
    }
    if (!created){
        if (!log) printf("启动 AssetStudio.CLI 失败 err=%lu\n", (unsigned long)GetLastError());
        return -1;
    }
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, log ? 30000 : INFINITE);
    DWORD process_rc = 1;
    if (wait_rc == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &process_rc);
    else if (log && TerminateProcess(pi.hProcess, 1))
        WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (wait_rc != WAIT_OBJECT_0 || process_rc != 0){
        if (!log) printf("AssetStudio 解包失败，退出码 %lu\n", (unsigned long)process_rc);
        return -1;
    }
    return 0;
}

static int scan_spine_text_assets(const wchar_t *dir){
    wchar_t pattern[1300];
    swprintf(pattern, 1300, L"%ls\\TextAsset\\*", dir);
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    int spine = 0;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t file[1300];
        swprintf(file, 1300, L"%ls\\TextAsset\\%ls", dir, entry.cFileName);
        FILE *f = _wfopen(file, L"rb");
        if (!f) continue;
        char head[512];
        size_t n = fread(head, 1, sizeof head - 1, f);
        fclose(f);
        head[n] = '\0';
        if ((n >= 4 && memcmp(head, "cysp", 4) == 0) ||
            (n >= 35 && head[0] == 0x1c && head[29] >= '2' && head[29] <= '4' && head[30] == '.') ||
            (strstr(head, "\"skeleton\"") && strstr(head, "\"spine\"") && strstr(head, "\"bones\"")) ||
            (strstr(head, "\nsize:") && strstr(head, "\nfilter:") && strstr(head, ".png"))){
            spine = 1;
            break;
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return spine;
}

static int read_spine_index(const wchar_t *path, const WIN32_FILE_ATTRIBUTE_DATA *input){
    wchar_t cache[1300];
    if (swprintf(cache, 1300, L"%ls.spine-index-v1", path) < 0) return -1;
    DWORD key[] = {input->ftLastWriteTime.dwLowDateTime, input->ftLastWriteTime.dwHighDateTime,
                   input->nFileSizeLow, input->nFileSizeHigh};
    DWORD record[5], bytes;
    HANDLE index = CreateFileW(cache, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (index != INVALID_HANDLE_VALUE){
        int match = ReadFile(index, record, sizeof record, &bytes, NULL) && bytes == sizeof record &&
                    memcmp(record, key, sizeof key) == 0 && record[4] <= 1;
        CloseHandle(index);
        if (match) return (int)record[4];
    }
    return -1;
}

static int make_hidden_scratch(wchar_t scratch[MAX_PATH]){
    wchar_t base[MAX_PATH];
    DWORD base_len = GetTempPathW(MAX_PATH, base);
    if (!base_len || base_len >= MAX_PATH || !GetTempFileNameW(base, L"cgs", 0, scratch)) return -1;
    if (!DeleteFileW(scratch) || !CreateDirectoryW(scratch, NULL)) return -1;
    SetFileAttributesW(scratch, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY);
    return 0;
}

static void clear_scratch(const wchar_t *scratch){
    wipe_dir(scratch);
    RemoveDirectoryW(scratch);
}

int probe_spine_bundle(const wchar_t *path){
    WIN32_FILE_ATTRIBUTE_DATA input;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &input) ||
        (input.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    int cached = read_spine_index(path, &input);
    if (cached >= 0) return cached;

    FILE *source = _wfopen(path, L"rb");
    if (!source) return -1;
    char signature[8];
    size_t n = fread(signature, 1, sizeof signature, source);
    fclose(source);
    if (n != sizeof signature || memcmp(signature, "UnityFS", 7) != 0) return -1;

    wchar_t scratch[MAX_PATH], log_path[MAX_PATH + 16];
    if (make_hidden_scratch(scratch) != 0) return -1;
    swprintf(log_path, MAX_PATH + 16, L"%ls\\scan.log", scratch);
    SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
    HANDLE log = CreateFileW(log_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             &security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    int result = -1;
    DWORD bytes;
    if (log != INVALID_HANDLE_VALUE){
        if (run_assetstudio(path, scratch, log) == 0){
            char tail[8192];
            LARGE_INTEGER size, offset;
            if (GetFileSizeEx(log, &size)){
                offset.QuadPart = size.QuadPart > sizeof tail - 1 ? size.QuadPart - (sizeof tail - 1) : 0;
                if (SetFilePointerEx(log, offset, NULL, FILE_BEGIN) &&
                    ReadFile(log, tail, sizeof tail - 1, &bytes, NULL)){
                    tail[bytes] = '\0';
                    if (strstr(tail, "Finished exporting") || strstr(tail, "Nothing exported."))
                        result = scan_spine_text_assets(scratch);
                }
            }
        }
        CloseHandle(log);
    }
    clear_scratch(scratch);

    if (result >= 0){
        DWORD key[] = {input.ftLastWriteTime.dwLowDateTime, input.ftLastWriteTime.dwHighDateTime,
                       input.nFileSizeLow, input.nFileSizeHigh, (DWORD)result};
        wchar_t cache[1300];
        if (swprintf(cache, 1300, L"%ls.spine-index-v1", path) < 0) return result;
        HANDLE index = CreateFileW(cache, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (index != INVALID_HANDLE_VALUE){
            WriteFile(index, key, sizeof key, &bytes, NULL);
            CloseHandle(index);
        }
    }
    return result;
}

static int run_res_assetstudio(const ResUnpackItem *it, int idx){
    wchar_t outdir[1200];
    make_res_outdir(idx, outdir, sizeof outdir / sizeof outdir[0]);
    wipe_dir(outdir);
    mkdirs(outdir);
    printf("解包 %s\\%s ...\n", it->sub, it->name);
    fflush(stdout);
    return run_assetstudio(it->path, outdir, NULL);
}

static void normalize_spine_binary_names(const wchar_t *outdir){
    wchar_t pattern[1300];
    swprintf(pattern, 1300, L"%ls\\TextAsset\\*.bytes", outdir);
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t length = wcslen(entry.cFileName);
        if (length <= 11) continue;
        const wchar_t *ext = entry.cFileName + length - 11;
        if (_wcsicmp(ext, L".skel.bytes") != 0 && _wcsicmp(ext, L".cysp.bytes") != 0) continue;
        wchar_t src[1300], dst[1300];
        swprintf(src, 1300, L"%ls\\TextAsset\\%ls", outdir, entry.cFileName);
        wcscpy(dst, src);
        dst[wcslen(dst) - 6] = L'\0';
        MoveFileW(src, dst);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

static int finish_res_one(const ResUnpackItem *it, const wchar_t *outdir){
    int n = 0;
    wchar_t destdir[1300];
    res_output_dir(it, destdir, sizeof destdir / sizeof destdir[0]);
    mkdirs(destdir);
    n += copy_dir(outdir, L"Texture2D", destdir, L"*.png");
    n += copy_dir(outdir, L"Texture2D", destdir, L"*.tga");
    n += copy_dir(outdir, L"Sprite", destdir, L"*.png");
    normalize_spine_binary_names(outdir);
    n += copy_dir(outdir, L"TextAsset", destdir, L"*");
    if (it->output == RES_FLAT || it->output == RES_STANDALONE_UNITY)
        n += copy_dir(outdir, L"MonoBehaviour", destdir, L"*.json");
    n += copy_dir(outdir, L"AudioClip", destdir, L"*");
    if (it->output == RES_STANDALONE_UNITY){
        n += copy_dir(outdir, L"AnimationClip", destdir, L"*");
        n += copy_dir(outdir, L"Material", destdir, L"*");
        n += copy_dir(outdir, L"Shader", destdir, L"*");
        n += copy_dir(outdir, L"MonoBehaviour", destdir, L"*.asset");
    }

    /* 卡面Spina动画/live2d 解出的 .skel 自动转一份 .json，方便浏览器预览 */
    int cn = convert_skels_in_dir(destdir);
    if (cn > 0)
        printf("  已转换 %d 个 skel 为 json（3.6 + 3.8.75，可用主菜单4预览）\n", cn);
    /* atlas 导出名为 *.atlas.asset，再复制一份 *.atlas 方便 Spine 编辑器直接打开 */
    {
        wchar_t apat[1300];
        swprintf(apat, 1300, L"%ls\\*.atlas.asset", destdir);
        WIN32_FIND_DATAW afd;
        HANDLE ah = FindFirstFileW(apat, &afd);
        if (ah != INVALID_HANDLE_VALUE){
            do {
                if (afd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t asrc[1300], adst[1300];
                swprintf(asrc, 1300, L"%ls\\%ls", destdir, afd.cFileName);
                wcscpy(adst, asrc);
                /* 去掉尾部 .asset：SPC301346.atlas.asset -> SPC301346.atlas */
                size_t alen = wcslen(adst);
                if (alen > 6 && _wcsicmp(adst + alen - 6, L".asset") == 0)
                    adst[alen - 6] = 0;
                if (it->output == RES_STANDALONE_SPINE || GetFileAttributesW(adst) == INVALID_FILE_ATTRIBUTES)
                    CopyFileW(asrc, adst, FALSE);
            } while (FindNextFileW(ah, &afd));
            FindClose(ah);
        }
        /* 清理旧版本误生成的 *.atlas.atlas（内容与 *.atlas 相同） */
        {
            wchar_t apat2[1300];
            swprintf(apat2, 1300, L"%ls\\*.atlas.atlas", destdir);
            WIN32_FIND_DATAW afd2;
            HANDLE ah2 = FindFirstFileW(apat2, &afd2);
            if (ah2 != INVALID_HANDLE_VALUE){
                do {
                    if (afd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    wchar_t adup[1300], aclean[1300];
                    swprintf(adup, 1300, L"%ls\\%ls", destdir, afd2.cFileName);
                    wcscpy(aclean, adup);
                    size_t alen2 = wcslen(aclean);
                    if (alen2 > 6 && _wcsicmp(aclean + alen2 - 6, L".atlas") == 0)
                        aclean[alen2 - 6] = 0;   /* X.atlas.atlas -> X.atlas */
                    if (GetFileAttributesW(aclean) != INVALID_FILE_ATTRIBUTES)
                        DeleteFileW(adup);
                } while (FindNextFileW(ah2, &afd2));
                FindClose(ah2);
            }
        }
    }
    /* 合成 RGB + A8 贴图，并生成引用它的 v38 atlas（Spine 3.8.75 编辑器用） */
    if (it->output == RES_CARD_SPINE || it->output == RES_STANDALONE_SPINE){
        int mn = merge_a8_textures_in_dir(destdir);
        if (mn > 0)
            printf("  已合成 %d 张贴图（3.8.75 编辑器用）\n", mn);
    }
    /* 共享骨架包自带的模板皮肤（卯月/杏示例）挪到子文件夹 */
    if (it->output == RES_SHARED_SPINE)
        move_shared_template_samples(destdir);

    if (n == 0){
        printf("  未生成可复制文件（可能包里没有图片/数据）\n");
        return -1;
    }
    wchar_t marker[1300];
    res_done_path(it, marker, sizeof marker / sizeof marker[0]);
    FILE *mf = _wfopen(marker, L"wb");
    if (mf){ fputs("done", mf); fclose(mf); }

    printf("  完成，导出 %d 个文件\n", n);
    return n;
}

static int has_export(const wchar_t *dir, const wchar_t *pattern){
    wchar_t path[1300];
    swprintf(path, 1300, L"%ls\\%ls", dir, pattern);
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(path, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    FindClose(search);
    return 1;
}

static int is_current_output(const ResUnpackItem *item){
    wchar_t marker[1300];
    res_done_path(item, marker, _countof(marker));
    WIN32_FILE_ATTRIBUTE_DATA done;
    return GetFileAttributesExW(marker, GetFileExInfoStandard, &done) &&
           CompareFileTime(&done.ftLastWriteTime, &item->source_time) >= 0;
}

int unpack_downloaded_unity(const wchar_t *path, const wchar_t *dir){
    static SRWLOCK publish = SRWLOCK_INIT;
    ResUnpackItem item = {0};
    if (wcslen(path) >= _countof(item.path) || wcslen(dir) >= _countof(item.dir)) return -1;
    wcscpy(item.path, path);
    wcscpy(item.dir, dir);
    const wchar_t *leaf = wcsrchr(dir, L'\\');
    leaf = leaf ? leaf + 1 : dir;
    WIN32_FILE_ATTRIBUTE_DATA source;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &source)) return -1;
    item.source_time = source.ftLastWriteTime;
    const int known_model = _wcsicmp(leaf, L"3D模型") == 0;
    const int known_flat = _wcsicmp(leaf, L"卡面") == 0 || _wcsicmp(leaf, L"背景") == 0 ||
                           _wcsicmp(leaf, L"3d照片") == 0;
    if (_wcsicmp(leaf, L"spine") == 0) item.output = RES_SHARED_SPINE;
    else if (_wcsicmp(leaf, L"Live2D") == 0 || _wcsicmp(leaf, L"卡面Spina动画") == 0 ||
             _wcsicmp(leaf, L"Spine_Live") == 0) item.output = RES_CARD_SPINE;
    else item.output = known_model || known_flat ? RES_FLAT : RES_STANDALONE_UNITY;
    if (is_current_output(&item)) return 0;
    if (item.output == RES_STANDALONE_UNITY){
        item.output = RES_STANDALONE_SPINE;
        if (is_current_output(&item)) return 0;
        item.output = RES_STANDALONE_UNITY;
    }
    wchar_t scratch[MAX_PATH];
    if (make_hidden_scratch(scratch) != 0) return -1;
    int result = -1;
    if (run_assetstudio(path, scratch, NULL) == 0){
        const int custom = item.output == RES_STANDALONE_UNITY;
        if (custom && scan_spine_text_assets(scratch)) item.output = RES_STANDALONE_SPINE;
        const int model = known_model || (item.output == RES_STANDALONE_UNITY &&
                          (has_export(scratch, L"Animator\\*.fbx") || has_export(scratch, L"Mesh\\*.obj")));
        AcquireSRWLockExclusive(&publish);
        if (model){
            wchar_t dest[1300];
            res_output_dir(&item, dest, _countof(dest));
            mkdirs(dest);
            result = copy_fbx_outputs(scratch, dest) > 0 ? 0 : -1;
            if (result == 0){
                wchar_t marker[1300];
                res_done_path(&item, marker, _countof(marker));
                FILE *file = _wfopen(marker, L"wb");
                if (file){ fputs("done", file); fclose(file); }
            }
        } else {
            result = finish_res_one(&item, scratch) > 0 ? 0 : -1;
        }
        ReleaseSRWLockExclusive(&publish);
    }
    clear_scratch(scratch);
    return result;
}

static void process_existing_files(const wchar_t *root, int video_pass, int *count, int *failed){
    wchar_t pattern[1300];
    if (swprintf(pattern, _countof(pattern), L"%ls\\*", root) < 0) return;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (entry.cFileName[0] == L'.' || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wchar_t path[1300];
        if (swprintf(path, _countof(path), L"%ls\\%ls", root, entry.cFileName) < 0) continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if (wcscmp(entry.cFileName, L"原文件unity3d") != 0 &&
                wcsncmp(entry.cFileName, L"_acb_", 5) != 0)
                process_existing_files(path, video_pass, count, failed);
            continue;
        }
        const wchar_t *ext = wcsrchr(entry.cFileName, L'.');
        if (!ext) continue;
        int result;
        if (!video_pass && _wcsicmp(ext, L".unity3d") == 0) result = unpack_downloaded_unity(path, root);
        else if (!video_pass && _wcsicmp(ext, L".acb") == 0) result = unpack_acb_file(path);
        else if (video_pass && _wcsicmp(ext, L".usm") == 0) result = unpack_usm_file(path);
        else continue;
        (*count)++;
        if (result < 0){
            (*failed)++;
            char name[512];
            wide_to_utf8(entry.cFileName, name, _countof(name));
            printf("自动解包失败，已保留原文件：%s\n", name);
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

int process_download_directory(const wchar_t *root){
    DWORD attrs = GetFileAttributesW(root);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)){
        fprintf(stderr, "找不到下载目录：%ls\n", root);
        return 1;
    }
    int count = 0, failed = 0;
    process_existing_files(root, 0, &count, &failed);
    process_existing_files(root, 1, &count, &failed);
    printf("补处理完成，共检查 %d 个资源，失败 %d 个。\n", count, failed);
    return failed ? 1 : 0;
}

typedef struct {
    const ResUnpackItem *items;
    const int *selected;
    int count;
    volatile LONG next;
    int *status;
} ResQueue;

static DWORD WINAPI res_unpack_worker(LPVOID param){
    ResQueue *queue = (ResQueue *)param;
    for (;;){
        LONG task = InterlockedIncrement(&queue->next) - 1;
        if (task < 0 || task >= queue->count) break;
        queue->status[task] = run_res_assetstudio(
            &queue->items[queue->selected[task] - 1], (int)task);
    }
    return 0;
}

static void run_selected_assetstudio(const ResUnpackItem *items, const int *selected,
                                     int count, int *status){
    ResQueue queue = {items, selected, count, 0, status};
    if (count <= 1){
        res_unpack_worker(&queue);
        return;
    }
    HANDLE workers[RES_UNPACK_WORKERS];
    unsigned started = 0;
    unsigned worker_count = (unsigned)(count < RES_UNPACK_WORKERS ? count : RES_UNPACK_WORKERS);
    for (; started < worker_count; started++){
        workers[started] = CreateThread(NULL, 0, res_unpack_worker, &queue, 0, NULL);
        if (!workers[started]) break;
    }
    if (started == 0){
        res_unpack_worker(&queue);
        return;
    }
    if (started < worker_count)
        res_unpack_worker(&queue);
    WaitForMultipleObjects(started, workers, TRUE, INFINITE);
    for (unsigned i = 0; i < started; i++) CloseHandle(workers[i]);
}

int unpack_resources_main(void) {
    wchar_t root[1024];
    GetModuleFileNameW(NULL, root, 1024);
    wchar_t *p = wcsrchr(root, L'\\');
    if (p) *p = 0;
    wcscat(root, L"\\CGSS_DOWN");
    return unpack_resources_path(root, 1);
}

static void scan_character_dir(const wchar_t *chara_dir, ResUnpackItem *items,
                               int *n, const char *folder_name){
    for (int s = 0; s < (int)(sizeof resource_subs / sizeof resource_subs[0]); s++){
        ResOutput output = s == 2 || s == 3 ? RES_CARD_SPINE : s == 5 ? RES_SHARED_SPINE : RES_FLAT;
        scan_res_dir(chara_dir, resource_subs[s], resource_subs_u8[s], items, n,
                     folder_name, output);
    }
}

static void path_leaf_utf8(const wchar_t *path, char *out, size_t out_size){
    wchar_t copy[1300];
    wcsncpy(copy, path, sizeof copy / sizeof copy[0] - 1);
    copy[sizeof copy / sizeof copy[0] - 1] = 0;
    size_t len = wcslen(copy);
    while (len > 3 && (copy[len - 1] == L'\\' || copy[len - 1] == L'/'))
        copy[--len] = 0;
    wchar_t *leaf = wcsrchr(copy, L'\\');
    wchar_t *alt = wcsrchr(copy, L'/');
    if (!leaf || (alt && alt > leaf)) leaf = alt;
    wide_to_utf8(leaf ? leaf + 1 : copy, out, out_size);
}

static void collect_resource_items(const wchar_t *root, ResUnpackItem *items, int *n){
    char root_name[256];
    path_leaf_utf8(root, root_name, sizeof root_name);
    int before = *n;
    scan_character_dir(root, items, n, root_name);
    scan_res_files(root, root_name, items, n, root_name, RES_FLAT, RES_SCAN_IDENTIFY);
    const int direct = *n > before;
    wchar_t custom[1300];
    swprintf(custom, 1300, L"%ls\\自定义", root);
    scan_res_files(custom, "Spine", items, n, "自定义", RES_FLAT, RES_SCAN_SPINE_ONLY);
    if (direct) return;

    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", root);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
            continue;
        wchar_t chara_dir[1300];
        swprintf(chara_dir, 1300, L"%ls\\%ls", root, fd.cFileName);
        char chara_name[256];
        wide_to_utf8(fd.cFileName, chara_name, sizeof chara_name);
        scan_character_dir(chara_dir, items, n, chara_name);
    } while (FindNextFileW(h, &fd) && *n < MAX_RES_ITEMS);
    FindClose(h);
}

int unpack_resources_path(const wchar_t *root, int interactive){
    DWORD attrs = GetFileAttributesW(root);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)){
        char path[1300];
        wide_to_utf8(root, path, sizeof path);
        fprintf(stderr, "资源目录不存在或不是目录: %s\n", path);
        return 1;
    }

    ResUnpackItem *items = (ResUnpackItem *)calloc(MAX_RES_ITEMS, sizeof *items);
    if (!items){
        fprintf(stderr, "内存不足\n");
        return 1;
    }
    int n = 0;
    collect_resource_items(root, items, &n);
    if (n == 0){
        printf("目录中没有找到可解包的 unity3d 资源\n");
        free(items);
        return 1;
    }

    int *selected = (int *)malloc(sizeof *selected * (size_t)n);
    if (!selected){
        fprintf(stderr, "内存不足\n");
        free(items);
        return 1;
    }
    int nsel = 0;
    if (interactive){
        int ndone = 0;
        for (int i = 0; i < n; i++){
            printf("[%d] %s : %s\\%s %s\n", i + 1, items[i].folder_name,
                   items[i].sub, items[i].name, items[i].done ? "[已解包]" : "[待解包]");
            if (items[i].done) ndone++;
        }
        printf("共 %d 个资源包，已解包 %d 个（a=解包全部未解包的，输编号可强制重解，0=返回）：", n, ndone);
        char buf[128];
        if (!fgets(buf, sizeof buf, stdin)){
            free(selected);
            free(items);
            return 1;
        }
        nsel = parse_multi(buf, selected, n);
        if (nsel < 0){
            nsel = 0;
            for (int i = 0; i < n; i++)
                if (!items[i].done) selected[nsel++] = i + 1;
            if (nsel == 0) printf("都已解包，无需处理\n");
        }
    } else {
        for (int i = 0; i < n; i++)
            if (!items[i].done) selected[nsel++] = i + 1;
        if (nsel == 0) printf("都已解包，无需处理\n");
    }

    int failed = 0;
    if (nsel > 0){
        int *status = (int *)calloc((size_t)nsel, sizeof *status);
        if (!status){
            fprintf(stderr, "内存不足\n");
            free(selected);
            free(items);
            return 1;
        }
        run_selected_assetstudio(items, selected, nsel, status);
        for (int i = 0; i < nsel; i++){
            if (status[i] < 0){
                failed++;
                continue;
            }
            wchar_t outdir[1200];
            make_res_outdir(i, outdir, sizeof outdir / sizeof outdir[0]);
            if (finish_res_one(&items[selected[i] - 1], outdir) < 0) failed++;
        }
        free(status);
        printf("解包完成: 成功 %d，失败 %d\n", nsel - failed, failed);
    }
    free(selected);
    free(items);
    return failed ? 1 : 0;
}
