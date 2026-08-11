// check_update.c: 检查 CGSS 数据库是否为最新版，不是则显示并更新；首次部署可一键补齐
// 原理：
//   1. 从 https://starlight.kirara.ca/api/v1/info 拿 truth_version（mishiro 的回退数据源）
//   2. 扫描本程序同目录的 manifest_*.db，取版本号最大的作为本地版本
//   3. 本地 < 最新 时：
//      a. 下载 /dl/<ver>/manifests/all_dbmanifest，解析出 Android_AHigh_SHigh 的 MD5
//      b. 下载 /dl/<ver>/manifests/Android_AHigh_SHigh（LZ4 包裹的 SQLite 清单库）
//      c. MD5 校验一致后 LZ4 解压，写成 manifest_<ver>.db
//   4. 若同目录没有 master.mdb：从清单库读出 master.mdb 的 hash，
//      下载 /dl/resources/Generic/xx/hash（LZ4 包裹）并解压为 master.mdb
// 用法：check_update.exe [工作目录]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>
#include "sqlite3.h"

#define CDN_HOST L"asset-starlight-stage.akamaized.net"
#define KIRARA_HOST L"starlight.kirara.ca"
#define CDN_UA L"User-Agent: UnityPlayer/2022.3.56f1 (UnityWebRequest/1.0, libcurl/8.10.1-DEV)\r\nX-Unity-Version: 2022.3.56f1"

/* ================== MD5（RFC 1321） ================== */

typedef struct {
    unsigned int state[4];
    unsigned long long len;
    unsigned char buf[64];
} MD5_CTX;

