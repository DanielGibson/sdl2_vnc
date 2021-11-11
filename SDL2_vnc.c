#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <SDL2/SDL.h>

#include "keysymdef.h"

#include "SDL2_vnc.h"

#define VNC_INITIAL_BUFSIZE 64;

typedef unsigned int uint;

typedef enum {
    RFB_33 = 3,
    RFB_37 = 7,
    RFB_38 = 8,
    RFB_OTHER
} VNC_RFBProtocolVersion;

typedef enum {
    FRAME_BUFFER_UPDATE = 0,
    SET_COLOUR_MAP_ENTRIES = 1,
    BELL = 2,
    SERVER_CUT_TEXT = 3
} VNC_ServerMessageType;

typedef enum {
    RAW = 0,
    COPY_RECT = 1,
    RRE = 2,
    HEXTILE = 5,
    TRLE = 15,
    ZRLE = 16,

    PSEUDO_CONTINUOUS_UPDATES = 150,
    PSEUDO_CURSOR = -239,
    PSEUDO_DESKTOP_SIZE = -223
} VNC_RectangleEncodingMethod;

typedef struct {
    SDL_Rect r;
    VNC_RectangleEncodingMethod e;
} VNC_RectangleHeader;

int VNC_SHUTDOWN;

#define RFB_33_STR "RFB 003.003\n"
#define RFB_37_STR "RFB 003.007\n"
#define RFB_38_STR "RFB 003.008\n"

#define debug(...) SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define info(...) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define warn(...) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)

char *VNC_ErrorString(VNC_Result err) {
    switch (err) {

        case VNC_OK:
            return "no error";

        case VNC_ERROR_OOM:
            return "could not allocate required memory";

        case VNC_ERROR_COULD_NOT_CREATE_SOCKET:
            return "could not create socket";

        case VNC_ERROR_COULD_NOT_CONNECT:
            return "could not connect to VNC server";

        case VNC_ERROR_SERVER_DISCONNECT:
            return "server disconnected";

        case VNC_ERROR_UNSUPPORTED_SECURITY_PROTOCOLS:
            return "unsupported security protocols";

        case VNC_ERROR_SECURITY_HANDSHAKE_FAILED:
            return "security handshake failed";

        case VNC_ERROR_UNIMPLEMENTED:
            return "feature unimplemented";

        default:
            return "unknown error";
    }
}

int VNC_InitBuffer(VNC_ConnectionBuffer *buffer) {
    buffer->size = VNC_INITIAL_BUFSIZE;
    buffer->data = SDL_malloc(buffer->size);

    return buffer->data == NULL;
}

int VNC_CreateSocket(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct timeval t;
    t.tv_sec = 0;
    t.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));

    return sock;
}

int VNC_Connect(VNC_Connection *vnc, char *host, uint port) {
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, host, &address.sin_addr);
    return connect(vnc->socket, (struct sockaddr *) &address, sizeof(address));
}

int VNC_ResizeBuffer(VNC_ConnectionBuffer buffer, size_t n) {
    buffer.data = realloc(buffer.data, n);

    return !buffer.data;
}

int VNC_AssureBufferSize(VNC_ConnectionBuffer buffer, size_t n) {
    if (n > buffer.size) {
        return VNC_ResizeBuffer(buffer, n);
    }

    return 0;
}

int VNC_FromServer(int socket, void *buffer, size_t n) {

    size_t left_to_read = n;
    char *needle = buffer;

    while (left_to_read > 0) {
        ssize_t bytes_read = recv(socket, needle, left_to_read, 0);

        if (bytes_read < 0) {
            return -1;
        }

        if (bytes_read == 0) {
            return n - left_to_read;
        }

        left_to_read -= bytes_read;
        needle += bytes_read;
    }

    return n;
}

int VNC_ServerToBuffer(VNC_Connection *vnc, size_t n) {
    VNC_AssureBufferSize(vnc->buffer, n);
    return VNC_FromServer(vnc->socket, vnc->buffer.data, n);
}

int VNC_AssureScratchBufferSize(VNC_Connection *vnc, size_t w, size_t h) {
    VNC_ServerDetails details = vnc->server_details;

    if (vnc->scratch_buffer && vnc->scratch_buffer->w == w &&
            vnc->scratch_buffer->h == h) {
        return 0;
    }

    if (vnc->scratch_buffer) {
        SDL_FreeSurface(vnc->scratch_buffer);
    }

    vnc->scratch_buffer = SDL_CreateRGBSurface(0, w, h, details.fmt.bpp,
            details.fmt.red_max << details.fmt.red_shift,
            details.fmt.green_max << details.fmt.green_shift,
            details.fmt.blue_max << details.fmt.blue_shift,
            0);
    SDL_SetSurfaceBlendMode(vnc->scratch_buffer, SDL_BLENDMODE_NONE);

    return 0;
}

