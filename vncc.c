#include <stdio.h>

#include "SDL2_vnc.h"

int main(int argc, char **argv) {

    SDL_Init(SDL_INIT_VIDEO);

    SDL_Renderer *rend;
    SDL_Window *wind;
    SDL_CreateWindowAndRenderer(800, 600, 0, &wind, &rend);
    SDL_Rect window_size;
    window_size.x = 0;
    window_size.y = 0;
    window_size.w = 800;
    window_size.h = 600;

    SDL_ShowCursor(SDL_DISABLE);

    SDL_vnc vnc;
    printf("trying to make connection\n");
    init_vnc_connection(&vnc, "127.0.0.1", 5905, 60);
    printf("connection succeeded!\n");

    bool running = true;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_KEYUP:
                case SDL_KEYDOWN:
                    key_event(&vnc, e.key.state == SDL_PRESSED, e.key.keysym);
                    break;

                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEMOTION: {
                    int x;
                    int y;
                    uint32_t buttons = SDL_GetMouseState(&x, &y);
                    pointer_event(&vnc, buttons, x, y, 0, 0);
                    break;
                }

                case SDL_MOUSEWHEEL: {
                    int x;
                    int y;
                    uint32_t buttons = SDL_GetMouseState(&x, &y);
                    pointer_event(&vnc, buttons, x, y, e.wheel.x, e.wheel.y);
                    pointer_event(&vnc, buttons, x, y, 0, 0);
                    break;
                }

                default:
                    break;
            }
        }

        SDL_Texture *text = SDL_CreateTextureFromSurface(rend, vnc.surface);
        SDL_RenderCopy(rend, text, &window_size, NULL);
        SDL_DestroyTexture(text);

        SDL_RenderPresent(rend);

        SDL_Delay(1000/60);
    }

    return 0;
}
