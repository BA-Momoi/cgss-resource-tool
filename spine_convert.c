// spine_convert.c: CGSS Spine 3.6 二进制 .skel -> Spine 3.6 JSON
// 与 Python 版 skel2json.py / 浏览器版 cgss_skel_parser.js 输出一致。
//
// CGSS skel 与标准 Spine 3.6 二进制的差异：
//   1. 文件头 44 字节：0x1C + 27 字节哈希 + 版本串 + 9 字节数据；
//   2. float / uint32 / int16 都是大端（标准是小端）；
//   3. 没有 nonessential 段（无骨骼颜色、mesh 的 edges/width/height、
//      boundingbox/path/point/clipping 的颜色）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "spine_convert.h"
#include "util.h"

/* ---------------- 二进制读取 ---------------- */
typedef struct {
    const unsigned char *b;
    long len;
    long pos;
    int err;
} Rd;

static int rd_byte(Rd *r){
    if (r->pos >= r->len){ r->err = 1; return 0; }
    return r->b[r->pos++];
}
static int rd_bool(Rd *r){ return rd_byte(r) != 0; }

static long rd_varint(Rd *r){
    int b = rd_byte(r);
    long result = b & 0x7f;
    if (b & 0x80){
        b = rd_byte(r); result |= (long)(b & 0x7f) << 7;
        if (b & 0x80){
            b = rd_byte(r); result |= (long)(b & 0x7f) << 14;
            if (b & 0x80){
                b = rd_byte(r); result |= (long)(b & 0x7f) << 21;
                if (b & 0x80){
                    b = rd_byte(r); result |= (long)(b & 0x7f) << 28;
                }
            }
        }
    }
    return result;
}
static long rd_int_opt(Rd *r, int opt){
    long v = rd_varint(r);
    return opt ? v : (v >> 1) ^ -(v & 1);
}
static int rd_u32(Rd *r, unsigned *out){
    if (r->pos + 4 > r->len){ r->err = 1; return -1; }
    *out = ((unsigned)r->b[r->pos] << 24) | ((unsigned)r->b[r->pos+1] << 16)
         | ((unsigned)r->b[r->pos+2] << 8) | (unsigned)r->b[r->pos+3];
    r->pos += 4;
    return 0;
}
static int rd_f32(Rd *r, float *out){
    union { unsigned u; float f; } v;
    if (rd_u32(r, &v.u) != 0) return -1;
    *out = v.f;
    return 0;
}
static int rd_i16(Rd *r, short *out){
    if (r->pos + 2 > r->len){ r->err = 1; return -1; }
    *out = (short)(((unsigned short)r->b[r->pos] << 8) | (unsigned short)r->b[r->pos+1]);
    r->pos += 2;
    return 0;
}
static char *rd_str(Rd *r){
    long n = rd_varint(r);
    if (r->err) return NULL;
    if (n == 0) return NULL;
    if (n == 1){
        char *s = (char*)malloc(1);
        if (s) s[0] = 0;
        return s;
    }
    n--;
    if (r->pos + n > r->len){ r->err = 1; return NULL; }
    char *s = (char*)malloc((size_t)n + 1);
    if (!s){ r->err = 1; return NULL; }
    memcpy(s, r->b + r->pos, (size_t)n);
    s[n] = 0;
    r->pos += n;
    return s;
}

/* ---------------- 动态字符串数组 ---------------- */
typedef struct {
    char **a;
    int n, cap;
} StrArr;
static void sa_push(StrArr *s, char *str){
    if (s->n == s->cap){
        s->cap = s->cap ? s->cap * 2 : 16;
        s->a = (char**)realloc(s->a, sizeof(char*) * (size_t)s->cap);
    }
    if (s->a) s->a[s->n++] = str;
}
static const char *sa_get(const StrArr *s, int i){
    if (i < 0 || i >= s->n) return "";
    return s->a[i];
}
static void sa_free(StrArr *s){
    for (int i = 0; i < s->n; i++) free(s->a[i]);
    free(s->a);
    memset(s, 0, sizeof *s);
}

/* ---------------- JSON 输出工具 ---------------- */
static void json_str(FILE *f, const char *s){
    if (!s) s = "";
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char*)s; *p; p++){
        unsigned char c = *p;
        switch (c){
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc(c, f);
        }
    }
    fputc('"', f);
}

static void json_float(FILE *f, float x){
    if (x != x) x = 0;                 /* NaN -> 0 */
    if (x == 0){ fputs("0", f); return; }
    char buf[64];
    snprintf(buf, sizeof buf, "%.6f", (double)x);
    /* 去掉末尾多余的 0 和可能的小数点 */
    int len = (int)strlen(buf);
    char *dot = strchr(buf, '.');
    if (dot){
        while (len > 0 && buf[len-1] == '0') buf[--len] = 0;
        if (len > 0 && buf[len-1] == '.') buf[--len] = 0;
    }
    fputs(buf, f);
}

static void json_curve(FILE *f, Rd *r){
    int t = rd_byte(r);
    if (t == 1) fputs(",\"curve\":\"stepped\"", f);
    else if (t == 2){
        float a, b, c, d;
        rd_f32(r, &a); rd_f32(r, &b); rd_f32(r, &c); rd_f32(r, &d);
        fputs(",\"curve\":[", f);
        json_float(f, a); fputc(',', f);
        json_float(f, b); fputc(',', f);
        json_float(f, c); fputc(',', f);
        json_float(f, d);
        fputc(']', f);
    }
}

