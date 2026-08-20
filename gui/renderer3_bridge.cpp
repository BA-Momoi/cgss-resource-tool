#include "imgui_impl_sdlrenderer3.h"
#include "renderer3_bridge.h"

extern "C" bool cgss_renderer3_init(SDL_Renderer *renderer)
{
    return ImGui_ImplSDLRenderer3_Init(renderer);
}

extern "C" void cgss_renderer3_shutdown(void)
{
    ImGui_ImplSDLRenderer3_Shutdown();
}

extern "C" void cgss_renderer3_new_frame(void)
{
    ImGui_ImplSDLRenderer3_NewFrame();
}

extern "C" void cgss_renderer3_render(ImDrawData *draw_data, SDL_Renderer *renderer)
{
    ImGui_ImplSDLRenderer3_RenderDrawData(draw_data, renderer);
}