int VNC_ServerToScratchBuffer(VNC_Connection *vnc, size_t w, size_t h) {
    VNC_AssureScratchBufferSize(vnc, w, h);

    size_t pixel_data_size = w * h * vnc->server_details.fmt.bpp / 8;

    if (SDL_MUSTLOCK(vnc->scratch_buffer)) {
        SDL_LockSurface(vnc->scratch_buffer);
    }

    VNC_FromServer(vnc->socket, vnc->scratch_buffer->pixels, pixel_data_size);

    if (SDL_MUSTLOCK(vnc->scratch_buffer)) {
        SDL_UnlockSurface(vnc->scratch_buffer);
    }

    return 0;
}

int VNC_ToServer(int socket, void *data, size_t n) {
    return send(socket, data, n, 0);
}

VNC_RFBProtocolVersion VNC_DeduceRFBProtocolVersion(char *str) {
    if (!strncmp(str, RFB_33_STR, 12)) {
        return RFB_33;

    } else if (!strncmp(str, RFB_37_STR, 12)) {
        return RFB_37;

    } else if (!strncmp(str, RFB_38_STR, 12)) {
        return RFB_38;

    } else {
        return RFB_OTHER;
    }
}

char *VNC_RFBVersionString(VNC_RFBProtocolVersion ver) {
    switch (ver) {
        case RFB_33:
            return RFB_33_STR;

        case RFB_37:
            return RFB_37_STR;

        case RFB_38:
            return RFB_38_STR;

        default: // including RFB_OTHER
            return RFB_33_STR;
    }
}

VNC_RFBProtocolVersion VNC_ReceiveServerVersion(VNC_Connection *vnc) {
    char protocol_string[12];
    VNC_FromServer(vnc->socket, protocol_string, 12);

    VNC_RFBProtocolVersion ver = VNC_DeduceRFBProtocolVersion(protocol_string);

    return ver;
}

int VNC_SendClientVersion(VNC_Connection *vnc, VNC_RFBProtocolVersion ver) {
    char *ver_str = VNC_RFBVersionString(ver);
    return VNC_ToServer(vnc->socket, ver_str, 12);
}

typedef enum {
    RFB_SECURITY_INVALID = 0,
    RFB_SECURITY_NONE = 1,
    RFB_SECURITY_VNC_AUTH = 2
} RFB_security_protocol;

SDL_bool VNC_NoAuthSupported(Uint8 *options, size_t n) {
    for (uint i = 0; i < n; i++) {
        if (options[i] == RFB_SECURITY_NONE) {
            return SDL_TRUE;
        }
    }

    return SDL_FALSE;
}

int VNC_NegotiateSecurity33(VNC_Connection *vnc) {
    return VNC_ERROR_UNIMPLEMENTED;
}

int VNC_NegotiateSecurity37(VNC_Connection *vnc) {
    return VNC_ERROR_UNIMPLEMENTED;
}

int VNC_NegotiateSecurity38(VNC_Connection *vnc) {
    Uint8 security_protocol_count;
    VNC_FromServer(vnc->socket, &security_protocol_count, 1);

    if (!security_protocol_count) {
        return VNC_ERROR_SERVER_DISCONNECT;
    }

    VNC_ServerToBuffer(vnc, security_protocol_count);

    SDL_bool no_security_supported =
        VNC_NoAuthSupported(vnc->buffer.data, security_protocol_count);

    if (!no_security_supported) {
        return VNC_ERROR_UNSUPPORTED_SECURITY_PROTOCOLS;
    }

    RFB_security_protocol no_sec = RFB_SECURITY_NONE;
    VNC_ToServer(vnc->socket, &no_sec, 1);

    Uint32 security_handshake_error;
    VNC_FromServer(vnc->socket, &security_handshake_error, 4);

    if (security_handshake_error) {
        return VNC_ERROR_SECURITY_HANDSHAKE_FAILED;
    }

    return 0;
}

int VNC_NegotiateSecurity(VNC_Connection *vnc, VNC_RFBProtocolVersion ver) {
    switch (ver) {
        case RFB_33:
            return VNC_NegotiateSecurity33(vnc);

        case RFB_37:
            return VNC_NegotiateSecurity37(vnc);

        case RFB_38:
            return VNC_NegotiateSecurity38(vnc);

        default:
            return VNC_NegotiateSecurity33(vnc);
    }
}

int VNC_ClientInitialisation(VNC_Connection *vnc) {
    Uint8 shared_flag = 0;
    VNC_ToServer(vnc->socket, &shared_flag, 1);
    return 0;
}

