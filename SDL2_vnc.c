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

    PSEUDO_CURSOR = -239,
    PSEUDO_DESKTOP_SIZE = -223,

    // https://vncdotool.readthedocs.io/en/0.8.0/rfbproto.html#encodings
    // says this should be -313 (this seems to be a TightVNC extension originally)
    PSEUDO_CONTINUOUS_UPDATES = -313,

    // QEMU extensions (at least extended keyevent is also implemented
    // by some other servers like wayvnc/neatvnc)
    // PSEUDO_QEMU_POINTER_MOTION_CHANGE = -257, // TODO
    PSEUDO_QEMU_EXTENDED_KEYEVENT = -258,
    // PSEUDO_QEMU_AUDIO = -259 // TODO ?
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

        case PSEUDO_QEMU_EXTENDED_KEYEVENT:
            vnc->qemu_keyevents_supported = SDL_TRUE;
            break;

        default:
            warn("unknown encoding method %i\n", header->e);
            exit(0);
    }
    return 0;
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

int VNC_CutText(VNC_Connection *vnc) {
    // https://datatracker.ietf.org/doc/html/rfc6143#section-7.6.4
    // 1 byte message-type (0x3, already read); 3 bytes padding
    // 4 bytes U32 length; $length U8 chars (in Latin-1) with text
    char buf[1024];
    VNC_FromServer(vnc->socket, buf, 7); // read padding and length
    Uint32 length;
    memcpy(&length, buf+3, 4); // skip padding and get length
    length = SDL_SwapBE32(length);

    char* readBuf = (length < sizeof(buf)) ? buf : malloc(length+1);
    VNC_FromServer(vnc->socket, readBuf, length);
    readBuf[length] = '\0'; // make sure the string is \0 terminated (unsure if VNC guarantees that)

    //printf("TODO: cut_text - received '%s'\n", readBuf); // TODO: what to do with it?

    if(readBuf != buf) {
        free(readBuf);
    }
    return 0;
}