static const unsigned int MD5_K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const int MD5_S[64] = {
     7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
     5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
     4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
     6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static unsigned int md5_rotl(unsigned int x, int c){
    return (x << c) | (x >> (32 - c));
}

static void md5_block(unsigned int state[4], const unsigned char *p){
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned int x[16];
    for (int i = 0; i < 16; i++){
        x[i] = (unsigned int)p[i*4] | ((unsigned int)p[i*4+1] << 8) |
               ((unsigned int)p[i*4+2] << 16) | ((unsigned int)p[i*4+3] << 24);
    }
    for (int i = 0; i < 64; i++){
        unsigned int f;
        int g;
        if (i < 16){ f = (b & c) | (~b & d); g = i; }
        else if (i < 32){ f = (d & b) | (~d & c); g = (5*i + 1) % 16; }
        else if (i < 48){ f = b ^ c ^ d; g = (3*i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7*i) % 16; }
        unsigned int t = d;
        d = c;
        c = b;
        b = b + md5_rotl(a + f + MD5_K[i] + x[g], MD5_S[i]);
        a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void md5_init(MD5_CTX *ctx){
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
    ctx->len = 0;
}

static void md5_update(MD5_CTX *ctx, const unsigned char *p, size_t n){
    size_t used = (size_t)(ctx->len & 63);
    ctx->len += n;
    if (used){
        size_t take = 64 - used;
        if (take > n) take = n;
        memcpy(ctx->buf + used, p, take);
        p += take;
        n -= take;
        if (used + take == 64) md5_block(ctx->state, ctx->buf);
    }
    while (n >= 64){
        md5_block(ctx->state, p);
        p += 64;
        n -= 64;
    }
    if (n) memcpy(ctx->buf, p, n);
}

static void md5_final(MD5_CTX *ctx, unsigned char out[16]){
    unsigned long long bits = ctx->len << 3;
    unsigned char pad = 0x80;
    md5_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while ((ctx->len & 63) != 56) md5_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (8*i));
    md5_update(ctx, lenb, 8);
    for (int i = 0; i < 4; i++){
        out[i*4]   = (unsigned char)(ctx->state[i]);
        out[i*4+1] = (unsigned char)(ctx->state[i] >> 8);
        out[i*4+2] = (unsigned char)(ctx->state[i] >> 16);
        out[i*4+3] = (unsigned char)(ctx->state[i] >> 24);
    }
}

static void md5_hex(const unsigned char d[16], char out[33]){
    for (int i = 0; i < 16; i++) sprintf(out + i*2, "%02x", d[i]);
}

/* ================== LZ4 块解压（与 net.c 同源，移植 cgss_lz4.py） ================== */

static unsigned char *lz4_block_decompress(const unsigned char *src, int n, int out_size){
    unsigned char *out = (unsigned char*)malloc(out_size > 0 ? out_size : 1);
    if (!out) return NULL;
    int pos = 0, opos = 0;
    while (pos < n){
        int token = src[pos++];
        int lit_len = token >> 4;
        if (lit_len == 15){
            while (1){
                int b = src[pos++];
                lit_len += b;
                if (b != 255) break;
            }
        }
        memcpy(out + opos, src + pos, lit_len);
        pos += lit_len;
        opos += lit_len;
        if (pos >= n) break;
        int offset = src[pos] | (src[pos + 1] << 8);
        pos += 2;
        int match_len = (token & 0x0F) + 4;
        if (match_len == 19){
            while (1){
                int b = src[pos++];
                match_len += b;
                if (b != 255) break;
            }
        }
        int start = opos - offset;
        for (int i = 0; i < match_len; i++)
            out[opos++] = out[start + i];
    }
    return out;
}

static int cgss_lz4_decompress(const unsigned char *raw, int raw_len, unsigned char **out, int *out_len){
    if (raw_len < 16) return -1;
    *out_len = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((int)raw[7] << 24);
    *out = lz4_block_decompress(raw + 16, raw_len - 16, *out_len);
    return *out ? 0 : -1;
}

/* ================== WinHttp 下载 ================== */

static HINTERNET g_sess = NULL;

static void net_init(void){
    if (g_sess) return;
    g_sess = WinHttpOpen(L"CGSS-CheckUpdate/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_sess) return;
    DWORD to = 60000;
    WinHttpSetTimeouts(g_sess, to, to, to, to);
#ifdef WINHTTP_OPTION_SECURE_PROTOCOLS
    DWORD prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                 WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    prot |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(g_sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot));
#endif
}

/* 小文件：全部读进内存。unity_ua=1 时带 Unity 伪装头 */
static int http_get_mem(const wchar_t *host, const wchar_t *path,
                        unsigned char **out, DWORD *out_len, int unity_ua){
    *out = NULL;
    *out_len = 0;
    net_init();
    if (!g_sess) return -1;
    HINTERNET conn = WinHttpConnect(g_sess, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) return -1;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req){ WinHttpCloseHandle(conn); return -1; }
    if (unity_ua)
        WinHttpAddRequestHeaders(req, CDN_UA, (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
    int rc = -1;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)){
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        if (status == 200){
            DWORD cap = 65536, len = 0;
            unsigned char *buf = (unsigned char*)malloc(cap);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0){
                if (len + avail > cap){
                    while (len + avail > cap) cap *= 2;
                    unsigned char *nb = (unsigned char*)realloc(buf, cap);
                    if (!nb){ free(buf); buf = NULL; break; }
                    buf = nb;
                }
                DWORD got = 0;
                if (!WinHttpReadData(req, buf + len, avail, &got) || got == 0) break;
                len += got;
            }
            if (buf){ *out = buf; *out_len = len; rc = 0; }
        } else {
            printf("HTTP %lu\n", (unsigned long)status);
        }
    } else {
        printf("网络错误 err=%lu\n", (unsigned long)GetLastError());
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return rc;
}

/* 大文件：边下边写，带进度点 */
static int http_get_file(const wchar_t *host, const wchar_t *path, const wchar_t *file){
    net_init();
    if (!g_sess) return -1;
    HINTERNET conn = WinHttpConnect(g_sess, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) return -1;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req){ WinHttpCloseHandle(conn); return -1; }
    WinHttpAddRequestHeaders(req, CDN_UA, (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
    int rc = -1;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)){
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        if (status == 200){
            HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE){
                unsigned char buf[131072];
                DWORD avail = 0, got = 0, wr = 0;
                LONGLONG total = 0;
                int lastdot = 0;
                for (;;){
                    if (!WinHttpQueryDataAvailable(req, &avail)) break;
                    if (avail == 0) break;
                    if (avail > sizeof buf) avail = sizeof buf;
                    if (!WinHttpReadData(req, buf, avail, &got) || got == 0) break;
                    WriteFile(f, buf, got, &wr, NULL);
                    total += wr;
                    if ((int)(total / (512 * 1024)) != lastdot){
                        lastdot = (int)(total / (512 * 1024));
                        printf(".");
                        fflush(stdout);
                    }
                }
                CloseHandle(f);
                printf(" (%lldKB)\n", (long long)(total / 1024));
                if (total > 0) rc = 0;
            }
        } else {
            printf("HTTP %lu\n", (unsigned long)status);
        }
    } else {
        printf("网络错误 err=%lu\n", (unsigned long)GetLastError());
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return rc;
}

/* ================== 版本解析 ================== */

static long long parse_truth_version(const unsigned char *buf, DWORD len){
    char *s = (char*)malloc(len + 1);
    if (!s) return -1;
    memcpy(s, buf, len);
    s[len] = 0;
    long long v = -1;
    char *p = strstr(s, "truth_version");
    if (p){
        p = strchr(p, ':');
        if (p){
            p++;
            while (*p == ' ' || *p == '"') p++;
            if (*p >= '0' && *p <= '9') v = atoll(p);
        }
    }
    free(s);
    return v;
}

