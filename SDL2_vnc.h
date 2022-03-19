/**
 * \file SDL2_vnc.h
 *
 * Main include header for the SDL2_vnc library.
 */

#ifndef _SDL2_VNC_H
#define _SDL2_VNC_H

#include <SDL2/SDL.h>

/**
 * Structure used for the VNC connection's data buffer.
 */
typedef struct {
    size_t size; /**< Size of buffer in bytes. */
    void *data;  /**< Pointer to data buffer. */
} VNC_ConnectionBuffer;

/**
 * Pixel data format as specified by a VNC server during server initialisation.
 *
 * This structure closely resembles the [_Pixel Format Data Structure_ from
 * RFC6143][1].
 *
 * [1]: https://datatracker.ietf.org/doc/html/rfc6143#section-7.4
 *      "RFC6143 Section 7.4: Pixel Format Data Structure"
 */
typedef struct {

    /**
     * The number of bits used for each pixel value on the wire.
     */
    Uint8 bpp;

    /**
     * The number of useful bits in a pixel value.
     *
     * Must be 8, 16, or 32.
     */
    Uint8 depth;

    /**
     * Non-zero if multi-byte pixel values are to be interpreted as big-endian.
     */
    Uint8 is_big_endian;

    /**
     * Non-zero if true color mode is enabled.
     *
     * In true color mode, the `*_max` and `*_shift` entries are used to
     * specify how to extract red, green, and blue intensities from a pixel
     * value.
     *
     * When not in true color mode, pixel values instead index into a color map.
     */
    Uint8 is_true_color;

    /**
     * In true color mode, specifies the maximum red value.
     *
     * Must be `2^N - 1`, where `N` is the number of bits used for red values.
     */
    Uint16 red_max;

    /**
     * In true color mode, specifies the maximum green value.
     *
     * Must be `2^N - 1`, where `N` is the number of bits used for green values.
     */
    Uint16 green_max;

    /**
     * In true color mode, specifies the maximum blue value.
     *
     * Must be `2^N - 1`, where `N` is the number of bits used for blue values.
     */
    Uint16 blue_max;

    /**
     * In true color mode, specifies the number of bit-shifts needed to get the
     * red value to the least significant bit.
     */
    Uint8 red_shift;

    /**
     * In true color mode, specifies the number of bit-shifts needed to get the
     * green value to the least significant bit.
     */
    Uint8 green_shift;

    /**
     * In true color mode, specifies the number of bit-shifts needed to get the
     * blue value to the least significant bit.
     */
    Uint8 blue_shift;

} VNC_PixelFormat;

/**
 * Various information about the VNC server announced during server
 * initialisation.
 *
 * This structure closely resembles the [_ServerInit_ message from RFC6143][1].
 *
 * [1]: https://datatracker.ietf.org/doc/html/rfc6143#section-7.3.2
 *      "RFC6143 Section 7.3.2: ServerInit"
 */
typedef struct {
    Uint16 w; /**< Framebuffer width in pixels. */
    Uint16 h; /**< Framebuffer height in pixels. */
    VNC_PixelFormat fmt; /**< Server's pixel format. */

    /**
     * Length of the connection's name.
     *
     * \note RFC6143 does not specify if the name's length should include a
     *       terminator in the count. Currently, this library will always add a
     *       null-terminator to the name received, and will record the
     *       name-length as sent by the server in its initialisation.
     *       Be careful when using this value!
     */
    Uint32 name_length;

    /**
     * Connection's name.
     *
     * Always null-terminated.
     */
    char *name;

} VNC_ServerDetails;

/**
 * Structure for an entry in a connection's color map.
 */
typedef struct {
    Uint16 r; /**< Color map entry's red component. */
    Uint16 g; /**< Color map entry's green component. */
    Uint16 b; /**< Color map entry's blue component. */
} VNC_ColorMapEntry;

/**
 * Alias for VNC_ColorMapEntry.
 */
#define VNC_ColourMapEntry VNC_ColorMapEntry

/**
 * Structure used for the VNC connection's color map buffer.
 */
typedef struct {
    VNC_ColorMapEntry *data; /**< Pointer to entries in the color map. */
    size_t size;             /**< Number of entries in the color map. */
} VNC_ColorMap;

/**
 * Alias for VNC_ColorMap.
 */
#define VNC_ColourMap VNC_ColorMap

/**
 * VNC client-server connection information.
 */
typedef struct {

    /**
     * The socket associated with the connection.
     */
    int socket;

    /**
     * Data buffer for receiving and processing incoming messages.
     */
    VNC_ConnectionBuffer buffer;

    /**
     * Surface buffer for processing incoming buffer updates.
     */
    SDL_Surface *scratch_buffer;

    /**
     * Details about the server side of the connection.
     */
    VNC_ServerDetails server_details;

    /**
     * Surface containing up-to-date visualisation of the desktop buffer.
     */
    SDL_Surface *surface;

    /**
     * Thread that repeatedly polls the VNC server for buffer updates.
     */
    SDL_Thread *thread;

    /**
     * Maximum polling rate of the polling thread, in hertz.
     */
    unsigned fps;

    /**
     * VNC_UpdateLoop() terminates if this is false
     */
    SDL_bool thread_keep_running;

    /**
     * Server allows using QEMU Extended Key Events
     */
    SDL_bool qemu_keyevents_supported;

    /**
     * Color map of the connection.
     *
     * `NULL` if no color map has been specified.
     */
    VNC_ColorMap color_map;

    /**
     * Window associated with connection.
     *
     * If a window has been associated with a connection, server messages that
     * declare a changed buffer size read by the polling thread will
     * subsequently resize the window.
     */
    SDL_Window *window;

} VNC_Connection;

/**
 * Result of SDL2_vnc operations.
 */
