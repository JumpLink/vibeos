#include "navi-backend.hpp"

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gl/renderable_resource.hpp>
#include <mln/renderer/renderer.hpp>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>

namespace navi {

namespace {

class Resource final : public mln::gl::RenderableResource {
public:
    explicit Resource(Backend& backend_)
        : backend(backend_) {}

    void bind() override {
        backend.setFramebufferBinding(backend.framebuffer());
        backend.setViewport(0, 0, backend.getSize());
    }

    // GTK presents the GL area's texture itself: no swap here
    void swap() override {}

private:
    Backend& backend;
};

} // namespace

Backend::Backend(GtkGLArea* area_)
    // Unique: the GL area's context is ours alone, so MapLibre may clear to the style's background
    : mln::gl::RendererBackend(mln::gfx::ContextMode::Unique),
      mln::gfx::Renderable({1, 1}, std::make_unique<Resource>(*this)),
      area(area_) {}

Backend::~Backend() {
    // the widget makes its context current before the map goes (unrealize)
    releaseOffscreen();
}

void Backend::releaseOffscreen() {
    if (offFbo) glDeleteFramebuffers(1, &offFbo);
    if (offColor) glDeleteRenderbuffers(1, &offColor);
    if (offDepth) glDeleteRenderbuffers(1, &offDepth);
    offFbo = offColor = offDepth = 0;
    offSize = offAreaSize = {0, 0};
}

void Backend::setFramebufferSize(mln::Size size) {
    areaSize = size;
    if (renderScale >= 1) {
        setRenderableSize(size);
        return;
    }
    setRenderableSize({std::max(1u, static_cast<uint32_t>(std::lround(size.width * renderScale))),
                       std::max(1u, static_cast<uint32_t>(std::lround(size.height * renderScale)))});
}

void Backend::setRenderScale(double scale) {
    renderScale = std::clamp(scale, 0.25, 1.0);
    setFramebufferSize(areaSize);
}

void Backend::beginFrame() {
    GLint bound = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    areaFbo = fbo = static_cast<uint32_t>(bound);
    const mln::Size size = getSize();
    if (usesOffscreen()) {
        if (size != offSize) {
            releaseOffscreen();
            glGenRenderbuffers(1, &offColor);
            glBindRenderbuffer(GL_RENDERBUFFER, offColor);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size.width, size.height);
            glGenRenderbuffers(1, &offDepth);
            glBindRenderbuffer(GL_RENDERBUFFER, offDepth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size.width, size.height);
            glGenFramebuffers(1, &offFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, offFbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, offColor);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, offDepth);
            offSize = size;
        }
        offAreaSize = areaSize;
        fbo = offFbo;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    // GtkGLArea binds its framebuffer but leaves the viewport at whatever it was (0x0 on a new context),
    // and updateAssumedState() tells MapLibre the viewport is already the full buffer: make that true,
    // or every draw lands in an empty viewport while clears still fill the buffer
    glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
}

// one copy (a linear stretch when scaled) onto the GL area's buffer, which GTK then composites as it is
void Backend::blitOffscreen() {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, areaFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, offFbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, static_cast<GLint>(offSize.width), static_cast<GLint>(offSize.height), 0, 0,
                      static_cast<GLint>(areaSize.width), static_cast<GLint>(areaSize.height), GL_COLOR_BUFFER_BIT,
                      offSize == areaSize ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, areaFbo);
}

void Backend::endFrame(bool drew) {
    if (fbo != areaFbo) {
        if (drew)
            blitOffscreen();
        else
            glBindFramebuffer(GL_FRAMEBUFFER, areaFbo);
    }
    if (!drew) {
        glViewport(0, 0, static_cast<GLsizei>(areaSize.width), static_cast<GLsizei>(areaSize.height));
        glClearColor(0.93f, 0.93f, 0.91f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

bool Backend::presentAhead() {
    if (!offFbo || offAreaSize != areaSize) return false;
    GLint bound = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    areaFbo = static_cast<uint32_t>(bound);
    blitOffscreen();
    return true;
}

void Backend::activate() {
    // inside "render" the context is current already; this matters for teardown and shader setup
    if (gtk_widget_get_realized(GTK_WIDGET(area))) gtk_gl_area_make_current(area);
}

void Backend::deactivate() {
    // GTK expects its area's context to stay current through "render": leave it
}

mln::gl::ProcAddress Backend::getExtensionFunctionPointer(const char* name) {
    // GTK 4 drives GL through EGL on Wayland and X11 alike
    return reinterpret_cast<mln::gl::ProcAddress>(eglGetProcAddress(name));
}

void Backend::updateAssumedState() {
    // GTK uses this context too (it binds the area's framebuffer and allocates its textures), so the GL
    // state MapLibre cached from the last frame is not to be trusted: start every frame from scratch
    getContext().setDirtyState();
    assumeFramebufferBinding(fbo);
    assumeViewport(0, 0, getSize());
}

Frontend::Frontend(Backend& backend_, float pixelRatio, std::function<void()> invalidate_)
    : backend(backend_),
      renderer(std::make_unique<mln::Renderer>(backend_, pixelRatio)),
      invalidate(std::move(invalidate_)) {}

Frontend::~Frontend() {
    reset();
}

void Frontend::reset() {
    if (!renderer) return;
    // MapLibre frees its GL objects here: the context has to be current (Backend::activate)
    mln::gfx::BackendScope guard{backend, mln::gfx::BackendScope::ScopeType::Explicit};
    renderer.reset();
}

void Frontend::setObserver(mln::RendererObserver& observer) {
    if (renderer) renderer->setObserver(&observer);
}

void Frontend::update(std::shared_ptr<mln::UpdateParameters> params) {
    updateParameters = std::move(params);
    if (invalidate) invalidate();
}

const mln::TaggedScheduler& Frontend::getThreadPool() const {
    return backend.getThreadPool();
}

bool Frontend::render() {
    if (!renderer || !updateParameters) return false;
    mln::gfx::BackendScope guard{backend, mln::gfx::BackendScope::ScopeType::Implicit};
    // rendering can call back into the map (a missing style image), which may replace updateParameters:
    // hold this frame's copy until render() returns
    auto params = updateParameters;
    renderer->render(params);
    return true;
}

} // namespace navi
