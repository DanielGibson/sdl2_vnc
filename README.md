SDL2_vnc: A companion library to SDL for working with VNC servers

This library should be useful to anyone who wnats to display VNC connection
output in a project using SDL2.

The codebase also includes a barebones VNC client, `vncc`, that acts as a
reference use of the library.

# Building SDL2_vnc

SDL2_vnc depends on SDL2, and so needs its headers available during building.

```
$ make
$ make install
```

Individual targets are declared in the `Makefile`. For example, to only build
SDL2_vnc as a shared library:

```
$ make SDL2_vnc.so
```

# Using SDL2_vnc

Be sure to initialise the library using `VNC_Init` before using any of its
functionality.

To get a VNC connection to a server up and running:
- allocate a `VNC_Connection`;
- initialise it using `VNC_InitConnection`.
From there, the surface containing the framebuffer data can be accessed at
`vnc_connection.surface`.

See `vncc.c` for a more thorough example use of the library.

# Issues

Issues can be raised in the GitLab project's issues. Alternatively, feel free to
contact me at <mag@magnostherobot.co.uk>.
