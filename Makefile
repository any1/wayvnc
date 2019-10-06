DEPENDENCIES := libuv egl glesv2 wayland-client

EXEC := wayvnc

SOURCES := \
	src/main.c \
	src/render.c \

include common.mk

PROTOCOLS := \
	$(BUILD_DIR)/proto-wlr-export-dmabuf-unstable-v1.o \

VERSION=0.0.0

ifndef DONT_STRIP
	INSTALL_STRIP := -s --strip-program=$(STRIP)
endif

protocols/build/%.c:
	make -C protocols

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/proto-%.o: protocols/build/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/$(EXEC): $(OBJECTS) $(PROTOCOLS) | $(BUILD_DIR) ; $(LINK_EXE)

.PHONY: install
install: $(EXEC)
	install $(INSTALL_STRIP) -Dt $(DESTDIR)$(PREFIX)/bin $(BUILD_DIR)/$(EXEC)

.PHONY: proto_clean
proto_clean:
	make -C protocols clean

clean: proto_clean
