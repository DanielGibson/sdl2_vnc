#include <SDL2/SDL.h>

#include "SDL2_vnc.h"

#define exit_error(ret, msg, ...) do { \
    fprintf(stderr, msg "\n", ## __VA_ARGS__); \
    exit(ret); \
} while (0)

void usage(char *name) {
    printf("usage:\n%s host[:port]\n", name);
    exit(1);
}

void exit_on_sdl_error(int res) {
    if (!res) {
        return;
    }

    exit_error(1, "SDL error: %s", SDL_GetError());
}

void exit_on_vnc_error(VNC_Result res) {
    if (!res) {
        return;
    }

    exit_error(res, "VNC error: %s", VNC_ErrorString(res));
}

int parse_address(char *address) {

    /*
     * expects address of format host:port or just host (port defaults to 5900 then)
     */
    int port = 5900;
    char *split = strchr(address, ':');
    if(split) {
        *split = '\0';
        char *port_start = split + 1;
        port = strtol(port_start, NULL, 10);
    }
    return port;
}

int main(int argc, char **argv) {

    if (argc < 2) {
        usage(argv[0]);
    }

    char *host = argv[1];
    int port = parse_address(host);

    SDL_Init(SDL_INIT_VIDEO);
    VNC_Init();

    VNC_Connection vnc;
    int connection_result = VNC_InitConnection(&vnc, host, port, 60);
    exit_on_vnc_error(connection_result);

    SDL_Window *wind = VNC_CreateWindowForConnection(&vnc, "vncc",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0);
    exit_on_sdl_error(!wind);

    SDL_Renderer *rend = SDL_CreateRenderer(wind, -1, 0);
    exit_on_sdl_error(!rend);

    SDL_bool running = SDL_TRUE;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    running = SDL_FALSE;
                    break;

                case SDL_KEYUP:
                case SDL_KEYDOWN:
                    VNC_SendKeyEvent(&vnc, e.key.state == SDL_PRESSED,
                            e.key.keysym);
                    break;

                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEMOTION: {
                    int x;
                    int y;
                    uint32_t buttons = SDL_GetMouseState(&x, &y);
                    VNC_SendPointerEvent(&vnc, buttons, x, y, 0, 0);
                    break;
                }

                case SDL_MOUSEWHEEL: {
                    int x;
                    int y;
                    uint32_t buttons = SDL_GetMouseState(&x, &y);

                    VNC_SendPointerEvent(&vnc, buttons, x, y,
                            e.wheel.x, e.wheel.y);
                    VNC_SendPointerEvent(&vnc, buttons, x, y, 0, 0);
                    break;
                }

                default:
                    if (e.type == VNC_SHUTDOWN) {
                        exit_on_vnc_error(e.user.code);
                        running = SDL_FALSE;
                    }
                    break;
            }

        }

        SDL_Texture *text = SDL_CreateTextureFromSurface(rend, vnc.surface);
        SDL_RenderCopy(rend, text, NULL, NULL);
        SDL_DestroyTexture(text);

        SDL_RenderPresent(rend);

        SDL_Delay(1000/vnc.fps);
    }

    return 0;
}

/* vim: se ft=c tw=80 ts=4 sw=4 et : */
