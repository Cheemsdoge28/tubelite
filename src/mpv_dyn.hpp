#ifndef MPV_DYN_HPP
#define MPV_DYN_HPP

// Include the client API header for types like mpv_handle, mpv_event, etc.
#include <mpv/client.h>

// Guard: mpv_render_context_update_fn was a named typedef in libmpv1 headers
// but was removed in libmpv2 (trixie). We include render.h + render_gl.h only
// if we are NOT going to define the callback type ourselves, since the system
// header may or may not declare the old typedef. We always include them for
// the struct/enum types (mpv_render_context, mpv_render_param, etc.) but we
// never reference the old mpv_render_context_update_fn typedef directly.
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

    // Property set/get — present in all libmpv versions
    int (*set_property)(mpv_handle* ctx, const char* name, mpv_format format, void* data) = nullptr;
    char* (*get_property_string)(mpv_handle* ctx, const char* name) = nullptr;
    int (*set_property_string)(mpv_handle* ctx, const char* name, const char* data) = nullptr;

    // Render context function pointers
    // NOTE: the callback type is spelled out as `void(*)(void*)` explicitly.
    // libmpv1 headers exposed this as a named typedef `mpv_render_context_update_fn`
    // which libmpv2 removed.  Using the raw pointer type here means we compile
    // correctly against both header generations.
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

        std::vector<std::string> errors;
        for (const auto& lib : libs) {
            handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (handle) {
                std::cerr << "[mpv-dyn] Successfully loaded " << lib << "\n";
                break;
            } else {
                const char* err = dlerror();
                errors.push_back(lib + ": " + (err ? err : "unknown error"));
            }
        }

        if (!handle) {
            std::cerr << "[mpv-dyn] ERROR: Failed to load libmpv. Attempts:\n";
            for (const auto& err : errors) {
                std::cerr << "  - " << err << "\n";
            }
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

        ok &= load_sym(set_property, "mpv_set_property");
        ok &= load_sym(get_property_string, "mpv_get_property_string");
        ok &= load_sym(set_property_string, "mpv_set_property_string");

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

#define mpv_set_property g_mpv_dyn.set_property
#define mpv_get_property_string g_mpv_dyn.get_property_string
#define mpv_set_property_string g_mpv_dyn.set_property_string

#define mpv_render_context_create g_mpv_dyn.render_context_create
#define mpv_render_context_set_update_callback g_mpv_dyn.render_context_set_update_callback
#define mpv_render_context_update g_mpv_dyn.render_context_update
#define mpv_render_context_render g_mpv_dyn.render_context_render
#define mpv_render_context_free g_mpv_dyn.render_context_free

#endif // MPV_DYN_HPP
