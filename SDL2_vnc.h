#ifndef _SDL2_VNC_H
#define _SDL2_VNC_H

#include <SDL2/SDL.h>

typedef struct {
    size_t size;
    void *data;
} VNC_ConnectionBuffer;

typedef struct {
    Uint8 bpp;
    Uint8 depth;
    Uint8 is_big_endian;
    Uint8 is_true_color;

    Uint16 red_max;
    Uint16 green_max;
    Uint16 blue_max;

    Uint8 red_shift;
    Uint8 green_shift;
    Uint8 blue_shift;
} VNC_PixelFormat;

typedef struct {
    Uint16 w;
    Uint16 h;
    VNC_PixelFormat fmt;
    Uint32 name_length;
    char *name;
} VNC_ServerDetails;

typedef struct {
    Uint16 r;
    Uint16 g;
    Uint16 b;
} VNC_ColorMapEntry;
#define VNC_ColourMapEntry VNC_ColorMapEntry

typedef struct {
    VNC_ColorMapEntry *data;
    size_t size;
} VNC_ColorMap;
#define VNC_ColourMap VNC_ColorMap

typedef struct {
    int socket;
    VNC_ConnectionBuffer buffer;
    SDL_Surface *scratch_buffer;
    VNC_ServerDetails server_details;
    SDL_Surface *surface;
    SDL_Thread *thread;
    unsigned fps;
    VNC_ColorMap color_map;
    SDL_Window *window;
} VNC_Connection;

typedef enum {
    VNC_OK = 0,

    VNC_ERROR_OOM,
    VNC_ERROR_COULD_NOT_CREATE_SOCKET,
    VNC_ERROR_COULD_NOT_CONNECT,
    VNC_ERROR_SERVER_DISCONNECT,
    VNC_ERROR_UNSUPPORTED_SECURITY_PROTOCOLS,
    VNC_ERROR_SECURITY_HANDSHAKE_FAILED,
    VNC_ERROR_UNIMPLEMENTED
} VNC_Result;

extern int VNC_SHUTDOWN;

int VNC_Init();

char *VNC_ErrorString(VNC_Result err);

VNC_Result VNC_InitConnection(VNC_Connection *vnc, char *host, Uint16 port,
        unsigned fps);

int VNC_WaitOnConnection(VNC_Connection *vnc);

SDL_Window *VNC_CreateWindowForConnection(VNC_Connection *vnc, char *title,
        int x, int y, Uint32 flags);

int VNC_SendKeyEvent(VNC_Connection *vnc, SDL_bool pressed, SDL_Keysym key);
int VNC_SendPointerEvent(VNC_Connection *vnc, Uint32 button_mask,
        Uint16 x, Uint16 y, Sint32 mw_x, Sint32 mw_y);

#endif /* _SDL2_VNC_H */

/* vim: se ft=c tw=80 ts=4 sw=4 et : */
