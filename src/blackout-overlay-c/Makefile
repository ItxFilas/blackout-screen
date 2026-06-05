CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags wayland-client)
LIBS    = $(shell pkg-config --libs wayland-client)
SCANNER = wayland-scanner

# wlr-layer-shell (vendored XML in this dir)
PROTO   = wlr-layer-shell.xml
GEN_H   = wlr-layer-shell-unstable-v1-client-protocol.h
GEN_C   = wlr-layer-shell-unstable-v1-client-protocol.c

# pointer-constraints (from the system wayland-protocols package)
WP_DIR  = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
PC_XML  = $(WP_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
GEN_H2  = pointer-constraints-unstable-v1-client-protocol.h
GEN_C2  = pointer-constraints-unstable-v1-client-protocol.c

# keyboard-shortcuts-inhibit (seals the blackout: blocks Alt+Tab etc. while shown)
KSI_XML = $(WP_DIR)/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml
GEN_H3  = keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h
GEN_C3  = keyboard-shortcuts-inhibit-unstable-v1-client-protocol.c

all: blackout-overlay

$(GEN_H): $(PROTO)
	$(SCANNER) client-header $< $@

$(GEN_C): $(PROTO)
	$(SCANNER) private-code $< $@

$(GEN_H2):
	$(SCANNER) client-header $(PC_XML) $@

$(GEN_C2):
	$(SCANNER) private-code $(PC_XML) $@

$(GEN_H3):
	$(SCANNER) client-header $(KSI_XML) $@

$(GEN_C3):
	$(SCANNER) private-code $(KSI_XML) $@

blackout-overlay: blackout.c $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3)
	$(CC) $(CFLAGS) -o $@ blackout.c $(GEN_C) $(GEN_C2) $(GEN_C3) $(LIBS)

install: blackout-overlay
	install -m755 blackout-overlay ~/.local/bin/blackout-overlay

clean:
	rm -f blackout-overlay $(GEN_H) $(GEN_C) $(GEN_H2) $(GEN_C2) $(GEN_H3) $(GEN_C3)
