#ifndef MPV_DYN_HPP
#define MPV_DYN_HPP

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <vector>

struct MpvDynLoader {
    void* handle = nullptr;

    // Function pointers
    mpv_handle* (*create)() = nullptr;
    int (*initialize)(mpv_handle* ctx) = nullptr;
    int (*set_option_string)(mpv_handle* ctx, const char* name, const char* data) = nullptr;
    int (*get_property)(mpv_handle* ctx, const char* name, mpv_format format, void* data) = nullptr;
    void (*free)(void* data) = nullptr;
    int (*command)(mpv_handle* ctx, const char** args) = nullptr;
    int (*command_async)(mpv_handle* ctx, uint64_t reply_userdata, const char** args) = nullptr;
    mpv_event* (*wait_event)(mpv_handle* ctx, double timeout) = nullptr;
    int (*observe_property)(mpv_handle* ctx, uint64_t reply_userdata, const char* name, mpv_format format) = nullptr;
    void (*terminate_destroy)(mpv_handle* ctx) = nullptr;
    const char* (*error_string)(int error) = nullptr;
    const char* (*event_name)(mpv_event_id event) = nullptr;

    // Render context function pointers
    int (*render_context_create)(mpv_render_context** res, mpv_handle* mal_ctx, mpv_render_param* params) = nullptr;
    void (*render_context_set_update_callback)(mpv_render_context* ctx, void (*callback)(void*), void* callback_data) = nullptr;
    uint64_t (*render_context_update)(mpv_render_context* ctx) = nullptr;
    int (*render_context_render)(mpv_render_context* ctx, mpv_render_param* params) = nullptr;
    void (*render_context_free)(mpv_render_context* ctx) = nullptr;

    bool load() {
        if (handle) return true;

        std::vector<std::string> libs = {
            "libmpv.so.2",
            "libmpv.so.1",
            "libmpv.so",
            "/roms/tools/tubelite/vendor/lib/libmpv.so.2",
            "/roms/tools/tubelite/vendor/lib/libmpv.so.1",
            "./vendor/lib/libmpv.so.2",
            "./vendor/lib/libmpv.so.1"
        };

        for (const auto& lib : libs) {
            handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (handle) {
                std::cerr << "[mpv-dyn] Successfully loaded " << lib << "\n";
                break;
            }
        }

        if (!handle) {
            std::cerr << "[mpv-dyn] ERROR: Failed to load libmpv (tried libmpv.so.2, libmpv.so.1, etc.): " << dlerror() << "\n";
            return false;
        }

        auto load_sym = [this](auto& ptr, const char* name) -> bool {
            ptr = reinterpret_cast<std::decay_t<decltype(ptr)>>(dlsym(handle, name));
            if (!ptr) {
                std::cerr << "[mpv-dyn] ERROR: Failed to load symbol " << name << ": " << dlerror() << "\n";
                return false;
            }
            return true;
        };

        bool ok = true;
        ok &= load_sym(create, "mpv_create");
        ok &= load_sym(initialize, "mpv_initialize");
        ok &= load_sym(set_option_string, "mpv_set_option_string");
        ok &= load_sym(get_property, "mpv_get_property");
        ok &= load_sym(free, "mpv_free");
        ok &= load_sym(command, "mpv_command");
        ok &= load_sym(command_async, "mpv_command_async");
        ok &= load_sym(wait_event, "mpv_wait_event");
        ok &= load_sym(observe_property, "mpv_observe_property");
        ok &= load_sym(terminate_destroy, "mpv_terminate_destroy");
        ok &= load_sym(error_string, "mpv_error_string");
        ok &= load_sym(event_name, "mpv_event_name");

        ok &= load_sym(render_context_create, "mpv_render_context_create");
        ok &= load_sym(render_context_set_update_callback, "mpv_render_context_set_update_callback");
        ok &= load_sym(render_context_update, "mpv_render_context_update");
        ok &= load_sym(render_context_render, "mpv_render_context_render");
        ok &= load_sym(render_context_free, "mpv_render_context_free");

        if (!ok) {
            dlclose(handle);
            handle = nullptr;
            return false;
        }
        return true;
    }

    ~MpvDynLoader() {
        if (handle) {
            dlclose(handle);
        }
    }
};

extern MpvDynLoader g_mpv_dyn;

// Define macros to map standard function calls to our dynamic pointers
#define mpv_create g_mpv_dyn.create
#define mpv_initialize g_mpv_dyn.initialize
#define mpv_set_option_string g_mpv_dyn.set_option_string
#define mpv_get_property g_mpv_dyn.get_property
#define mpv_free g_mpv_dyn.free
#define mpv_command g_mpv_dyn.command
#define mpv_command_async g_mpv_dyn.command_async
#define mpv_wait_event g_mpv_dyn.wait_event
#define mpv_observe_property g_mpv_dyn.observe_property
#define mpv_terminate_destroy g_mpv_dyn.terminate_destroy
#define mpv_error_string g_mpv_dyn.error_string
#define mpv_event_name g_mpv_dyn.event_name

#define mpv_render_context_create g_mpv_dyn.render_context_create
#define mpv_render_context_set_update_callback g_mpv_dyn.render_context_set_update_callback
#define mpv_render_context_update g_mpv_dyn.render_context_update
#define mpv_render_context_render g_mpv_dyn.render_context_render
#define mpv_render_context_free g_mpv_dyn.render_context_free

#endif // MPV_DYN_HPP
