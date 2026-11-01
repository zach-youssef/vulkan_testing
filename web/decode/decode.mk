# The web runtime's image decoder: stb_image built to WebAssembly (web/decode/stb_image_wasm.c), so
# images decode to exactly the bytes they do natively. Included from the Makefile, which sets
# ZGAL_ROOT and OUT.
#   make $(ZGAL_DECODE_WASM)  - builds build/web-decode/stb_image.wasm
#   make wasi-sdk             - installs the pinned wasi-sdk (once; see below)
#
# The compiler is wasi-sdk's clang (Apple's has no wasm32 target), pinned by
# .github/actions/setup-zgal/wasi-sdk.sh. WASI_SDK_PATH names an installed one (setup-zgal sets it
# in CI); without it, the pinned release is downloaded into $(ZGAL_ROOT)/.cache (git-ignored, kept
# by make clean) the first time it's needed.
ZGAL_WASI_SDK_SCRIPT = $(ZGAL_ROOT)/.github/actions/setup-zgal/wasi-sdk.sh
ZGAL_WASI_SDK_VERSION := $(shell sh $(ZGAL_WASI_SDK_SCRIPT) version)
ifeq ($(WASI_SDK_PATH),)
ZGAL_WASI_SDK = $(ZGAL_ROOT)/.cache/wasi-sdk-$(ZGAL_WASI_SDK_VERSION)
# Named by its version, so it's downloaded again only when the pin changes, not when the script does
$(ZGAL_WASI_SDK)/bin/clang:
	sh $(ZGAL_WASI_SDK_SCRIPT) install $(ZGAL_WASI_SDK)
	@touch $@
else
ZGAL_WASI_SDK = $(WASI_SDK_PATH)
endif
ZGAL_WASI_CLANG = $(ZGAL_WASI_SDK)/bin/clang

ZGAL_DECODE_DIR = $(ZGAL_ROOT)/web/decode
ZGAL_DECODE_OUT = $(OUT)/web-decode
ZGAL_DECODE_WASM = $(ZGAL_DECODE_OUT)/stb_image.wasm

# Freestanding: wasi-libc's headers only, no libc linked (the shim defines what stb_image calls),
# so a missing function fails the link rather than becoming an import. Bulk memory implements
# memcpy and memset (every WebGPU browser has it). Exports the shim's functions and the memory.
ZGAL_DECODE_CFLAGS = --target=wasm32-wasip1 --sysroot=$(ZGAL_WASI_SDK)/share/wasi-sysroot -O2 \
	-ffreestanding -fno-builtin -mbulk-memory -nostdlib -I$(ZGAL_ROOT)/third_party/stb
ZGAL_DECODE_LDFLAGS = -Wl,--no-entry -Wl,-z,stack-size=262144 -Wl,--strip-all \
	$(addprefix -Wl$(ZGAL_COMMA)--export=,zgal_alloc zgal_decode zgal_failure_reason zgal_reset)

$(ZGAL_DECODE_WASM): $(ZGAL_DECODE_DIR)/stb_image_wasm.c $(ZGAL_ROOT)/third_party/stb/stb_image.h \
		$(ZGAL_DECODE_DIR)/decode.mk $(ZGAL_WASI_CLANG)
	@mkdir -p $(dir $@)
	$(ZGAL_WASI_CLANG) $(ZGAL_DECODE_CFLAGS) -o $@ $< $(ZGAL_DECODE_LDFLAGS)

wasi-sdk: $(ZGAL_WASI_CLANG)

.PHONY: wasi-sdk

