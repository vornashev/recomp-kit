// present_frame.h - the presenter's frame records, shared with its tests.
// Every texture here is a gpu.h handle owned through a lease: Target owns the
// scene textures it allocated, Composite owns one composed image. Nothing is
// freed by a handle going out of scope.
#pragma once
#include "compositor.h"
#include "gpu/gpu.h"
#include "present.h"
#include "ui_layer.h"

#include <memory>
#include <array>
#include <vector>

struct Fence {
    bool done = true, success = true;
};
// A texture the presenter allocated, destroyed with the last reference.
struct Composite {
    gpu::Device *device = nullptr;
    gpu::Texture texture;
    int w = 0, h = 0;
    gpu::Format format = gpu::Format::RGBA8;
    ~Composite() {
        if (device && texture)
            device->destroy(texture);
    }
};
struct Target {
    gpu::Device *device = nullptr;
    HostSceneTarget scene;
    gpu::Texture pixels;
    int pixels_w = 0, pixels_h = 0;
    int overlay_w = 0, overlay_h = 0;
    gpu::Format pixels_format = gpu::Format::RGBA8;
    std::vector<uint8_t> test_pixels;
    // A producer-owned copy until seal, then immutable until the presenter
    // has uploaded it. Reused only after this target's GPU leases retire.
    std::vector<uint8_t> upload_pixels;
    std::array<uint32_t, 256> upload_palette{};
    int upload_w = 0, upload_h = 0, upload_bpp = 0, upload_pitch = 0;
    bool upload_pending = false, upload_has_palette = false;
    gpu::Buffer upload_buffer;
    size_t upload_capacity = 0;
    void release_scene() {
        if (device) {
            if (scene.world)
                device->destroy(scene.world);
            if (scene.overlay)
                device->destroy(scene.overlay);
        }
        scene = {};
    }
    ~Target() {
        release_scene();
        if (device && pixels)
            device->destroy(pixels);
        if (device && upload_buffer)
            device->destroy(upload_buffer);
    }
};
struct Frame {
    uint64_t frame_id = 0, epoch = 0;
    HostScreenClass cls = HOST_SCREEN_MENU;
    bool had_draws = false, supplied = false, gpu = false, shown = false;
    bool released = false, dropped = false, success = true, repeat = false, staged_pixels = false;
    bool completion_fallback = false;
    double presented_ts = 0, gpu_ts = 0, submitted_ts = 0, sealed_ts = 0, gpu_ms = 0;
    double acknowledgement_deadline = 0;
    unsigned pending_prefixes = 0;
    std::shared_ptr<Target> target;
    // Scene history can advance before this drawable is acknowledged.
    std::shared_ptr<Target> scene_lease;
    std::shared_ptr<Fence> prefix = std::make_shared<Fence>();
    // Kept independently of the arena: no frame pointer crosses threads.
    UiFrame ui{};
    CompositorInput input{};
    LayoutSnapshot layout;
    HostFrameCapture capture;
    std::shared_ptr<Composite> composed;
    // The settings page texture behind input.settings_page, if any.
    std::shared_ptr<Composite> settings_page;
};
struct Message {
    void *surface = nullptr;
    int w = 640, h = 480;
    bool install = false;
};
