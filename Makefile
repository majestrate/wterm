
include config.mk

SRC=src
WLDSRC=$(SRC)/wld

PKGS = fontconfig wayland-client wayland-cursor xkbcommon pixman-1 libdrm

SOURCES = $(wildcard $(WLDSRC)/*.c)
SOURCES += $(wildcard $(SRC)/*.c)

ifeq ($(ENABLE_INTEL),1)
PKGS += libdrm_intel
SOURCES += $(wildcard $(WLDSRC)/intel/*.c)
endif
ifeq ($(ENABLE_NOUVEAU),1)
PKGS += libdrm_nouveau
SOURCES += $(wildcard $(WLDSRC)/nouveau/*.c)
endif

CFLAGS = -std=c99 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -I include -I $(WLDSRC)
LDFLAGS = $(shell pkg-config --libs $(PKGS))


OBJECTS = $(SOURCES:.c=.o)

WAYLAND_HEADERS = $(wildcard include/*.xml)

HDRS = $(WAYLAND_HEADERS:.xml=-client-protocol.h)

all: wterm

$(HDRS): $(WAYLAND_HEADERS)
	wayland-scanner client-header < $< > $@

$(OBJECTS): $(HDRS)

wterm: $(OBJECTS)
	$(CC) -o wterm $(OBJECTS) $(LDFLAGS)

clean:
	rm -f $(OBJECTS) $(HDRS)
