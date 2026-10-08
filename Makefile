CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

# 기본: GTK 버전 2개. 툴킷 없는 Xlib 버전은 `make xlib-hello`
all: gtk-lab cube

gtk-lab: gtk/main.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gtk4) -o $@ $< $(shell pkg-config --libs gtk4)

cube: gfx/cube.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gtk4) -o $@ $< $(shell pkg-config --libs gtk4) -lm

xlib-hello: xlib/main.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags x11) -o $@ $< $(shell pkg-config --libs x11)

run: gtk-lab
	./gtk-lab

clean:
	rm -f gtk-lab cube xlib-hello

.PHONY: all run clean
