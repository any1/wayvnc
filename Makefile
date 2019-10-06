DEPENDENCIES := libuv egl glesv2

EXEC := wayvnc

SOURCES := \
	src/main.c \
	src/render.c \

include common.mk

VERSION=0.0.0

ifndef DONT_STRIP
	INSTALL_STRIP := -s --strip-program=$(STRIP)
endif

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/$(EXEC): $(OBJECTS) | $(BUILD_DIR) ; $(LINK_EXE)

.PHONY: install
install: $(EXEC)
	install $(INSTALL_STRIP) -Dt $(DESTDIR)$(PREFIX)/bin $(BUILD_DIR)/$(EXEC)
