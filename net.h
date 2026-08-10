#ifndef _CGSS_NET_H
#define _CGSS_NET_H
#include <windows.h>
int cgss_lz4_decompress(const unsigned char *raw, int raw_len, unsigned char **out, int *out_len);
int dl_one(const char *name, const char *hash, const wchar_t *save_dir);
#endif
