#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
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
} RFB_version;

#define RFB_33_STR "RFB 003.003\n"
#define RFB_37_STR "RFB 003.007\n"
#define RFB_38_STR "RFB 003.008\n"

#define debug(...) fprintf(stderr, __VA_ARGS__)
#define warn(...) fprintf(stderr, __VA_ARGS__)

uint16_t swap_endianness_u16b(uint16_t x) {
    uint16_t a = x;
    uint16_t b = 0;

    uint8_t *pa = (uint8_t *) &a;
    uint8_t *pb = (uint8_t *) &b;

    pb[0] = pa[1];
    pb[1] = pa[0];

    return b;
}

uint32_t swap_endianness_u32b(uint32_t x) {
    uint32_t a = x;
    uint32_t b = 0;

    uint8_t *pa = (uint8_t *) &a;
    uint8_t *pb = (uint8_t *) &b;

    pb[0] = pa[3];
    pb[1] = pa[2];
    pb[2] = pa[1];
    pb[3] = pa[0];

    return b;
}

int32_t swap_endianness_32b(int32_t x) {
    int32_t a = x;
    int32_t b = 0;

    int8_t *pa = (int8_t *) &a;
    int8_t *pb = (int8_t *) &b;

    pb[0] = pa[3];
    pb[1] = pa[2];
    pb[2] = pa[1];
    pb[3] = pa[0];

    return b;
}

int init_vnc_buffer(vnc_buf *buffer) {
    buffer->size = VNC_INITIAL_BUFSIZE;
    buffer->data = malloc(buffer->size);

    return buffer->data == NULL;
}

int create_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct timeval t;
    t.tv_sec = 0;
    t.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));

    return sock;
}

int vnc_connect(SDL_vnc *vnc, char *host, uint port) {
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, host, &address.sin_addr);
    return connect(vnc->socket, (struct sockaddr *) &address, sizeof(address));
}

int resize_vnc_buffer(vnc_buf buffer, size_t n) {
    buffer.data = realloc(buffer.data, n);

    return !buffer.data;
}

int assure_buffer_size(vnc_buf buffer, size_t n) {
    if (n > buffer.size) {
        return resize_vnc_buffer(buffer, n);
    }

    return 0;
}

int from_server(int socket, void *buffer, size_t n, bool spin) {

    size_t left_to_read = n;
    void *needle = buffer;

    while (left_to_read > 0) {
        ssize_t bytes_read = recv(socket, needle, left_to_read, 0);

        if (bytes_read < 0) {
            return -1;
        }

        if (!spin && bytes_read == 0) {
            return n - left_to_read;
        }

        left_to_read -= bytes_read;
        needle += bytes_read;
    }

    return n;
}

int server_to_vnc_buffer(SDL_vnc *vnc, size_t n, bool spin) {
    assure_buffer_size(vnc->buffer, n);
    return from_server(vnc->socket, vnc->buffer.data, n, spin);
}

