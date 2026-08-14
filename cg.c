/* cg.c: CG/USM 解包
 *
 * CG 命名规律(已实测确认):
 *   影片:  m/AnivCount/<NNN>/movie_XXXX.usm
 *   高清:  m/AnivCount/<NNN>/movie_XXXX_alt.usm   (带 _alt 的是高清版)
 *   音频:  m/bgm_anivcount_<NNN>_movie_XXXX.acb   (和影片配对, 同一个 movie id)
 *   其他:  m/live/high/2drichXXXX.usm              (2D live 背景)
 *
 * 功能:
 *   1. 自定义 USM 解包: 给一个文件或目录, 解出 mp4, 同目录配对 acb 解 wav
 *   2. 解包已下载的 CG: 扫描 CGSS_DOWN\CG 里的分组, 选中一个全解
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <direct.h>
#include <windows.h>
#include "paper.h"
#include "util.h"
#include "cg.h"

/* ================== CRID 解密(和 usm.c 同一套) ================== */

static unsigned be32(const unsigned char *p){
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16)
         | ((unsigned)p[2] << 8)  | p[3];
}
static unsigned be16(const unsigned char *p){
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned char videoMask1[0x20], videoMask2[0x20], audioMask[0x20];

static void InitMask(unsigned int key1, unsigned int key2){
    unsigned char t[0x20];
    t[0x00] = ((unsigned char *)&key1)[0];
    t[0x01] = ((unsigned char *)&key1)[1];
    t[0x02] = ((unsigned char *)&key1)[2];
    t[0x03] = ((unsigned char *)&key1)[3] - 0x34;
    t[0x04] = ((unsigned char *)&key2)[0] + 0xF9;
    t[0x05] = ((unsigned char *)&key2)[1] ^ 0x13;
    t[0x06] = ((unsigned char *)&key2)[2] + 0x61;
    t[0x07] = t[0x00] ^ 0xFF;
    t[0x08] = t[0x02] + t[0x01];
    t[0x09] = t[0x01] - t[0x07];
    t[0x0A] = t[0x02] ^ 0xFF;
    t[0x0B] = t[0x01] ^ 0xFF;
    t[0x0C] = t[0x0B] + t[0x09];
    t[0x0D] = t[0x08] - t[0x03];
    t[0x0E] = t[0x0D] ^ 0xFF;
    t[0x0F] = t[0x0A] - t[0x0B];
    t[0x10] = t[0x08] - t[0x0F];
    t[0x11] = t[0x10] ^ t[0x07];
    t[0x12] = t[0x0F] ^ 0xFF;
    t[0x13] = t[0x03] ^ 0x10;
    t[0x14] = t[0x04] - 0x32;
    t[0x15] = t[0x05] + 0xED;
    t[0x16] = t[0x06] ^ 0xF3;
    t[0x17] = t[0x13] - t[0x0F];
    t[0x18] = t[0x15] + t[0x07];
    t[0x19] = 0x21 - t[0x13];
    t[0x1A] = t[0x14] ^ t[0x17];
    t[0x1B] = t[0x16] + t[0x16];
    t[0x1C] = t[0x17] + 0x44;
    t[0x1D] = t[0x03] + t[0x04];
    t[0x1E] = t[0x05] - t[0x16];
    t[0x1F] = t[0x1D] ^ t[0x13];

    unsigned char t2[4] = {'U','R','U','C'};
    for (int i = 0; i < 0x20; i++){
        videoMask1[i] = t[i];
        videoMask2[i] = t[i] ^ 0xFF;
        audioMask[i]  = (i & 1) ? t2[(i >> 1) & 3] : t[i] ^ 0xFF;
    }
}

static void MaskVideo(unsigned char *data, int size){
    data += 0x40;
    size -= 0x40;
    if (size < 0x200) return;
    unsigned char mask[0x20];
    memcpy(mask, videoMask2, 0x20);
    for (int i = 0x100; i < size; i++){
        data[i] ^= mask[i & 0x1F];
        mask[i & 0x1F] = data[i] ^ videoMask2[i & 0x1F];
    }
    memcpy(mask, videoMask1, 0x20);
    for (int i = 0; i < 0x100; i++){
        mask[i & 0x1F] ^= data[0x100 + i];
        data[i] ^= mask[i & 0x1F];
    }
}

static void MaskAudio(unsigned char *data, int size){
    data += 0x140;
    size -= 0x140;
    for (int i = 0; i < size; i++)
        data[i] ^= audioMask[i & 0x1F];
}

/* 解一个 usm: 产出 outdir\video.m2v 和 audio.adx(有音频时) */
static int demux_file(const wchar_t *usm_path, const wchar_t *outdir){
    FILE *fp = _wfopen(usm_path, L"rb");
    if (!fp){ printf("打不开 %ls\n", usm_path); return -1; }
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);

    wchar_t vpath[1300], apath[1300];
    swprintf(vpath, 1300, L"%ls\\video.m2v", outdir);
    swprintf(apath, 1300, L"%ls\\audio.adx", outdir);
    FILE *vo = NULL, *ao = NULL;
    InitMask(0xF27E3B22, 0x00003657);

    long pos = 0;
    unsigned nvideo = 0, naudio = 0;
    while (pos < fileSize){
        unsigned char h[32];
        fseek(fp, pos, SEEK_SET);
        if (fread(h, 1, 32, fp) != 32) break;

        unsigned ds  = be32(h + 4);
        unsigned dof = h[9];
        unsigned pad = be16(h + 10);
        unsigned typ = h[15] & 3;
        unsigned dlen = ds - dof - pad;
        unsigned dpos = pos + 8 + dof;

        unsigned char *data = (unsigned char*)malloc(dlen ? dlen : 1);
        fseek(fp, dpos, SEEK_SET);
        fread(data, 1, dlen, fp);

        if (memcmp(h, "@SFV", 4) == 0 && typ == 0){
            MaskVideo(data, dlen);
            if (!vo) vo = _wfopen(vpath, L"wb");
            fwrite(data, 1, dlen, vo);
            nvideo++;
        } else if (memcmp(h, "@SFA", 4) == 0 && typ == 0){
            MaskAudio(data, dlen);
            if (!ao) ao = _wfopen(apath, L"wb");
            fwrite(data, 1, dlen, ao);
            naudio++;
        }
        free(data);
        pos += 8 + ds;
    }
    fclose(fp);
    if (vo) fclose(vo);
    if (ao) fclose(ao);
    printf("解出: 视频块 %u, 音频块 %u\n", nvideo, naudio);
    return (nvideo > 0) ? 0 : -1;
}

