#pragma once
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <functional>

// Layer encapsulates an SDL_Texture and its GLES rendering state,
// solving EGL context and state corruption bugs on GLES hardware (RK3326).
class Layer {
public:
    Layer() = default;
    ~Layer() { destroy(); }

    // Initialize layer with specific dimensions and destination rectangle on the screen
    bool init(SDL_Renderer* renderer, int w, int h, const SDL_Rect& dstRect);
    void destroy();

    // Resize or reposition the layer geometry
    void setGeometry(const SDL_Rect& dstRect);

    // Target SDL drawing to this layer's offscreen texture. Clears it with clearColor.
    void begin(SDL_Renderer* renderer, SDL_Color clearColor = {0, 0, 0, 0});
    
    // Restore the previous SDL render target
    void end(SDL_Renderer* renderer);

    // Safely render GLES graphics (e.g. mpv frames) into this layer's FBO/texture
    // while completely protecting SDL's internal rendering cache and state.
    void renderGLES(SDL_Renderer* renderer, void* egl_display, void* egl_draw, void* egl_read, void* egl_context,
                    std::function<void(unsigned int fbo)> glesRenderCallback);

    // Present this layer onto the screen (current render target)
    void present(SDL_Renderer* renderer, int opacity = 255);

    SDL_Texture* getTexture() const { return texture_; }
    int getWidth() const { return w_; }
    int getHeight() const { return h_; }
    SDL_Rect getDstRect() const { return dstRect_; }

private:
    SDL_Texture* texture_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    SDL_Rect dstRect_{0, 0, 0, 0};
    unsigned int fbo_ = 0;

    SDL_Texture* prevTarget_ = nullptr;

    // GLES state cache
    GLint last_framebuffer_ = 0;
    GLint last_program_ = 0;
    GLint last_array_buffer_ = 0;
    GLint last_element_array_buffer_ = 0;
    GLint last_active_texture_ = 0;
    GLint last_texture_2d_ = 0;
    GLboolean last_enable_blend_ = GL_FALSE;
    GLboolean last_enable_depth_test_ = GL_FALSE;
    GLboolean last_enable_scissor_test_ = GL_FALSE;
    GLboolean last_enable_cull_face_ = GL_FALSE;
    GLint last_viewport_[4]{0};
    GLint last_scissor_box_[4]{0};

    // EGL context cache
    void* old_display_ = nullptr;
    void* old_draw_ = nullptr;
    void* old_read_ = nullptr;
    void* old_context_ = nullptr;

    void saveGLESState();
    void restoreGLESState();
    void saveEGLContext();
    void restoreEGLContext();
};
