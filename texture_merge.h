#ifndef _CGSS_TEXTURE_MERGE_H
#define _CGSS_TEXTURE_MERGE_H
#include <windows.h>

/* 扫描目录里所有 xxx_A8.png，把 xxx.png 的 RGB 与 A8 的 alpha 合成 xxx_merged.png，
 * 并生成引用它的 xxx_v38.atlas（Spine 3.8.75 编辑器用，编辑器不支持双贴图）。
 * 返回成功合并的纹理数量。 */
#ifdef __cplusplus
extern "C" {
#endif
int merge_a8_textures_in_dir(const wchar_t *dir);
#ifdef __cplusplus
}
#endif

#endif