/* 从 all_dbmanifest 文本里取 Android_AHigh_SHigh 行的 hash */
static int parse_android_hash(const unsigned char *buf, DWORD len, char *hash_out){
    char *s = (char*)malloc(len + 1);
    if (!s) return -1;
    memcpy(s, buf, len);
    s[len] = 0;
    int rc = -1;
    char *p = strstr(s, "Android_AHigh_SHigh,");
    if (p){
        p += strlen("Android_AHigh_SHigh,");
        char *e = strchr(p, ',');
        if (e && e - p == 32){
            memcpy(hash_out, p, 32);
            hash_out[32] = 0;
            rc = 0;
        }
    }
    free(s);
    return rc;
}

/* 扫描目录里的 manifest_*.db，返回最大版本号，并回填完整路径 */
static long long find_local_manifest(const wchar_t *dir, wchar_t *path_out, int n){
    wchar_t pat[1024];
    swprintf(pat, 1024, L"%ls\\manifest_*.db", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    long long best = -1;
    if (h == INVALID_HANDLE_VALUE) return best;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        long long v = 0;
        int fields = swscanf(fd.cFileName, L"manifest_%lld.db", &v);
        if (fields == 1 && v > best){
            best = v;
            swprintf(path_out, n, L"%ls\\%ls", dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

static void get_exe_dir(wchar_t *buf, int n){
    GetModuleFileNameW(NULL, buf, n);
    wchar_t *p = wcsrchr(buf, L'\\');
    if (p) *p = 0;
}

/* ================== 数据库文件读取 / 解压保存 ================== */

static void wide_to_utf8_buf(const wchar_t *in, char *out, int n){
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, n, NULL, NULL);
}

/* 读 LZ4 文件 -> MD5 校验（expect 可传 NULL 跳过）-> 解压 -> 原子写入 out_file */
static int lz4_to_file(const wchar_t *lz4_file, const char *expect_hash, const wchar_t *out_file){
    HANDLE f = CreateFileW(lz4_file, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE){
        printf("读取 %ls 失败\n", lz4_file);
        return -1;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    unsigned char *raw = (unsigned char*)malloc((size_t)sz.QuadPart);
    if (!raw){ CloseHandle(f); printf("内存不足\n"); return -1; }
    DWORD total_read = 0, got = 0;
    while (total_read < (DWORD)sz.QuadPart &&
           ReadFile(f, raw + total_read, (DWORD)sz.QuadPart - total_read, &got, NULL) && got > 0)
        total_read += got;
    CloseHandle(f);

    if (expect_hash){
        MD5_CTX ctx;
        md5_init(&ctx);
        md5_update(&ctx, raw, (size_t)sz.QuadPart);
        unsigned char digest[16];
        md5_final(&ctx, digest);
        char got_hash[33];
        md5_hex(digest, got_hash);
        if (strcmp(got_hash, expect_hash) != 0){
            printf("MD5 校验失败：期望 %s，实际 %s\n", expect_hash, got_hash);
            free(raw);
            DeleteFileW(lz4_file);
            return -1;
        }
        printf("MD5 校验通过: %s\n", got_hash);
    }

    unsigned char *out = NULL;
    int out_len = 0;
    if (cgss_lz4_decompress(raw, (int)sz.QuadPart, &out, &out_len) != 0 || !out || out_len <= 0){
        printf("LZ4 解压失败\n");
        free(raw);
        return -1;
    }
    free(raw);

    wchar_t tmp[1300];
    swprintf(tmp, 1300, L"%ls.tmp", out_file);
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE){ printf("写入 %ls 失败\n", tmp); free(out); return -1; }
    DWORD wr = 0;
    WriteFile(f, out, (DWORD)out_len, &wr, NULL);
    CloseHandle(f);
    free(out);
    if (wr != (DWORD)out_len){ printf("写入不完整\n"); return -1; }
    MoveFileExW(tmp, out_file, MOVEFILE_REPLACE_EXISTING);
    printf("完成: %ls (%dKB -> %dKB)\n", out_file, (int)(sz.QuadPart/1024), out_len/1024);
    DeleteFileW(lz4_file);
    return 0;
}

/* 从清单库读 master.mdb 的 hash */
static int get_master_hash(const wchar_t *manifest_path, char *hash_out, int n){
    char mpath[1200];
    wide_to_utf8_buf(manifest_path, mpath, 1200);
    sqlite3 *db = NULL;
    if (sqlite3_open(mpath, &db) != SQLITE_OK){
        printf("打开 %ls 失败\n", manifest_path);
        return -1;
    }
    int rc = -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT hash FROM manifests WHERE name='master.mdb'",
                           -1, &stmt, NULL) == SQLITE_OK){
        if (sqlite3_step(stmt) == SQLITE_ROW){
            snprintf(hash_out, n, "%s", (const char*)sqlite3_column_text(stmt, 0));
            rc = 0;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

/* master.mdb 不存在时自动下载补齐 */
static int ensure_master(const wchar_t *dir, const wchar_t *manifest_path){
    wchar_t master_file[1200];
    swprintf(master_file, 1200, L"%ls\\master.mdb", dir);
    if (GetFileAttributesW(master_file) != INVALID_FILE_ATTRIBUTES){
        printf("master.mdb 已存在，跳过\n");
        return 0;
    }
    char hash[64] = "";
    if (get_master_hash(manifest_path, hash, 64) != 0){
        printf("清单库中找不到 master.mdb 的下载地址\n");
        return -1;
    }
    printf("master.mdb 不存在，从服务器下载（约 15~20MB）...\n");
    wchar_t path[512];
    swprintf(path, 512, L"/dl/resources/Generic/%.2s/%s", hash, hash);
    wchar_t lz4_file[1200];
    swprintf(lz4_file, 1200, L"%ls\\master.mdb.lz4", dir);
    if (http_get_file(CDN_HOST, path, lz4_file) != 0){
        printf("下载 master.mdb 失败\n");
        return -1;
    }
    return lz4_to_file(lz4_file, hash, master_file);
}

int main(int argc, char **argv){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    wchar_t dir[1024];
    if (argc > 1){
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, dir, 1024);
    } else {
        get_exe_dir(dir, 1024);
    }

    printf("== CGSS 资源清单更新检查 ==\n");
    printf("工作目录: %ls\n", dir);

    wchar_t local_path[1200] = L"";
    long long local_ver = find_local_manifest(dir, local_path, 1200);
    if (local_ver > 0)
        printf("本地清单: manifest_%lld.db\n", local_ver);
    else
        printf("本地清单: 未找到 manifest_*.db（将全新下载）\n");

    printf("正在查询最新资源版本 ...\n");
    unsigned char *info = NULL;
    DWORD info_len = 0;
    if (http_get_mem(KIRARA_HOST, L"/api/v1/info", &info, &info_len, 0) != 0){
        printf("查询最新版本失败（starlight.kirara.ca 无法访问）\n");
        return 1;
    }
    long long latest = parse_truth_version(info, info_len);
    free(info);
    if (latest <= 0){
        printf("解析最新版本失败\n");
        return 1;
    }
    printf("最新清单: manifest_%lld.db\n", latest);

    wchar_t active_manifest[1200] = L"";
    if (local_ver >= latest){
        printf(local_ver > latest
            ? "本地版本(%lld)比数据站记录(%lld)还新？以本地为准，无需更新。\n"
            : "已是最新版本（%lld），无需更新。\n", local_ver, latest);
        wcscpy(active_manifest, local_path);
    } else {
        printf("发现新版本 %lld -> %lld，开始更新...\n", local_ver, latest);

        /* 1. all_dbmanifest：拿 Android 清单的 MD5 */
        wchar_t path[512];
        swprintf(path, 512, L"/dl/%lld/manifests/all_dbmanifest", latest);
        printf("获取清单索引 ...\n");
        unsigned char *idx = NULL;
        DWORD idx_len = 0;
        if (http_get_mem(CDN_HOST, path, &idx, &idx_len, 1) != 0){
            printf("获取 all_dbmanifest 失败，版本 %lld 可能已不可用\n", latest);
            return 1;
        }
        char expect_hash[64] = "";
        if (parse_android_hash(idx, idx_len, expect_hash) != 0){
            printf("解析 all_dbmanifest 失败\n");
            free(idx);
            return 1;
        }
        free(idx);
        printf("预期 MD5: %s\n", expect_hash);

        /* 2. 下载 Android_AHigh_SHigh（LZ4 包裹） */
        swprintf(path, 512, L"/dl/%lld/manifests/Android_AHigh_SHigh", latest);
        wchar_t lz4_file[1200], db_file[1200];
        swprintf(lz4_file, 1200, L"%ls\\manifest_%lld.db.lz4", dir, latest);
        swprintf(db_file, 1200, L"%ls\\manifest_%lld.db", dir, latest);
        printf("下载清单库（11~15MB）...\n");
        if (http_get_file(CDN_HOST, path, lz4_file) != 0){
            printf("下载清单库失败\n");
            return 1;
        }
        if (lz4_to_file(lz4_file, expect_hash, db_file) != 0)
            return 1;
        wcscpy(active_manifest, db_file);
    }

    /* 3. master.mdb 不存在时自动补齐 */
    if (ensure_master(dir, active_manifest) != 0)
        return 1;

    printf("完成：清单库与主库均已就绪（程序会自动使用 %ls）。\n", active_manifest);
    return 0;
}
