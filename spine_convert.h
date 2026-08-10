#ifndef _CGSS_SPINE_CONVERT_H
#define _CGSS_SPINE_CONVERT_H
#include <windows.h>

/* Convert CGSS Spine 3.6 binary .skel to standard Spine 3.6 JSON.
 * Return 0 on success, -1 on failure. */
int convert_skel_to_json(const char *skel_path, const char *json_path);
int convert_skel_to_json_w(const wchar_t *skel_path, const wchar_t *json_path);
/* Convert to Spine 3.8.75 JSON (safe when data has no 3.6-only IK/Transform/Path) */
int convert_skel_to_json_v38(const char *skel_path, const char *json_path);
int convert_skel_to_json_v38_w(const wchar_t *skel_path, const wchar_t *json_path);

/* Spine 2.1 shared petit skeleton (SPSprachen):
 * scale aligns skeleton coordinates to the paired atlas (0.5 for SPC cards). */
int convert_skel21_to_json_w(const wchar_t *skel_path, const wchar_t *json_path, float scale);
int convert_skel21_to_json_v38_w(const wchar_t *skel_path, const wchar_t *json_path, float scale);

/* Scan a directory for *.skel and convert each to same-name .json.
 * Returns the number of successfully converted files. */
int convert_skels_in_dir(const wchar_t *dir);

#endif