int VNC_ServerInitialisation(VNC_Connection *vnc) {
    VNC_ServerToBuffer(vnc, 24);

    Uint16 *framebuffer_info = (Uint16 *) vnc->buffer.data;
    vnc->server_details.w = SDL_SwapBE16(framebuffer_info[0]);
    vnc->server_details.h = SDL_SwapBE16(framebuffer_info[1]);

    Uint8 *server_pixel_info = (Uint8 *) &framebuffer_info[2];
    vnc->server_details.fmt.bpp = server_pixel_info[0];
    vnc->server_details.fmt.depth = server_pixel_info[1];
    vnc->server_details.fmt.is_big_endian = server_pixel_info[2];
    vnc->server_details.fmt.is_true_color = server_pixel_info[3];

    Uint16 *server_color_maxima = (Uint16 *) &server_pixel_info[4];
    vnc->server_details.fmt.red_max = SDL_SwapBE16(server_color_maxima[0]);
    vnc->server_details.fmt.green_max = SDL_SwapBE16(server_color_maxima[1]);
    vnc->server_details.fmt.blue_max = SDL_SwapBE16(server_color_maxima[2]);

    Uint8 *server_color_shifts = (Uint8 *) &server_color_maxima[3];
    vnc->server_details.fmt.red_shift = server_color_shifts[0];
    vnc->server_details.fmt.green_shift = server_color_shifts[1];
    vnc->server_details.fmt.blue_shift = server_color_shifts[2];

    char *padding = (char *) &server_color_shifts[3];
    padding += 3;

    Uint32 *server_name_info = (Uint32 *) padding;
    vnc->server_details.name_length = SDL_SwapBE32(server_name_info[0]);

    if (vnc->server_details.name_length) {

        vnc->server_details.name = malloc(vnc->server_details.name_length + 1);
        VNC_FromServer(vnc->socket, vnc->server_details.name,
                vnc->server_details.name_length);
        vnc->server_details.name[vnc->server_details.name_length] = '\0';

    } else {
        vnc->server_details.name = NULL;
    }

    info(
            "server '%s':\n"
            "  pixel depth: %u (%u bpp)\n"
            "  true color %s\n"
            "  %s pixel values\n"
            "  red:   %x << %2u\n"
            "  green: %x << %2u\n"
            "  blue:  %x << %2u\n",
            vnc->server_details.name ?
                vnc->server_details.name : "UNNAMED",
            vnc->server_details.fmt.depth,
            vnc->server_details.fmt.bpp,
            vnc->server_details.fmt.is_true_color
                ? "enabled" : "disabled",
            vnc->server_details.fmt.is_big_endian
                ? "big-endian" : "little-endian",
            vnc->server_details.fmt.red_max,
            vnc->server_details.fmt.red_shift,
            vnc->server_details.fmt.green_max,
            vnc->server_details.fmt.green_shift,
            vnc->server_details.fmt.blue_max,
            vnc->server_details.fmt.blue_shift
        );

    return 0;
}

SDL_Surface *VNC_CreateSurfaceForServer(VNC_ServerDetails *details) {
    return SDL_CreateRGBSurface(0, details->w, details->h, details->fmt.bpp,
            details->fmt.red_max << details->fmt.red_shift,
            details->fmt.green_max << details->fmt.green_shift,
            details->fmt.blue_max << details->fmt.blue_shift,
            0);
}

int VNC_SetEncodings(VNC_Connection *vnc,
        VNC_RectangleEncodingMethod *encodings, uint n) {

    /*
     * Message size is:
     * - 2 bytes for message type;
     * - 1 byte padding;
     * - 2 bytes for number of encodings;
     * - 4-byte-wide encoding ID per encoding.
     */
    size_t msg_size = 4 + n * 4;

    VNC_AssureBufferSize(vnc->buffer, msg_size);

    Uint8 *msg = (Uint8 *) vnc->buffer.data;
    *msg++ = 2; // ID of SetEncoding message
    msg++;

    Uint16 *encoding_count = (Uint16 *) msg;
    *encoding_count++ = SDL_SwapBE16(n);

    Sint32 *encoding_ids = (Sint32 *) encoding_count;
    for (uint i = 0; i < n; i++) {
        *encoding_ids++ = SDL_SwapBE32(encodings[i]);
    }

    return VNC_ToServer(vnc->socket, vnc->buffer.data, msg_size);
}

int VNC_RawFromServer(VNC_Connection *vnc, VNC_RectangleHeader *header) {
    VNC_ServerToScratchBuffer(vnc, header->r.w, header->r.h);
    return SDL_BlitSurface(vnc->scratch_buffer, NULL, vnc->surface, &header->r);
}

