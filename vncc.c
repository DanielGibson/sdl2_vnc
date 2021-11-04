#include <stdio.h>

#include "SDL2_vnc.h"

int main(int argc, char **argv) {

    SDL_Init(SDL_INIT_VIDEO);

    SDL_vnc vnc;
    printf("trying to make connection\n");
    init_vnc_connection(&vnc, "127.0.0.1", 5905, 60);
    printf("connection succeeded!\n");

    SDL_Window *wind = create_window_for_connection(&vnc, NULL,
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0);

    SDL_Renderer *rend = SDL_CreateRenderer(wind, -1, 0);

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
        SDL_RenderCopy(rend, text, NULL, NULL);
        SDL_DestroyTexture(text);

        SDL_RenderPresent(rend);

        SDL_Delay(1000/60);
    }

    return 0;
}
