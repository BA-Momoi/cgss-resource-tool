#ifndef _CGSS_STICKER_H
#define _CGSS_STICKER_H

#include <windows.h>

/* Downloaded sticker asset -> Spine files/JSON and cropped PNG frames. */
int sticker_unpack_file(const char *resource_name,
                        const wchar_t *raw_dir,
                        const wchar_t *spine_dir,
                        const wchar_t *png_dir,
                        int work_index);

#endif
