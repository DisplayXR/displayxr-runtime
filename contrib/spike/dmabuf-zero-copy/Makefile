# Standalone spike harness for epic #1699 -- NOT part of the runtime build.
# libgbm-dev may be absent: we vendor gbm.h (third_party/, Mesa 26.0.8, MIT) and
# link libgbm.so.1 through a local unversioned symlink.
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS += -std=gnu11 $(shell pkg-config --cflags libdrm)
OUT := build
GBM_SO := $(firstword $(wildcard /usr/lib/x86_64-linux-gnu/libgbm.so.1 /usr/lib64/libgbm.so.1 /usr/lib/libgbm.so.1))
LINUX_DMABUF_XML := /usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml

all: $(OUT)/producer $(OUT)/consumer $(OUT)/wl_query

$(OUT):
	mkdir -p $(OUT)

$(OUT)/libgbm.so: | $(OUT)
	ln -sf $(GBM_SO) $@

$(OUT)/sample_comp_spv.h: sample.comp | $(OUT)
	glslangValidator -V --target-env vulkan1.1 --vn sample_comp_spv -o $@ $<

$(OUT)/producer: producer.c common.h vkcommon.h third_party/gbm.h $(OUT)/libgbm.so
	$(CC) $(CFLAGS) -o $@ producer.c -L$(OUT) -lgbm -lEGL -lGLESv2 -lvulkan -ldrm

$(OUT)/consumer: consumer.c common.h vkcommon.h $(OUT)/sample_comp_spv.h
	$(CC) $(CFLAGS) -I$(OUT) -o $@ consumer.c -lvulkan -ldrm

$(OUT)/linux-dmabuf-v1-client.h: $(LINUX_DMABUF_XML) | $(OUT)
	wayland-scanner client-header $< $@
$(OUT)/linux-dmabuf-v1-protocol.c: $(LINUX_DMABUF_XML) | $(OUT)
	wayland-scanner private-code $< $@

$(OUT)/wl_query: wl_query.c $(OUT)/linux-dmabuf-v1-client.h $(OUT)/linux-dmabuf-v1-protocol.c
	$(CC) $(CFLAGS) -I$(OUT) -o $@ wl_query.c $(OUT)/linux-dmabuf-v1-protocol.c $(shell pkg-config --cflags --libs wayland-client) -ldrm

clean:
	rm -rf $(OUT)

.PHONY: all clean
