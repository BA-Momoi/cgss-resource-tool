#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>
#include "net.h"
#include "util.h"

#define CDN_HOST L"asset-starlight-stage.akamaized.net"

// net.c: CDN 下载 + LZ4

/* LZ4 块解压（移植 cgss_lz4.py） */
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

int cgss_lz4_decompress(const unsigned char *raw, int raw_len, unsigned char **out, int *out_len){
    if (raw_len < 16) return -1;
    *out_len = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((int)raw[7] << 24);
    *out = lz4_block_decompress(raw + 16, raw_len - 16, *out_len);
    return *out ? 0 : -1;
}

/* ================== HTTP 下载 ================== */

static HINTERNET g_sess = NULL, g_conn = NULL;

/* ??/????????????? TLS ?? */

static void http_init(void){
    if (g_sess) return;
    g_sess = WinHttpOpen(L"CGSS-DL/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_sess) return;
    DWORD to = 60000;   /* ??/??/??/???? 60 ? */
    WinHttpSetTimeouts(g_sess, to, to, to, to);
    g_conn = WinHttpConnect(g_sess, CDN_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
}


static int http_get(const char *url_path, const wchar_t *wsave){
    wchar_t wpath[512];
    utf8_to_wide(url_path, wpath, 512);

    http_init();
    if (!g_sess || !g_conn) return -1;
    HINTERNET req = WinHttpOpenRequest(g_conn, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) return -1;
    WinHttpAddRequestHeaders(req,
        L"User-Agent: UnityPlayer/2022.3.56f1 (UnityWebRequest/1.0, libcurl/8.10.1-DEV)\r\n"
        L"X-Unity-Version: 2022.3.56f1",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);

    int rc = -1;
    BOOL ok_send = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok_send){
        printf("SendRequest err=%lu\n", (unsigned long)GetLastError());
    }
    BOOL ok_recv = ok_send && WinHttpReceiveResponse(req, NULL);
    if (ok_send && !ok_recv){
        printf("ReceiveResponse err=%lu\n", (unsigned long)GetLastError());
    }
    if (ok_recv){
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        if (status != 200){
            printf("HTTP err: %lu\n", (unsigned long)status);
        }
        if (status == 200){
            HANDLE f = CreateFileW(wsave, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE){
                printf("CreateFileW err=%lu\n", (unsigned long)GetLastError());
            }
            if (f != INVALID_HANDLE_VALUE){
                unsigned char buf[131072];
                DWORD dwSize = 0, dwRead = 0, wr = 0;
                LONGLONG total = 0;
                int lastdot = 0;
                int completed = 0;
                for (;;){
                    if (!WinHttpQueryDataAvailable(req, &dwSize)) break;
                    if (dwSize == 0){ completed = 1; break; }
                    if (dwSize > sizeof buf) dwSize = sizeof buf;
                    if (!WinHttpReadData(req, buf, dwSize, &dwRead) || dwRead == 0) break;
                    WriteFile(f, buf, dwRead, &wr, NULL);
                    total += dwRead;
                    if ((int)(total / (512 * 1024)) != lastdot){
                        lastdot = (int)(total / (512 * 1024));
                        printf(".");
                        fflush(stdout);
                    }
                }
                CloseHandle(f);
                if (completed && total > 0){
                    printf("(%lldKB)\n", (long long)(total / 1024));
                    rc = 0;
                } else {
                    printf("下载未完成 err=%lu\n", (unsigned long)GetLastError());
                    DeleteFileW(wsave);
                }
            }
        }
    }
    WinHttpCloseHandle(req);
    return rc;
}

/* 下载一个资源并保存到 save_dir，.unity3d 自动 LZ4 解压 */
int dl_one(const char *name, const char *hash, const wchar_t *save_dir){
    char url_path[512];
    /* CDN 路径类别(实测确认):
     *   .unity3d -> AssetBundles
     *   .acb     -> Sound
     *   .usm     -> Movie
     *   .bdb     -> Generic
     * 猜错类别会 403 */
    const char *cat = "AssetBundles";
    if (strstr(name, ".acb"))      cat = "Sound";
    else if (strstr(name, ".usm")) cat = "Movie";
    else if (strstr(name, ".bdb")) cat = "Generic";
    snprintf(url_path, sizeof url_path, "/dl/resources/%s/%.2s/%s", cat, hash, hash);

    wchar_t wsave[1024];
    wchar_t wfile[512];
    utf8_to_wide(base_name(name), wfile, 512);
    swprintf(wsave, 1024, L"%ls\\%ls", save_dir, wfile);
    printf("下载 %s ... ", name);
    if (http_get(url_path, wsave) != 0){
        printf("失败(HTTP错误)\n");
        return -1;
    }
    /* .unity3d ?? LZ4 ?? */
    if (strstr(name, ".unity3d")){
        FILE *f = _wfopen(wsave, L"rb");
        if (!f){ printf("打开失败\n"); return -1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *raw = (unsigned char*)malloc(sz > 0 ? sz : 1);
        if (sz > 0) fread(raw, 1, sz, f);
        fclose(f);
        unsigned char *out = NULL;
        int out_len = 0;
        if (cgss_lz4_decompress(raw, (int)sz, &out, &out_len) == 0 && out && out_len > 0){
            FILE *fo = _wfopen(wsave, L"wb");
            if (fo){
                fwrite(out, 1, out_len, fo);    //将out写入fo文件
                fclose(fo);
                printf("完成(已LZ4解压 %d -> %d)\n", (int)sz, out_len);
            } else {
                printf("写文件失败\n");
            }
        } else {
            printf("完成(非LZ4包裹)\n");
        }
        free(raw);
        free(out);
    } else {
        printf("完成\n");
    }
    return 0;
}

/* ================== 清单查询与资源收集 ================== */

typedef struct {
    char name[256];
    char hash[64];
    wchar_t sub[64];
} ResItem;

