// The MapLibre side of the widget: a renderer backend that draws into a GtkGLArea's framebuffer, and a
// renderer frontend that turns MapLibre's "something changed" into a GTK redraw.
#pragma once

#include <mln/gfx/renderable.hpp>
#include <mln/gl/renderer_backend.hpp>
#include <mln/renderer/renderer_frontend.hpp>

#include <gtk/gtk.h>

#include <functional>
#include <memory>

namespace mln {
class Renderer;
class RendererObserver;
class UpdateParameters;
} // namespace mln

namespace navi {

class Backend final : public mln::gl::RendererBackend, public mln::gfx::Renderable {
public:
    explicit Backend(GtkGLArea*);
    ~Backend() override;

    // the GL area's buffer, in device pixels
    void setFramebufferSize(mln::Size size) { setRenderableSize(size); }
    // GtkGLArea binds its own framebuffer object before "render": remember which one it is
    void beginFrame();
    uint32_t framebuffer() const { return fbo; }

    mln::gfx::Renderable& getDefaultRenderable() override { return *this; }

protected:
    void activate() override;
    void deactivate() override;
    mln::gl::ProcAddress getExtensionFunctionPointer(const char*) override;
    void updateAssumedState() override;

private:
    GtkGLArea* area;
    uint32_t fbo = 0;
};

class Frontend final : public mln::RendererFrontend {
public:
    // `invalidate` asks GTK for a frame
    Frontend(Backend&, float pixelRatio, std::function<void()> invalidate);
    ~Frontend() override;

    void reset() override;
    void setObserver(mln::RendererObserver&) override;
    void update(std::shared_ptr<mln::UpdateParameters>) override;
    const mln::TaggedScheduler& getThreadPool() const override;

    // draws the last update into the current GL framebuffer; false when there is nothing to draw yet
    bool render();
    mln::Renderer* getRenderer() { return renderer.get(); }

private:
    Backend& backend;
    std::unique_ptr<mln::Renderer> renderer;
    std::shared_ptr<mln::UpdateParameters> updateParameters;
    std::function<void()> invalidate;
};

} // namespace navi