int VNC_UpdateLoop(void *data) {
    VNC_Connection *vnc = data;

    SDL_Event disconnect_event;
    disconnect_event.type = VNC_SHUTDOWN;
    disconnect_event.user.code = 0;

    while (vnc->thread) {
        char buf[1];
        int res = VNC_FromServer(vnc->socket, buf, 1);

        if (res <= 0) {
            disconnect_event.user.code = VNC_ERROR_SERVER_DISCONNECT;
            break;
        }
        VNC_ServerMessageType msg = buf[0];

        switch (msg) {
            case FRAME_BUFFER_UPDATE:
                VNC_FrameBufferUpdate(vnc);
                break;

            case SET_COLOUR_MAP_ENTRIES:
                VNC_SetColorMapEntries(vnc);
                break;

            case BELL:
                // this is the whole message - nothing more to do,
                // except maybe playing a bell sound
                // TODO: printf("TODO: bell\n");
                break;

            case SERVER_CUT_TEXT:
                VNC_CutText(vnc);
                break;

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

    // probably a good idea to clear the whole struct
    memset(vnc, 0, sizeof(*vnc));

    vnc->fps = fps;

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
        PSEUDO_QEMU_EXTENDED_KEYEVENT,
        // PSEUDO_CURSOR
    };
    VNC_SetEncodings(vnc, encodings,
            (sizeof (encodings) / sizeof (VNC_RectangleEncodingMethod)));

    VNC_SendInitialFramebufferUpdateRequest(vnc);

    vnc->surface = VNC_CreateSurfaceForServer(&vnc->server_details);
    vnc->thread = VNC_CreateUpdateThread(vnc);

    return 0;
}

void VNC_WaitOnConnection(VNC_Connection *vnc) {
    SDL_WaitThread(vnc->thread, NULL);
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


Uint32 VNC_TranslateKey(SDL_Keycode key, SDL_bool shift) {
    // printable ASCII and Latin-1 High-ASCII keys are the same
    // in SDL_Keycode and XK_* (both use the corresponding Unicode values)
    if(key < 0xFF) {
        if((key >= ' ' && key <= '~') || (key >= 0xA0)) {
            // take care of shifted letters
            if( shift && (  (key >= SDLK_a && key <= SDLK_z)
                         || (key >= 0xC0 && key <= 0xDE) )  ) {
                key -= 32;
            }
            return key;
        }
    }

    switch (key) {
        case SDLK_UNKNOWN: return XK_VoidSymbol;

        case SDLK_RETURN: return XK_Return;
        case SDLK_ESCAPE: return XK_Escape;
        case SDLK_BACKSPACE: return XK_BackSpace;
        case SDLK_TAB: return XK_Tab;
        // the other keycodes around here are handled in the ASCII case above

        case SDLK_CAPSLOCK: return XK_Caps_Lock;

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

        case SDLK_PRINTSCREEN: return XK_Print;
        case SDLK_SCROLLLOCK: return XK_Scroll_Lock;
        case SDLK_PAUSE: return XK_Pause;
        case SDLK_INSERT: return XK_Insert;
        case SDLK_HOME: return XK_Home;
        case SDLK_PAGEUP: return XK_Page_Up;
        case SDLK_DELETE: return XK_Delete;
        case SDLK_END: return XK_End;
        case SDLK_PAGEDOWN: return XK_Page_Down;
        case SDLK_RIGHT: return XK_Right;
        case SDLK_LEFT: return XK_Left;
        case SDLK_DOWN: return XK_Down;
        case SDLK_UP: return XK_Up;

        case SDLK_NUMLOCKCLEAR: return XK_Num_Lock;
        case SDLK_KP_DIVIDE: return XK_KP_Divide;
        case SDLK_KP_MULTIPLY: return XK_KP_Multiply;
        case SDLK_KP_MINUS: return XK_KP_Subtract;
        case SDLK_KP_PLUS: return XK_KP_Add;
        case SDLK_KP_ENTER: return XK_KP_Enter;
        case SDLK_KP_1: return XK_KP_1;
        case SDLK_KP_2: return XK_KP_2;
        case SDLK_KP_3: return XK_KP_3;
        case SDLK_KP_4: return XK_KP_4;
        case SDLK_KP_5: return XK_KP_5;
        case SDLK_KP_6: return XK_KP_6;
        case SDLK_KP_7: return XK_KP_7;
        case SDLK_KP_8: return XK_KP_8;
        case SDLK_KP_9: return XK_KP_9;
        case SDLK_KP_0: return XK_KP_0;
        case SDLK_KP_COMMA: return XK_KP_Separator;
        case SDLK_KP_PERIOD: return XK_period;

        case SDLK_APPLICATION: return XK_Menu; // compose or windows context menu
        // TODO: SDLK_POWER? (more of a status flag, says SDL)
        case SDLK_KP_EQUALS: return XK_KP_Equal;
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
        case SDLK_EXECUTE: return XK_Execute;
        case SDLK_HELP: return XK_Help;
        case SDLK_MENU: return XK_Menu;
        case SDLK_SELECT: return XK_Select;
        case SDLK_STOP: return XK_Cancel;
        case SDLK_AGAIN: return XK_Redo;
        case SDLK_UNDO: return XK_Undo;
        //TODO: SDLK_CUT - maybe use XF86XK_Cut etc? does vnc support those?
        case SDLK_COPY: return XK_3270_Copy; // TODO: really?
        // TODO: SDLK_PASTE
        case SDLK_FIND: return XK_Find;
        // TODO: SDLK_MUTE, SDLK_VOLUMEUP, SDLK_VOLUMEDOWN - also XF86K_*
        // TODO: SDLK_KP_EQUALSAS400 wtf is a AS/400 keyboard

        case SDLK_ALTERASE: return XK_3270_EraseInput; // TODO: really?
        case SDLK_SYSREQ: return XK_Sys_Req;
        case SDLK_CANCEL: return XK_Cancel;
        case SDLK_CLEAR: return XK_Clear;
        case SDLK_PRIOR: return XK_Prior;
        // TODO: SDLK_RETURN2, SDLK_SEPARATOR ("Keyboard Separator" - probably not XK_KP_Separator)
        // TODO: SDLK_OUT, SDLK_OPER, SDLK_CLEARAGAIN
        case SDLK_CRSEL: return XK_3270_CursorSelect; // TODO: really?
        case SDLK_EXSEL: return XK_3270_ExSelect; // TODO: really?

        // TODO: SDLK_KP_00, SDLK_KP_000, SDLK_THOUSANDSSEPARATOR
        case SDLK_DECIMALSEPARATOR: return XK_decimalpoint;
        case SDLK_CURRENCYUNIT: return XK_currency;
        case SDLK_CURRENCYSUBUNIT: return XK_cent; // TODO: maybe not *exactly* the same

        // there sure are a lot of keypad keys that don't really exist..
        case SDLK_KP_LEFTPAREN: return XK_parenleft;
        case SDLK_KP_RIGHTPAREN: return XK_parenright;
        case SDLK_KP_LEFTBRACE: return XK_braceleft;
        case SDLK_KP_RIGHTBRACE: return XK_braceright;

        case SDLK_KP_TAB: return XK_KP_Tab;
        case SDLK_KP_BACKSPACE: return XK_BackSpace; // TODO: XK_KP_Delete?

        case SDLK_KP_A: return XK_a;
        case SDLK_KP_B: return XK_b;
        case SDLK_KP_C: return XK_c;
        case SDLK_KP_D: return XK_d;
        case SDLK_KP_E: return XK_e;
        case SDLK_KP_F: return XK_f;

        // TODO: SDLK_KP_XOR
        case SDLK_KP_POWER: return XK_asciicircum;
        case SDLK_KP_PERCENT: return XK_percent;
        case SDLK_KP_LESS: return XK_less;
        case SDLK_KP_GREATER: return XK_greater;
        case SDLK_KP_AMPERSAND: return XK_ampersand;
        // TODO: SDLK_KP_DBLAMPERSAND - maybe send two events?
        case SDLK_KP_VERTICALBAR: return XK_bar;
        // TODO: SDLK_KP_DBLVERTICALBAR - maybe send two events?
        case SDLK_KP_COLON: return XK_colon;
        case SDLK_KP_HASH: return XK_numbersign;
        case SDLK_KP_SPACE: return XK_KP_Space;
        case SDLK_KP_AT: return XK_at;
        case SDLK_KP_EXCLAM: return XK_exclam;
        // TODO: SDLK_KP_MEMSTORE, SDLK_KP_MEMRECALL, SDLK_KP_MEMCLEAR,
        //       SDLK_KP_MEMADD, SDLK_KP_MEMSUBTRACT, SDLK_KP_MEMMULTIPLY, SDLK_KP_MEMDIVIDE
        case SDLK_KP_PLUSMINUS: return XK_plusminus;
        case SDLK_KP_CLEAR: return XK_Clear;
        case SDLK_KP_CLEARENTRY: return XK_Clear;
        // TODO: SDLK_KP_BINARY, SDLK_KP_OCTAL, SDLK_KP_HEXADECIMAL
        case SDLK_KP_DECIMAL: return XK_KP_Decimal; // TODO: not sure about this

        case SDLK_LALT: return XK_Alt_L;
        case SDLK_RALT: return XK_Alt_R;
        case SDLK_LCTRL: return XK_Control_L;
        case SDLK_RCTRL: return XK_Control_R;
        case SDLK_LGUI: return XK_Meta_L;
        case SDLK_RGUI: return XK_Meta_R;
        case SDLK_LSHIFT: return XK_Shift_L;
        case SDLK_RSHIFT: return XK_Shift_R;

        case SDLK_MODE: return XK_ISO_Level3_Shift; // apparently this is supposed to be the AltGr key

/*      TODO: look at XF86keysym.h
        case SDLK_AUDIONEXT: return ;
        case SDLK_AUDIOPREV: return ;
        case SDLK_AUDIOSTOP: return ;
        case SDLK_AUDIOPLAY: return ;
        case SDLK_AUDIOMUTE: return ;
        case SDLK_MEDIASELECT return ;
        case SDLK_WWW: return ;
        case SDLK_MAIL: return ;
        case SDLK_CALCULATOR: return ;
        case SDLK_COMPUTER: return ;
        case SDLK_AC_SEARCH: return ;
        case SDLK_AC_HOME: return ;
        case SDLK_AC_BACK: return ;
        case SDLK_AC_FORWARD: return ;
        case SDLK_AC_STOP: return ;
        case SDLK_AC_REFRESH: return ;
        case SDLK_AC_BOOKMARKS: return ;

        case SDLK_BRIGHTNESSDOWN: return ;
        case SDLK_BRIGHTNESSUP: return ;
        case SDLK_DISPLAYSWITCH: return ;
        case SDLK_KBDILLUMTOGGLE: return ;
        case SDLK_KBDILLUMDOWN: return ;
        case SDLK_KBDILLUMUP: return ;
        case SDLK_EJECT: return ;
        case SDLK_SLEEP: return ;
        case SDLK_APP1: return ;
        case SDLK_APP2: return ;

        case SDLK_AUDIOREWIND: return ;
        case SDLK_AUDIOFASTFORWARD: return ;
*/
        default:
            return XK_VoidSymbol;
    }
}


/* Table to map SDL_Scancode to QEMU codes.
 * SDL2 scancodes are based on USB HID usage codes,
 * so I generated this with https://github.com/qemu/keycodemapdb
 * ./keymap-gen code-map ../data/keymaps.csv usb qnum --lang stdc++
 * and adjusted the code to be C-compatible (stdc++ because --lang stdc uses
 * C99-specific notation for the array and I think it'd be useful if this table
 * could compile as C++ as well)
 * Then I manually adjusted it for SDL scancodes, which deviate from the
 * "USB HID Keyboard/Keypad Page (0x07)" standard to also support "multimedia keys"
 * from the USB Consumer Page (0x0C).
 * Because many of those codes are identical to DirectInput DIK_* constants,
 * I was able to reuse some of this:
 * https://github.com/DanielGibson/Snippets/blob/master/sdl2_scancode_to_dinput.h
 */
static const Uint16 map_sdl2_scancode_to_qnum[] = {
  0, /* usb:0 -> SDL_SCANCODE_UNKNOWN -> qnum:None */
  // unused:
  0, /* usb:1 -> linux:None (unnamed) -> qnum:None */
  0, /* usb:2 -> linux:None (unnamed) -> qnum:None */
  0, /* usb:3 -> linux:None (unnamed) -> qnum:None */

  0x1e, /* usb:4 -> linux:30 (KEY_A) -> qnum:30 */
  0x30, /* usb:5 -> linux:48 (KEY_B) -> qnum:48 */
  0x2e, /* usb:6 -> linux:46 (KEY_C) -> qnum:46 */
  0x20, /* usb:7 -> linux:32 (KEY_D) -> qnum:32 */
  0x12, /* usb:8 -> linux:18 (KEY_E) -> qnum:18 */
  0x21, /* usb:9 -> linux:33 (KEY_F) -> qnum:33 */
  0x22, /* usb:10 -> linux:34 (KEY_G) -> qnum:34 */
  0x23, /* usb:11 -> linux:35 (KEY_H) -> qnum:35 */
  0x17, /* usb:12 -> linux:23 (KEY_I) -> qnum:23 */
  0x24, /* usb:13 -> linux:36 (KEY_J) -> qnum:36 */
  0x25, /* usb:14 -> linux:37 (KEY_K) -> qnum:37 */
  0x26, /* usb:15 -> linux:38 (KEY_L) -> qnum:38 */
  0x32, /* usb:16 -> linux:50 (KEY_M) -> qnum:50 */
  0x31, /* usb:17 -> linux:49 (KEY_N) -> qnum:49 */
  0x18, /* usb:18 -> linux:24 (KEY_O) -> qnum:24 */
  0x19, /* usb:19 -> linux:25 (KEY_P) -> qnum:25 */
  0x10, /* usb:20 -> linux:16 (KEY_Q) -> qnum:16 */
  0x13, /* usb:21 -> linux:19 (KEY_R) -> qnum:19 */
  0x1f, /* usb:22 -> linux:31 (KEY_S) -> qnum:31 */
  0x14, /* usb:23 -> linux:20 (KEY_T) -> qnum:20 */
  0x16, /* usb:24 -> linux:22 (KEY_U) -> qnum:22 */
  0x2f, /* usb:25 -> linux:47 (KEY_V) -> qnum:47 */
  0x11, /* usb:26 -> linux:17 (KEY_W) -> qnum:17 */
  0x2d, /* usb:27 -> linux:45 (KEY_X) -> qnum:45 */
  0x15, /* usb:28 -> linux:21 (KEY_Y) -> qnum:21 */
  0x2c, /* usb:29 -> linux:44 (KEY_Z) -> qnum:44 */

  0x2, /* usb:30 -> linux:2 (KEY_1) -> qnum:2 */
  0x3, /* usb:31 -> linux:3 (KEY_2) -> qnum:3 */
  0x4, /* usb:32 -> linux:4 (KEY_3) -> qnum:4 */
  0x5, /* usb:33 -> linux:5 (KEY_4) -> qnum:5 */
  0x6, /* usb:34 -> linux:6 (KEY_5) -> qnum:6 */
  0x7, /* usb:35 -> linux:7 (KEY_6) -> qnum:7 */
  0x8, /* usb:36 -> linux:8 (KEY_7) -> qnum:8 */
  0x9, /* usb:37 -> linux:9 (KEY_8) -> qnum:9 */
  0xa, /* usb:38 -> linux:10 (KEY_9) -> qnum:10 */
  0xb, /* usb:39 -> linux:11 (KEY_0) -> qnum:11 */

  0x1c, /* usb:40 -> linux:28 (KEY_ENTER) -> qnum:28 */
  0x1, /* usb:41 -> linux:1 (KEY_ESC) -> qnum:1 */
  0xe, /* usb:42 -> linux:14 (KEY_BACKSPACE) -> qnum:14 */
  0xf, /* usb:43 -> linux:15 (KEY_TAB) -> qnum:15 */
  0x39, /* usb:44 -> linux:57 (KEY_SPACE) -> qnum:57 */

  0xc, /* usb:45 -> linux:12 (KEY_MINUS) -> qnum:12 */
  0xd, /* usb:46 -> linux:13 (KEY_EQUAL) -> qnum:13 */
  0x1a, /* usb:47 -> linux:26 (KEY_LEFTBRACE) -> qnum:26 */
  0x1b, /* usb:48 -> linux:27 (KEY_RIGHTBRACE) -> qnum:27 */

  0x2b, /* usb:49 -> linux:43 (KEY_BACKSLASH) -> qnum:43 */
  0x2b, /* usb:50 -> linux:43 (KEY_BACKSLASH) -> qnum:43 */

  0x27, /* usb:51 -> linux:39 (KEY_SEMICOLON) -> qnum:39 */
  0x28, /* usb:52 -> linux:40 (KEY_APOSTROPHE) -> qnum:40 */
  0x29, /* usb:53 -> linux:41 (KEY_GRAVE) -> qnum:41 */
  0x33, /* usb:54 -> linux:51 (KEY_COMMA) -> qnum:51 */
  0x34, /* usb:55 -> linux:52 (KEY_DOT) -> qnum:52 */
  0x35, /* usb:56 -> linux:53 (KEY_SLASH) -> qnum:53 */

  0x3a, /* usb:57 -> linux:58 (KEY_CAPSLOCK) -> qnum:58 */

  0x3b, /* usb:58 -> linux:59 (KEY_F1) -> qnum:59 */
  0x3c, /* usb:59 -> linux:60 (KEY_F2) -> qnum:60 */
  0x3d, /* usb:60 -> linux:61 (KEY_F3) -> qnum:61 */
  0x3e, /* usb:61 -> linux:62 (KEY_F4) -> qnum:62 */
  0x3f, /* usb:62 -> linux:63 (KEY_F5) -> qnum:63 */
  0x40, /* usb:63 -> linux:64 (KEY_F6) -> qnum:64 */
  0x41, /* usb:64 -> linux:65 (KEY_F7) -> qnum:65 */
  0x42, /* usb:65 -> linux:66 (KEY_F8) -> qnum:66 */
  0x43, /* usb:66 -> linux:67 (KEY_F9) -> qnum:67 */
  0x44, /* usb:67 -> linux:68 (KEY_F10) -> qnum:68 */
  0x57, /* usb:68 -> linux:87 (KEY_F11) -> qnum:87 */
  0x58, /* usb:69 -> linux:88 (KEY_F12) -> qnum:88 */

  0x54, /* usb:70 -> SDL_SCANCODE_PRINTSCREEN -> qnum:84 - same as SYSREQ! */
  0x46, /* usb:71 -> linux:70 (KEY_SCROLLLOCK) -> qnum:70 */
  0xc6, /* usb:72 -> linux:119 (KEY_PAUSE) -> qnum:198 */
  0xd2, /* usb:73 -> linux:110 (KEY_INSERT) -> qnum:210 */

  0xc7, /* usb:74 -> linux:102 (KEY_HOME) -> qnum:199 */
  0xc9, /* usb:75 -> linux:104 (KEY_PAGEUP) -> qnum:201 */
  0xd3, /* usb:76 -> linux:111 (KEY_DELETE) -> qnum:211 */
  0xcf, /* usb:77 -> linux:107 (KEY_END) -> qnum:207 */
  0xd1, /* usb:78 -> linux:109 (KEY_PAGEDOWN) -> qnum:209 */
  0xcd, /* usb:79 -> linux:106 (KEY_RIGHT) -> qnum:205 */
  0xcb, /* usb:80 -> linux:105 (KEY_LEFT) -> qnum:203 */
  0xd0, /* usb:81 -> linux:108 (KEY_DOWN) -> qnum:208 */
  0xc8, /* usb:82 -> linux:103 (KEY_UP) -> qnum:200 */

  0x45, /* usb:83 -> linux:69 (KEY_NUMLOCK) -> qnum:69 */

  0xb5, /* usb:84 -> linux:98 (KEY_KPSLASH) -> qnum:181 */
  0x37, /* usb:85 -> linux:55 (KEY_KPASTERISK) -> qnum:55 */
  0x4a, /* usb:86 -> linux:74 (KEY_KPMINUS) -> qnum:74 */
  0x4e, /* usb:87 -> linux:78 (KEY_KPPLUS) -> qnum:78 */
  0x9c, /* usb:88 -> linux:96 (KEY_KPENTER) -> qnum:156 */
  0x4f, /* usb:89 -> linux:79 (KEY_KP1) -> qnum:79 */
  0x50, /* usb:90 -> linux:80 (KEY_KP2) -> qnum:80 */
  0x51, /* usb:91 -> linux:81 (KEY_KP3) -> qnum:81 */
  0x4b, /* usb:92 -> linux:75 (KEY_KP4) -> qnum:75 */
  0x4c, /* usb:93 -> linux:76 (KEY_KP5) -> qnum:76 */
  0x4d, /* usb:94 -> linux:77 (KEY_KP6) -> qnum:77 */
  0x47, /* usb:95 -> linux:71 (KEY_KP7) -> qnum:71 */
  0x48, /* usb:96 -> linux:72 (KEY_KP8) -> qnum:72 */
  0x49, /* usb:97 -> linux:73 (KEY_KP9) -> qnum:73 */
  0x52, /* usb:98 -> linux:82 (KEY_KP0) -> qnum:82 */
  0x53, /* usb:99 -> linux:83 (KEY_KPDOT) -> qnum:83 */

  0x56, /* usb:100 -> linux:86 (KEY_102ND) -> qnum:86 */
  0xdd, /* usb:101 -> linux:127 (KEY_COMPOSE) -> qnum:221 */
  0xde, /* usb:102 -> linux:116 (KEY_POWER) -> qnum:222 */
  0x59, /* usb:103 -> linux:117 (KEY_KPEQUAL) -> qnum:89 */
  0x5d, /* usb:104 -> linux:183 (KEY_F13) -> qnum:93 */
  0x5e, /* usb:105 -> linux:184 (KEY_F14) -> qnum:94 */
  0x5f, /* usb:106 -> linux:185 (KEY_F15) -> qnum:95 */
  0x55, /* usb:107 -> linux:186 (KEY_F16) -> qnum:85 */
  0x83, /* usb:108 -> linux:187 (KEY_F17) -> qnum:131 */
  0xf7, /* usb:109 -> linux:188 (KEY_F18) -> qnum:247 */
  0x84, /* usb:110 -> linux:189 (KEY_F19) -> qnum:132 */
  0x5a, /* usb:111 -> linux:190 (KEY_F20) -> qnum:90 */
  0x74, /* usb:112 -> linux:191 (KEY_F21) -> qnum:116 */
  0xf9, /* usb:113 -> linux:192 (KEY_F22) -> qnum:249 */
  0x6d, /* usb:114 -> linux:193 (KEY_F23) -> qnum:109 */
  0x6f, /* usb:115 -> linux:194 (KEY_F24) -> qnum:111 */
  0x64, /* usb:116 -> linux:134 (KEY_OPEN) -> qnum:100 */
  0xf5, /* usb:117 -> linux:138 (KEY_HELP) -> qnum:245 */
  0x9e, /* usb:118 -> linux:139 (KEY_MENU) -> qnum:158 */
  0x8c, /* usb:119 -> linux:132 (KEY_FRONT) -> qnum:140 */
  0xe8, /* usb:120 -> linux:128 (KEY_STOP) -> qnum:232 */
  0x85, /* usb:121 -> linux:129 (KEY_AGAIN) -> qnum:133 */
  0x87, /* usb:122 -> linux:131 (KEY_UNDO) -> qnum:135 */
  0xbc, /* usb:123 -> linux:137 (KEY_CUT) -> qnum:188 */
  0xf8, /* usb:124 -> linux:133 (KEY_COPY) -> qnum:248 */
  0x65, /* usb:125 -> linux:135 (KEY_PASTE) -> qnum:101 */
  0xc1, /* usb:126 -> linux:136 (KEY_FIND) -> qnum:193 */
  0xa0, /* usb:127 -> linux:113 (KEY_MUTE) -> qnum:160 */
  0xb0, /* usb:128 -> linux:115 (KEY_VOLUMEUP) -> qnum:176 */
  0xae, /* usb:129 -> linux:114 (KEY_VOLUMEDOWN) -> qnum:174 */

  // locking capslock, numlock und scrolllock, SDL2 says
  // "not sure whether there's a reason to enable these" and doesn't define them
  0, /* usb:130 -> linux:None (unnamed) -> qnum:None */
  0, /* usb:131 -> linux:None (unnamed) -> qnum:None */
  0, /* usb:132 -> linux:None (unnamed) -> qnum:None */

  0x7e, /* usb:133 -> linux:121 (KEY_KPCOMMA) -> qnum:126 */
  0, /* usb:134 -> SDL_SCANCODE_KP_EQUALSAS400 -> qnum:None */

  // SDL_SCANCODE_INTERNATIONAL*:
  0x73, /* usb:135 -> linux:89 (KEY_RO) -> qnum:115 */
  0x70, /* usb:136 -> linux:93 (KEY_KATAKANAHIRAGANA) -> qnum:112 */
  0x7d, /* usb:137 -> linux:124 (KEY_YEN) -> qnum:125 */
  0x79, /* usb:138 -> linux:92 (KEY_HENKAN) -> qnum:121 */
  0x7b, /* usb:139 -> linux:94 (KEY_MUHENKAN) -> qnum:123 */
  0x5c, /* usb:140 -> linux:95 (KEY_KPJPCOMMA) -> qnum:92 */
  0, /* usb:141 -> SDL_SCANCODE_INTERNATIONAL7 -> qnum:None */
  0, /* usb:142 -> SDL_SCANCODE_INTERNATIONAL8 -> qnum:None */
  0, /* usb:143 -> SDL_SCANCODE_INTERNATIONAL9 -> qnum:None */

  // SDL_SCANCODE_LANG*:
  0x72, /* usb:144 -> linux:122 (KEY_HANGEUL) -> qnum:114 */
  0x71, /* usb:145 -> linux:123 (KEY_HANJA) -> qnum:113 */
  0x78, /* usb:146 -> linux:90 (KEY_KATAKANA) -> qnum:120 */
  0x77, /* usb:147 -> linux:91 (KEY_HIRAGANA) -> qnum:119 */
  0x76, /* usb:148 -> linux:85 (KEY_ZENKAKUHANKAKU) -> qnum:118 */
  0, /* usb:149 -> SDL_SCANCODE_LANG6 (reserved) -> qnum:None */
  0, /* usb:150 -> SDL_SCANCODE_LANG7 (reserved) -> qnum:None */
  0, /* usb:151 -> SDL_SCANCODE_LANG8 (reserved) -> qnum:None */
  0, /* usb:152 -> SDL_SCANCODE_LANG9 (reserved) -> qnum:None */

  // SDL_SCANCODE_ALTERASE etc:
  0x94, // usb:153 -> SDL_SCANCODE_ALTERASE - KEY_ALTERASE
  0x54, // usb:154 -> SDL_SCANCODE_SYSREQ -> same as print screen!
  0xCA, // usb:155 -> SDL_SCANCODE_CANCEL - KEY_CANCEL
  0, // usb:156 -> SDL_SCANCODE_CLEAR
  0, // usb:157 -> SDL_SCANCODE_PRIOR
  0, // usb:158 -> SDL_SCANCODE_RETURN2
  0, // usb:159 -> SDL_SCANCODE_SEPARATOR
  0, // usb:160 -> SDL_SCANCODE_OUT
  0, // usb:161 -> SDL_SCANCODE_OPER
  0, // usb:162 -> SDL_SCANCODE_CLEARAGAIN
  0, // usb:163 -> SDL_SCANCODE_CRSEL
  0, // usb:164 -> SDL_SCANCODE_EXSEL

  // 165-175 is unused in SDL
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

  // lots of SDL_SCANCODE_KP stuff:
  0, // usb:176 -> SDL_SCANCODE_KP_00
  0, // usb:177 -> SDL_SCANCODE_KP_000
  0, // usb:178 -> SDL_SCANCODE_THOUSANDSSEPARATOR
  0, // usb:179 -> SDL_SCANCODE_DECIMALSEPARATOR
  0, // usb:180 -> SDL_SCANCODE_CURRENCYUNIT
  0, // usb:181 -> SDL_SCANCODE_CURRENCYSUBUNIT
  0xF6, // usb:182 -> SDL_SCANCODE_KP_LEFTPAREN  -> qnum:246 - KEY_KPLEFTPAREN
  0xFB, // usb:183 -> SDL_SCANCODE_KP_RIGHTPAREN -> qnum:251 - KEY_KPRIGHTPAREN
  0, // usb:184 -> SDL_SCANCODE_KP_LEFTBRACE
  0, // usb:185 -> SDL_SCANCODE_KP_RIGHTBRACE
  0, // usb:186 -> SDL_SCANCODE_KP_TAB
  0, // usb:187 -> SDL_SCANCODE_KP_BACKSPACE
  0, // usb:188 -> SDL_SCANCODE_KP_A
  0, // usb:189 -> SDL_SCANCODE_KP_B
  0, // usb:190 -> SDL_SCANCODE_KP_C
  0, // usb:191 -> SDL_SCANCODE_KP_D
  0, // usb:192 -> SDL_SCANCODE_KP_E
  0, // usb:193 -> SDL_SCANCODE_KP_F
  0, // usb:194 -> SDL_SCANCODE_KP_XOR
  0, // usb:195 -> SDL_SCANCODE_KP_POWER
  0, // usb:196 -> SDL_SCANCODE_KP_PERCENT
  0, // usb:197 -> SDL_SCANCODE_KP_LESS
  0, // usb:198 -> SDL_SCANCODE_KP_GREATER
  0, // usb:199 -> SDL_SCANCODE_KP_AMPERSAND
  0, // usb:200 -> SDL_SCANCODE_KP_DBLAMPERSAND
  0, // usb:201 -> SDL_SCANCODE_KP_VERTICALBAR
  0, // usb:202 -> SDL_SCANCODE_KP_DBLVERTICALBAR
  0, // usb:203 -> SDL_SCANCODE_KP_COLON
  0, // usb:204 -> SDL_SCANCODE_KP_HASH
  0, // usb:205 -> SDL_SCANCODE_KP_SPACE
  0, // usb:206 -> SDL_SCANCODE_KP_AT
  0, // usb:207 -> SDL_SCANCODE_KP_EXCLAM
  0, // usb:208 -> SDL_SCANCODE_KP_MEMSTORE
  0, // usb:209 -> SDL_SCANCODE_KP_MEMRECALL
  0, // usb:210 -> SDL_SCANCODE_KP_MEMCLEAR
  0, // usb:211 -> SDL_SCANCODE_KP_MEMADD
  0, // usb:212 -> SDL_SCANCODE_KP_MEMSUBTRACT
  0, // usb:213 -> SDL_SCANCODE_KP_MEMMULTIPLY
  0, // usb:214 -> SDL_SCANCODE_KP_MEMDIVIDE
  0xCE, // usb:215 -> SDL_SCANCODE_KP_PLUSMINUS - KEY_KPPLUSMINUS
  0, // usb:216 -> SDL_SCANCODE_KP_CLEAR
  0, // usb:217 -> SDL_SCANCODE_KP_CLEARENTRY
  0, // usb:218 -> SDL_SCANCODE_KP_BINARY
  0, // usb:219 -> SDL_SCANCODE_KP_OCTAL
  0, // usb:220 -> SDL_SCANCODE_KP_DECIMAL
  0, // usb:221 -> SDL_SCANCODE_KP_HEXADECIMAL

  // unused:
  0, /* usb:222 -> linux:None (unnamed) -> qnum:None */
  0, /* usb:223 -> linux:None (unnamed) -> qnum:None */

  0x1d, /* usb:224 -> linux:29 (KEY_LEFTCTRL) -> qnum:29 */
  0x2a, /* usb:225 -> linux:42 (KEY_LEFTSHIFT) -> qnum:42 */
  0x38, /* usb:226 -> linux:56 (KEY_LEFTALT) -> qnum:56 */
  0xdb, /* usb:227 -> linux:125 (KEY_LEFTMETA) -> qnum:219 */
  0x9d, /* usb:228 -> linux:97 (KEY_RIGHTCTRL) -> qnum:157 */
  0x36, /* usb:229 -> linux:54 (KEY_RIGHTSHIFT) -> qnum:54 */
  0xb8, /* usb:230 -> linux:100 (KEY_RIGHTALT) -> qnum:184 */
  0xdc, /* usb:231 -> linux:126 (KEY_RIGHTMETA) -> qnum:220 */

  // from here on, SDL_Scancode deviates from the USB standard
  // (because the affected keys are usually implemented via HID consumer page)
  // 232 - 256 are unused
  0, 0, 0, 0, 0, 0, 0, 0, 0,    // 232 - 240 unused
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 241-250 unused
  0, 0, 0, 0, 0, 0,             // 251-256 unused

  0xb8, //  SDL_SCANCODE_MODE = 257,  // this seems to be the AltGr Key, so also map to KEY_RIGHTALT

          // These values are mapped from usage page 0x0C (USB consumer page).
  // used the output of keymap-gen code-map --lang=stdc keymaps.csv qnum qcode to match names
  // furthermore, here the qcodes seem to match the Win32 DIK_ constants for the same keys
  0x99,   //  SDL_SCANCODE_AUDIONEXT = 258, - KEY_NEXTSONG - Q_KEY_CODE_AUDIONEXT
  0x90,   //  SDL_SCANCODE_AUDIOPREV = 259, - KEY_PREVIOUSSONG - Q_KEY_CODE_AUDIOPREV
  0xA4,   //  SDL_SCANCODE_AUDIOSTOP = 260, - KEY_STOPCD - Q_KEY_CODE_AUDIOSTOP
  0xA2,   //  SDL_SCANCODE_AUDIOPLAY = 261, - KEY_PLAYPAUSE - Q_KEY_CODE_AUDIOPLAY
  0xA0,   //  SDL_SCANCODE_AUDIOMUTE = 262, - KEY_MUTE - Q_KEY_CODE_AUDIOMUTE
  0xED,   //  SDL_SCANCODE_MEDIASELECT = 263, - Q_KEY_CODE_MEDIASELECT

  0x82,   //  SDL_SCANCODE_WWW = 264, - KEY_WWW
  0xEC,   //  SDL_SCANCODE_MAIL = 265, - KEY_MAIL - Q_KEY_CODE_MAIL
  0xA1,   //  SDL_SCANCODE_CALCULATOR = 266, - KEY_CALC - Q_KEY_CODE_CALCULATOR
  0xEB,   //  SDL_SCANCODE_COMPUTER = 267, - KEY_COMPUTER - Q_KEY_CODE_COMPUTER
  0xE5,   //  SDL_SCANCODE_AC_SEARCH = 268, - KEY_SEARCH
  0xB2,   //  SDL_SCANCODE_AC_HOME = 269, - KEY_HOMEPAGE - Q_KEY_CODE_AC_HOME
  0xEA,   //  SDL_SCANCODE_AC_BACK = 270, - KEY_BACK - Q_KEY_CODE_AC_BACK
  0xE9,   //  SDL_SCANCODE_AC_FORWARD = 271, - KEY_FORWARD - Q_KEY_CODE_AC_FORWARD
  0xE8,   //  SDL_SCANCODE_AC_STOP = 272, - KEY_STOP - Q_KEY_CODE_STOP
  0xE7,   //  SDL_SCANCODE_AC_REFRESH = 273, - KEY_REFRESH - Q_KEY_CODE_AC_REFRESH
  0xE6,   //  SDL_SCANCODE_AC_BOOKMARKS = 274, - KEY_BOOKMARKS - Q_KEY_CODE_AC_BOOKMARKS

          // These are values that Christian Walther added (for mac keyboard?).
  0xCC,   //  SDL_SCANCODE_BRIGHTNESSDOWN = 275, - KEY_BRIGHTNESSDOWN
  0xD4,   //  SDL_SCANCODE_BRIGHTNESSUP = 276, - KEY_BRIGHTNESSUP
  0xD6,   //  SDL_SCANCODE_DISPLAYSWITCH = 277, - KEY_SWITCHVIDEOMODE - display mirroring/dual display switch, video mode switch
  0xD7,   //  SDL_SCANCODE_KBDILLUMTOGGLE = 278, - KEY_KBDILLUMTOGGLE
  0xD8,   //  SDL_SCANCODE_KBDILLUMDOWN = 279, - KEY_KBDILLUMDOWN
  0xD9,   //  SDL_SCANCODE_KBDILLUMUP = 280, - KEY_KBDILLUMUP

  0x6c,   //  SDL_SCANCODE_EJECT = 281, - KEY_EJECTCD

  0xDF,   //  SDL_SCANCODE_SLEEP = 282, - KEY_SLEEP - Q_KEY_CODE_SLEEP

  0x9f,   //  SDL_SCANCODE_APP1 = 283, - KEY_PROG1
  0x97,   //  SDL_SCANCODE_APP2 = 284, - KEY_PROG2
          // end of Walther-keys

          // (additional media keys from consumer page)
  0x98,   // SDL_SCANCODE_AUDIOREWIND = 285, - KEY_REWIND
  0xB4,   // SDL_SCANCODE_AUDIOFASTFORWARD = 286, - KEY_FASTFORWARD

      // the rest up to 511 are currently not named in SDL

};

Uint32 VNC_ToQemuKeynum(SDL_Keysym sym) {

    /* https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#74121qemu-extended-key-event-message
     * says:
     * > The keycode is the XT keycode that produced the keysym. An XT keycode
     * > is an XT make scancode sequence encoded to fit in a single U32 quantity.
     * > Single byte XT scancodes with a byte value less than 0x7f are encoded as is.
     * > 2-byte XT scancodes whose first byte is 0xe0 and second byte is less
     * > than 0x7f are encoded with the high bit of the first byte set.
     *
     * DG: However, I'm not sure that's 100% correct, and it's not enough to
     *  map all SDL scancodes.
     *  Luckily, SDL2 scancodes are based on USB Keyboard Usage IDs and qemu has
     *  a tool to generate mappings (and show supported keys), so with that
     *  (and some manual labor) I got map_sdl2_scancode_to_qnum[]
     */
    Uint32 sc = sym.scancode;
    if(sc < sizeof(map_sdl2_scancode_to_qnum)/sizeof(map_sdl2_scancode_to_qnum[0]))
        return map_sdl2_scancode_to_qnum[sym.scancode];

    return 0; // invalid or unsupported scancode
}

int VNC_SendKeyEvent(VNC_Connection *vnc, SDL_bool pressed, SDL_Keysym sym) {

    SDL_Keycode key = sym.sym;

    SDL_bool shift = (sym.mod & KMOD_SHIFT) != 0;
    Uint32 keysym = VNC_TranslateKey(key, shift);
    Uint32 qemuKeycode = 0;

    if(vnc->qemu_keyevents_supported) {
        qemuKeycode = VNC_ToQemuKeynum(sym);
    }

    char buf[12] = {0};
    // if qemu key events are not supported, or we couldn't map the SDL scancode,
    // send a normal key event message
    if(qemuKeycode == 0) {

        if(keysym == XK_VoidSymbol) {
            // couldn't map the key => nothing to send
            return 0;
        }

        Uint8 *msg = (Uint8 *) buf;
        *msg++ = 4;
        *msg++ = pressed;
        msg += 2; // skip padding

        *(Uint32 *) msg = SDL_SwapBE32(keysym);

        return VNC_ToServer(vnc->socket, buf, 8);
    }

    /* QEMU Extended Key Event Message:
     * U8  message-type (255)
     * U8  submessage-type (0)
     * U16 down-flag
     * U32 keysym // like in regular KeyEvent message
     * U32 keycode // XT/qemu keycode
     */
    Uint8 *msg = (Uint8 *) buf;
    *msg++ = 255; // message-type (QEMU Client Message)
    *msg++ = 0;   // submessage-type (extended key event)
    *msg++ = 0;   // first byte of down-flag
    *msg++ = pressed; // second byte of down-flag
    *(Uint32 *) msg = SDL_SwapBE32(keysym); // "classic" VNC/X11 keysym
    msg += 4;
    *(Uint32 *) msg = SDL_SwapBE32(qemuKeycode); // XT/qemu keycode

    return VNC_ToServer(vnc->socket, buf, 12);
}

SDL_Window *VNC_CreateWindowForConnection(VNC_Connection *vnc, char *title,
        int x, int y, Uint32 flags) {

    vnc->window = SDL_CreateWindow(title, x, y, vnc->server_details.w,
            vnc->server_details.h, flags);

    SDL_ShowCursor(SDL_DISABLE);

    return vnc->window;
}

/* vim: se ft=c tw=80 ts=4 sw=4 et : */
