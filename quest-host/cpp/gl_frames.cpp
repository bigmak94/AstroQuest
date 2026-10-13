// SPDX-License-Identifier: GPL-2.0-or-later

#include "gl_frames.h"

#include <algorithm>
#include <cstring>

#include <android/hardware_buffer.h>
#include <dlfcn.h>

#include "core_process.h"
#include "log.h"

namespace {

/// How gralloc describes a buffer; the first file descriptor is the dma-buf with the pixels.
struct NativeHandle {
    int version;
    int num_fds;
    int num_ints;
    int data[1];
};

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("shader compilation failed: %s", log);
    }
    return shader;
}

const char* const BlitVertexShader = R"(#version 300 es
out vec2 uv;
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    // The frame's first row is its top row, GL's first row is the bottom one.
    uv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* const BlitFragmentShader = R"(#version 300 es
precision highp float;
uniform sampler2D frame;
uniform int swap_red_blue;
uniform int decode_srgb;
uniform int reduced_fov;
uniform vec4 fov_tan;
in vec2 uv;
out vec4 color;
void main() {
    vec3 c = texture(frame, uv).rgb;
    if (swap_red_blue != 0) {
        c = c.bgr;
    }
    if (reduced_fov != 0) {
        // Each eye's aperture is centred on straight ahead, not on the texture midpoint.
        // With asymmetric headset optics those midpoints point in opposite directions.
        vec2 eye_uv = vec2(fract(uv.x * 2.0), uv.y);
        vec2 axis = vec2((uv.x < 0.5 ? fov_tan.x : fov_tan.y) /
                            (fov_tan.x + fov_tan.y),
                        fov_tan.z / (fov_tan.z + fov_tan.w));
        vec2 radius = mix(axis, vec2(1.0) - axis, step(axis, eye_uv));
        // The pillowed rectangle reaches each frustum edge, with its curved corners around
        // the optical axis. Only visibility changes; the scene and lens warp are untouched.
        vec2 q = abs((eye_uv - axis) / radius);
        vec2 q2 = q * q;
        float edge = dot(q2, q2);
        float feather = max(0.10, 1.5 * fwidth(edge));
        // Finish the fade just inside the image, so filtering never leaks a colored seam
        // from the other eye or leaves a hard line at the projection's rectangular bounds.
        c *= 1.0 - smoothstep(0.98 - feather, 0.98, edge);
    }
    // The emulator delivers display-ready values. An sRGB target encodes whatever is written to
    // it, so those have to be made linear first.
    if (decode_srgb != 0) {
        c = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
    }
    color = vec4(c, 1.0);
}
)";

} // namespace

bool GlContext::Create() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }
    const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,                  EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,       8,                  EGL_ALPHA_SIZE,   8,
        EGL_DEPTH_SIZE,      0,                  EGL_NONE,
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &num_configs) ||
        num_configs < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    // Nothing is drawn to this surface, the context just needs one to be current.
    const EGLint surface_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        LOGE("unable to create the GL context: 0x%x", eglGetError());
        return false;
    }
    return true;
}

void GlContext::Destroy() {
    if (display == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) {
        eglDestroySurface(display, surface);
    }
    if (context != EGL_NO_CONTEXT) {
        eglDestroyContext(display, context);
    }
    eglTerminate(display);
    display = EGL_NO_DISPLAY;
    surface = EGL_NO_SURFACE;
    context = EGL_NO_CONTEXT;
}

bool FrameBuffers::Create(EGLDisplay display, uint32_t width, uint32_t height,
                          CoreProcess& core) {
    using GetNativeHandle = const NativeHandle* (*)(const AHardwareBuffer*);
    const auto get_native_handle = reinterpret_cast<GetNativeHandle>(
        dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle"));
    if (get_native_handle == nullptr) {
        LOGE("AHardwareBuffer_getNativeHandle is not available");
        return false;
    }

    AHardwareBuffer_Desc description{};
    description.width = width;
    description.height = height;
    description.layers = 1;
    description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    // The CPU bits keep the buffer in a plain row-by-row layout, which is what the emulator's
    // GPU driver can attach to and what its fallback path writes.
    description.usage =
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
        AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;

    std::vector<int> fds;
    uint32_t stride = 0;
    for (uint32_t i = 0; i < Count; ++i) {
        AHardwareBuffer* buffer = nullptr;
        if (AHardwareBuffer_allocate(&description, &buffer) != 0) {
            LOGE("unable to allocate frame buffer %u", i);
            return false;
        }
        buffers.push_back(buffer);
        AHardwareBuffer_Desc actual{};
        AHardwareBuffer_describe(buffer, &actual);
        stride = actual.stride;

        const NativeHandle* handle = get_native_handle(buffer);
        if (handle == nullptr || handle->num_fds < 1) {
            LOGE("frame buffer %u has no file descriptor", i);
            return false;
        }
        fds.push_back(handle->data[0]);

        const EGLClientBuffer client_buffer = eglGetNativeClientBufferANDROID(buffer);
        const EGLint image_attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        const EGLImageKHR image = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                                    EGL_NATIVE_BUFFER_ANDROID, client_buffer,
                                                    image_attributes);
        if (image == EGL_NO_IMAGE_KHR) {
            LOGE("unable to wrap frame buffer %u in an EGL image: 0x%x", i, eglGetError());
            return false;
        }
        images.push_back(image);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        textures.push_back(texture);
    }
    LOGI("frame buffers: %u x %ux%u, stride %u", Count, width, height, stride);
    core.SetBuffers(fds, width, height, stride);
    return true;
}

