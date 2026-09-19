#pragma once
// offline stand-in: the hook is not installed in the harness
struct funchook_t;
inline funchook_t *funchook_create() { return nullptr; }
inline int funchook_prepare(funchook_t *, void **, void *) { return -1; }
inline int funchook_install(funchook_t *, int) { return -1; }
