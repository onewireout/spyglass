PKGS = wayland-client
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -Wall -Wextra
LIBS = $(shell pkg-config --libs $(PKGS))

PROBE_PKGS = wayland-client gbm libdrm
PROBE_CFLAGS = $(shell pkg-config --cflags $(PROBE_PKGS)) -Wall -Wextra
PROBE_LIBS = $(shell pkg-config --libs $(PROBE_PKGS))

CAPTURE_PKGS = wayland-client gbm libdrm egl glesv2
CAPTURE_CFLAGS = $(shell pkg-config --cflags $(CAPTURE_PKGS)) -Wall -Wextra
CAPTURE_LIBS = $(shell pkg-config --libs $(CAPTURE_PKGS))

CAPCAP_XML = /usr/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
CAPCAP_H   = ext-image-copy-capture-v1-client-protocol.h
CAPCAP_C   = ext-image-copy-capture-v1-protocol.c

CAPSRC_XML = /usr/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
CAPSRC_H   = ext-image-capture-source-v1-client-protocol.h
CAPSRC_C   = ext-image-capture-source-v1-protocol.c

DMABUF_XML = /usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml
DMABUF_H   = linux-dmabuf-v1-client-protocol.h
DMABUF_C   = linux-dmabuf-v1-protocol.c

FTLIST_XML = /usr/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
FTLIST_H   = ext-foreign-toplevel-list-v1-client-protocol.h
FTLIST_C   = ext-foreign-toplevel-list-v1-protocol.c

PROTO_XML = /usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
PROTO_H   = wlr-screencopy-unstable-v1-client-protocol.h
PROTO_C   = wlr-screencopy-unstable-v1-protocol.c

FTOP_XML  = /usr/share/wlr-protocols/unstable/wlr-foreign-toplevel-management-unstable-v1.xml
FTOP_H    = wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
FTOP_C    = wlr-foreign-toplevel-management-unstable-v1-protocol.c

XDGOUT_XML = /usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml
XDGOUT_H   = xdg-output-unstable-v1-client-protocol.h
XDGOUT_C   = xdg-output-unstable-v1-protocol.c

all: screencopy dmabuf_probe capture_dmabuf

$(PROTO_H): $(PROTO_XML)
	wayland-scanner client-header $< $@

$(PROTO_C): $(PROTO_XML)
	wayland-scanner private-code $< $@

$(FTOP_H): $(FTOP_XML)
	wayland-scanner client-header $< $@

$(FTOP_C): $(FTOP_XML)
	wayland-scanner private-code $< $@

$(XDGOUT_H): $(XDGOUT_XML)
	wayland-scanner client-header $< $@

$(XDGOUT_C): $(XDGOUT_XML)
	wayland-scanner private-code $< $@

screencopy.o: screencopy.c $(PROTO_H) $(FTOP_H) $(XDGOUT_H)
	$(CC) -c $< $(CFLAGS) -o $@

wlr-screencopy-unstable-v1-protocol.o: $(PROTO_C) $(PROTO_H)
	$(CC) -c $< $(CFLAGS) -o $@

wlr-foreign-toplevel-management-unstable-v1-protocol.o: $(FTOP_C) $(FTOP_H)
	$(CC) -c $< $(CFLAGS) -o $@

xdg-output-unstable-v1-protocol.o: $(XDGOUT_C) $(XDGOUT_H)
	$(CC) -c $< $(CFLAGS) -o $@

screencopy: screencopy.o wlr-screencopy-unstable-v1-protocol.o wlr-foreign-toplevel-management-unstable-v1-protocol.o xdg-output-unstable-v1-protocol.o
	$(CC) $^ $(LIBS) -o $@

$(CAPCAP_H): $(CAPCAP_XML)
	wayland-scanner client-header $< $@

$(CAPCAP_C): $(CAPCAP_XML)
	wayland-scanner private-code $< $@

$(CAPSRC_H): $(CAPSRC_XML)
	wayland-scanner client-header $< $@

$(CAPSRC_C): $(CAPSRC_XML)
	wayland-scanner private-code $< $@

$(FTLIST_H): $(FTLIST_XML)
	wayland-scanner client-header $< $@

$(FTLIST_C): $(FTLIST_XML)
	wayland-scanner private-code $< $@

$(DMABUF_H): $(DMABUF_XML)
	wayland-scanner client-header $< $@

$(DMABUF_C): $(DMABUF_XML)
	wayland-scanner private-code $< $@

dmabuf_probe.o: dmabuf_probe.c $(CAPCAP_H) $(CAPSRC_H) $(DMABUF_H) $(FTLIST_H)
	$(CC) -c $< $(PROBE_CFLAGS) -o $@

ext-image-copy-capture-v1-protocol.o: $(CAPCAP_C) $(CAPCAP_H)
	$(CC) -c $< $(PROBE_CFLAGS) -o $@

ext-image-capture-source-v1-protocol.o: $(CAPSRC_C) $(CAPSRC_H) $(FTLIST_H)
	$(CC) -c $< $(PROBE_CFLAGS) -o $@

linux-dmabuf-v1-protocol.o: $(DMABUF_C) $(DMABUF_H)
	$(CC) -c $< $(PROBE_CFLAGS) -o $@

ext-foreign-toplevel-list-v1-protocol.o: $(FTLIST_C) $(FTLIST_H)
	$(CC) -c $< $(PROBE_CFLAGS) -o $@

dmabuf_probe: dmabuf_probe.o ext-image-copy-capture-v1-protocol.o ext-image-capture-source-v1-protocol.o linux-dmabuf-v1-protocol.o ext-foreign-toplevel-list-v1-protocol.o
	$(CC) $^ $(PROBE_LIBS) -o $@

capture_dmabuf.o: capture_dmabuf.c $(CAPCAP_H) $(CAPSRC_H) $(DMABUF_H) $(FTLIST_H)
	$(CC) -c $< $(CAPTURE_CFLAGS) -o $@

capture_dmabuf: capture_dmabuf.o ext-image-copy-capture-v1-protocol.o ext-image-capture-source-v1-protocol.o linux-dmabuf-v1-protocol.o ext-foreign-toplevel-list-v1-protocol.o
	$(CC) $^ $(CAPTURE_LIBS) -o $@

clean:
	rm -f screencopy dmabuf_probe capture_dmabuf *.o $(PROTO_H) $(PROTO_C) $(FTOP_H) $(FTOP_C) $(XDGOUT_H) $(XDGOUT_C) \
		$(CAPCAP_H) $(CAPCAP_C) $(CAPSRC_H) $(CAPSRC_C) $(DMABUF_H) $(DMABUF_C) $(FTLIST_H) $(FTLIST_C)

.PHONY: all clean
