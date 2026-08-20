#ifndef CGSS_SPINE_SCENE_H
#define CGSS_SPINE_SCENE_H

#include <stdbool.h>

/* SDL 的具体结构体定义只在 spine_scene.c 中需要。 */
struct SDL_Renderer;

typedef struct SpineScene SpineScene;
/*创建Spine场景*/
SpineScene *spine_scene_create(struct SDL_Renderer *renderer);
/*删除Spine场景*/
void spine_scene_destroy(SpineScene *scene);

/*
 * 在 exe 目录、其父目录以及 CGSS\build\CGSS_DOWN 中寻找第一套可用
 * 的 Spine 资源。资源目录必须同时包含 JSON 和 atlas 文件。
 */
bool spine_scene_autoload(SpineScene *scene, const char *exe_dir_utf8);

/* 直接加载一个已经解包的 Spine 目录。 */
bool spine_scene_load_directory(SpineScene *scene, const char *directory_utf8);

void spine_scene_update(SpineScene *scene, float delta_seconds);

/* 在 SDL_Renderer 当前目标上绘制场景；绘制前应先清屏。 */
bool spine_scene_render(SpineScene *scene, int width, int height,
                        float opacity, float zoom);

bool spine_scene_is_loaded(const SpineScene *scene);
const char *spine_scene_directory(const SpineScene *scene);
const char *spine_scene_name(const SpineScene *scene);
const char *spine_scene_error(const SpineScene *scene);
int spine_scene_layer_count(const SpineScene *scene);
const char *spine_scene_layer_name(const SpineScene *scene, int index);
const char *spine_scene_layer_animation(const SpineScene *scene, int index);

#endif /* CGSS_SPINE_SCENE_H */
