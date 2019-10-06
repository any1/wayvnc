MACHINE := $(shell $(CC) -dumpmachine)
ARCH := $(firstword $(subst -, ,$(MACHINE)))
BUILD_DIR ?= build-$(MACHINE)

PREFIX ?= /usr/local

ifeq ($(ARCH),x86_64)
	ARCH_CFLAGS := -mavx
else
ifeq ($(ARCH),arm)
	ARCH_CFLAGS := -mfpu=neon
endif # end arm block
endif # end x86_64 block

ifeq (, $(shell which $(MACHINE)-strip 2>/dev/null))
	STRIP ?= strip
else
	STRIP ?= $(MACHINE)-strip
endif

ifeq (, $(shell which $(MACHINE)-pkg-config 2>/dev/null))
	PKGCONFIG ?= pkg-config
else
	PKGCONFIG ?= $(MACHINE)-pkg-config
endif

CFLAGS ?= -g -O3 $(ARCH_CFLAGS) -flto -DNDEBUG
LDFLAGS ?= -flto

CFLAGS += -std=gnu11 -D_GNU_SOURCE -Iinclude -Iprotocols/build

CC_OBJ = $(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $(@:.o=.deps)
LINK_EXE = $(CC) $^ $(LDFLAGS) -o $@

CFLAGS += $(foreach dep,$(DEPENDENCIES),$(shell $(PKGCONFIG) --cflags $(dep)))
LDFLAGS += $(foreach dep,$(DEPENDENCIES),$(shell $(PKGCONFIG) --libs $(dep)))
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)

$(BUILD_DIR): ; mkdir -p $(BUILD_DIR)

.PHONY: clean
clean: ; rm -rf $(BUILD_DIR)

-include $(BUILD_DIR)/*.deps

.SUFFIXES:
.SECONDARY:

# This clears the default target set by this file
.DEFAULT_GOAL :=
