#ifndef _CGSS_NET_H
#define _CGSS_NET_H
#include <windows.h>
#include <stddef.h>

typedef struct {
    const char *name;       /* manifest 资源名；dl_one 根据扩展名决定 CDN 类别 */
    const char *hash;       /* manifest 内容哈希，同时用于生成 CDN URL */
    const wchar_t *save_dir;/* 目标目录；dl_many 调用期间调用方需保证该字符串仍有效 */
} DlTask;

int cgss_lz4_decompress(const unsigned char *raw, int raw_len, unsigned char **out, int *out_len);
int dl_one(const char *name, const char *hash, const wchar_t *save_dir);
/* 同时下载最多四个独立资源；返回成功项数。每项最终由 dl_one 执行 HTTP、落盘和 UnityBundle LZ4 解包。 */
int dl_many(const DlTask *tasks, size_t count);
#endif