static void json_u32_color(FILE *f, unsigned c){
    fprintf(f, "\"%02x%02x%02x%02x\"",
            (c >> 24) & 0xff, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}
static void json_u32_rgb(FILE *f, unsigned c){
    fprintf(f, "\"%02x%02x%02x\"", (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

/* ---------------- 顶点 ---------------- */
/* 输出 JSON 顶点数组到 f；返回 1 表示权重顶点（数组带 boneCount 前缀），0 表示普通顶点 */
static int emit_vertices(FILE *f, Rd *r, int vc){
    if (!rd_bool(r)){
        fputc('[', f);
        for (int i = 0; i < vc * 2; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v);
        }
        fputc(']', f);
        return 0;
    }
    fputc('[', f);
    int first = 1;
    for (int i = 0; i < vc; i++){
        long bc = rd_varint(r);
        if (!first) fputc(',', f); first = 0;
        fprintf(f, "%ld", bc);
        for (long j = 0; j < bc; j++){
            long bi = rd_varint(r);
            float x, y, w;
            rd_f32(r, &x); rd_f32(r, &y); rd_f32(r, &w);
            fprintf(f, ",%ld,", bi);
            json_float(f, x); fputc(',', f);
            json_float(f, y); fputc(',', f);
            json_float(f, w);
        }
    }
    fputc(']', f);
    return 1;
}

/* ---------------- 主解析（流式输出 JSON） ---------------- */
typedef struct {
    StrArr bones, slots, ik, transform, path, skins, events;
} Ctx;

static void emit_attachment(FILE *f, Rd *r, Ctx *ctx, const char *key){
    char *name = rd_str(r);
    if (!name) name = strdup(key ? key : "");
    int type = rd_byte(r);
    if (type == 0){  /* region */
        char *path = rd_str(r);
        int path_own = path != NULL;
        if (!path) path = name;
        float rot, x, y, sx, sy, w, h;
        rd_f32(r, &rot); rd_f32(r, &x); rd_f32(r, &y);
        rd_f32(r, &sx); rd_f32(r, &sy); rd_f32(r, &w); rd_f32(r, &h);
        unsigned c; rd_u32(r, &c);
        fputs("{\"type\":\"region\",\"name\":", f); json_str(f, name);
        fputs(",\"path\":", f); json_str(f, path);
        fputs(",\"rotation\":", f); json_float(f, rot);
        fputs(",\"x\":", f); json_float(f, x);
        fputs(",\"y\":", f); json_float(f, y);
        fputs(",\"scaleX\":", f); json_float(f, sx);
        fputs(",\"scaleY\":", f); json_float(f, sy);
        fputs(",\"width\":", f); json_float(f, w);
        fputs(",\"height\":", f); json_float(f, h);
        fputs(",\"color\":", f); json_u32_color(f, c);
        fputc('}', f);
        if (path_own) free(path);
    } else if (type == 1){  /* boundingbox */
        long vc = rd_varint(r);
        fputs("{\"type\":\"boundingbox\",\"name\":", f); json_str(f, name);
        fprintf(f, ",\"vertexCount\":%ld,\"vertices\":", vc);
        emit_vertices(f, r, (int)vc);
        fputc('}', f);
    } else if (type == 2){  /* mesh */
        char *path = rd_str(r);
        int path_own = path != NULL;
        if (!path) path = name;
        unsigned c; rd_u32(r, &c);
        long vc = rd_varint(r);
        fputs("{\"type\":\"mesh\",\"name\":", f); json_str(f, name);
        fputs(",\"path\":", f); json_str(f, path);
        fputs(",\"color\":", f); json_u32_color(f, c);
        fputs(",\"uvs\":[", f);
        for (long i = 0; i < vc * 2; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v);
        }
        fputs("],\"triangles\":[", f);
        long nt = rd_varint(r);
        for (long i = 0; i < nt; i++){
            short v; rd_i16(r, &v);
            if (i) fputc(',', f);
            fprintf(f, "%d", v);
        }
        fputs("],\"vertices\":", f);
        emit_vertices(f, r, (int)vc);
        long hull = rd_varint(r);
        fprintf(f, ",\"hull\":%ld}", hull);
        if (path_own) free(path);
    } else if (type == 3){  /* linkedmesh */
        char *path = rd_str(r);
        int path_own = path != NULL;
        if (!path) path = name;
        unsigned c; rd_u32(r, &c);
        char *skin = rd_str(r);
        char *parent = rd_str(r);
        int inherit = rd_bool(r);
        fputs("{\"type\":\"linkedmesh\",\"name\":", f); json_str(f, name);
        fputs(",\"path\":", f); json_str(f, path);
        fputs(",\"parent\":", f); json_str(f, parent);
        fputs(",\"deform\":", f); fputs(inherit ? "true" : "false", f);
        fputs(",\"color\":", f); json_u32_color(f, c);
        if (skin) { fputs(",\"skin\":", f); json_str(f, skin); }
        fputc('}', f);
        if (path_own) free(path); free(skin); free(parent);
    } else if (type == 4){  /* path attachment */
        int closed = rd_bool(r), cs = rd_bool(r);
        long vc = rd_varint(r);
        fputs("{\"type\":\"path\",\"name\":", f); json_str(f, name);
        fputs(closed ? ",\"closed\":true" : ",\"closed\":false", f);
        fputs(cs ? ",\"constantSpeed\":true" : ",\"constantSpeed\":false", f);
        fprintf(f, ",\"vertexCount\":%ld,\"vertices\":", vc);
        emit_vertices(f, r, (int)vc);
        fputs(",\"lengths\":[", f);
        for (long i = 0; i < vc / 3; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v);
        }
        fputc(']', f);
        fputc('}', f);
    } else if (type == 5){  /* point */
        float rot, x, y;
        rd_f32(r, &rot); rd_f32(r, &x); rd_f32(r, &y);
        fputs("{\"type\":\"point\",\"name\":", f); json_str(f, name);
        fputs(",\"rotation\":", f); json_float(f, rot);
        fputs(",\"x\":", f); json_float(f, x);
        fputs(",\"y\":", f); json_float(f, y);
        fputc('}', f);
    } else if (type == 6){  /* clipping */
        long end_slot = rd_varint(r);
        long vc = rd_varint(r);
        fputs("{\"type\":\"clipping\",\"name\":", f); json_str(f, name);
        fputs(",\"end\":", f); json_str(f, sa_get(&ctx->slots, (int)end_slot));
        fprintf(f, ",\"vertexCount\":%ld,\"vertices\":", vc);
        emit_vertices(f, r, (int)vc);
        fputc('}', f);
    }
    free(name);
}

static int emit_skin(FILE *f, Rd *r, Ctx *ctx, const char *skin_name, int *first){
    long slot_count = rd_varint(r);
    if (slot_count == 0) return 0;   /* null skin */
    if (*first) *first = 0; else fputc(',', f);
    fputs("\"", f); fputs(skin_name, f); fputs("\":{", f);
    for (long i = 0; i < slot_count; i++){
        long slot_idx = rd_varint(r);
        if (i) fputc(',', f);
        fputs("\"", f); fputs(sa_get(&ctx->slots, (int)slot_idx), f); fputs("\":{", f);
        long att_count = rd_varint(r);
        for (long j = 0; j < att_count; j++){
            char *att_name = rd_str(r);
            if (j) fputc(',', f);
            fputs("\"", f);
            if (att_name) fputs(att_name, f);
            fputs("\":", f);
            emit_attachment(f, r, ctx, att_name);
            free(att_name);
        }
        fputc('}', f);
    }
    fputc('}', f);
    return 1;
}

/* 动画帧 JSON 公共前缀（time）与 curve */
static void frame_begin(FILE *f, Rd *r, int *first){
    if (*first) *first = 0; else fputc(',', f);
    float t; rd_f32(r, &t);
    fputs("{\"time\":", f); json_float(f, t);
}

static void emit_animation(FILE *f, Rd *r, Ctx *ctx, const char *name){
    long i, j, n;
    json_str(f, name); fputs(":{", f);
    int sec_first = 1;
    #define SEC_BEGIN() do { if (!sec_first) fputc(',', f); sec_first = 0; } while (0)

    /* slots */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"slots\":{", f);
        for (i = 0; i < n; i++){
            long slot_idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->slots, (int)slot_idx), f); fputs("\":{", f);
            long tt_count = rd_varint(r);
            for (j = 0; j < tt_count; j++){
                int ttype = rd_byte(r);
                long fc = rd_varint(r);
                if (j) fputc(',', f);
                const char *tn = ttype == 0 ? "attachment" : (ttype == 1 ? "color" : "twoColor");
                fputs("\"", f); fputs(tn, f); fputs("\":[", f);
                int first = 1;
                for (long k = 0; k < fc; k++){
                    frame_begin(f, r, &first);
                    if (ttype == 0){
                        char *an = rd_str(r);
                        fputs(",\"name\":", f);
                        if (an) json_str(f, an); else fputs("null", f);
                        free(an);
                    } else if (ttype == 1){
                        unsigned c; rd_u32(r, &c);
                        fputs(",\"color\":", f); json_u32_color(f, c);
                    } else {
                        unsigned c1, c2; rd_u32(r, &c1); rd_u32(r, &c2);
                        fputs(",\"light\":", f); json_u32_color(f, c1);
                        fputs(",\"dark\":", f); json_u32_rgb(f, c2);
                    }
                    if (k < fc - 1 && ttype != 0) json_curve(f, r);
                    fputc('}', f);
                }
                fputs("]", f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* bones */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"bones\":{", f);
        for (i = 0; i < n; i++){
            long bone_idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->bones, (int)bone_idx), f); fputs("\":{", f);
            long tt_count = rd_varint(r);
            for (j = 0; j < tt_count; j++){
                int ttype = rd_byte(r);
                long fc = rd_varint(r);
                if (j) fputc(',', f);
                const char *tn = ttype == 0 ? "rotate" : (ttype == 1 ? "translate" : (ttype == 2 ? "scale" : "shear"));
                fputs("\"", f); fputs(tn, f); fputs("\":[", f);
                int first = 1;
                for (long k = 0; k < fc; k++){
                    frame_begin(f, r, &first);
                    if (ttype == 0){
                        float a; rd_f32(r, &a);
                        fputs(",\"angle\":", f); json_float(f, a);
                    } else {
                        float x, y; rd_f32(r, &x); rd_f32(r, &y);
                        fputs(",\"x\":", f); json_float(f, x);
                        fputs(",\"y\":", f); json_float(f, y);
                    }
                    if (k < fc - 1) json_curve(f, r);
                    fputc('}', f);
                }
                fputs("]", f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* ik */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"ik\":{", f);
        for (i = 0; i < n; i++){
            long idx = rd_varint(r);
            long fc = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->ik, (int)idx), f); fputs("\":[", f);
            int first = 1;
            for (long k = 0; k < fc; k++){
                frame_begin(f, r, &first);
                float m; rd_f32(r, &m);
                int bp = rd_byte(r) == 1;
                fputs(",\"mix\":", f); json_float(f, m);
                fputs(bp ? ",\"bendPositive\":true" : ",\"bendPositive\":false", f);
                if (k < fc - 1) json_curve(f, r);
                fputc('}', f);
            }
            fputs("]", f);
        }
        fputs("}", f);
    }

    /* transform */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"transform\":{", f);
        for (i = 0; i < n; i++){
            long idx = rd_varint(r);
            long fc = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->transform, (int)idx), f); fputs("\":[", f);
            int first = 1;
            for (long k = 0; k < fc; k++){
                frame_begin(f, r, &first);
                float rm, tm2, sm, shm;
                rd_f32(r, &rm); rd_f32(r, &tm2); rd_f32(r, &sm); rd_f32(r, &shm);
                fputs(",\"rotateMix\":", f); json_float(f, rm);
                fputs(",\"translateMix\":", f); json_float(f, tm2);
                fputs(",\"scaleMix\":", f); json_float(f, sm);
                fputs(",\"shearMix\":", f); json_float(f, shm);
                if (k < fc - 1) json_curve(f, r);
                fputc('}', f);
            }
            fputs("]", f);
        }
        fputs("}", f);
    }

    /* paths */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"paths\":{", f);
        for (i = 0; i < n; i++){
            long idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->path, (int)idx), f); fputs("\":{", f);
            long tt_count = rd_varint(r);
            for (j = 0; j < tt_count; j++){
                int ttype = rd_byte(r);
                long fc = rd_varint(r);
                if (j) fputc(',', f);
                const char *tn = ttype == 0 ? "position" : (ttype == 1 ? "spacing" : "mix");
                fputs("\"", f); fputs(tn, f); fputs("\":[", f);
                int first = 1;
                for (long k = 0; k < fc; k++){
                    frame_begin(f, r, &first);
                    if (ttype == 0 || ttype == 1){
                        float v; rd_f32(r, &v);
                        fputs(ttype == 0 ? ",\"position\":" : ",\"spacing\":", f);
                        json_float(f, v);
                    } else {
                        float rm, tm2; rd_f32(r, &rm); rd_f32(r, &tm2);
                        fputs(",\"rotateMix\":", f); json_float(f, rm);
                        fputs(",\"translateMix\":", f); json_float(f, tm2);
                    }
                    if (k < fc - 1) json_curve(f, r);
                    fputc('}', f);
                }
                fputs("]", f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* deform */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"deform\":{", f);
        for (i = 0; i < n; i++){
            long skin_idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->skins, (int)skin_idx), f); fputs("\":{", f);
            long slot_count = rd_varint(r);
            for (j = 0; j < slot_count; j++){
                long slot_idx = rd_varint(r);
                if (j) fputc(',', f);
                fputs("\"", f); fputs(sa_get(&ctx->slots, (int)slot_idx), f); fputs("\":{", f);
                long att_count = rd_varint(r);
                for (long k = 0; k < att_count; k++){
                    char *att_name = rd_str(r);
                    long fc = rd_varint(r);
                    if (k) fputc(',', f);
                    fputs("\"", f);
                    if (att_name) fputs(att_name, f);
                    fputs("\":[", f);
                    int first = 1;
                    for (long m = 0; m < fc; m++){
                        float t; rd_f32(r, &t);
                        long end = rd_varint(r);
                        if (first) first = 0; else fputc(',', f);
                        fputs("{\"time\":", f); json_float(f, t);
                        if (end != 0){
                            long start = rd_varint(r);
                            end += start;
                            fprintf(f, ",\"offset\":%ld,\"vertices\":[", start);
                            for (long v = start; v < end; v++){
                                float fv; rd_f32(r, &fv);
                                if (v > start) fputc(',', f);
                                json_float(f, fv);
                            }
                            fputc(']', f);
                        }
                        if (m < fc - 1) json_curve(f, r);
                        fputc('}', f);
                    }
                    fputs("]", f);
                    free(att_name);
                }
                fputc('}', f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* drawOrder */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"drawOrder\":[", f);
        for (i = 0; i < n; i++){
            float t; rd_f32(r, &t);
            long oc = rd_varint(r);
            if (i) fputc(',', f);
            fputs("{\"time\":", f); json_float(f, t);
            fputs(",\"offsets\":[", f);
            for (j = 0; j < oc; j++){
                long si = rd_varint(r), so = rd_varint(r);
                fprintf(stderr, "DBG do si=%ld so=%ld pos=%ld\n", si, so, r->pos);
                if (j) fputc(',', f);
                fputs("{\"slot\":", f); json_str(f, sa_get(&ctx->slots, (int)si));
                fprintf(f, ",\"offset\":%ld}", so);
            }
            fputs("]}", f);
        }
        fputs("]", f);
    }

    /* events */
    n = rd_varint(r);
    if (n > 0){
        SEC_BEGIN();
        fputs("\"events\":[", f);
        for (i = 0; i < n; i++){
            float t; rd_f32(r, &t);
            long ev = rd_varint(r);
            long iv = rd_int_opt(r, 0);
            float fv; rd_f32(r, &fv);
            int has_str = rd_bool(r);
            char *sv = has_str ? rd_str(r) : NULL;
            if (i) fputc(',', f);
            fputs("{\"time\":", f); json_float(f, t);
            fputs(",\"name\":", f); json_str(f, sa_get(&ctx->events, (int)ev));
            if (iv != 0) fprintf(f, ",\"int\":%ld", iv);
            if (fv != 0){ fputs(",\"float\":", f); json_float(f, fv); }
            if (sv){ fputs(",\"string\":", f); json_str(f, sv); }
            fputc('}', f);
            free(sv);
        }
        fputs("]", f);
    }
    fputc('}', f);   /* end animation */
    #undef SEC_BEGIN
}

