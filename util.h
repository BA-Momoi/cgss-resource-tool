#ifndef _CGSS_UTIL_H
#define _CGSS_UTIL_H
#include <windows.h>
void utf8_to_wide(const char *in, wchar_t *out, int n);
void wide_to_utf8(const wchar_t *in, char *out, int n);
void mkdirs(const wchar_t *path);
void get_dl_root(wchar_t *buf, int n);
const char *base_name(const char *name);
const char *find_manifest(void);
int parse_multi(const char *line, int *sel, int max);
int selected(const int *sel, int n, int v);
#endif
