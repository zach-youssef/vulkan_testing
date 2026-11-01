// stb_image (third_party/stb, the copy the native build uses) built freestanding for wasm32, so the
// web runtime decodes images to exactly the RGBA8 bytes native stbi_load(..., 4) gives. There is no
// libc: this file gives stb_image the few functions it calls, and the module imports nothing.
// web/zgal/image-decode.ts drives it, one decode at a time:
//   zgal_alloc(size)                 room for the encoded file, which the caller copies in
//   zgal_decode(file, size, out)     its RGBA8 pixels (out[0] = width, out[1] = height), or 0
//   zgal_failure_reason()            why the last decode failed (stbi_failure_reason)
//   zgal_reset()                     frees everything allocated since the last reset
// Built by web/decode/decode.mk.

#include <stddef.h>
#include <stdint.h>

// Allocation: a bump allocator over the memory after the linker's heap base, grown as needed. The
// caller resets it after each decode, so nothing outlives one, and free only returns the block on
// top. Each block records its size before it, for realloc.
extern unsigned char __heap_base;
static uintptr_t heapTop = (uintptr_t)&__heap_base;

#define ALIGNMENT 16

static void* bumpAlloc(size_t size) {
  uintptr_t block = (heapTop + sizeof(size_t) + ALIGNMENT - 1) & ~(uintptr_t)(ALIGNMENT - 1);
  if (size > SIZE_MAX - block) return 0;
  uintptr_t end = block + size;
  size_t available = __builtin_wasm_memory_size(0) * 65536;
  if (end > available) {
    size_t pages = (end - available + 65535) / 65536;
    if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return 0;
  }
  ((size_t*)block)[-1] = size;
  heapTop = end;
  return (void*)block;
}

void* malloc(size_t size) { return bumpAlloc(size); }

void free(void* p) {
  if (p && (uintptr_t)p + ((size_t*)p)[-1] == heapTop) heapTop = (uintptr_t)p - sizeof(size_t);
}

void* realloc(void* p, size_t size) {
  if (!p) return bumpAlloc(size);
  size_t old = ((size_t*)p)[-1];
  // The top block grows in place
  if ((uintptr_t)p + old == heapTop) {
    heapTop = (uintptr_t)p - sizeof(size_t);
    void* same = bumpAlloc(size);
    if (same) return same;
    heapTop = (uintptr_t)p + old;
    return 0;
  }
  void* moved = bumpAlloc(size);
  if (moved) __builtin_memcpy(moved, p, old < size ? old : size);
  return moved;
}

// What the compiler and stb_image call. This file is built with -fno-builtin, and bulk memory
// (memory.copy and memory.fill) implements the builtins, so they don't call themselves.
void* memcpy(void* dst, const void* src, size_t n) { return __builtin_memcpy(dst, src, n); }
void* memmove(void* dst, const void* src, size_t n) { return __builtin_memmove(dst, src, n); }
void* memset(void* dst, int c, size_t n) { return __builtin_memset(dst, c, n); }

int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* x = a;
  const unsigned char* y = b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return x[i] - y[i];
  }
  return 0;
}

int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
  for (; n > 0; n--, a++, b++) {
    if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
  }
  return 0;
}

int abs(int x) { return x < 0 ? -x : x; }

// stb_image as native code has it (every format, failure strings), but for what needs libc's maths
// or files: HDR images (stbi_load converts them with pow, which needn't round as native libm does)
// and stdio. A failed assertion traps, as native's aborts.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((x) ? (void)0 : __builtin_trap())
#include "stb_image.h"

void* zgal_alloc(size_t size) { return malloc(size); }

unsigned char* zgal_decode(const unsigned char* file, int size, int* out) {
  int channels;
  return stbi_load_from_memory(file, size, &out[0], &out[1], &channels, STBI_rgb_alpha);
}

const char* zgal_failure_reason(void) { return stbi_failure_reason(); }

void zgal_reset(void) { heapTop = (uintptr_t)&__heap_base; }
