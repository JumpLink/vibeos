#include "navi-backend.hpp"

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gl/renderable_resource.hpp>
#include <mln/renderer/renderer.hpp>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

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

Backend::~Backend() = default;

void Backend::beginFrame() {
    GLint bound = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    fbo = static_cast<uint32_t>(bound);
    // GtkGLArea binds its framebuffer but leaves the viewport at whatever it was (0x0 on a new context),
    // and updateAssumedState() tells MapLibre the viewport is already the full buffer: make that true,
    // or every draw lands in an empty viewport while clears still fill the buffer
    glViewport(0, 0, static_cast<GLsizei>(getSize().width), static_cast<GLsizei>(getSize().height));
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
