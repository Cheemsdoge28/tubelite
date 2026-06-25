// ── GBM "modifier" symbol compatibility shims ──────────────────────────────
// Some handheld images pair a NEWER libmpv (e.g. Debian trixie's libmpv.so.2,
// built 0.40.0) with an OLDER / vendor libgbm that does not export the
// "modifier" family of GBM functions (added in Mesa 17.1, ~2017).  Because
// trixie builds libmpv with BIND_NOW (DF_BIND_NOW), the dynamic linker resolves
// ALL of libmpv's symbols at dlopen() time, so a single missing gbm_* symbol
// makes the whole load fail with "undefined symbol" — and no RTLD_LAZY can
// rescue it (BIND_NOW overrides lazy binding).
//
// Every one of these functions lives ONLY in mpv's gbm-allocated scanout/
// windowing path (--gpu-context=drm / context_drm_egl), which TubeLite never
// uses: we drive mpv through the OpenGL render API with our own SDL/EGL
// context, and the hwdec dmabuf-interop path takes its offsets/strides/
// modifiers from the AVDRMFrameDescriptor (libavutil), NOT from gbm.  So these
// are never called — they exist purely to satisfy the dynamic linker so
// libmpv.so.2 loads.
//
// They are WEAK: on systems whose libgbm DOES export the real functions, the
// strong libgbm definitions win (glibc's resolver prefers a strong global over
// a weak one), so we never shadow a working symbol.  SDL is unaffected either
// way — it resolves its own GBM calls through a private dlopen(libgbm)+dlsym,
// not the global symbol scope these stubs live in.
//
// Signatures are irrelevant for resolution (C linkage matches by name only) and
// the bodies are never executed, so every stub is declared uniformly as
// `void* name(void)`.  Exported from the executable via -rdynamic.

#define GBM_STUB(name) \
    __attribute__((weak, visibility("default"))) \
    void* name(void) { return nullptr; }

extern "C" {

GBM_STUB(gbm_surface_create_with_modifiers)
GBM_STUB(gbm_surface_create_with_modifiers2)
GBM_STUB(gbm_bo_create_with_modifiers)
GBM_STUB(gbm_bo_create_with_modifiers2)
GBM_STUB(gbm_bo_get_modifier)
GBM_STUB(gbm_bo_get_plane_count)
GBM_STUB(gbm_bo_get_handle_for_plane)
GBM_STUB(gbm_bo_get_stride_for_plane)
GBM_STUB(gbm_bo_get_offset)
GBM_STUB(gbm_device_get_format_modifier_plane_count)

}  // extern "C"

#undef GBM_STUB

// The stubs have no in-program caller (only the dlopen'd libmpv references
// them, which the linker cannot see), so -ffunction-sections + --gc-sections
// would strip them before -rdynamic can export them.  Reference their addresses
// from a `used` table so the linker is forced to keep them.  (Storing function
// pointers — not void* — avoids the -Wpedantic function-to-object-pointer cast
// warning; `__attribute__((retain))` would also work but needs binutils >= 2.36
// which older on-device toolchains lack.)
typedef void* (*gbm_compat_fn)(void);
static const gbm_compat_fn gbm_compat_keep[] __attribute__((used)) = {
    &gbm_surface_create_with_modifiers,
    &gbm_surface_create_with_modifiers2,
    &gbm_bo_create_with_modifiers,
    &gbm_bo_create_with_modifiers2,
    &gbm_bo_get_modifier,
    &gbm_bo_get_plane_count,
    &gbm_bo_get_handle_for_plane,
    &gbm_bo_get_stride_for_plane,
    &gbm_bo_get_offset,
    &gbm_device_get_format_modifier_plane_count,
};