int VNC_CopyRectFromServer(VNC_Connection *vnc, VNC_RectangleHeader *header) {
    VNC_ServerToBuffer(vnc, 4);
    Uint16 *src_info = (Uint16 *) vnc->buffer.data;

    SDL_Rect src;
    src.x = SDL_SwapBE16(*src_info++);
    src.y = SDL_SwapBE16(*src_info++);
    src.w = header->r.w;
    src.h = header->r.h;

    return SDL_BlitSurface(vnc->surface, &src, vnc->surface, &header->r);
}

int VNC_DesktopSizeFromServer(VNC_Connection *vnc, VNC_RectangleHeader *header) {
    vnc->server_details.w = header->r.w;
    vnc->server_details.h = header->r.h;

    if (vnc->surface) {
        SDL_FreeSurface(vnc->surface);
        vnc->surface = VNC_CreateSurfaceForServer(&vnc->server_details);
    }

    if (vnc->window) {
        SDL_SetWindowSize(vnc->window, header->r.w, header->r.h);
    }

    return 0;
}

int VNC_HandleRectangle(VNC_Connection *vnc, VNC_RectangleHeader *header) {
    VNC_ServerToBuffer(vnc, 12);

    Uint16 *rect_info = (Uint16 *) vnc->buffer.data;
    header->r.x = SDL_SwapBE16(*rect_info++);
    header->r.y = SDL_SwapBE16(*rect_info++);
    header->r.w = SDL_SwapBE16(*rect_info++);
    header->r.h = SDL_SwapBE16(*rect_info++);

    header->e = SDL_SwapBE32(*((Sint32 *) (rect_info)));

    switch (header->e) {
        case RAW:
            return VNC_RawFromServer(vnc, header);

        case COPY_RECT:
            return VNC_CopyRectFromServer(vnc, header);

        case PSEUDO_DESKTOP_SIZE:
            return VNC_DesktopSizeFromServer(vnc, header);

        default:
            warn("unknown encoding method %i\n", header->e);
            exit(0);
            return 0;
    }
}

int VNC_FrameBufferUpdate(VNC_Connection *vnc) {
    char buf[3];
    VNC_FromServer(vnc->socket, buf, 3);
    Uint16 rect_count = SDL_SwapBE16(*((Uint16 *) (buf + 1)));

    debug("receiving framebuffer update of %u rectangles\n", rect_count);

    for (uint i = 0; i < rect_count; i++) {
        VNC_RectangleHeader header;
        VNC_HandleRectangle(vnc, &header);
    }

    return 0;
}

int VNC_FramebufferUpdateRequest(int socket, SDL_bool incremental, Uint16 x,
        Uint16 y, Uint16 w, Uint16 h) {

    debug("sending framebuffer update request\n");

    char msg[10];

    Uint8 *msg_as_8b = (Uint8 *) msg;
    *msg_as_8b++ = 3;
    *msg_as_8b++ = incremental;

    Uint16 *msg_as_16b = (Uint16 *) msg_as_8b;
    *msg_as_16b++ = x;
    *msg_as_16b++ = y;
    *msg_as_16b++ = w;
    *msg_as_16b++ = h;

    return VNC_ToServer(socket, msg, 10);
}

int VNC_ResizeColorMap(VNC_ColorMap *color_map, size_t n) {
    color_map->data = realloc(color_map->data, n * sizeof (VNC_ColorMapEntry));

    return !color_map->data;
}

int VNC_AssureColourMapSize(VNC_ColorMap *color_map, size_t n) {
    if (color_map->size < n) {
        return VNC_ResizeColorMap(color_map, n);
    }

    return 0;
}

int VNC_SetColorMapEntries(VNC_Connection *vnc) {
    VNC_ServerToBuffer(vnc, 5);

    char *blank = (char *) (vnc->buffer.data);
    blank++;

    Uint16 *buf = (Uint16 *) blank;
    Uint16 first_color_index = *buf++;
    Uint16 number_of_colors = *buf++;
    Uint16 color_index_end = first_color_index + number_of_colors;

    debug("updating colors %u-%u in color map\n", first_color_index,
            color_index_end - 1);

    VNC_AssureColourMapSize(&vnc->color_map, color_index_end);

    for (uint i = first_color_index; i < color_index_end; i++) {
        VNC_ServerToBuffer(vnc, 6);

        Uint16 *colors = (Uint16 *) vnc->buffer.data;
        vnc->color_map.data[i].r = *colors++;
        vnc->color_map.data[i].g = *colors++;
        vnc->color_map.data[i].b = *colors++;
    }

    return 0;
}

