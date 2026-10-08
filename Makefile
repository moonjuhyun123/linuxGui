CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

# 기본: GTK 버전. 툴킷 없는 Xlib 버전은 `make xlib-hello`
all: gtk-lab

gtk-lab: gtk/main.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gtk4) -o $@ $< $(shell pkg-config --libs gtk4)

xlib-hello: xlib/main.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags x11) -o $@ $< $(shell pkg-config --libs x11)

run: gtk-lab
	./gtk-lab

clean:
	rm -f gtk-lab xlib-hello

.PHONY: all run clean
