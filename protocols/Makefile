WAYLAND_SCANNER ?= wayland-scanner

PROTOCOLS := \
	wlr-export-dmabuf-unstable-v1.xml \
	wlr-screencopy-unstable-v1.xml \

SOURCES := $(PROTOCOLS:%.xml=build/%.c)
HEADERS := $(PROTOCOLS:%.xml=build/%.h)

all: $(SOURCES) $(HEADERS)

build:
	mkdir -p build

build/%.c: %.xml | build
	$(WAYLAND_SCANNER) private-code $< $@

build/%.h: %.xml | build
	$(WAYLAND_SCANNER) client-header $< $@

.PHONY: clean
clean:
	rm -rf build

.SUFFIXES:
.SECONDARY:
