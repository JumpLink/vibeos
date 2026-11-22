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
    void setFramebufferSize(mln::Size size);
    // Below 1, MapLibre draws into a buffer of its own at this fraction of the GL area's size, which
    // endFrame() stretches onto the GL area's buffer: fewer pixels to fill for a GPU that cannot fill
    // the whole display at the display's rate.
    void setRenderScale(double scale);
    double getRenderScale() const { return renderScale; }
    // MapLibre draws into a buffer of its own at full size too: needed to draw a frame ahead, outside
    // GtkGLArea's "render", and show it in a later one
    void setOffscreen(bool on) { offscreen = on; }
    bool usesOffscreen() const { return offscreen || renderScale < 1; }
    // GtkGLArea binds its own framebuffer object before "render": remember which one it is, and bind
    // the one MapLibre draws into
    void beginFrame();
    // after MapLibre's frame (drew = false: it had nothing yet, so the GL area gets a plain colour)
    void endFrame(bool drew);
    // inside "render": the frame drawn ahead (beginFrame() and MapLibre's render outside "render") onto
    // the GL area's framebuffer; false when there is none at the area's present size
    bool presentAhead();
    uint32_t framebuffer() const { return fbo; }

    mln::gfx::Renderable& getDefaultRenderable() override { return *this; }

protected:
    void activate() override;
    void deactivate() override;
    mln::gl::ProcAddress getExtensionFunctionPointer(const char*) override;
    void updateAssumedState() override;

private:
    void releaseOffscreen();
    void blitOffscreen();

    GtkGLArea* area;
    uint32_t fbo = 0;         // where MapLibre draws this frame
    uint32_t areaFbo = 0;     // the GL area's
    mln::Size areaSize{1, 1}; // and its size
    double renderScale = 1;
    bool offscreen = false;
    // MapLibre's own buffer: a framebuffer with colour and depth-stencil renderbuffers
    uint32_t offFbo = 0, offColor = 0, offDepth = 0;
    mln::Size offSize{0, 0};      // its size
    mln::Size offAreaSize{0, 0};  // the GL area's size when it was last drawn
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