typedef enum {

    /**
     * Operation completed successfully.
     */
    VNC_OK = 0,

    /**
     * Operation ran out of memory.
     */
    VNC_ERROR_OOM,

    /**
     * Operation could not create a socket.
     */
    VNC_ERROR_COULD_NOT_CREATE_SOCKET,

    /**
     * Operation could not connect to a VNC server.
     */
    VNC_ERROR_COULD_NOT_CONNECT,

    /**
     * VNC Server disconnected unexpectedly.
     */
    VNC_ERROR_SERVER_DISCONNECT,

    /**
     * VNC Server specified only security protocols that SDL2_vnc does not
     * support.
     */
    VNC_ERROR_UNSUPPORTED_SECURITY_PROTOCOLS,

    /**
     * Security handshake between client and server failed.
     */
    VNC_ERROR_SECURITY_HANDSHAKE_FAILED,

    /**
     * Current operation or feature is unimplemented in SDL2_vnc.
     */
    VNC_ERROR_UNIMPLEMENTED

} VNC_Result;

/**
 * SDL_Event type ID, for the event in which a VNC server disconnects from
 * SDL2_vnc while the polling thread is still active.
 *
 * \note Due to the nature of registering events with SDL, `VNC_SHUTDOWN` is not
 *       a constant, and so cannot be switch-cased against.
 */
extern int VNC_SHUTDOWN;

/**
 * Initialise SDL2_vnc for use.
 *
 * Must be called before other functions can be used, except VNC_ErrorString.
 *
 * \return 0 on successful initialisation; one of the \ref VNC_Result values
 *         otherwise.
 */
int VNC_Init();

/**
 * Get a relevant error string for a VNC_Result.
 *
 * The returned strings are constant. Two operations that return the same error
 * code are (likely) complaining about the same thing. It is unnecessary to
 * clear errors a la in the core SDL2 library.
 *
 * \param err The VNC_Result error/result code to fetch a string explanation
 *            for.
 *
 * \return A string explanation, or `"unknown error"` if `err` is unknown to
 *         SDL2_vnc.
 */
char *VNC_ErrorString(VNC_Result err);

/**
 * Initialise a VNC connection to a server.
 *
 * \param vnc  An allocated but not-yet-initialised VNC_Connection struct.
 * \param host The address of the server's host, specified as an IPv4 4-tuple
 *             string.
 * \param port The port of the server to connect through.
 * \param fps  The maximum polling rate of the connection, in hertz.
 *
 * \return 0 on successful connection; one of the \ref VNC_Result values
 *         otherwise.
 */
VNC_Result VNC_InitConnection(VNC_Connection *vnc, char *host, Uint16 port,
        unsigned fps);

/**
 * Wait on a connection's polling thread.
 *
 * This function will effectively wait on a VNC connection until it is
 * disconnected (for whatever reason).
 *
 * To get the reason for disconnection, poll SDL events for a \ref VNC_SHUTDOWN
 * event.
 *
 * \param vnc The connection to wait on.
 */
void VNC_WaitOnConnection(VNC_Connection *vnc);

/**
 * Create a window for displaying the frame buffer in, and associate it with the
 * given connection.
 *
 * The created window will have appropriate width and height for displaying the
 * frame buffer, and additionally will be resized whenever the VNC server
 * indicates that the frame buffer's size has changed.
 *
 * Many of the arguments given to this function are passed straight to
 * [`SDL_CreateWindow`][1]; see there for more information on those parameters.
 *
 * \param vnc The VNC connection to base the window create on.
 * \param title The title of the new window.
 * \param x The x-coordinate to place the window at.
 * \param y The y-coordinate to place the window at.
 * \param flags SDL2 window creation flags.
 *
 * \return The newly-created window.
 *
 * [1]: https://wiki.libsdl.org/SDL_CreateWindow
 *      "SDL_CreateWindow on the SDL Wiki"
 */
SDL_Window *VNC_CreateWindowForConnection(VNC_Connection *vnc, char *title,
        int x, int y, Uint32 flags);

/**
 * Send a keypress event to the VNC server.
 *
 * \param vnc     The VNC connection connected to the server.
 * \param pressed `SDL_TRUE` if the key was pressed; `SDL_FALSE` otherwise.
 * \param key     The keysym of the key.
 *
 * \return 0 on success; one of the \ref VNC_Result values otherwise.
 */
int VNC_SendKeyEvent(VNC_Connection *vnc, SDL_bool pressed, SDL_Keysym key);

/**
 * Send a mouse button-press or movement event to the VNC server.
 *
 * Sending a "full mouse click" requires sending two pointer events: one for the
 * mouse button press; another for the mouse button release. This is also true
 * of mouse wheel events: while SDL reports a scroll of a mousewheel as a single
 * atomic action, VNC connections require scrolling to be sent as a press
 * followed by a release.
 *
 * \param vnc         The VNC connection connected to the server.
 * \param button_mask The button mask describing the status of mouse keys, as
 *                    given by [SDL_GetMouseState][1].
 * \param x           The x-position of the pointer.
 * \param y           The y-position of the pointer.
 * \param mw_x        The status of the mousewheel's x-axis.
 * \param mw_y        The status of the mousewheel's y-axis.
 *
 * \return 0 on success; one of the \ref VNC_Result values otherwise.
 *
 * [1]: https://wiki.libsdl.org/SDL_GetMouseState
 *      "SDL_GetMouseState on the SDL Wiki"
 */
int VNC_SendPointerEvent(VNC_Connection *vnc, Uint32 button_mask,
        Uint16 x, Uint16 y, Sint32 mw_x, Sint32 mw_y);

#endif /* _SDL2_VNC_H */

/* vim: se ft=c tw=80 ts=4 sw=4 et : */
