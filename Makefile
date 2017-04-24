
include config.mk

SRC=src
WLDSRC=$(SRC)/wld

PKGS = fontconfig wayland-client wayland-cursor xkbcommon pixman-1 libdrm

SOURCES = $(wildcard $(WLDSRC)/*.c)
SOURCES += $(wildcard $(SRC)/*.c)

ifeq ($(ENABLE_INTEL),1)
PKGS += libdrm_intel
SOURCES += $(wildcard $(WLDSRC)/intel/*.c)
CFLAGS += -DWITH_INTEL_DRM
endif
ifeq ($(ENABLE_NOUVEAU),1)
PKGS += libdrm_nouveau
SOURCES += $(wildcard $(WLDSRC)/nouveau/*.c)
CFLAGS += -DWITH_NOUVEAU_DRM
endif

CFLAGS += -std=c99 -Wall -g -DWITH_WAYLAND_DRM -DWITH_WAYLAND_SHM
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -I include
LDFLAGS = $(shell pkg-config --libs $(PKGS)) -lm -lutil

WAYLAND_HEADERS = $(wildcard include/*.xml)

HDRS = $(WAYLAND_HEADERS:.xml=-client-protocol.h)
WAYLAND_SRC = $(HDRS:.h=.c)
SOURCES += $(WAYLAND_SRC)

OBJECTS = $(SOURCES:.c=.o)

BIN_PREFIX = $(PREFIX)
SHARE_PREFIX = $(PREFIX)

all: wterm

include/config.h:
	cp config.def.h include/config.h

include/%-client-protocol.c: include/%.xml
	wayland-scanner code < $? > $@

include/%-client-protocol.h: include/%.xml
	wayland-scanner client-header < $? > $@

$(OBJECTS): $(HDRS) include/config.h

wterm: $(OBJECTS)
	$(CC) -o wterm $(OBJECTS) $(LDFLAGS)

clean:
	rm -f $(OBJECTS) $(HDRS) $(WAYLAND_SRC) include/config.h wterm

install-icons:
	mkdir -p $(SHARE_PREFIX)/share/icons/hicolor/scalable/apps/
	cp contrib/logo/wterm.svg $(SHARE_PREFIX)/share/icons/hicolor/scalable/apps/wterm.svg
	mkdir -p $(SHARE_PREFIX)/share/icons/hicolor/128x128/apps/
	cp contrib/logo/wterm.png $(SHARE_PREFIX)/share/icons/hicolor/128x128/apps/wterm.png

install-bin: wterm
	tic -s wterm.info
	mkdir -p $(BIN_PREFIX)/bin/
	cp wterm $(BIN_PREFIX)/bin/

install: install-bin install-icons

uninstall-icons:
	rm -f $(SHARE_PREFIX)/share/icons/hicolor/128x128/apps/wterm.png
	rm -f $(sHARE_PREFIX)/share/icons/hicolor/scalable/apps/wterm.svg

uninstall-bin:
	rm -f $(BIN_PREFIX)/bin/wterm

uninstall: uninstall-bin uninstall-icons