static int convert_skel_fp(FILE *in, const wchar_t *json_path, const char *spine_ver){
    /* 读整个文件 */
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz < 44) return -1;
    unsigned char *buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) return -1;
    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz){
        free(buf);
        return -1;
    }
    Rd r;
    r.b = buf; r.len = sz; r.pos = 44; r.err = 0;

    FILE *f = _wfopen(json_path, L"wb");
    if (!f){ free(buf); return -1; }

    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);

    fputs("{\"skeleton\":{\"hash\":\"cgss\",\"spine\":\"", f);
    fputs(spine_ver ? spine_ver : "3.6.47", f);
    fputs("\",\"width\":0,\"height\":0,\"fps\":30,\"images\":\"\",\"audio\":\"\"},\"bones\":[", f);
    long nb = rd_varint(&r);
    for (long i = 0; i < nb; i++){
        char *name = rd_str(&r);
        long parent = (i == 0) ? -1 : rd_varint(&r);
        float rot, x, y, sx, sy, shx, shy, len;
        rd_f32(&r, &rot); rd_f32(&r, &x); rd_f32(&r, &y);
        rd_f32(&r, &sx); rd_f32(&r, &sy); rd_f32(&r, &shx); rd_f32(&r, &shy);
        rd_f32(&r, &len);
        long tm = rd_varint(&r);
        if (i) fputc(',', f);
        fputs("{\"name\":", f); json_str(f, name ? name : "");
        if (parent >= 0){
            fputs(",\"parent\":", f); json_str(f, sa_get(&ctx.bones, (int)parent));
        }
        if (len != 0 && len != 1){ fputs(",\"length\":", f); json_float(f, len); }
        if (x != 0 && x != 1){ fputs(",\"x\":", f); json_float(f, x); }
        if (y != 0 && y != 1){ fputs(",\"y\":", f); json_float(f, y); }
        if (rot != 0 && rot != 1){ fputs(",\"rotation\":", f); json_float(f, rot); }
        if (sx != 1){ fputs(",\"scaleX\":", f); json_float(f, sx); }
        if (sy != 1){ fputs(",\"scaleY\":", f); json_float(f, sy); }
        if (shx != 0 && shx != 1){ fputs(",\"shearX\":", f); json_float(f, shx); }
        if (shy != 0 && shy != 1){ fputs(",\"shearY\":", f); json_float(f, shy); }
        static const char *tm_names[] = {"normal","onlyTranslation","noRotationOrReflection","noScale","noScaleOrReflection"};
        if (tm >= 0 && tm < 5 && tm != 0){ fputs(",\"transform\":\"", f); fputs(tm_names[tm], f); fputc('"', f); }
        fputc('}', f);
        sa_push(&ctx.bones, name ? name : strdup(""));
    }

    /* slots */
    fputs("],\"slots\":[", f);
    long ns = rd_varint(&r);
    for (long i = 0; i < ns; i++){
        char *name = rd_str(&r);
        long bone = rd_varint(&r);
        unsigned c; rd_u32(&r, &c);
        unsigned dark; rd_u32(&r, &dark);
        char *att = rd_str(&r);
        long blend_mode = rd_varint(&r);
        if (i) fputc(',', f);
        fputs("{\"name\":", f); json_str(f, name ? name : "");
        fputs(",\"bone\":", f); json_str(f, sa_get(&ctx.bones, (int)bone));
        fputs(",\"color\":", f); json_u32_color(f, c);
        if (dark != 0xffffffffu){ fputs(",\"dark\":", f); json_u32_rgb(f, dark); }
        if (att){ fputs(",\"attachment\":", f); json_str(f, att); }
        static const char *blend_names[] = {"normal","additive","multiply","screen"};
        fputs(",\"blend\":\"", f);
        fputs(blend_names[(blend_mode >= 0 && blend_mode < 4) ? blend_mode : 0], f);
        fputs("\"}", f);
        sa_push(&ctx.slots, name ? name : strdup(""));
        free(att);
    }

    fputc(']', f);   /* 闭合 slots 数组 */
    /* ik（空则不输出，与 Python 一致） */
    long nik = rd_varint(&r);
    if (nik > 0){
        fputs(",\"ik\":[", f);
        for (long i = 0; i < nik; i++){
            char *name = rd_str(&r);
            long order = rd_varint(&r);
            long bc = rd_varint(&r);
            if (i) fputc(',', f);
            fputs("{\"name\":", f); json_str(f, name ? name : "");
            fprintf(f, ",\"order\":%ld,\"bones\":[", order);
            for (long j = 0; j < bc; j++){
                long bi = rd_varint(&r);
                if (j) fputc(',', f);
                json_str(f, sa_get(&ctx.bones, (int)bi));
            }
            long target = rd_varint(&r);
            float mix; rd_f32(&r, &mix);
            int bp = rd_byte(&r) == 1;
            fputs("],\"target\":", f); json_str(f, sa_get(&ctx.bones, (int)target));
            fputs(",\"mix\":", f); json_float(f, mix);
            fputs(bp ? ",\"bendPositive\":true}" : ",\"bendPositive\":false}", f);
            sa_push(&ctx.ik, name ? name : strdup(""));
        }
        fputs("]", f);
    }

    /* transform */
    long ntf = rd_varint(&r);
    if (ntf > 0){
        fputs(",\"transform\":[", f);
        for (long i = 0; i < ntf; i++){
            char *name = rd_str(&r);
            long order = rd_varint(&r);
            long bc = rd_varint(&r);
            if (i) fputc(',', f);
            fputs("{\"name\":", f); json_str(f, name ? name : "");
            fprintf(f, ",\"order\":%ld,\"bones\":[", order);
            for (long j = 0; j < bc; j++){
                long bi = rd_varint(&r);
                if (j) fputc(',', f);
                json_str(f, sa_get(&ctx.bones, (int)bi));
            }
            long target = rd_varint(&r);
            int local = rd_bool(&r), relative = rd_bool(&r);
            float rot, x, y, sx, sy, shy, rm, tm2, sm, shm;
            rd_f32(&r, &rot); rd_f32(&r, &x); rd_f32(&r, &y);
            rd_f32(&r, &sx); rd_f32(&r, &sy); rd_f32(&r, &shy);
            rd_f32(&r, &rm); rd_f32(&r, &tm2); rd_f32(&r, &sm); rd_f32(&r, &shm);
            fputs("],\"target\":", f); json_str(f, sa_get(&ctx.bones, (int)target));
            fputs(local ? ",\"local\":true" : ",\"local\":false", f);
            fputs(relative ? ",\"relative\":true" : ",\"relative\":false", f);
            fputs(",\"rotation\":", f); json_float(f, rot);
            fputs(",\"x\":", f); json_float(f, x);
            fputs(",\"y\":", f); json_float(f, y);
            fputs(",\"scaleX\":", f); json_float(f, sx);
            fputs(",\"scaleY\":", f); json_float(f, sy);
            fputs(",\"shearY\":", f); json_float(f, shy);
            fputs(",\"rotateMix\":", f); json_float(f, rm);
            fputs(",\"translateMix\":", f); json_float(f, tm2);
            fputs(",\"scaleMix\":", f); json_float(f, sm);
            fputs(",\"shearMix\":", f); json_float(f, shm);
            fputc('}', f);
            sa_push(&ctx.transform, name ? name : strdup(""));
        }
        fputs("]", f);
    }

    /* path */
    long npc = rd_varint(&r);
    if (npc > 0){
        fputs(",\"path\":[", f);
        for (long i = 0; i < npc; i++){
            char *name = rd_str(&r);
            long order = rd_varint(&r);
            long bc = rd_varint(&r);
            if (i) fputc(',', f);
            fputs("{\"name\":", f); json_str(f, name ? name : "");
            fprintf(f, ",\"order\":%ld,\"bones\":[", order);
            for (long j = 0; j < bc; j++){
                long bi = rd_varint(&r);
                if (j) fputc(',', f);
                json_str(f, sa_get(&ctx.bones, (int)bi));
            }
            long target = rd_varint(&r);
            long pm = rd_varint(&r), sm = rd_varint(&r), rtm = rd_varint(&r);
            float rot, pos, sp, rm2, tm2;
            rd_f32(&r, &rot); rd_f32(&r, &pos); rd_f32(&r, &sp);
            rd_f32(&r, &rm2); rd_f32(&r, &tm2);
            fputs("],\"target\":", f); json_str(f, sa_get(&ctx.slots, (int)target));
            static const char *pm_names[] = {"fixed","percent"};
            static const char *sm_names[] = {"length","fixed","percent"};
            static const char *rtm_names[] = {"tangent","chain","chainScale"};
            fputs(",\"positionMode\":\"", f);
            fputs(pm_names[(pm >= 0 && pm < 2) ? pm : 0], f);
            fputs("\",\"spacingMode\":\"", f);
            fputs(sm_names[(sm >= 0 && sm < 3) ? sm : 0], f);
            fputs("\",\"rotateMode\":\"", f);
            fputs(rtm_names[(rtm >= 0 && rtm < 3) ? rtm : 0], f);
            fputs("\"", f);
            fputs(",\"rotation\":", f); json_float(f, rot);
            fputs(",\"position\":", f); json_float(f, pos);
            fputs(",\"spacing\":", f); json_float(f, sp);
            fputs(",\"rotateMix\":", f); json_float(f, rm2);
            fputs(",\"translateMix\":", f); json_float(f, tm2);
            fputc('}', f);
            sa_push(&ctx.path, name ? name : strdup(""));
        }
        fputs("]", f);
    }

    /* skins：default 可能为空（null），后面再跟若干命名皮肤 */
    fputs(",\"skins\":{", f);
    int skin_first = 1;
    emit_skin(f, &r, &ctx, "default", &skin_first);
    long nsk = rd_varint(&r);
    for (long i = 0; i < nsk; i++){
        char *sk_name = rd_str(&r);
        emit_skin(f, &r, &ctx, sk_name ? sk_name : "", &skin_first);
        free(sk_name);
    }
    fputs("}", f);
    /* events：{"名":{"int":..,"float":..,"string":..}}（空则不输出） */
    long nev = rd_varint(&r);
    if (nev > 0){
        fputs(",\"events\":{", f);
        for (long i = 0; i < nev; i++){
            char *name = rd_str(&r);
            long iv = rd_int_opt(&r, 0);
            float fv; rd_f32(&r, &fv);
            char *sv = rd_str(&r);
            if (i) fputc(',', f);
            json_str(f, name ? name : ""); fputs(":{", f);
            int any = 0;
            if (iv != 0){ fprintf(f, "\"int\":%ld", iv); any = 1; }
            if (fv != 0){ if (any) fputc(',', f); fputs("\"float\":", f); json_float(f, fv); any = 1; }
            if (sv && sv[0]){ if (any) fputc(',', f); fputs("\"string\":", f); json_str(f, sv); }
            fputc('}', f);
            free(name); free(sv);
        }
        fputs("}", f);
    }

    /* animations */
    fputs(",\"animations\":{", f);
    long nan = rd_varint(&r);
    for (long i = 0; i < nan; i++){
        char *an_name = rd_str(&r);
        if (i) fputc(',', f);
        emit_animation(f, &r, &ctx, an_name ? an_name : "");
        free(an_name);
    }
    fputs("}}", f);
    fclose(f);
    sa_free(&ctx.bones); sa_free(&ctx.slots); sa_free(&ctx.ik);
    sa_free(&ctx.transform); sa_free(&ctx.path); sa_free(&ctx.skins); sa_free(&ctx.events);
    free(buf);
    return r.err ? -1 : 0;
}

