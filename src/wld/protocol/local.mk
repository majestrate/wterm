# wld: protocol/local.mk

dir := protocol

PROTOCOL_EXTENSIONS = $(dir)/wayland-drm.xml

$(dir)/%-protocol.c: $(dir)/%.xml
	$(call quiet,GEN,$(WAYLAND_SCANNER)) code < $< > $@

$(dir)/%-client-protocol.h: $(dir)/%.xml
	$(call quiet,GEN,$(WAYLAND_SCANNER)) client-header < $< > $@

CLEAN_FILES +=                                          \
    $(PROTOCOL_EXTENSIONS:%.xml=%-protocol.c)           \
    $(PROTOCOL_EXTENSIONS:%.xml=%-client-protocol.h)

include common.mk

