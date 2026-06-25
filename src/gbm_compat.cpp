// ── GBM "modifier" symbol compatibility shims ──────────────────────────────
// Some handheld images pair a NEWER libmpv (e.g. Debian trixie's libmpv.so.2,
// built 0.40.0) with an OLDER / vendor libgbm that does not export the
// "modifier" family of GBM surface functions.  Because trixie builds libmpv
// with BIND_NOW (DF_BIND_NOW), the dynamic linker resolves ALL of libmpv's
// symbols at dlopen() time — so the single missing
// `gbm_surface_create_with_modifiers` makes the whole load fail with
// "undefined symbol", and no RTLD_LAZY can rescue it (BIND_NOW overrides lazy).
//
// These functions live ONLY in mpv's standalone GBM/DRM windowing backend
// (--gpu-context=drm), which TubeLite never uses: we drive mpv through the
// OpenGL render API with our own SDL/EGL context, so mpv allocates no GBM
// surfaces itself.  We therefore provide never-called stubs and export them
// from the executable (-rdynamic) so libmpv's dangling reference resolves to
// them and the library loads.
//
// They are WEAK: on systems whose libgbm DOES export the real functions, the
// strong libgbm definitions win (glibc's resolver prefers a strong global over
// a weak one), so we never shadow a working symbol.  SDL is unaffected either
// way — it resolves its own GBM calls through a private dlopen(libgbm)+dlsym,
// not the global symbol scope these stubs live in.

extern "C" {

__attribute__((weak, visibility("default")))
void* gbm_surface_create_with_modifiers(void)  { return nullptr; }

__attribute__((weak, visibility("default")))
void* gbm_surface_create_with_modifiers2(void) { return nullptr; }

}  // extern "C"

// The stubs have no in-program caller (only the dlopen'd libmpv references
// them, which the linker cannot see), so -ffunction-sections + --gc-sections
// would strip them before -rdynamic can export them.  Reference their
// addresses from a `used` table so the linker is forced to keep them.
// (`__attribute__((retain))` would also work but needs binutils >= 2.36, which
// the older on-device toolchains lack — this trick works everywhere.)
typedef void* (*gbm_compat_fn)(void);
static const gbm_compat_fn gbm_compat_keep[] __attribute__((used)) = {
    &gbm_surface_create_with_modifiers,
    &gbm_surface_create_with_modifiers2,
};
