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
/* 从 src_png 裁出 (x,y,w,h) 区域存为 dst_png（RGBA 保留）。rotate=1 时顺时针转 90 度 */
int crop_png_region(const wchar_t *src_png, int x, int y, int w, int h,
                    int rotate, const wchar_t *dst_png);
/* 解析 Spine atlas（文本），把每个区域裁成 out_dir\prefix_1.png / _2.png ...
 * 返回成功裁剪的区域数（贴纸动作一般是 2 帧） */
int crop_atlas_regions(const wchar_t *atlas_path, const wchar_t *png_path,
                       const wchar_t *out_dir, const wchar_t *prefix);
#ifdef __cplusplus
}
#endif

#endif
