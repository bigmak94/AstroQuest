// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The GL side of showing the emulator's frames, shared by the OpenXR host and by the self test
// that exercises the same path without a headset session.

#include <cstdint>
#include <vector>

// Extension entry points (EGL images over hardware buffers) are only declared on request.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// The extension header relies on the types the one above defines.
#include <GLES2/gl2ext.h>

struct AHardwareBuffer;
class CoreProcess;

/// An OpenGL ES 3 context that draws to nothing: everything is rendered to textures.
struct GlContext {
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLConfig config{};
    EGLContext context{EGL_NO_CONTEXT};
    EGLSurface surface{EGL_NO_SURFACE};

    bool Create();
    void Destroy();
};

/// The images the emulator renders into: hardware buffers handed to it as dma-bufs, each of them
/// also a GL texture here, so that a frame gets from one process to the other without a copy.
class FrameBuffers {
public:
    static constexpr uint32_t Count = 3;

    /// Allocates the buffers and offers them to the core. Needs a current GL context.
    bool Create(EGLDisplay display, uint32_t width, uint32_t height, CoreProcess& core);
    void Destroy(EGLDisplay display);

    bool IsValid(uint32_t index) const {
        return index < textures.size();
    }
    GLuint Texture(uint32_t index) const {
        return textures[index];
    }

private:
    std::vector<AHardwareBuffer*> buffers;
    std::vector<EGLImageKHR> images;
    std::vector<GLuint> textures;
};

/// Copies a frame to the framebuffer that is bound, upside up and in the right color encoding.
class FrameBlitter {
public:
    bool Create();
    void Destroy();

    /// `srgb_target` is for sRGB targets: they encode whatever is written to them, while the
    /// emulator delivers values that are display-ready already. Where the GPU can be told to
    /// leave that encoding out, the frame is written as it is; elsewhere it is decoded first,
    /// which costs a power function per pixel.
    /// `reduced_fov` rounds each eye's boundary into a feathered squircle. The game already
    /// rendered the narrower projection; masking its corners preserves its pixel density.
    /// `fov` is that frame's out/in/up/down tangents: the aperture follows each eye's optical
    /// axis, which need not be at the centre of an asymmetric projection's texture.
    void Draw(GLuint texture, uint32_t width, uint32_t height, bool swap_red_blue,
              bool srgb_target, bool reduced_fov = false, const float* fov = nullptr) const;

    /// Whether frames go into sRGB targets unchanged (see Draw).
    bool WritesUnencoded() const {
        return write_control;
    }

private:
    GLuint program{};
    GLuint vertex_array{};
    GLint uniform_swap{-1};
    GLint uniform_decode{-1};
    GLint uniform_mask{-1};
    GLint uniform_fov{-1};
    bool write_control{};
};

/// The GPU's time from the start of what is drawn between Begin and End (a frame's copy) to its
/// end, as opposed to the time from handing it over to its end, which includes waiting behind
/// the emulator's work. What another context does on the GPU in between (the compositor, which
/// takes the GPU at every refresh) is counted too. Needs GL_EXT_disjoint_timer_query; does
/// nothing without it.
class GpuTimer {
public:
    bool Create();
    void Destroy();

    void Begin();
    void End();

    struct Summary {
        float mean_ms{};
        float worst_ms{};
        uint32_t count{};
    };
    /// What the timed work took since this was last asked.
    Summary Take();

private:
    void Collect();

    static constexpr uint32_t Slots = 4;
    GLuint queries[Slots]{};
    bool pending[Slots]{};
    uint32_t next{};
    bool available{};
    bool timing{};
    double sum_ms{};
    float worst_ms{};
    uint32_t count{};
};
