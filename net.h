#ifndef _CGSS_NET_H
#define _CGSS_NET_H
#include <windows.h>
#include <stddef.h>

typedef struct {
    const char *name;
    const char *hash;
    const wchar_t *save_dir;
} DlTask;

int cgss_lz4_decompress(const unsigned char *raw, int raw_len, unsigned char **out, int *out_len);
int dl_one(const char *name, const char *hash, const wchar_t *save_dir);
/* Download up to four independent resources concurrently; returns successes. */
int dl_many(const DlTask *tasks, size_t count);
#endif
