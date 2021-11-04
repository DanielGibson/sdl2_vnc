#ifndef _SDL2_VNC_H
#define _SDL2_VNC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>

typedef struct {
    size_t size;
    void *data;
} vnc_buf;

typedef struct {
    uint8_t bpp;
    uint8_t depth;
    uint8_t is_big_endian;
    uint8_t is_true_colour;

    uint16_t red_max;
    uint16_t green_max;
    uint16_t blue_max;

    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
} vnc_pixel_format;

typedef struct {
    uint16_t w;
    uint16_t h;
    vnc_pixel_format fmt;
    uint32_t name_length;
    char *name;
} vnc_server_details;

typedef struct {
    uint16_t r;
    uint16_t g;
    uint16_t b;
} colour_map_entry;

typedef struct {
    colour_map_entry *data;
    size_t size;
} vnc_colour_map;

typedef struct {
    int socket;
    vnc_buf buffer;
    SDL_Surface *scratch_buffer;
    vnc_server_details server_details;
    SDL_Surface *surface;
    SDL_Thread *thread;
    unsigned int fps;
    vnc_colour_map colour_map;
    SDL_Window *window;
} SDL_vnc;

typedef enum {
    SDL_VNC_ERROR_OOM,
    SDL_VNC_ERROR_COULD_NOT_CREATE_SOCKET,
    SDL_VNC_ERROR_COULD_NOT_CONNECT,
    SDL_VNC_ERROR_SERVER_DISCONNECT,
    SDL_VNC_ERROR_UNSUPPORTED_SECURITY_PROTOCOLS,
    SDL_VNC_ERROR_SECURITY_HANDSHAKE_FAILED,
    SDL_VNC_ERROR_UNIMPLEMENTED,

    OK = 0
} SDL_vnc_result;

SDL_vnc_result init_vnc_connection(SDL_vnc *vnc, char *host, unsigned int port,
        unsigned int fps);

int wait_on_vnc_connection(SDL_vnc *vnc);

SDL_Window *create_window_for_connection(SDL_vnc *vnc, char *title, int x,
        int y, Uint32 flags);

int key_event(SDL_vnc *vnc, bool pressed, SDL_Keysym key);
int pointer_event(SDL_vnc *vnc, uint32_t button_mask, uint16_t x, uint16_t y,
        int32_t mw_x, int32_t mw_y);

#endif /* _SDL2_VNC_H */
