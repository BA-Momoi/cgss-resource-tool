#ifndef _CGSS_UNPACK_H
#define _CGSS_UNPACK_H
#include <windows.h>
int unpack_main(void);
int unpack_fbx_main(void);
int unpack_resources_main(void);
void find_assetstudio(wchar_t *out, int n);
void wipe_dir(const wchar_t *dir);
int copy_dir(const wchar_t *outdir, const wchar_t *sub, const wchar_t *dest, const wchar_t *ext);
int is_done(const wchar_t *dir, const wchar_t *pkg);
void print_gui_guide(const wchar_t *model_dir);
#endif
