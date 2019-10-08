DEPENDENCIES := libuv egl glesv2 wayland-client neatvnc pixman-1

EXEC := wayvnc

SOURCES := \
	src/main.c \
	src/render.c \
	src/dmabuf.c \
	src/strlcpy.c \
	src/shm.c \
	src/screencopy.c \

include common.mk

PROTOCOLS := \
	$(BUILD_DIR)/proto-wlr-export-dmabuf-unstable-v1.o \
	$(BUILD_DIR)/proto-wlr-screencopy-unstable-v1.o \

VERSION=0.0.0

ifndef DONT_STRIP
	INSTALL_STRIP := -s --strip-program=$(STRIP)
endif

all: $(BUILD_DIR)/$(EXEC) | proto

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/proto-%.o: protocols/build/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/$(EXEC): $(OBJECTS) $(PROTOCOLS) | $(BUILD_DIR) ; $(LINK_EXE)

.PHONY: install
install: $(EXEC)
	install $(INSTALL_STRIP) -Dt $(DESTDIR)$(PREFIX)/bin $(BUILD_DIR)/$(EXEC)

.PHONY: proto
proto:
	make -C protocols

.PHONY: proto_clean
proto_clean:
	make -C protocols clean

clean: proto_clean
