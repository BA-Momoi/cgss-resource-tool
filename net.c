#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
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

static int http_get(const char *url_path, const wchar_t *wsave){
    wchar_t wpath[512];
    utf8_to_wide(url_path, wpath, 512);

    HINTERNET sess = WinHttpOpen(L"CGSS-DL/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess){
        fprintf(stderr, "WinHTTP 初始化失败 (错误 %lu)\n", (unsigned long)GetLastError());
        return -1;
    }
    DWORD timeout = 60000;
    WinHttpSetTimeouts(sess, timeout, timeout, timeout, timeout);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
        fprintf(stderr, "设置 TLS 1.2/1.3 失败 (错误 %lu)\n", (unsigned long)GetLastError());

    HINTERNET conn = WinHttpConnect(sess, CDN_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn){
        fprintf(stderr, "连接资源 CDN 失败 (错误 %lu)\n", (unsigned long)GetLastError());
        WinHttpCloseHandle(sess);
        return -1;
    }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req){
        fprintf(stderr, "创建 CDN 请求失败 (错误 %lu)\n", (unsigned long)GetLastError());
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(sess);
        return -1;
    }
    if (!WinHttpAddRequestHeaders(req,
        L"User-Agent: UnityPlayer/2022.3.56f1 (UnityWebRequest/1.0, libcurl/8.10.1-DEV)\r\n"
        L"X-Unity-Version: 2022.3.56f1",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD)){
        fprintf(stderr, "设置 Unity 请求头失败 (错误 %lu)\n", (unsigned long)GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(sess);
        return -1;
    }

    int rc = -1;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)){
        fprintf(stderr, "发送 CDN 请求失败 (错误 %lu)\n", (unsigned long)GetLastError());
        goto cleanup;
    }
    if (!WinHttpReceiveResponse(req, NULL)){
        fprintf(stderr, "接收 CDN 响应失败 (错误 %lu)\n", (unsigned long)GetLastError());
        goto cleanup;
    }

    DWORD status = 0, slen = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX)){
        fprintf(stderr, "读取 CDN HTTP 状态失败 (错误 %lu)\n", (unsigned long)GetLastError());
        goto cleanup;
    }
    if (status != HTTP_STATUS_OK){
        fprintf(stderr, "CDN 返回 HTTP %lu: %s\n", (unsigned long)status, url_path);
        goto cleanup;
    }

    HANDLE f = CreateFileW(wsave, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE){
        fprintf(stderr, "创建下载文件失败 (错误 %lu)\n", (unsigned long)GetLastError());
        goto cleanup;
    }

    unsigned char buf[131072];
    DWORD dwSize = 0, dwRead = 0, wr = 0;
    LONGLONG total = 0;
    int completed = 0;
    DWORD transfer_error = ERROR_SUCCESS;
    for (;;){
        if (!WinHttpQueryDataAvailable(req, &dwSize)){
            transfer_error = GetLastError();
            break;
        }
        if (dwSize == 0){ completed = 1; break; }
        if (dwSize > sizeof buf) dwSize = sizeof buf;
        if (!WinHttpReadData(req, buf, dwSize, &dwRead)){
            transfer_error = GetLastError();
            break;
        }
        if (dwRead == 0){
            transfer_error = ERROR_HANDLE_EOF;
            break;
        }
        if (!WriteFile(f, buf, dwRead, &wr, NULL) || wr != dwRead){
            transfer_error = GetLastError();
            if (transfer_error == ERROR_SUCCESS) transfer_error = ERROR_WRITE_FAULT;
            break;
        }
        total += dwRead;
    }
    CloseHandle(f);
    if (completed && total > 0){
        rc = 0;
    } else {
        if (transfer_error != ERROR_SUCCESS)
            fprintf(stderr, "接收或写入资源失败 (错误 %lu)\n", (unsigned long)transfer_error);
        DeleteFileW(wsave);
    }

cleanup:
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
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
    if (http_get(url_path, wsave) != 0){
        printf("下载 %s ... 失败（网络诊断见上方）\n", name);
        return -1;
    }
    /* .unity3d ?? LZ4 ?? */
    if (strstr(name, ".unity3d")){
        FILE *f = _wfopen(wsave, L"rb");
        if (!f){ printf("下载 %s ... 打开失败\n", name); return -1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > INT_MAX){
            fclose(f);
            printf("下载 %s ... 文件大小无效\n", name);
            return -1;
        }
        unsigned char *raw = (unsigned char*)malloc(sz > 0 ? sz : 1);
        if (!raw){ fclose(f); printf("下载 %s ... 内存不足\n", name); return -1; }
        if (sz > 0) fread(raw, 1, sz, f);
        fclose(f);
        unsigned char *out = NULL;
        int out_len = 0;
        if (cgss_lz4_decompress(raw, (int)sz, &out, &out_len) == 0 && out && out_len > 0){
            FILE *fo = _wfopen(wsave, L"wb");
            if (fo){
                size_t written = fwrite(out, 1, out_len, fo);
                fclose(fo);
                if (written != (size_t)out_len){
                    free(raw);
                    free(out);
                    printf("下载 %s ... 写入解压文件失败\n", name);
                    return -1;
                }
                printf("下载 %s ... 完成(LZ4 %d -> %d)\n", name, (int)sz, out_len);
            } else {
                free(raw);
                free(out);
                printf("下载 %s ... 写文件失败\n", name);
                return -1;
            }
        } else {
            printf("下载 %s ... 完成(非LZ4包裹)\n", name);
        }
        free(raw);
        free(out);
    } else {
        printf("下载 %s ... 完成\n", name);
    }
    return 0;
}

typedef struct {
    const DlTask *tasks;
    size_t count;
    volatile LONG next;
    volatile LONG successes;
} DlQueue;

static DWORD WINAPI dl_worker(LPVOID param){
    DlQueue *queue = (DlQueue*)param;
    for (;;){
        LONG index = InterlockedIncrement(&queue->next) - 1;
        if (index < 0 || (size_t)index >= queue->count) break;
        const DlTask *task = &queue->tasks[index];
        if (dl_one(task->name, task->hash, task->save_dir) == 0)
            InterlockedIncrement(&queue->successes);
    }
    return 0;
}

int dl_many(const DlTask *tasks, size_t count){
    if (!tasks || count == 0) return 0;
    if (count > LONG_MAX) return -1;

    unsigned worker_count = (unsigned)(count < 4 ? count : 4);
    DlQueue queue = {tasks, count, -1, 0};
    HANDLE workers[4];
    unsigned started = 0;
    for (; started < worker_count; started++){
        workers[started] = CreateThread(NULL, 0, dl_worker, &queue, 0, NULL);
        if (!workers[started]) break;
    }
    if (started == 0){
        dl_worker(&queue);
        return (int)queue.successes;
    }
    for (unsigned i = 0; i < started; i++){
        WaitForSingleObject(workers[i], INFINITE);
        CloseHandle(workers[i]);
    }
    return (int)queue.successes;
}

/* ================== 清单查询与资源收集 ================== */

typedef struct {
    char name[256];
    char hash[64];
    wchar_t sub[64];
} ResItem;