static int convert_skel_to_json_ver_w(const wchar_t *skel_path, const wchar_t *json_path, const char *spine_ver){
    FILE *in = _wfopen(skel_path, L"rb");
    if (!in) return -1;
    int rc = convert_skel_fp(in, json_path, spine_ver);
    fclose(in);
    return rc;
}

int convert_skel_to_json_w(const wchar_t *skel_path, const wchar_t *json_path){
    return convert_skel_to_json_ver_w(skel_path, json_path, "3.6.47");
}

int convert_skel_to_json(const char *skel_path, const char *json_path){
    wchar_t ws[1300], wj[1300];
    if (!MultiByteToWideChar(CP_UTF8, 0, skel_path, -1, ws, 1300)) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, json_path, -1, wj, 1300)) return -1;
    return convert_skel_to_json_w(ws, wj);
}

int convert_skel_to_json_v38_w(const wchar_t *skel_path, const wchar_t *json_path){
    return convert_skel_to_json_ver_w(skel_path, json_path, "3.8.75");
}

int convert_skel_to_json_v38(const char *skel_path, const char *json_path){
    wchar_t ws[1300], wj[1300];
    if (!MultiByteToWideChar(CP_UTF8, 0, skel_path, -1, ws, 1300)) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, json_path, -1, wj, 1300)) return -1;
    return convert_skel_to_json_v38_w(ws, wj);
}

/* ================= Spine 2.1（小人 SPSprachen 共享骨架） =================
 * 与 3.6 相同的 44 字节 CGSS 头，但数据布局是 Spine 2.1：
 *   bones:    parent 每个骨骼都存(varint+1)；x,y,scaleX,scaleY,rotation,length,
 *             flipX,flipY,inheritScale,inheritRotation（无 shear/transform）
 *   slots:    无 dark，additiveBlending 布尔
 *   attachments: region/boundingbox/mesh/skinnedmesh
 *   animations: color=4, attachment=3, rotate=1, translate=2, scale=0,
 *             flipX=5, flipY=6；无 two-color/shear/transform/path
 *   drawOrder: offsetCount+offsets 在前，time 在最后
 * scale 用于把骨架坐标对齐到配套 atlas（SPC 卡面 atlas 是 0.5 倍分辨率）。
 */

static void emit_attachment21(FILE *f, Rd *r, Ctx *ctx, const char *key, float scale){
    char *name = rd_str(r);
    if (!name) name = strdup(key ? key : "");
    int type = rd_byte(r);
    if (type == 0){  /* region: path, x, y, scaleX, scaleY, rotation, width, height, color */
        char *path = rd_str(r);
        int path_own = path != NULL;
        if (!path) path = name;
        float x, y, sx, sy, rot, w, h;
        rd_f32(r, &x); rd_f32(r, &y);
        rd_f32(r, &sx); rd_f32(r, &sy);
        rd_f32(r, &rot);
        rd_f32(r, &w); rd_f32(r, &h);
        unsigned c; rd_u32(r, &c);
        fputs("{\"type\":\"region\",\"name\":", f); json_str(f, name);
        fputs(",\"path\":", f); json_str(f, path);
        fputs(",\"rotation\":", f); json_float(f, rot);
        fputs(",\"x\":", f); json_float(f, x * scale);
        fputs(",\"y\":", f); json_float(f, y * scale);
        fputs(",\"scaleX\":", f); json_float(f, sx);
        fputs(",\"scaleY\":", f); json_float(f, sy);
        fputs(",\"width\":", f); json_float(f, w * scale);
        fputs(",\"height\":", f); json_float(f, h * scale);
        fputs(",\"color\":", f); json_u32_color(f, c);
        fputc('}', f);
        if (path_own) free(path);
    } else if (type == 1){  /* boundingbox: 无权重 float 数组 */
        long vc = rd_varint(r);
        fputs("{\"type\":\"boundingbox\",\"name\":", f); json_str(f, name);
        fprintf(f, ",\"vertexCount\":%ld,\"vertices\":[", vc);
        for (long i = 0; i < vc; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v * scale);
        }
        fputs("]}", f);
    } else if (type == 2){  /* mesh（无权重） */
        char *path = rd_str(r);
        int path_own = path != NULL;
        if (!path) path = name;
        unsigned c; rd_u32(r, &c);
        long uv_count = rd_varint(r);
        fputs("{\"type\":\"mesh\",\"name\":", f); json_str(f, name);
        fputs(",\"path\":", f); json_str(f, path);
        fputs(",\"color\":", f); json_u32_color(f, c);
        fputs(",\"uvs\":[", f);
        for (long i = 0; i < uv_count; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v);
        }
        fputs("],\"triangles\":[", f);
        long nt = rd_varint(r);
        for (long i = 0; i < nt; i++){
            short v; rd_i16(r, &v);
            if (i) fputc(',', f);
            fprintf(f, "%d", (int)v);
        }
        fputs("],\"vertices\":[", f);
        long nv = rd_varint(r);
        for (long i = 0; i < nv; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v * scale);
        }
        fputc(']', f);
        long hull = rd_varint(r);
        fprintf(f, ",\"hull\":%ld}", hull);
        if (path_own) free(path);
    } else if (type == 3){  /* skinnedmesh -> 3.6 加权 mesh 数组 */
        char *path = rd_str(r);
        int path_own = path != NULL;
        if (!path) path = name;
        unsigned c; rd_u32(r, &c);
        long uv_count = rd_varint(r);
        fputs("{\"type\":\"mesh\",\"name\":", f); json_str(f, name);
        fputs(",\"path\":", f); json_str(f, path);
        fputs(",\"color\":", f); json_u32_color(f, c);
        fputs(",\"uvs\":[", f);
        for (long i = 0; i < uv_count; i++){
            float v; rd_f32(r, &v);
            if (i) fputc(',', f);
            json_float(f, v);
        }
        fputs("],\"triangles\":[", f);
        long nt = rd_varint(r);
        for (long i = 0; i < nt; i++){
            short v; rd_i16(r, &v);
            if (i) fputc(',', f);
            fprintf(f, "%d", (int)v);
        }
        fputs("],\"vertices\":[", f);
        long vc = rd_varint(r);
        int first = 1;
        for (long i = 0; i < vc; i++){
            float bcf; rd_f32(r, &bcf);
            long bc = (long)bcf;
            if (!first) fputc(',', f); first = 0;
            fprintf(f, "%ld", bc);
            for (long j = 0; j < bc; j++){
                float bi, x, y, w;
                rd_f32(r, &bi); rd_f32(r, &x); rd_f32(r, &y); rd_f32(r, &w);
                fprintf(f, ",%ld,", (long)bi);
                json_float(f, x * scale); fputc(',', f);
                json_float(f, y * scale); fputc(',', f);
                json_float(f, w);
            }
        }
        fputc(']', f);
        long hull = rd_varint(r);
        fprintf(f, ",\"hull\":%ld}", hull);
        if (path_own) free(path);
    }
    free(name);
}