int assure_scratch_buffer_size(SDL_vnc *vnc, size_t w, size_t h) {
    vnc_server_details details = vnc->server_details;

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

int server_to_scratch_buffer(SDL_vnc *vnc, size_t w, size_t h, bool spin) {
    assure_scratch_buffer_size(vnc, w, h);

    size_t pixel_data_size = w * h * vnc->server_details.fmt.bpp / 8;

    if (SDL_MUSTLOCK(vnc->scratch_buffer)) {
        SDL_LockSurface(vnc->scratch_buffer);
    }

    from_server(vnc->socket, vnc->scratch_buffer->pixels, pixel_data_size,
            spin);

    if (SDL_MUSTLOCK(vnc->scratch_buffer)) {
        SDL_UnlockSurface(vnc->scratch_buffer);
    }

    return 0;
}

int to_server(int socket, void *data, size_t n) {
    return send(socket, data, n, 0);
}

RFB_version deduce_version(char *str) {
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

char *rfb_ver_to_str(RFB_version ver) {
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

RFB_version get_server_version(SDL_vnc *vnc) {
    char protocol_string[12];
    from_server(vnc->socket, protocol_string, 12, true);

    RFB_version ver = deduce_version(protocol_string);

    return ver;
}

int send_client_version(SDL_vnc *vnc, RFB_version ver) {
    char *ver_str = rfb_ver_to_str(ver);
    return to_server(vnc->socket, ver_str, 12);
}

typedef enum {
    RFB_SECURITY_INVALID = 0,
    RFB_SECURITY_NONE = 1,
    RFB_SECURITY_VNC_AUTH = 2
} RFB_security_protocol;

bool look_for_no_auth(uint8_t *options, size_t n) {
    for (uint i = 0; i < n; i++) {
        if (options[i] == RFB_SECURITY_NONE) {
            return true;
        }
    }

    return false;
}

int negotiate_security_33(SDL_vnc *vnc) {
    return SDL_VNC_ERROR_UNIMPLEMENTED;
}

int negotiate_security_37(SDL_vnc *vnc) {
    return SDL_VNC_ERROR_UNIMPLEMENTED;
}

int negotiate_security_38(SDL_vnc *vnc) {
    uint8_t security_protocol_count;
    from_server(vnc->socket, &security_protocol_count, 1, true);

    if (!security_protocol_count) {
        return SDL_VNC_ERROR_SERVER_DISCONNECT;
    }

    server_to_vnc_buffer(vnc, security_protocol_count, true);

    bool no_security_supported =
        look_for_no_auth(vnc->buffer.data, security_protocol_count);

    if (!no_security_supported) {
        return SDL_VNC_ERROR_UNSUPPORTED_SECURITY_PROTOCOLS;
    }

    RFB_security_protocol no_sec = RFB_SECURITY_NONE;
    to_server(vnc->socket, &no_sec, 1);

    uint32_t security_handshake_error;
    from_server(vnc->socket, &security_handshake_error, 4, true);

    if (security_handshake_error) {
        return SDL_VNC_ERROR_SECURITY_HANDSHAKE_FAILED;
    }

    return OK;
}

int negotiate_security(SDL_vnc *vnc, RFB_version ver) {
    switch (ver) {
        case RFB_33:
            return negotiate_security_33(vnc);

        case RFB_37:
            return negotiate_security_37(vnc);

        case RFB_38:
            return negotiate_security_38(vnc);

        default:
            return negotiate_security_33(vnc);
    }
}

int client_initialisation(SDL_vnc *vnc) {
    uint8_t shared_flag = 0;
    to_server(vnc->socket, &shared_flag, 1);
    return 0;
}

int server_initialisation(SDL_vnc *vnc) {
    server_to_vnc_buffer(vnc, 24, true);

    uint16_t *framebuffer_info = (uint16_t *) vnc->buffer.data;
    vnc->server_details.w = swap_endianness_u16b(framebuffer_info[0]);
    vnc->server_details.h = swap_endianness_u16b(framebuffer_info[1]);

    uint8_t *server_pixel_info = (uint8_t *) &framebuffer_info[2];
    vnc->server_details.fmt.bpp = server_pixel_info[0];
    vnc->server_details.fmt.depth = server_pixel_info[1];
    vnc->server_details.fmt.is_big_endian = server_pixel_info[2];
    vnc->server_details.fmt.is_true_colour = server_pixel_info[3];

    // always in big endian order
    uint16_t *server_colour_maxima = (uint16_t *) &server_pixel_info[4];
    vnc->server_details.fmt.red_max = swap_endianness_u16b(server_colour_maxima[0]);
    vnc->server_details.fmt.green_max = swap_endianness_u16b(server_colour_maxima[1]);
    vnc->server_details.fmt.blue_max = swap_endianness_u16b(server_colour_maxima[2]);

    uint8_t *server_colour_shifts = (uint8_t *) &server_colour_maxima[3];
    vnc->server_details.fmt.red_shift = server_colour_shifts[0];
    vnc->server_details.fmt.green_shift = server_colour_shifts[1];
    vnc->server_details.fmt.blue_shift = server_colour_shifts[2];

    char *padding = (char *) &server_colour_shifts[3];
    padding += 3;

    uint32_t *server_name_info = (uint32_t *) padding;
    vnc->server_details.name_length = swap_endianness_u32b(server_name_info[0]);

    if (vnc->server_details.name_length) {

        vnc->server_details.name = malloc(vnc->server_details.name_length + 1);
        from_server(vnc->socket, vnc->server_details.name,
                vnc->server_details.name_length, true);
        vnc->server_details.name[vnc->server_details.name_length] = '\0';

    } else {
        vnc->server_details.name = NULL;
    }

    printf("server '%s':\n"
            "  pixel depth: %u (%u bpp)\n"
            "  true colour %s\n"
            "  %s pixel values\n"
            "  red:   %x << %2u\n"
            "  green: %x << %2u\n"
            "  blue:  %x << %2u\n",
            vnc->server_details.name ?  vnc->server_details.name : "UNNAMED",
            vnc->server_details.fmt.depth,
            vnc->server_details.fmt.bpp,
            vnc->server_details.fmt.is_true_colour ? "enabled" : "disabled",
            vnc->server_details.fmt.is_big_endian ? "big-endian" : "little-endian",
            vnc->server_details.fmt.red_max, vnc->server_details.fmt.red_shift,
            vnc->server_details.fmt.green_max, vnc->server_details.fmt.green_shift,
            vnc->server_details.fmt.blue_max, vnc->server_details.fmt.blue_shift);

    return 0;
}

SDL_Surface *create_surface(vnc_server_details *details) {
    return SDL_CreateRGBSurface(0, details->w, details->h, details->fmt.bpp,
            details->fmt.red_max << details->fmt.red_shift,
            details->fmt.green_max << details->fmt.green_shift,
            details->fmt.blue_max << details->fmt.blue_shift,
            0);
}

typedef enum {
    FRAME_BUFFER_UPDATE = 0,
    SET_COLOUR_MAP_ENTRIES = 1,
    BELL = 2,
    SERVER_CUT_TEXT = 3
} server_msg_t;

typedef enum {
    RAW = 0,
    COPY_RECT = 1,
    RRE = 2,
    HEXTILE = 5,
    TRLE = 15,
    ZRLE = 16,
    PSEUDO_CURSOR = -239,
    PSEUDO_DESKTOP_SIZE = -223
} RFB_encoding_type;

typedef struct {
    SDL_Rect r;
    RFB_encoding_type e;
} SDL_vnc_rect_header;

int set_encodings(SDL_vnc *vnc, RFB_encoding_type *encodings, uint n) {

    /*
     * Message size is:
     * - 2 bytes for message type;
     * - 1 byte padding;
     * - 2 bytes for number of encodings;
     * - 4-byte-wide encoding ID per encoding.
     */
    size_t msg_size = 4 + n * 4;

    assure_buffer_size(vnc->buffer, msg_size);

    uint8_t *msg = (uint8_t *) vnc->buffer.data;
    *msg++ = 2; // ID of SetEncoding message
    msg++;

    uint16_t *encoding_count = (uint16_t *) msg;
    *encoding_count++ = swap_endianness_u16b(n);

    int32_t *encoding_ids = (int32_t *) encoding_count;
    for (uint i = 0; i < n; i++) {
        *encoding_ids++ = swap_endianness_32b(encodings[i]);
    }

    return to_server(vnc->socket, vnc->buffer.data, msg_size);
}

int raw_from_server(SDL_vnc *vnc, SDL_vnc_rect_header *header) {
    size_t data_size =
        header->r.w * header->r.h * vnc->server_details.fmt.bpp / 8;

    server_to_scratch_buffer(vnc, header->r.w, header->r.h, true);

    return SDL_BlitSurface(vnc->scratch_buffer, NULL, vnc->surface, &header->r);
}

int copy_rect_from_server(SDL_vnc *vnc, SDL_vnc_rect_header *header) {
    server_to_vnc_buffer(vnc, 4, true);
    uint16_t *src_info = (uint16_t *) vnc->buffer.data;

    SDL_Rect src;
    src.x = swap_endianness_u16b(*src_info++);
    src.y = swap_endianness_u16b(*src_info++);
    src.w = header->r.w;
    src.h = header->r.h;

    return SDL_BlitSurface(vnc->surface, &src, vnc->surface, &header->r);
}

int rect_from_server(SDL_vnc *vnc, SDL_vnc_rect_header *header) {
    server_to_vnc_buffer(vnc, 12, true);

    uint16_t *rect_info = (uint16_t *) vnc->buffer.data;
    header->r.x = swap_endianness_u16b(*rect_info++);
    header->r.y = swap_endianness_u16b(*rect_info++);
    header->r.w = swap_endianness_u16b(*rect_info++);
    header->r.h = swap_endianness_u16b(*rect_info++);

    header->e = swap_endianness_32b(*((int32_t *) (rect_info)));

    switch (header->e) {
        case RAW:
            return raw_from_server(vnc, header);

        case COPY_RECT:
            return copy_rect_from_server(vnc, header);

        default:
            warn("unknown encoding method %i\n", header->e);
            return 0;
    }
}

int frame_buffer_update(SDL_vnc *vnc) {
    char buf[3];
    from_server(vnc->socket, buf, 3, true);
    uint16_t rect_count = swap_endianness_u16b(*((uint16_t *) (buf + 1)));

    debug("receiving framebuffer update of %u rectangles\n", rect_count);

    for (uint i = 0; i < rect_count; i++) {
        SDL_vnc_rect_header header;
        rect_from_server(vnc, &header);
    }
}

int framebuffer_update_request(int socket, bool incremental, uint16_t x,
        uint16_t y, uint16_t w, uint16_t h) {

    debug("sending framebuffer update request\n");

    char msg[10];

    uint8_t *msg_as_8b = (uint8_t *) msg;
    *msg_as_8b++ = 3;
    *msg_as_8b++ = incremental;

    uint16_t *msg_as_16b = (uint16_t *) msg_as_8b;
    *msg_as_16b++ = x;
    *msg_as_16b++ = y;
    *msg_as_16b++ = w;
    *msg_as_16b++ = h;

    return to_server(socket, msg, 10);
}

int resize_colour_map(vnc_colour_map *colour_map, size_t n) {
    colour_map->data = realloc(colour_map->data, n * sizeof (colour_map_entry));

    return !colour_map->data;
}

int assure_colour_map_size(vnc_colour_map *colour_map, size_t n) {
    if (colour_map->size < n) {
        return resize_colour_map(colour_map, n);
    }

    return 0;
}

int set_colour_map_entries(SDL_vnc *vnc) {
    server_to_vnc_buffer(vnc, 5, true);

    uint16_t *buf = (uint16_t *) (vnc->buffer.data + 1);
    uint16_t first_colour_index = *buf++;
    uint16_t number_of_colours = *buf++;
    uint16_t colour_index_end = first_colour_index + number_of_colours;

    debug("updating colours %u-%u in colour map\n", first_colour_index,
            colour_index_end - 1);

    assure_colour_map_size(&vnc->colour_map, colour_index_end);

    for (uint i = first_colour_index; i < colour_index_end; i++) {
        server_to_vnc_buffer(vnc, 6, true);

        uint16_t *colours = (uint16_t *) vnc->buffer.data;
        vnc->colour_map.data[i].r = *colours++;
        vnc->colour_map.data[i].g = *colours++;
        vnc->colour_map.data[i].b = *colours++;
    }

    return 0;
}

int update_loop(void *data) {
    SDL_vnc *vnc = data;

    while (vnc->thread) {
        server_msg_t msg;
        from_server(vnc->socket, &msg, 1, true);

        switch (msg) {
            case FRAME_BUFFER_UPDATE:
                frame_buffer_update(vnc);
                break;

            case SET_COLOUR_MAP_ENTRIES:
                set_colour_map_entries(vnc);
                break;

            //case BELL:
            //    //server_bell(vnc);
            //    break;

            //case SERVER_CUT_TEXT:
            //    //server_cut_text(vnc);
            //    break;

            default:
                printf("unknown msg code %u\n", msg);
                break;
        }

        framebuffer_update_request(vnc->socket, true, 0, 0,
                vnc->server_details.w, vnc->server_details.h);

        SDL_Delay(1000 / vnc->fps);
    }
}

SDL_Thread *create_update_thread(SDL_vnc *vnc) {
    return SDL_CreateThread(update_loop, "RFB Listener", vnc);
}

int handshake(SDL_vnc *vnc) {
    RFB_version server_version = get_server_version(vnc);
    RFB_version client_version =
        server_version == RFB_OTHER ? RFB_33 : server_version;

    send_client_version(vnc, client_version);

    negotiate_security(vnc, client_version);

    client_initialisation(vnc);
    server_initialisation(vnc);
}

int send_initial_framebuffer_update_request(SDL_vnc *vnc) {
    return framebuffer_update_request(vnc->socket, false, 0, 0,
            vnc->server_details.w, vnc->server_details.h);
}

SDL_vnc_result init_vnc_connection(SDL_vnc *vnc, char *host, uint port,
        uint fps) {

    int res;

    vnc->fps = fps;
    vnc->scratch_buffer = NULL;

    res = init_vnc_buffer(&vnc->buffer);
    if (res) {
        return SDL_VNC_ERROR_OOM;
    }

    vnc->socket = create_socket();
    if (vnc->socket <= 0) {
        return SDL_VNC_ERROR_COULD_NOT_CREATE_SOCKET;
    }

    res = vnc_connect(vnc, host, port);
    if (res) {
        return SDL_VNC_ERROR_COULD_NOT_CONNECT;
    }

    handshake(vnc);

    RFB_encoding_type encodings[] = {
        COPY_RECT, RAW
    };
    set_encodings(vnc, encodings, 2);
    send_initial_framebuffer_update_request(vnc);

    vnc->surface = create_surface(&vnc->server_details);
    vnc->thread = create_update_thread(vnc);

    return OK;
}

int wait_on_vnc_connection(SDL_vnc *vnc) {
    SDL_WaitThread(vnc->thread, NULL);
    return 0;
}

uint32_t translate_key(SDL_KeyCode key, bool shift) {
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

int pointer_event(SDL_vnc *vnc, uint32_t buttons, uint16_t x, uint16_t y,
        int32_t mw_x, int32_t mw_y) {

    uint8_t button_mask = 0;
    button_mask |= (buttons & SDL_BUTTON_LMASK) ? (1 << 0) : 0; // LMB
    button_mask |= (buttons & SDL_BUTTON_MMASK) ? (1 << 1) : 0; // MMB
    button_mask |= (buttons & SDL_BUTTON_RMASK) ? (1 << 2) : 0; // RMB

    button_mask |= (mw_y > 0) ? (1 << 3) : 0; // MW up
    button_mask |= (mw_y < 0) ? (1 << 4) : 0; // MW down
    button_mask |= (mw_x < 0) ? (1 << 5) : 0; // MW left
    button_mask |= (mw_x > 0) ? (1 << 6) : 0; // MW right

    debug("buttons: %x\nbutton mask: %x\n", buttons, button_mask);

    char buf[6];

    uint8_t *msg = (uint8_t *) buf;
    *msg++ = 5;
    *msg++ = button_mask;

    uint16_t *pos = (uint16_t *) msg;
    *pos++ = swap_endianness_u16b(x);
    *pos++ = swap_endianness_u16b(y);

    to_server(vnc->socket, buf, 6);
}

int key_event(SDL_vnc *vnc, bool pressed, SDL_Keysym sym) {
    char buf[8];
    SDL_Keycode key = sym.sym;

    bool shift = sym.mod & KMOD_SHIFT;

    uint8_t *msg = (uint8_t *) buf;
    *msg++ = 4;
    *msg++ = pressed;
    msg += 2;

    uint32_t *key_p = (uint32_t *) msg;
    *key_p = swap_endianness_u32b(translate_key(key, shift));

    to_server(vnc->socket, buf, 8);
}
