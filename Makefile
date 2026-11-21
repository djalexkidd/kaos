CC ?= cc
HOSTCC ?= cc
PKG_CONFIG ?= pkg-config
PKGS = sdl2 gl libopenmpt
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
OBJECTS = $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))
KAOS_ADF ?= Megademo 1 (1990-06-29)(Budbrain Productions)(Disk 1 of 2).adf
KAOS_MOD ?= kaos.mod
empty :=
space := $(empty) $(empty)
ASSET_INPUTS = $(subst $(space),\ ,$(KAOS_ADF)) $(subst $(space),\ ,$(KAOS_MOD))

.PHONY: all check clean FORCE
all: kaos

kaos: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/kaos.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/assets.o: CPPFLAGS += -Ibuild
build/assets.o: build/embedded_assets.h

build/embed-assets: tools/embed_assets.c src/kaos.h | build
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Isrc -o $@ $<

build/asset-inputs: FORCE | build
	@printf '%s\n' "$(KAOS_ADF)" "$(KAOS_MOD)" > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

build/embedded_assets.h: build/embed-assets build/asset-inputs $(ASSET_INPUTS)
	./build/embed-assets "$(KAOS_ADF)" "$(KAOS_MOD)" > $@.tmp && mv $@.tmp $@

build:
	mkdir -p $@

clean:
	rm -rf build kaos