static int emit_skin21(FILE *f, Rd *r, Ctx *ctx, const char *skin_name, int *first, float scale){
    long slot_count = rd_varint(r);
    if (slot_count == 0) return 0;
    if (*first) *first = 0; else fputc(',', f);
    fputs("\"", f); fputs(skin_name, f); fputs("\":{", f);
    for (long i = 0; i < slot_count; i++){
        long slot_idx = rd_varint(r);
        if (i) fputc(',', f);
        fputs("\"", f); fputs(sa_get(&ctx->slots, (int)slot_idx), f); fputs("\":{", f);
        long att_count = rd_varint(r);
        for (long j = 0; j < att_count; j++){
            char *att_name = rd_str(r);
            if (j) fputc(',', f);
            fputs("\"", f);
            if (att_name) fputs(att_name, f);
            fputs("\":", f);
            emit_attachment21(f, r, ctx, att_name, scale);
            free(att_name);
        }
        fputc('}', f);
    }
    fputc('}', f);
    return 1;
}

static void emit_animation21(FILE *f, Rd *r, Ctx *ctx, const char *name, float scale){
    long i, j, n;
    json_str(f, name); fputs(":{", f);
    int sec_first = 1;
    #define SEC21_BEGIN() do { if (!sec_first) fputc(',', f); sec_first = 0; } while (0)

    /* slot timelines: 3=attachment, 4=color */
    n = rd_varint(r);
    if (n > 0){
        SEC21_BEGIN();
        fputs("\"slots\":{", f);
        for (i = 0; i < n; i++){
            long slot_idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->slots, (int)slot_idx), f); fputs("\":{", f);
            long tt_count = rd_varint(r);
            for (j = 0; j < tt_count; j++){
                int ttype = rd_byte(r);
                long fc = rd_varint(r);
                if (j) fputc(',', f);
                fputs(ttype == 4 ? "\"color\":[" : "\"attachment\":[", f);
                int first = 1;
                for (long k = 0; k < fc; k++){
                    frame_begin(f, r, &first);
                    if (ttype == 4){
                        unsigned c; rd_u32(r, &c);
                        fputs(",\"color\":", f); json_u32_color(f, c);
                    } else {
                        char *an = rd_str(r);
                        fputs(",\"name\":", f);
                        if (an) json_str(f, an); else fputs("null", f);
                        free(an);
                    }
                    if (k < fc - 1 && ttype == 4) json_curve(f, r);
                    fputc('}', f);
                }
                fputs("]", f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* bone timelines: 1=rotate, 2=translate, 0=scale, 5/6=flipX/flipY */
    n = rd_varint(r);
    if (n > 0){
        SEC21_BEGIN();
        fputs("\"bones\":{", f);
        for (i = 0; i < n; i++){
            long bone_idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->bones, (int)bone_idx), f); fputs("\":{", f);
            long tt_count = rd_varint(r);
            int first_tl = 1;
            for (j = 0; j < tt_count; j++){
                int ttype = rd_byte(r);
                long fc = rd_varint(r);
                if (ttype == 5 || ttype == 6){  /* flip 时间线在 3.6 JSON 无对应，跳过 */
                    for (long k = 0; k < fc; k++){
                        float t; rd_f32(r, &t);
                        rd_bool(r);
                    }
                    continue;
                }
                if (!first_tl) fputc(',', f); first_tl = 0;
                const char *tn = ttype == 1 ? "rotate" : (ttype == 2 ? "translate" : "scale");
                fputs("\"", f); fputs(tn, f); fputs("\":[", f);
                int first = 1;
                for (long k = 0; k < fc; k++){
                    frame_begin(f, r, &first);
                    if (ttype == 1){
                        float a; rd_f32(r, &a);
                        fputs(",\"angle\":", f); json_float(f, a);
                    } else {
                        float x, y; rd_f32(r, &x); rd_f32(r, &y);
                        if (ttype == 2){ x *= scale; y *= scale; }
                        fputs(",\"x\":", f); json_float(f, x);
                        fputs(",\"y\":", f); json_float(f, y);
                    }
                    if (k < fc - 1) json_curve(f, r);
                    fputc('}', f);
                }
                fputs("]", f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* ik timelines */
    n = rd_varint(r);
    if (n > 0){
        SEC21_BEGIN();
        fputs("\"ik\":{", f);
        for (i = 0; i < n; i++){
            long idx = rd_varint(r);
            long fc = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->ik, (int)idx), f); fputs("\":[", f);
            int first = 1;
            for (long k = 0; k < fc; k++){
                frame_begin(f, r, &first);
                float m; rd_f32(r, &m);
                int bp = rd_byte(r) == 1;
                fputs(",\"mix\":", f); json_float(f, m);
                fputs(bp ? ",\"bendPositive\":true" : ",\"bendPositive\":false", f);
                if (k < fc - 1) json_curve(f, r);
                fputc('}', f);
            }
            fputs("]", f);
        }
        fputs("}", f);
    }

    /* deform */
    n = rd_varint(r);
    if (n > 0){
        SEC21_BEGIN();
        fputs("\"deform\":{", f);
        for (i = 0; i < n; i++){
            long skin_idx = rd_varint(r);
            if (i) fputc(',', f);
            fputs("\"", f); fputs(sa_get(&ctx->skins, (int)skin_idx), f); fputs("\":{", f);
            long slot_count = rd_varint(r);
            for (j = 0; j < slot_count; j++){
                long slot_idx = rd_varint(r);
                if (j) fputc(',', f);
                fputs("\"", f); fputs(sa_get(&ctx->slots, (int)slot_idx), f); fputs("\":{", f);
                long att_count = rd_varint(r);
                for (long k = 0; k < att_count; k++){
                    char *att_name = rd_str(r);
                    long fc = rd_varint(r);
                    if (k) fputc(',', f);
                    fputs("\"", f);
                    if (att_name) fputs(att_name, f);
                    fputs("\":[", f);
                    int first = 1;
                    for (long m = 0; m < fc; m++){
                        float t; rd_f32(r, &t);
                        long end = rd_varint(r);
                        if (first) first = 0; else fputc(',', f);
                        fputs("{\"time\":", f); json_float(f, t);
                        if (end != 0){
                            long start = rd_varint(r);
                            end += start;
                            fprintf(f, ",\"offset\":%ld,\"vertices\":[", start);
                            for (long v = start; v < end; v++){
                                float fv; rd_f32(r, &fv);
                                if (v > start) fputc(',', f);
                                json_float(f, fv * scale);
                            }
                            fputc(']', f);
                        }
                        if (m < fc - 1) json_curve(f, r);
                        fputc('}', f);
                    }
                    fputs("]", f);
                    free(att_name);
                }
                fputc('}', f);
            }
            fputc('}', f);
        }
        fputs("}", f);
    }

    /* drawOrder: 2.1 是 offsets 在前 time 在后 */
    n = rd_varint(r);
    if (n > 0){
        SEC21_BEGIN();
        fputs("\"drawOrder\":[", f);
        for (i = 0; i < n; i++){
            long oc = rd_varint(r);
            if (i) fputc(',', f);
            fputs("{\"offsets\":[", f);
            for (j = 0; j < oc; j++){
                long si = rd_varint(r), so = rd_varint(r);
                if (j) fputc(',', f);
                fputs("{\"slot\":", f); json_str(f, sa_get(&ctx->slots, (int)si));
                fprintf(f, ",\"offset\":%ld}", so);
            }
            float t; rd_f32(r, &t);
            fputs("],\"time\":", f); json_float(f, t);
            fputc('}', f);
        }
        fputs("]", f);
    }

    /* events */
    n = rd_varint(r);
    if (n > 0){
        SEC21_BEGIN();
        fputs("\"events\":[", f);
        for (i = 0; i < n; i++){
            float t; rd_f32(r, &t);
            long ev = rd_varint(r);
            long iv = rd_int_opt(r, 0);
            float fv; rd_f32(r, &fv);
            int has_str = rd_bool(r);
            char *sv = has_str ? rd_str(r) : NULL;
            if (i) fputc(',', f);
            fputs("{\"time\":", f); json_float(f, t);
            fputs(",\"name\":", f); json_str(f, sa_get(&ctx->events, (int)ev));
            if (iv != 0) fprintf(f, ",\"int\":%ld", iv);
            if (fv != 0){ fputs(",\"float\":", f); json_float(f, fv); }
            if (sv){ fputs(",\"string\":", f); json_str(f, sv); }
            fputc('}', f);
            free(sv);
        }
        fputs("]", f);
    }
    fputc('}', f);
    #undef SEC21_BEGIN
}

static int convert_skel21_fp(FILE *in, const wchar_t *json_path, const char *spine_ver, float scale){
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz < 44) return -1;
    unsigned char *buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) return -1;
    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz){
        free(buf);
        return -1;
    }
    Rd r;
    r.b = buf; r.len = sz; r.pos = 44; r.err = 0;

    FILE *f = _wfopen(json_path, L"wb");
    if (!f){ free(buf); return -1; }
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);

    fputs("{\"skeleton\":{\"hash\":\"cgss\",\"spine\":\"", f);
    fputs(spine_ver ? spine_ver : "2.1.27", f);
    fputs("\",\"width\":0,\"height\":0,\"fps\":30,\"images\":\"\",\"audio\":\"\"},\"bones\":[", f);

    /* bones */
    long nb = rd_varint(&r);
    for (long i = 0; i < nb; i++){
        char *name = rd_str(&r);
        long parent = rd_varint(&r) - 1;   /* 2.1: 每个骨骼都存 parent */
        float x, y, sx, sy, rot, len;
        rd_f32(&r, &x); rd_f32(&r, &y);
        rd_f32(&r, &sx); rd_f32(&r, &sy);
        rd_f32(&r, &rot);
        rd_f32(&r, &len);
        int flipX = rd_bool(&r), flipY = rd_bool(&r);
        rd_bool(&r);  /* inheritScale */
        rd_bool(&r);  /* inheritRotation */
        if (flipX) sx = -sx;
        if (flipY) sy = -sy;
        if (i) fputc(',', f);
        fputs("{\"name\":", f); json_str(f, name ? name : "");
        if (parent >= 0){
            fputs(",\"parent\":", f); json_str(f, sa_get(&ctx.bones, (int)parent));
        }
        if (rot != 0){ fputs(",\"rotation\":", f); json_float(f, rot); }
        if (x != 0 && x != 1){ fputs(",\"x\":", f); json_float(f, x * scale); }
        if (y != 0 && y != 1){ fputs(",\"y\":", f); json_float(f, y * scale); }
        if (sx != 1){ fputs(",\"scaleX\":", f); json_float(f, sx); }
        if (sy != 1){ fputs(",\"scaleY\":", f); json_float(f, sy); }
        if (len != 0 && len != 1){ fputs(",\"length\":", f); json_float(f, len * scale); }
        fputc('}', f);
        sa_push(&ctx.bones, strdup(name ? name : ""));
        free(name);
    }
    fputs("]", f);

    /* ik（2.1 顺序：bones -> ik -> slots） */
    long nik = rd_varint(&r);
    if (nik > 0){
        fputs(",\"ik\":[", f);
        for (long i = 0; i < nik; i++){
            char *name = rd_str(&r);
            long bc = rd_varint(&r);
            if (i) fputc(',', f);
            fputs("{\"name\":", f); json_str(f, name ? name : "");
            fputs(",\"bones\":[", f);
            for (long j = 0; j < bc; j++){
                long bi = rd_varint(&r);
                if (j) fputc(',', f);
                json_str(f, sa_get(&ctx.bones, (int)bi));
            }
            long target = rd_varint(&r);
            float mix; rd_f32(&r, &mix);
            int bend = rd_byte(&r);
            fputs("],\"target\":", f); json_str(f, sa_get(&ctx.bones, (int)target));
            fputs(",\"mix\":", f); json_float(f, mix);
            fputs(bend == 1 ? ",\"bendPositive\":true}" : ",\"bendPositive\":false}", f);
            sa_push(&ctx.ik, strdup(name ? name : ""));
            free(name);
        }
        fputs("]", f);
    }

    fputs(",\"slots\":[", f);

    /* slots */
    long nsl = rd_varint(&r);
    for (long i = 0; i < nsl; i++){
        char *name = rd_str(&r);
        long bi = rd_varint(&r);
        unsigned c; rd_u32(&r, &c);
        char *att = rd_str(&r);
        int additive = rd_bool(&r);
        if (i) fputc(',', f);
        fputs("{\"name\":", f); json_str(f, name ? name : "");
        fputs(",\"bone\":", f); json_str(f, sa_get(&ctx.bones, (int)bi));
        fputs(",\"color\":", f); json_u32_color(f, c);
        if (att){ fputs(",\"attachment\":", f); json_str(f, att); }
        fputs(additive ? ",\"blend\":\"additive\"}" : ",\"blend\":\"normal\"}", f);
        sa_push(&ctx.slots, strdup(name ? name : ""));
        free(name); free(att);
    }
    fputs("],\"skins\":{", f);

    /* default skin + skins */
    int first_skin = 1;
    int has_default = emit_skin21(f, &r, &ctx, "default", &first_skin, scale);
    long nsk = rd_varint(&r);
    for (long i = 0; i < nsk; i++){
        char *skin_name = rd_str(&r);
        sa_push(&ctx.skins, strdup(skin_name ? skin_name : ""));
        emit_skin21(f, &r, &ctx, skin_name ? skin_name : "", &first_skin, scale);
        free(skin_name);
    }
    fputs("}", f);

    /* events */
    long nev = rd_varint(&r);
    if (nev > 0){
        fputs(",\"events\":{", f);
        for (long i = 0; i < nev; i++){
            char *name = rd_str(&r);
            long iv = rd_int_opt(&r, 0);
            float fv; rd_f32(&r, &fv);
            char *sv = rd_str(&r);
            if (i) fputc(',', f);
            fputs("\"", f);
            if (name) fputs(name, f);
            fputs("\":{", f);
            if (iv != 0) fprintf(f, "\"int\":%ld", iv);
            if (fv != 0){ if (iv != 0) fputc(',', f); fputs("\"float\":", f); json_float(f, fv); }
            if (sv && *sv){ if (iv != 0 || fv != 0) fputc(',', f); fputs("\"string\":", f); json_str(f, sv); }
            fputc('}', f);
            free(name); free(sv);
        }
        fputs("}", f);
    }

    /* animations */
    long nanim = rd_varint(&r);
    fputs(",\"animations\":{", f);
    for (long i = 0; i < nanim; i++){
        char *name = rd_str(&r);
        if (i) fputc(',', f);
        emit_animation21(f, &r, &ctx, name ? name : "", scale);
        free(name);
    }
    fputs("}}", f);

    int ok = (r.err == 0);
    fclose(f);
    sa_free(&ctx.bones); sa_free(&ctx.slots); sa_free(&ctx.ik);
    sa_free(&ctx.skins); sa_free(&ctx.events);
    free(buf);
    if (!ok){
        _wremove(json_path);
        return -1;
    }
    return 0;
}