int VNC_UpdateLoop(void *data) {
    VNC_Connection *vnc = data;

    SDL_Event disconnect_event;
    disconnect_event.type = VNC_SHUTDOWN;
    disconnect_event.user.code = 0;

    while (vnc->thread) {
        VNC_ServerMessageType msg;
        int res = VNC_FromServer(vnc->socket, &msg, 1);

        if (res <= 0) {
            disconnect_event.user.code = VNC_ERROR_SERVER_DISCONNECT;
            break;
        }

        switch (msg) {
            case FRAME_BUFFER_UPDATE:
                VNC_FrameBufferUpdate(vnc);
                break;

            case SET_COLOUR_MAP_ENTRIES:
                VNC_SetColorMapEntries(vnc);
                break;

            //case BELL:
            //    //server_bell(vnc);
            //    break;

            //case SERVER_CUT_TEXT:
            //    //server_cut_text(vnc);
            //    break;

            default:
                disconnect_event.user.code = VNC_ERROR_UNIMPLEMENTED;
                goto out_of_loop;
        }

        VNC_FramebufferUpdateRequest(vnc->socket, SDL_TRUE, 0, 0,
                vnc->server_details.w, vnc->server_details.h);

        SDL_Delay(1000 / vnc->fps);
    }

out_of_loop:

    SDL_PushEvent(&disconnect_event);

    return 0;
}

SDL_Thread *VNC_CreateUpdateThread(VNC_Connection *vnc) {
    return SDL_CreateThread(VNC_UpdateLoop, "RFB Listener", vnc);
}

int VNC_Handshake(VNC_Connection *vnc) {
    VNC_RFBProtocolVersion server_version = VNC_ReceiveServerVersion(vnc);
    VNC_RFBProtocolVersion client_version =
        server_version == RFB_OTHER ? RFB_33 : server_version;

    VNC_SendClientVersion(vnc, client_version);

    VNC_NegotiateSecurity(vnc, client_version);

    VNC_ClientInitialisation(vnc);
    VNC_ServerInitialisation(vnc);

    return 0;
}

int VNC_SendInitialFramebufferUpdateRequest(VNC_Connection *vnc) {
    return VNC_FramebufferUpdateRequest(vnc->socket, SDL_FALSE, 0, 0,
            vnc->server_details.w, vnc->server_details.h);
}

int VNC_Init(void) {
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    VNC_SHUTDOWN = SDL_RegisterEvents(1);
    return 0;
}

VNC_Result VNC_InitConnection(VNC_Connection *vnc, char *host, Uint16 port,
        unsigned fps) {

    int res;

    vnc->fps = fps;
    vnc->scratch_buffer = NULL;

    res = VNC_InitBuffer(&vnc->buffer);
    if (res) {
        return VNC_ERROR_OOM;
    }

    vnc->socket = VNC_CreateSocket();
    if (vnc->socket <= 0) {
        return VNC_ERROR_COULD_NOT_CREATE_SOCKET;
    }

    res = VNC_Connect(vnc, host, port);
    if (res) {
        return VNC_ERROR_COULD_NOT_CONNECT;
    }

    VNC_Handshake(vnc);

    VNC_RectangleEncodingMethod encodings[] = {
        COPY_RECT,
        RAW,
        PSEUDO_DESKTOP_SIZE,
        PSEUDO_CONTINUOUS_UPDATES,
        PSEUDO_CURSOR
    };
    VNC_SetEncodings(vnc, encodings,
            (sizeof (encodings) / sizeof (VNC_RectangleEncodingMethod)));

    VNC_SendInitialFramebufferUpdateRequest(vnc);

    vnc->surface = VNC_CreateSurfaceForServer(&vnc->server_details);
    vnc->thread = VNC_CreateUpdateThread(vnc);

    return 0;
}

int VNC_WaitOnConnection(VNC_Connection *vnc) {
    SDL_WaitThread(vnc->thread, NULL);
    return 0;
}

