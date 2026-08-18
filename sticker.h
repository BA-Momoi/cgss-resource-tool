#ifndef _CGSS_STICKER_H
#define _CGSS_STICKER_H

#include <windows.h>

/*
 * Process one downloaded spine_motion_sticker resource.
 * raw_dir contains the downloaded/decompressed .unity3d file.
 * spine_dir and png_dir are the two output roots under the sticker folder.
 * work_index only controls the temporary AssetStudio_out directory name.
 * Returns 0 when the AssetStudio/Spine pipeline completed, -1 otherwise.
 */
int sticker_unpack_file(const char *resource_name,
                        const wchar_t *raw_dir,
                        const wchar_t *spine_dir,
                        const wchar_t *png_dir,
                        int work_index);

#endif
