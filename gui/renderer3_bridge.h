#ifndef CGSS_GUI_RENDERER3_BRIDGE_H
#define CGSS_GUI_RENDERER3_BRIDGE_H

/*
 * cimgui 目前没有为 SDLRenderer3 生成 C 包装函数。
 * 这个小桥接层把 Dear ImGui 的 C++ 后端转成 C 可以调用的函数。
 */
#include <SDL3/SDL.h>

/* C/C++ 两边只需要这个不完整类型，不要在桥接文件里展开 cimgui 的结构体。 */
#ifdef __cplusplus
struct ImDrawData;
#else
typedef struct ImDrawData ImDrawData;
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool cgss_renderer3_init(SDL_Renderer *renderer);
void cgss_renderer3_shutdown(void);
void cgss_renderer3_new_frame(void);
void cgss_renderer3_render(ImDrawData *draw_data, SDL_Renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif
