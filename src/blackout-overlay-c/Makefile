CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags wayland-client)
LIBS    = $(shell pkg-config --libs wayland-client)
SCANNER = wayland-scanner
PROTO   = wlr-layer-shell.xml
GEN_H   = wlr-layer-shell-unstable-v1-client-protocol.h
GEN_C   = wlr-layer-shell-unstable-v1-client-protocol.c

all: blackout-overlay

$(GEN_H): $(PROTO)
	$(SCANNER) client-header $< $@

$(GEN_C): $(PROTO)
	$(SCANNER) private-code $< $@

blackout-overlay: blackout.c $(GEN_H) $(GEN_C)
	$(CC) $(CFLAGS) -o $@ blackout.c $(GEN_C) $(LIBS)

install: blackout-overlay
	install -m755 blackout-overlay ~/.local/bin/blackout-overlay

clean:
	rm -f blackout-overlay $(GEN_H) $(GEN_C)
