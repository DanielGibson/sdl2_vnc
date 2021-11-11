CFLAGS += -Wpedantic -Wall

CFLAGS += $(shell sdl2-config --cflags)
LDLIBS += $(shell sdl2-config --libs)

PREFIX ?= /usr/local

default: all

all: vncc libSDL2_vnc.so libSDL2_vnc.a

vncc: libSDL2_vnc.so

libSDL2_vnc.so: SDL2_vnc.o
	$(CC) $(CFLAGS) -shared $(OUTPUT_OPTION) $^

libSDL2_vnc.a: libSDL2_vnc.a(SDL2_vnc.o)

install: all
	install -d $(DESTDIR)$(PREFIX)/{bin,include/SDL2,lib}/
	install -m 755 vncc $(DESTDIR)$(PREFIX)/bin/
	install -m 644 SDL2_vnc.h $(DESTDIR)$(PREFIX)/include/SDL2/SDL_vnc.h
	install -m 755 libSDL2_vnc.so $(DESTDIR)$(PREFIX)/lib/
	install -m 644 libSDL2_vnc.a $(DESTDIR)$(PREFIX)/lib/

clean:
	$(RM) vncc *.a *.so *.o

.PHONY: default all install clean
