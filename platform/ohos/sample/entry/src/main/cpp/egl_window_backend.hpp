#pragma once

#include "window_backend.hpp"

#include <mln/gfx/renderable.hpp>
#include <mln/gl/renderer_backend.hpp>
#include <mln/util/size.hpp>

#include <EGL/egl.h>
#include <native_window/external_window.h>

#include <cstdint>
#include <memory>
#include <string>

namespace mln {
namespace ohos {

class EGLWindowBackend final : public WindowBackend, public gl::RendererBackend, public gfx::Renderable {
public:
    EGLWindowBackend(OHNativeWindow*, Size);
    ~EGLWindowBackend() override;

    EGLWindowBackend(const EGLWindowBackend&) = delete;
    EGLWindowBackend& operator=(const EGLWindowBackend&) = delete;

    gfx::RendererBackend& getRendererBackend() override { return *this; }
    gfx::Renderable& getDefaultRenderable() override { return *this; }

    std::int32_t getGlesContextClientVersion() const override { return contextClientVersion; }
    void setSize(Size) override;
    void swap();
    std::uint64_t getLastSubmitDurationUs() const override { return lastSubmitDurationUs; }
    void bindWindowFramebuffer();
    void requestSnapshot() override;
    bool writeSnapshot(const std::string&) override;

protected:
    void activate() override;
    void deactivate() override;

    gl::ProcAddress getExtensionFunctionPointer(const char*) override;
    void updateAssumedState() override;

private:
    class DisplayConfig;

    std::shared_ptr<const DisplayConfig> displayConfig;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    OHNativeWindow* window = nullptr;
    std::int32_t contextClientVersion = 0;
    std::string framebufferDiagnostic;
    unsigned framebuffer = 0;
    unsigned colorBuffer = 0;
    unsigned depthStencilBuffer = 0;
    Size framebufferSize;
    bool captureRequested = false;
    std::uint64_t lastSubmitDurationUs = 0;
    std::string capturedPng;
    void releaseFramebuffer();
};

} // namespace ohos
} // namespace mln