static int convert_skel21_to_json_ver_w(const wchar_t *skel_path, const wchar_t *json_path,
                                        const char *spine_ver, float scale){
    FILE *in = _wfopen(skel_path, L"rb");
    if (!in) return -1;
    int rc = convert_skel21_fp(in, json_path, spine_ver, scale);
    fclose(in);
    return rc;
}

int convert_skel21_to_json_w(const wchar_t *skel_path, const wchar_t *json_path, float scale){
    return convert_skel21_to_json_ver_w(skel_path, json_path, "3.6.47", scale);
}

int convert_skel21_to_json_v38_w(const wchar_t *skel_path, const wchar_t *json_path, float scale){
    return convert_skel21_to_json_ver_w(skel_path, json_path, "3.8.75", scale);
}

/* 识别 2.1 版骨架（头 44 字节里的版本串不是 3.x）并转换 */
static int skel21_version(const wchar_t *skel_path){
    FILE *in = _wfopen(skel_path, L"rb");
    if (!in) return 0;
    unsigned char h[48] = {0};
    size_t got = fread(h, 1, sizeof h, in);
    fclose(in);
    if (got < 36 || h[0] != 0x1C) return 0;
    /* 头: 0x1C + 27字节hash + 版本长度字节 + 版本串 */
    long vlen = h[28];
    if (vlen < 2 || vlen > 20 || 29 + vlen > 44) return 0;
    /* 3.6.47 / 2.1.27 等，长度 6 */
    if (vlen == 7 && 29 + 6 <= (int)got){
        if (memcmp(h + 29, "3.6.", 4) == 0 || memcmp(h + 29, "3.8.", 4) == 0) return 0;
        if (memcmp(h + 29, "2.1.", 4) == 0) return 1;
    }
    return 0;
}

