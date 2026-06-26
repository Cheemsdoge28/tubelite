#include "layer.hpp"
#include <dlfcn.h>
#include <iostream>

using EGLDisplay = void*;
using EGLSurface = void*;
using EGLContext = void*;

#define EGL_DRAW 0x3059
#define EGL_READ 0x305A

typedef EGLDisplay (*PFN_eglGetCurrentDisplay)(void);
typedef EGLSurface (*PFN_eglGetCurrentSurface)(int re);
typedef EGLContext (*PFN_eglGetCurrentContext)(void);
typedef int (*PFN_eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);

static auto egl_get_current_display = []() -> PFN_eglGetCurrentDisplay {
    void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
    return lib ? reinterpret_cast<PFN_eglGetCurrentDisplay>(dlsym(lib, "eglGetCurrentDisplay")) : nullptr;
}();
static auto egl_get_current_surface = []() -> PFN_eglGetCurrentSurface {
    void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
    return lib ? reinterpret_cast<PFN_eglGetCurrentSurface>(dlsym(lib, "eglGetCurrentSurface")) : nullptr;
}();
static auto egl_get_current_context = []() -> PFN_eglGetCurrentContext {
    void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
    return lib ? reinterpret_cast<PFN_eglGetCurrentContext>(dlsym(lib, "eglGetCurrentContext")) : nullptr;
}();
static auto egl_make_current = []() -> PFN_eglMakeCurrent {
    void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_GLOBAL);
    return lib ? reinterpret_cast<PFN_eglMakeCurrent>(dlsym(lib, "eglMakeCurrent")) : nullptr;
}();

static bool local_restore_egl_context(void* dpy, void* draw, void* read, void* ctx) {
    if (egl_get_current_context && egl_get_current_context() == ctx) {
        return true;
    }
    if (egl_make_current && dpy && ctx) {
        return egl_make_current(dpy, draw, read, ctx) != 0;
    }
    return false;
}

bool Layer::init(SDL_Renderer* renderer, int w, int h, const SDL_Rect& dstRect) {
    destroy();
    w_ = w;
    h_ = h;
    dstRect_ = dstRect;

    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_TARGET, w, h);
    if (!texture_) {
        std::cerr << "[Layer] Failed to create SDL target texture: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
    return true;
}

void Layer::destroy() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (fbo_ != 0) {
        // We need EGL context current to delete the GLES framebuffer.
        // It will be deleted at context destruction otherwise.
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    w_ = 0;
    h_ = 0;
}

void Layer::setGeometry(const SDL_Rect& dstRect) {
    dstRect_ = dstRect;
}

void Layer::begin(SDL_Renderer* renderer, SDL_Color clearColor) {
    prevTarget_ = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, texture_);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}

void Layer::end(SDL_Renderer* renderer) {
    SDL_SetRenderTarget(renderer, prevTarget_);
    prevTarget_ = nullptr;
}

void Layer::saveGLESState() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer_);
    glGetIntegerv(GL_CURRENT_PROGRAM, &last_program_);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer_);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer_);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture_);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture_2d_);
    
    last_enable_blend_ = glIsEnabled(GL_BLEND);
    last_enable_depth_test_ = glIsEnabled(GL_DEPTH_TEST);
    last_enable_scissor_test_ = glIsEnabled(GL_SCISSOR_TEST);
    last_enable_cull_face_ = glIsEnabled(GL_CULL_FACE);
    
    glGetIntegerv(GL_VIEWPORT, last_viewport_);
    glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box_);
}

void Layer::restoreGLESState() {
    glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer_);
    glUseProgram(last_program_);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer_);
    glActiveTexture(last_active_texture_);
    glBindTexture(GL_TEXTURE_2D, last_texture_2d_);
    
    if (last_enable_blend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_enable_depth_test_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_enable_scissor_test_) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_enable_cull_face_) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    
    glViewport(last_viewport_[0], last_viewport_[1], last_viewport_[2], last_viewport_[3]);
    glScissor(last_scissor_box_[0], last_scissor_box_[1], last_scissor_box_[2], last_scissor_box_[3]);
}

void Layer::saveEGLContext() {
    if (egl_get_current_display && egl_get_current_surface && egl_get_current_context) {
        old_display_ = egl_get_current_display();
        old_draw_    = egl_get_current_surface(EGL_DRAW);
        old_read_    = egl_get_current_surface(EGL_READ);
        old_context_ = egl_get_current_context();
    }
}

void Layer::restoreEGLContext() {
    if (old_display_ && old_context_) {
        local_restore_egl_context(old_display_, old_draw_, old_read_, old_context_);
    }
}

void Layer::renderGLES(SDL_Renderer* renderer, void* egl_display, void* egl_draw, void* egl_read, void* egl_context,
                       std::function<void(unsigned int fbo)> glesRenderCallback) {
    if (!texture_) return;

    // ── Phase 1: Context-Safe Texture Extraction ─────────────────────────────
    // Bind/unbind SDL texture to extract its GL texture ID while the SDL EGL context
    // is guaranteed to be current on this thread.
    float texw = 0.0f, texh = 0.0f;
    SDL_GL_BindTexture(texture_, &texw, &texh);

    GLint texture_id = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_id);

    SDL_GL_UnbindTexture(texture_);

    // ── Phase 2: Save State ──────────────────────────────────────────────────
    // Save current GLES and EGL context states in the source context first.
    saveEGLContext();
    saveGLESState();

    // ── Phase 3: Switch Context and Render ───────────────────────────────────
    // Make target (mpv) EGL context current.
    local_restore_egl_context(egl_display, egl_draw, egl_read, egl_context);

    // Create and bind target FBO in the current EGL context.
    if (fbo_ == 0) {
        glGenFramebuffers(1, &fbo_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);

    // Setup viewport/scissor local coordinates for rendering
    glViewport(0, 0, w_, h_);
    glDisable(GL_SCISSOR_TEST);

    // Render GLES graphics
    glesRenderCallback(fbo_);

    // Unbind FBO from current context to avoid leaks/state pollution
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ── Phase 4: Restore Context and State ───────────────────────────────────
    // Critical: Switch EGL context back to SDL's context FIRST.
    restoreEGLContext();

    // Now restore GLES state in SDL's context where the states belong.
    restoreGLESState();
}

void Layer::present(SDL_Renderer* renderer, int opacity) {
    if (!texture_) return;
    Uint8 oldAlpha = 255;
    SDL_GetTextureAlphaMod(texture_, &oldAlpha);
    if (opacity != 255) {
        SDL_SetTextureAlphaMod(texture_, static_cast<Uint8>(opacity));
    }
    SDL_RenderCopy(renderer, texture_, nullptr, &dstRect_);
    if (opacity != 255) {
        SDL_SetTextureAlphaMod(texture_, oldAlpha);
    }
}