/* ================== 转 mp4 / 解 acb ================== */

static void find_ffmpeg(wchar_t *out, int n){
    out[0] = 0;
    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    wchar_t cand[1300];
    swprintf(cand, 1300, L"%ls\\ffmpeg.exe", exedir);
    if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES){
        wcscpy(out, cand);
        return;
    }
    swprintf(cand, 1300, L"D:\\CGSS动作\\工具\\ffmpeg\\ffmpeg.exe");
    if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES){
        wcscpy(out, cand);
        return;
    }
    wcscpy(out, L"ffmpeg");   /* 最后赌 PATH 里有 */
}

/* video.m2v (+音频) -> mp4name
 * extra_wav: 外部配对的音频(acb 解出的 wav), 没有就传 NULL */
static int convert_mp4(const wchar_t *outdir, const wchar_t *mp4name,
                       const wchar_t *extra_wav){
    wchar_t ffmpeg[1024];
    find_ffmpeg(ffmpeg, 1024);
    if (!ffmpeg[0]){ printf("没找到 ffmpeg, 跳过转 mp4(保留 video.m2v)\n"); return -1; }

    wchar_t cmd[4000];
    wchar_t audio_src[1300] = L"";
    if (extra_wav && extra_wav[0] &&
        GetFileAttributesW(extra_wav) != INVALID_FILE_ATTRIBUTES){
        wcscpy(audio_src, extra_wav);
    } else {
        wchar_t adx[1300];
        swprintf(adx, 1300, L"%ls\\audio.adx", outdir);
        if (GetFileAttributesW(adx) != INVALID_FILE_ATTRIBUTES)
            wcscpy(audio_src, adx);
    }
    if (audio_src[0]){
        swprintf(cmd, 4000,
                 L"\"%ls\" -y -i \"%ls\\video.m2v\" -i \"%ls\" "
                 L"-c:v libx264 -pix_fmt yuv420p -crf 18 -c:a aac -shortest \"%ls\\%ls\"",
                 ffmpeg, outdir, audio_src, outdir, mp4name);
    } else {
        swprintf(cmd, 4000,
                 L"\"%ls\" -y -i \"%ls\\video.m2v\" "
                 L"-c:v libx264 -pix_fmt yuv420p -crf 18 \"%ls\\%ls\"",
                 ffmpeg, outdir, outdir, mp4name);
    }
    printf("转换 %ls ...\n", mp4name);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
        printf("启动 ffmpeg 失败(可能没装), 已保留 video.m2v\n");
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("完成 -> %ls\\%ls\n", outdir, mp4name);
    return 0;
}

static void find_acb2wavs(wchar_t *out, int n){
    out[0] = 0;
    wchar_t exedir[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    wchar_t *p = wcsrchr(exedir, L'\\');
    if (p) *p = 0;
    wchar_t cand[1300];
    swprintf(cand, 1300, L"%ls\\acb2wavs.exe", exedir);
    if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES)
        wcscpy(out, cand);
}