Uint32 VNC_TranslateKey(SDL_KeyCode key, SDL_bool shift) {
    switch (key) {
        case SDLK_0: return XK_0;
        case SDLK_1: return XK_1;
        case SDLK_2: return XK_2;
        case SDLK_3: return XK_3;
        case SDLK_4: return XK_4;
        case SDLK_5: return XK_5;
        case SDLK_6: return XK_6;
        case SDLK_7: return XK_7;
        case SDLK_8: return XK_8;
        case SDLK_9: return XK_9;

        case SDLK_F1: return XK_F1;
        case SDLK_F2: return XK_F2;
        case SDLK_F3: return XK_F3;
        case SDLK_F4: return XK_F4;
        case SDLK_F5: return XK_F5;
        case SDLK_F6: return XK_F6;
        case SDLK_F7: return XK_F7;
        case SDLK_F8: return XK_F8;
        case SDLK_F9: return XK_F9;
        case SDLK_F10: return XK_F10;
        case SDLK_F11: return XK_F11;
        case SDLK_F12: return XK_F12;
        case SDLK_F13: return XK_F13;
        case SDLK_F14: return XK_F14;
        case SDLK_F15: return XK_F15;
        case SDLK_F16: return XK_F16;
        case SDLK_F17: return XK_F17;
        case SDLK_F18: return XK_F18;
        case SDLK_F19: return XK_F19;
        case SDLK_F20: return XK_F20;
        case SDLK_F21: return XK_F21;
        case SDLK_F22: return XK_F22;
        case SDLK_F23: return XK_F23;
        case SDLK_F24: return XK_F24;

        case SDLK_a: return shift ? XK_A : XK_a;
        case SDLK_b: return shift ? XK_B : XK_b;
        case SDLK_c: return shift ? XK_C : XK_c;
        case SDLK_d: return shift ? XK_D : XK_d;
        case SDLK_e: return shift ? XK_E : XK_e;
        case SDLK_f: return shift ? XK_F : XK_f;
        case SDLK_g: return shift ? XK_G : XK_g;
        case SDLK_h: return shift ? XK_H : XK_h;
        case SDLK_i: return shift ? XK_I : XK_i;
        case SDLK_j: return shift ? XK_J : XK_j;
        case SDLK_k: return shift ? XK_K : XK_k;
        case SDLK_l: return shift ? XK_L : XK_l;
        case SDLK_m: return shift ? XK_M : XK_m;
        case SDLK_n: return shift ? XK_N : XK_n;
        case SDLK_o: return shift ? XK_O : XK_o;
        case SDLK_p: return shift ? XK_P : XK_p;
        case SDLK_q: return shift ? XK_Q : XK_q;
        case SDLK_r: return shift ? XK_R : XK_r;
        case SDLK_s: return shift ? XK_S : XK_s;
        case SDLK_t: return shift ? XK_T : XK_t;
        case SDLK_u: return shift ? XK_U : XK_u;
        case SDLK_v: return shift ? XK_V : XK_v;
        case SDLK_w: return shift ? XK_W : XK_w;
        case SDLK_x: return shift ? XK_X : XK_x;
        case SDLK_y: return shift ? XK_Y : XK_y;
        case SDLK_z: return shift ? XK_Z : XK_z;

        case SDLK_SPACE: return XK_space;
        case SDLK_QUOTE: return XK_apostrophe;
        case SDLK_BACKSLASH: return XK_backslash;
        case SDLK_COMMA: return XK_comma;
        case SDLK_AMPERSAND: return XK_ampersand;
        case SDLK_ASTERISK: return XK_asterisk;
        case SDLK_AT: return XK_at;
        case SDLK_CARET: return XK_asciicircum;
        case SDLK_COLON: return XK_colon;
        case SDLK_DOLLAR: return XK_dollar;
        case SDLK_EXCLAIM: return XK_exclam;
        case SDLK_GREATER: return XK_greater;
        case SDLK_HASH: return XK_numbersign;
        case SDLK_LEFTPAREN: return XK_parenleft;
        case SDLK_LESS: return XK_less;
        case SDLK_PERCENT: return XK_percent;
        case SDLK_PLUS: return XK_plus;
        case SDLK_QUESTION: return XK_question;
        case SDLK_QUOTEDBL: return XK_quotedbl;
        case SDLK_RIGHTPAREN: return XK_parenright;
        case SDLK_UNDERSCORE: return XK_underscore;
        case SDLK_EQUALS: return XK_equal;
        case SDLK_BACKQUOTE: return XK_grave;
        case SDLK_LEFTBRACKET: return XK_bracketleft;
        case SDLK_RIGHTBRACKET: return XK_bracketright;
        case SDLK_PERIOD: return XK_period;
        case SDLK_SEMICOLON: return XK_semicolon;
        case SDLK_SLASH: return XK_slash;

        case SDLK_BACKSPACE: return XK_BackSpace;
        case SDLK_CAPSLOCK: return XK_Caps_Lock;
        case SDLK_NUMLOCKCLEAR: return XK_Num_Lock;
        case SDLK_SCROLLLOCK: return XK_Scroll_Lock;
        case SDLK_DELETE: return XK_Delete;
        case SDLK_INSERT: return XK_Insert;
        case SDLK_ESCAPE: return XK_Escape;
        case SDLK_EXECUTE: return XK_Execute;
        case SDLK_FIND: return XK_Find;
        case SDLK_HELP: return XK_Help;
        case SDLK_AGAIN: return XK_Redo;
        case SDLK_UNDO: return XK_Undo;
        case SDLK_MENU: return XK_Menu;
        case SDLK_RETURN: return XK_Return;
        case SDLK_SEPARATOR: return XK_KP_Separator;
        case SDLK_STOP: return XK_Cancel;
        case SDLK_SYSREQ: return XK_Sys_Req;
        case SDLK_TAB: return XK_Tab;

        case SDLK_LALT: return XK_Alt_L;
        case SDLK_RALT: return XK_Alt_R;
        case SDLK_LCTRL: return XK_Control_L;
        case SDLK_RCTRL: return XK_Control_R;
        case SDLK_LGUI: return XK_Meta_L;
        case SDLK_RGUI: return XK_Meta_R;
        case SDLK_LSHIFT: return XK_Shift_L;
        case SDLK_RSHIFT: return XK_Shift_R;

        case SDLK_KP_0: return XK_KP_0;
        case SDLK_KP_1: return XK_KP_1;
        case SDLK_KP_2: return XK_KP_2;
        case SDLK_KP_3: return XK_KP_3;
        case SDLK_KP_4: return XK_KP_4;
        case SDLK_KP_5: return XK_KP_5;
        case SDLK_KP_6: return XK_KP_6;
        case SDLK_KP_7: return XK_KP_7;
        case SDLK_KP_8: return XK_KP_8;
        case SDLK_KP_9: return XK_KP_9;

        case SDLK_KP_A: return XK_a;
        case SDLK_KP_B: return XK_b;
        case SDLK_KP_C: return XK_c;
        case SDLK_KP_D: return XK_d;
        case SDLK_KP_E: return XK_e;
        case SDLK_KP_F: return XK_f;

        case SDLK_KP_AMPERSAND: return XK_ampersand;
        case SDLK_KP_AT: return XK_at;
        case SDLK_KP_COLON: return XK_colon;
        case SDLK_KP_COMMA: return XK_comma;
        case SDLK_KP_EXCLAM: return XK_exclam;
        case SDLK_KP_GREATER: return XK_greater;
        case SDLK_KP_LESS: return XK_less;
        case SDLK_KP_HASH: return XK_numbersign;
        case SDLK_KP_LEFTBRACE: return XK_braceleft;
        case SDLK_KP_RIGHTBRACE: return XK_braceright;
        case SDLK_KP_LEFTPAREN: return XK_parenleft;
        case SDLK_KP_RIGHTPAREN: return XK_parenright;
        case SDLK_KP_PERCENT: return XK_percent;
        case SDLK_KP_PERIOD: return XK_period;
        case SDLK_KP_PLUSMINUS: return XK_plusminus;
        case SDLK_KP_POWER: return XK_asciicircum;
        case SDLK_KP_VERTICALBAR: return XK_bar;

        case SDLK_KP_MINUS: return XK_KP_Subtract;
        case SDLK_KP_MULTIPLY: return XK_KP_Multiply;
        case SDLK_KP_DECIMAL: return XK_KP_Decimal;
        case SDLK_KP_DIVIDE: return XK_KP_Divide;
        case SDLK_KP_EQUALS: return XK_KP_Equal;
        case SDLK_KP_PLUS: return XK_KP_Add;

        case SDLK_KP_SPACE: return XK_KP_Space;
        case SDLK_KP_TAB: return XK_KP_Tab;
        case SDLK_KP_BACKSPACE: return XK_BackSpace;
        case SDLK_KP_CLEAR: return XK_Clear;
        case SDLK_KP_CLEARENTRY: return XK_Clear;
        case SDLK_KP_ENTER: return XK_KP_Enter;

        case SDLK_CURRENCYUNIT: return XK_currency;
        case SDLK_DECIMALSEPARATOR: return XK_decimalpoint;

        case SDLK_SELECT: return XK_Select;
        case SDLK_CRSEL: return XK_3270_CursorSelect;
        case SDLK_EXSEL: return XK_3270_ExSelect;
        case SDLK_PRINTSCREEN: return XK_3270_PrintScreen;

        case SDLK_UP: return XK_Up;
        case SDLK_DOWN: return XK_Down;

        case SDLK_HOME: return XK_Home;
        case SDLK_END: return XK_End;
        case SDLK_PAGEDOWN: return XK_Page_Down;
        case SDLK_PAGEUP: return XK_Page_Up;
        case SDLK_PRIOR: return XK_Prior;

        case SDLK_COPY: return XK_3270_Copy;
        case SDLK_PAUSE: return XK_Pause;

        case SDLK_CANCEL: return XK_Cancel;
        case SDLK_CLEAR: return XK_Clear;

        case SDLK_ALTERASE: return XK_3270_EraseInput;

        case SDLK_UNKNOWN:

        case SDLK_EJECT:

        case SDLK_OPER:
        case SDLK_OUT:

        case SDLK_CURRENCYSUBUNIT:
        case SDLK_KP_00:
        case SDLK_KP_000:
        case SDLK_KP_BINARY:
        case SDLK_KP_HEXADECIMAL:
        case SDLK_KP_OCTAL:
        case SDLK_KP_DBLAMPERSAND:
        case SDLK_KP_EQUALSAS400:
        case SDLK_KP_XOR:

        case SDLK_THOUSANDSSEPARATOR:

        case SDLK_KP_MEMADD:
        case SDLK_KP_MEMCLEAR:
        case SDLK_KP_MEMDIVIDE:
        case SDLK_KP_MEMMULTIPLY:
        case SDLK_KP_MEMRECALL:
        case SDLK_KP_MEMSTORE:
        case SDLK_KP_MEMSUBTRACT:

        case SDLK_CLEARAGAIN:
        case SDLK_CUT:

        case SDLK_COMPUTER:
        case SDLK_CALCULATOR:
        case SDLK_DISPLAYSWITCH:
        case SDLK_MAIL: 
        case SDLK_MEDIASELECT:
        case SDLK_WWW:

        case SDLK_POWER:
        case SDLK_SLEEP:

        case SDLK_AUDIOMUTE:
        case SDLK_AUDIONEXT:
        case SDLK_AUDIOPLAY:
        case SDLK_AUDIOPREV:
        case SDLK_AUDIOSTOP:

        case SDLK_AC_BACK:
        case SDLK_AC_BOOKMARKS:
        case SDLK_AC_FORWARD:
        case SDLK_AC_SEARCH:
        case SDLK_AC_REFRESH:
        case SDLK_AC_STOP:
        case SDLK_AC_HOME:

        case SDLK_BRIGHTNESSUP:
        case SDLK_BRIGHTNESSDOWN:
        case SDLK_VOLUMEUP:
        case SDLK_VOLUMEDOWN:

        case SDLK_KBDILLUMDOWN:
        case SDLK_KBDILLUMTOGGLE:
        case SDLK_KBDILLUMUP:

        case SDLK_APPLICATION:
        default:
            return XK_VoidSymbol;
    }
}