int convert_skels_in_dir(const wchar_t *dir){
    wchar_t pat[1300];
    /* 兼容 AssetStudio 导出的 .skel 与 .skel.asset 两种命名 */
    swprintf(pat, 1300, L"%ls\\*.skel*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        /* 跳过上次生成的 .json（*.skel* 会匹配到 *.skel.json，避免二次转换截断文件） */
        {
            const wchar_t *fn = fd.cFileName;
            const wchar_t *dot = wcsrchr(fn, L'.');
            if (dot && _wcsicmp(dot, L".json") == 0) continue;
        }
        /* 校验文件头 0x1C（CGSS skel 头），防止把文本文件当二进制解析 */
        {
            wchar_t probe[1300];
            swprintf(probe, 1300, L"%ls\\%ls", dir, fd.cFileName);
            FILE *pf = _wfopen(probe, L"rb");
            if (pf){
                unsigned char b0 = 0;
                if (fread(&b0, 1, 1, pf) != 1 || b0 != 0x1C){ fclose(pf); continue; }
                fclose(pf);
            }
        }
        wchar_t src[1300], dst[1300];
        swprintf(src, 1300, L"%ls\\%ls", dir, fd.cFileName);
        wcscpy(dst, src);
        /* 去掉 .skel.asset / .skel 后缀，统一生成 name.json + name_v38.json */
        {
            size_t len = wcslen(dst);
            if (len > 11 && _wcsicmp(dst + len - 11, L".skel.asset") == 0)
                wcscpy(dst + len - 11, L".json");
            else if (len > 5 && _wcsicmp(dst + len - 5, L".skel") == 0)
                wcscpy(dst + len - 5, L".json");
            else {
                wchar_t *dot = wcsrchr(dst, L'.');
                if (dot) wcscpy(dot, L".json");
            }
        }
        int is21 = skel21_version(src);
        int ok36 = is21 ? (convert_skel21_to_json_w(src, dst, 0.5f) == 0)
                        : (convert_skel_to_json_w(src, dst) == 0);
        if (ok36){
            wchar_t v38[1300];
            wcscpy(v38, src);
            size_t len2 = wcslen(v38);
            if (len2 > 11 && _wcsicmp(v38 + len2 - 11, L".skel.asset") == 0)
                wcscpy(v38 + len2 - 11, L"_v38.json");
            else if (len2 > 5 && _wcsicmp(v38 + len2 - 5, L".skel") == 0)
                wcscpy(v38 + len2 - 5, L"_v38.json");
            else {
                wchar_t *d2 = wcsrchr(v38, L'.');
                if (d2) wcscpy(d2, L"_v38.json");
            }
            int v38ok = is21 ? (convert_skel21_to_json_v38_w(src, v38, 0.5f) == 0)
                             : (convert_skel_to_json_v38_w(src, v38) == 0);
            printf("  已生成 %ls + %ls%s\n", fd.cFileName,
                   wcsrchr(v38, L'\\') ? wcsrchr(v38, L'\\') + 1 : v38,
                   v38ok ? "" : "（3.8版失败）");
            n++;
        } else {
            printf("  转换失败 %ls\n", fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