static int decode_acb(const wchar_t *acb_path){
    wchar_t tool[1300];
    find_acb2wavs(tool, 1300);
    if (!tool[0]){
        printf("没找到 acb2wavs.exe(放到程序同目录), 跳过音频解码\n");
        return -1;
    }
    wchar_t cmd[2600];
    swprintf(cmd, 2600, L"\"%ls\" \"%ls\"", tool, acb_path);
    printf("解码 %ls ...\n", acb_path);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
        printf("启动 acb2wavs 失败\n");
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("解码完成(输出在 acb 同目录)\n");
    return 0;
}

/* ================== 路径小工具 ================== */

static const wchar_t *wbase(const wchar_t *path){
    const wchar_t *p = wcsrchr(path, L'\\');
    const wchar_t *q = wcsrchr(path, L'/');
    const wchar_t *r = (p && q) ? (p > q ? p : q) : (p ? p : q);
    return r ? r + 1 : path;
}

static void wstrip_ext(wchar_t *s){
    wchar_t *dot = wcsrchr(s, L'.');
    if (dot) *dot = 0;
}

/* 从文件名里提取 movie_XXXX 的数字部分(找不到就是空串) */
static void get_movie_id(const wchar_t *name, wchar_t *out, int n){
    out[0] = 0;
    const wchar_t *p = wcsstr(name, L"movie_");
    if (!p) return;
    p += 6;
    int k = 0;
    while (p[k] && iswdigit(p[k]) && k < n - 1){ out[k] = p[k]; k++; }
    out[k] = 0;
}