int VNC_SendPointerEvent(VNC_Connection *vnc, Uint32 buttons,
        Uint16 x, Uint16 y, Sint32 mw_x, Sint32 mw_y) {

    Uint8 button_mask = 0;
    button_mask |= (buttons & SDL_BUTTON_LMASK) ? (1 << 0) : 0; // LMB
    button_mask |= (buttons & SDL_BUTTON_MMASK) ? (1 << 1) : 0; // MMB
    button_mask |= (buttons & SDL_BUTTON_RMASK) ? (1 << 2) : 0; // RMB

    button_mask |= (mw_y > 0) ? (1 << 3) : 0; // MW up
    button_mask |= (mw_y < 0) ? (1 << 4) : 0; // MW down
    button_mask |= (mw_x < 0) ? (1 << 5) : 0; // MW left
    button_mask |= (mw_x > 0) ? (1 << 6) : 0; // MW right

    char buf[6];

    Uint8 *msg = (Uint8 *) buf;
    *msg++ = 5;
    *msg++ = button_mask;

    Uint16 *pos = (Uint16 *) msg;
    *pos++ = SDL_SwapBE16(x);
    *pos++ = SDL_SwapBE16(y);

    return VNC_ToServer(vnc->socket, buf, 6);
}

int VNC_SendKeyEvent(VNC_Connection *vnc, SDL_bool pressed, SDL_Keysym sym) {
    char buf[8];
    SDL_Keycode key = sym.sym;

    SDL_bool shift = sym.mod & KMOD_SHIFT;

    Uint8 *msg = (Uint8 *) buf;
    *msg++ = 4;
    *msg++ = pressed;
    msg += 2;

    Uint32 *key_p = (Uint32 *) msg;
    *key_p = SDL_SwapBE32(VNC_TranslateKey(key, shift));

    return VNC_ToServer(vnc->socket, buf, 8);
}

SDL_Window *VNC_CreateWindowForConnection(VNC_Connection *vnc, char *title,
        int x, int y, Uint32 flags) {

    vnc->window = SDL_CreateWindow(title, x, y, vnc->server_details.w,
            vnc->server_details.h, flags);

    SDL_ShowCursor(SDL_DISABLE);

    return vnc->window;
}

/* vim: se ft=c tw=80 ts=4 sw=4 et : */
