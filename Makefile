CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  := $(shell pkg-config --libs x11)
CFLAGS  += $(shell pkg-config --cflags x11)

xlib-hello: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run: xlib-hello
	./xlib-hello -v

clean:
	rm -f xlib-hello

.PHONY: run clean
