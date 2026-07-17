# PocketForge apps — tools build.
#
# Two build flavors from one source tree:
#   make native   host build (x86_64 dev box) — runs the vkms smoke
#   make cross    aarch64 device build — MUST be built with the pinned
#                 glibc-2.33 toolchain (ARM 10.3-2021.07); the stock
#                 Ubuntu cross gcc links against glibc 2.34+ symbols the
#                 device rejects. Use scripts/cross-build.sh, which runs
#                 this target inside pocketforge/build:10.3-2021.07-bookworm.
#
# Binaries are rpath-clean and link only libc/libdl/libm — the closed
# EGL/GLES userspace is dlopen'd at runtime, never linked.

BUILD ?= build
CC ?= cc
CROSS_COMPILE ?= aarch64-none-linux-gnu-
CROSS_CC := $(CROSS_COMPILE)gcc

CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
# -D__user= : the vendored kernel headers are raw in-tree UAPI copies
# (not `make headers_install` output, which would strip the annotation).
CPPFLAGS := -Isrc/vendor/drm-uapi -Isrc/vendor/linux-uapi -Isrc/vendor/khronos \
            -D__user=
LDLIBS := -ldl -lm

COMMON_SRC := src/common/drm_kms.c src/common/egl_dl.c
PRESENT_SRC := src/pf-drm-present/main.c $(COMMON_SRC)
AUDIT_SRC := src/pf-egl-audit/main.c src/common/egl_dl.c

.PHONY: all native cross smoke clean

all: native

native: $(BUILD)/native/pf-drm-present $(BUILD)/native/pf-egl-audit

cross: $(BUILD)/aarch64/pf-drm-present $(BUILD)/aarch64/pf-egl-audit

$(BUILD)/native/pf-drm-present: $(PRESENT_SRC) $(wildcard src/common/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(PRESENT_SRC) $(LDLIBS)

$(BUILD)/native/pf-egl-audit: $(AUDIT_SRC) $(wildcard src/common/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(AUDIT_SRC) $(LDLIBS)

$(BUILD)/aarch64/pf-drm-present: $(PRESENT_SRC) $(wildcard src/common/*.h)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(PRESENT_SRC) $(LDLIBS)

$(BUILD)/aarch64/pf-egl-audit: $(AUDIT_SRC) $(wildcard src/common/*.h)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(AUDIT_SRC) $(LDLIBS)

# Device-free CI/dev smoke: DRM half against vkms (load with
# `sudo modprobe vkms` first; scans for the vkms node).
smoke: $(BUILD)/native/pf-drm-present
	$(BUILD)/native/pf-drm-present --smoke --frames 60

clean:
	rm -rf $(BUILD)