/* 找 acb2wavs 解出来的 wav: <acb目录>\_acb_<名>.acb\internal\*.wav */
static int find_wav_from_acb(const wchar_t *acb_path, wchar_t *wav_out, int n){
    wav_out[0] = 0;
    wchar_t dir[1200], base[512];
    wcscpy(dir, acb_path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return -1;
    *slash = 0;
    wcscpy(base, slash + 1);
    wstrip_ext(base);
    wchar_t wdir[1300], pat[1300];
    swprintf(wdir, 1300, L"%ls\\_acb_%ls.acb\\internal", dir, base);
    swprintf(pat, 1300, L"%ls\\*.wav", wdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    swprintf(wav_out, n, L"%ls\\%ls", wdir, fd.cFileName);
    FindClose(h);
    return 0;
}

/* 解一个 usm: 解出 mp4 + 配对 acb 的 wav */
static void unpack_one(const wchar_t *usm_path, const wchar_t *custom_name){
    wchar_t base[512];
    wcscpy(base, wbase(usm_path));
    wstrip_ext(base);

    /* 输出到 usm 同目录下的 "<名字>_解包" */
    wchar_t usmdir[1200];
    wcscpy(usmdir, usm_path);
    wchar_t *slash = wcsrchr(usmdir, L'\\');
    if (slash) *slash = 0; else wcscpy(usmdir, L".");
    wchar_t dir[1200];
    swprintf(dir, 1200, L"%ls\\%ls_解包", usmdir, base);
    mkdirs(dir);

    printf("\n==== 解包 %ls ====\n", usm_path);
    if (demux_file(usm_path, dir) != 0){
        printf("解包失败\n");
        return;
    }

    wchar_t mp4name[512];
    if (custom_name && custom_name[0])
        swprintf(mp4name, 512, L"%ls.mp4", custom_name);
    else
        swprintf(mp4name, 512, L"%ls.mp4", base);
    /* 配对 acb: usm 同目录 / 上级目录 / 上级目录\音频 里,
     * 名字含 movie id 或含文件名的 .acb */
    wchar_t movie[64];
    get_movie_id(usm_path, movie, 64);
    /* 2drich<歌id>.usm 的配对是 song_<歌id>.acb */
    wchar_t songtok[64] = L"";
    if (wcsncmp(base, L"2drich", 6) == 0){
        const wchar_t *d = base + 6;
        int k = 0;
        swprintf(songtok, 64, L"song_");
        while (d[k] && iswdigit(d[k]) && k < 56){
            songtok[5 + k] = d[k];
            k++;
        }
        songtok[5 + k] = 0;
    }
    wchar_t parent[1200], audiodir[1300], first_acb[1300] = L"";
    swprintf(parent, 1200, L"%ls\\..", usmdir);
    swprintf(audiodir, 1300, L"%ls\\..\\音频", usmdir);
    const wchar_t *cands[3];
    cands[0] = usmdir;
    cands[1] = parent;
    cands[2] = audiodir;
    int found = 0;
    for (int c = 0; c < 3 && !found; c++){
        wchar_t pat[1300];
        swprintf(pat, 1300, L"%ls\\*.acb", cands[c]);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            int match = 0;
            if (movie[0] && wcsstr(fd.cFileName, movie)) match = 1;
            if (!match && wcsstr(fd.cFileName, base)) match = 1;
            if (!match && songtok[0] && wcsstr(fd.cFileName, songtok)) match = 1;
            if (match){
                wchar_t acb[1300];
                swprintf(acb, 1300, L"%ls\\%ls", cands[c], fd.cFileName);
                if (!first_acb[0]) wcscpy(first_acb, acb);
                decode_acb(acb);
                found++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!found)
        printf("没找到配对 acb(可手动用 acb2wavs 解)\n");

    /* 有配对 acb 的话, 把解出的 wav 合成进 mp4 */
    wchar_t audio_wav[1300] = L"";
    if (first_acb[0])
        find_wav_from_acb(first_acb, audio_wav, 1300);
    if (audio_wav[0])
        printf("找到音频: %ls, 准备合成进视频\n", audio_wav);
    convert_mp4(dir, mp4name, audio_wav);
}

/* 递归解一个目录里的所有 usm */
static void unpack_folder(const wchar_t *dir){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE){
        printf("目录不存在或为空: %ls\n", dir);
        return;
    }
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            wchar_t sub[1300];
            swprintf(sub, 1300, L"%ls\\%ls", dir, fd.cFileName);
            unpack_folder(sub);
            continue;
        }
        const wchar_t *dot = wcsrchr(fd.cFileName, L'.');
        if (dot && _wcsicmp(dot, L".usm") == 0){
            wchar_t usm[1300];
            swprintf(usm, 1300, L"%ls\\%ls", dir, fd.cFileName);
            unpack_one(usm, NULL);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 给 browse.c 用: 解包已下载的 CG 分组目录 */
int unpack_cg_folder(const wchar_t *dir){
    unpack_folder(dir);
    return 0;
}

/* ================== 菜单入口 ================== */

static int unpack_custom(void){
    char path[1024];
    printf("输入 usm 文件或目录路径(留空=取消): ");
    if (fgets(path, sizeof path, stdin) == NULL) return -1;
    path[strcspn(path, "\r\n")] = 0;
    if (!path[0]) return -1;

    wchar_t wpath[1100];
    utf8_to_wide(path, wpath, 1100);
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES){
        printf("路径不存在: %s\n", path);
        return -1;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY){
        unpack_folder(wpath);
        return -1;
    }
    char vname[256];
    printf("输出视频名(留空=用文件名): ");
    if (fgets(vname, sizeof vname, stdin) == NULL) return -1;
    vname[strcspn(vname, "\r\n")] = 0;
    wchar_t wvname[256] = L"";
    if (vname[0]) utf8_to_wide(vname, wvname, 256);
    unpack_one(wpath, wvname);
    return 0;
}

static int unpack_dl_cg(void){
    wchar_t wroot[1024], wcg[1200];
    get_dl_root(wroot, 1024);
    swprintf(wcg, 1200, L"%ls\\CG", wroot);
    DWORD attr = GetFileAttributesW(wcg);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)){
        printf("还没有下载的 CG(CGSS_DOWN\\CG 不存在), 先去资源查找与下载里下\n");
        return -1;
    }

    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*", wcg);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    static def menu[128];
    int n = 0;
    if (h != INVALID_HANDLE_VALUE){
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            if (n < 120){
                wide_to_utf8(fd.cFileName, menu[n].name, sizeof menu[n].name);
                menu[n].func = NULL;
                menu[n].state = 0;
                n++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n == 0){
        printf("CGSS_DOWN\\CG 是空的\n");
        return -1;
    }
    snprintf(menu[n].name, sizeof menu[n].name, "返回");
    menu[n].func = NULL; menu[n].state = 0; n++;
    snprintf(menu[n].name, sizeof menu[n].name, "END");
    menu[n].func = NULL; menu[n].state = 0;

    int rc = pager_pick("已下载的CG(选一个解包)", menu, 0);
    if (rc < 0 || rc == n - 2) return -1;   /* 取消或选到"返回" */

    wchar_t sel[1300], selpath[1300];
    utf8_to_wide(menu[rc].name, sel, 1300);
    swprintf(selpath, 1300, L"%ls\\%ls", wcg, sel);
    unpack_folder(selpath);
    return 0;
}

int unpack_usm(void){
    def menu[] = {
        {"1.自定义USM解包(文件/目录)", unpack_custom, 0},
        {"2.解包已下载的CG", unpack_dl_cg, 0},
        {"3.返回", NULL, 0},
        {"END", NULL, 0}
    };
    while (1){
        int rc = pager_pick("USM/CG解包", menu, 0);
        if (rc == -1)
            continue;
        if (rc == 2)
            break;
    }
    return 1;
}