void FrameBuffers::Destroy(EGLDisplay display) {
    for (const GLuint texture : textures) {
        glDeleteTextures(1, &texture);
    }
    for (const EGLImageKHR image : images) {
        eglDestroyImageKHR(display, image);
    }
    for (AHardwareBuffer* buffer : buffers) {
        AHardwareBuffer_release(buffer);
    }
    textures.clear();
    images.clear();
    buffers.clear();
}

bool FrameBlitter::Create() {
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, BlitVertexShader);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, BlitFragmentShader);
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        LOGE("unable to link the blit program");
        return false;
    }
    uniform_swap = glGetUniformLocation(program, "swap_red_blue");
    uniform_decode = glGetUniformLocation(program, "decode_srgb");
    uniform_mask = glGetUniformLocation(program, "reduced_fov");
    uniform_fov = glGetUniformLocation(program, "fov_tan");
    glGenVertexArrays(1, &vertex_array);
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    write_control =
        extensions != nullptr && std::strstr(extensions, "GL_EXT_sRGB_write_control") != nullptr;
    return true;
}

void FrameBlitter::Destroy() {
    if (program != 0) {
        glDeleteProgram(program);
        program = 0;
    }
    if (vertex_array != 0) {
        glDeleteVertexArrays(1, &vertex_array);
        vertex_array = 0;
    }
}

bool GpuTimer::Create() {
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    available = extensions != nullptr &&
                std::strstr(extensions, "GL_EXT_disjoint_timer_query") != nullptr;
    if (available) {
        glGenQueries(Slots, queries);
        // Reading the flag clears it: whatever happened before counts for nothing.
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    }
    return available;
}

void GpuTimer::Destroy() {
    if (available) {
        glDeleteQueries(Slots, queries);
        available = false;
    }
}

void GpuTimer::Begin() {
    if (!available) {
        return;
    }
    Collect();
    // A slot whose time is not known yet is not used again: that frame goes untimed.
    timing = !pending[next];
    if (timing) {
        glBeginQuery(GL_TIME_ELAPSED_EXT, queries[next]);
    }
}

void GpuTimer::End() {
    if (!timing) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED_EXT);
    pending[next] = true;
    next = (next + 1) % Slots;
    timing = false;
}

void GpuTimer::Collect() {
    float times[Slots];
    uint32_t done = 0;
    for (uint32_t slot = 0; slot < Slots; ++slot) {
        if (!pending[slot]) {
            continue;
        }
        GLuint ready = 0;
        glGetQueryObjectuiv(queries[slot], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready == 0) {
            continue;
        }
        pending[slot] = false;
        GLuint nanoseconds = 0;
        glGetQueryObjectuiv(queries[slot], GL_QUERY_RESULT, &nanoseconds);
        times[done++] = static_cast<float>(nanoseconds) * 1e-6f;
    }
    if (done == 0) {
        return;
    }
    // Whatever made the GPU's clock jump since it was last asked (a change of its frequency, a
    // reset) leaves the times read now unusable. (Reading the flag clears it: once for all.)
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint != 0) {
        return;
    }
    for (uint32_t i = 0; i < done; ++i) {
        sum_ms += times[i];
        worst_ms = std::max(worst_ms, times[i]);
        ++count;
    }
}

GpuTimer::Summary GpuTimer::Take() {
    Collect();
    const Summary summary{
        .mean_ms = count != 0 ? static_cast<float>(sum_ms / count) : 0.0f,
        .worst_ms = worst_ms,
        .count = count,
    };
    sum_ms = 0.0;
    worst_ms = 0.0f;
    count = 0;
    return summary;
}

void FrameBlitter::Draw(GLuint texture, uint32_t width, uint32_t height, bool swap_red_blue,
                        bool srgb_target, bool reduced_fov, const float* fov) const {
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    const bool unencoded = srgb_target && write_control;
    if (unencoded) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
    glUseProgram(program);
    glUniform1i(uniform_swap, swap_red_blue ? 1 : 0);
    glUniform1i(uniform_decode, srgb_target && !write_control ? 1 : 0);
    glUniform1i(uniform_mask, reduced_fov ? 1 : 0);
    glUniform4f(uniform_fov, fov != nullptr ? fov[0] : 1.0f,
                fov != nullptr ? fov[1] : 1.0f, fov != nullptr ? fov[2] : 1.0f,
                fov != nullptr ? fov[3] : 1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vertex_array);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (unencoded) {
        // As it is by default: everything else drawn to such a target counts on it.
        glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    }
}
