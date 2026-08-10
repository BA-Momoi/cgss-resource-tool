// texture_merge.cpp: CGSS Spine 的 RGB + A8 双贴图合并（GDI+）
// tex.png 完全不透明（RGB），透明通道在 tex_A8.png。
// 浏览器预览用 canvas 合成；Spine 编辑器只认单张贴图，这里用 GDI+ 合成一张。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <gdiplus.h>
#include "texture_merge.h"

using namespace Gdiplus;

static void wide_to_utf8_local(const wchar_t *in, char *out, int n){
    if (!in || !out || n <= 0){ if (out && n > 0) out[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, n, NULL, NULL);
}

static int get_png_encoder_clsid(CLSID *clsid){
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (size == 0) return -1;
    ImageCodecInfo *infos = (ImageCodecInfo*)malloc(size);
    if (!infos) return -1;
    GetImageEncoders(n, size, infos);
    int found = -1;
    for (UINT i = 0; i < n; i++){
        if (wcscmp(infos[i].MimeType, L"image/png") == 0){
            *clsid = infos[i].Clsid;
            found = 0;
            break;
        }
    }
    free(infos);
    return found;
}

static int merge_one(const wchar_t *dir, const wchar_t *a8name){
    wchar_t a8[1300], base[1300], merged[1300], atlas[1300], v38atlas[1300];
    swprintf(a8, 1300, L"%ls\\%ls", dir, a8name);

    /* xxx_A8.png -> xxx.png */
    wchar_t basename[512];
    wcscpy(basename, a8name);
    wchar_t *p = wcsstr(basename, L"_A8");
    if (!p) return 0;
    wcscpy(p, L".png");
    swprintf(base, 1300, L"%ls\\%ls", dir, basename);
    if (GetFileAttributesW(base) == INVALID_FILE_ATTRIBUTES) return 0;

    ULONG_PTR token = 0;
    GdiplusStartupInput si;
    si.GdiplusVersion = 1;
    si.DebugEventCallback = NULL;
    si.SuppressBackgroundThread = FALSE;
    si.SuppressExternalCodecs = FALSE;
    if (GdiplusStartup(&token, &si, NULL) != Ok) return 0;

    Bitmap *bmpBase = Bitmap::FromFile(base);
    Bitmap *bmpA8 = Bitmap::FromFile(a8);
    if (!bmpBase || !bmpA8 || bmpBase->GetLastStatus() != Ok || bmpA8->GetLastStatus() != Ok){
        delete bmpBase; delete bmpA8;
        GdiplusShutdown(token);
        return 0;
    }
    UINT w = bmpBase->GetWidth(), h = bmpBase->GetHeight();
    if (bmpA8->GetWidth() != w || bmpA8->GetHeight() != h){
        printf("  %ls 与 %ls 尺寸不一致，跳过合并\n", basename, a8name);
        delete bmpBase; delete bmpA8;
        GdiplusShutdown(token);
        return 0;
    }

    int result = 0;
    {
        Bitmap mergedBmp(w, h, PixelFormat32bppARGB);
        BitmapData db, da, dm;
        Rect rc(0, 0, (INT)w, (INT)h);
        int ok = 0;
        if (bmpBase->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &db) == Ok
            && bmpA8->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &da) == Ok
            && mergedBmp.LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &dm) == Ok){
            for (UINT y = 0; y < h; y++){
                BYTE *sb = (BYTE*)db.Scan0 + (INT)y * db.Stride;
                BYTE *sa = (BYTE*)da.Scan0 + (INT)y * da.Stride;
                BYTE *sm = (BYTE*)dm.Scan0 + (INT)y * dm.Stride;
                for (UINT x = 0; x < w; x++){
                    sm[x*4+0] = sb[x*4+0];
                    sm[x*4+1] = sb[x*4+1];
                    sm[x*4+2] = sb[x*4+2];
                    sm[x*4+3] = sa[x*4+3];
                }
            }
            bmpBase->UnlockBits(&db);
            bmpA8->UnlockBits(&da);
            mergedBmp.UnlockBits(&dm);
            ok = 1;
        }
        delete bmpBase; delete bmpA8;

        if (ok){
            CLSID clsid;
            if (get_png_encoder_clsid(&clsid) == 0){
                swprintf(merged, 1300, L"%ls\\%ls", dir, basename);
                wchar_t *dot = wcsrchr(merged, L'.');
                if (dot) wcscpy(dot, L"_merged.png");
                if (mergedBmp.Save(merged, &clsid, NULL) == Ok){
                    /* 复制 atlas 并把页面文件名改成 merged */
                    wchar_t atlas_name[512];
                    wcscpy(atlas_name, basename);
                    wchar_t *d2 = wcsrchr(atlas_name, L'.');
                    if (d2) wcscpy(d2, L".atlas");
                    swprintf(atlas, 1300, L"%ls\\%ls", dir, atlas_name);
                    /* 兼容 AssetStudio 导出的 .atlas.asset 命名 */
                    if (GetFileAttributesW(atlas) == INVALID_FILE_ATTRIBUTES){
                        wchar_t atlas_name2[512];
                        wcscpy(atlas_name2, basename);
                        wchar_t *d2b = wcsrchr(atlas_name2, L'.');
                        if (d2b) wcscpy(d2b, L".atlas.asset");
                        swprintf(atlas, 1300, L"%ls\\%ls", dir, atlas_name2);
                    }
                    swprintf(v38atlas, 1300, L"%ls", atlas);
                    wchar_t *d3 = wcsrchr(v38atlas, L'.');
                    if (d3) wcscpy(d3, L"_v38.atlas");
                    FILE *fin = _wfopen(atlas, L"rb");
                    FILE *fout = _wfopen(v38atlas, L"wb");
                    if (fin && fout){
                        wchar_t merged_name[512];
                        wcscpy(merged_name, basename);
                        wchar_t *mn = wcsrchr(merged_name, L'.');
                        if (mn) wcscpy(mn, L"_merged.png");
                        char merged_u8[512];
                        wide_to_utf8_local(merged_name, merged_u8, sizeof merged_u8);
                        int page_replaced = 0;
                        char line[4096];
                        while (fgets(line, sizeof line, fin)){
                            if (!page_replaced && strstr(line, ".png")){
                                fputs(merged_u8, fout);
                                if (strchr(line, '\n')) fputc('\n', fout);
                                page_replaced = 1;
                            } else {
                                fputs(line, fout);
                            }
                        }
                    }
                    if (fin) fclose(fin);
                    if (fout) fclose(fout);
                    printf("  已合并透明度：%ls + %ls -> %ls\n", basename, a8name, merged);
                    result = 1;
                }
            }
        }
    }
    GdiplusShutdown(token);
    return result;
}

int merge_a8_textures_in_dir(const wchar_t *dir){
    wchar_t pat[1300];
    swprintf(pat, 1300, L"%ls\\*_A8.png", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (merge_one(dir, fd.cFileName)) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
