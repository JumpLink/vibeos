// MapLibre's main-thread run loop, driven by the GLib main loop.
//
// On Linux mln::util::RunLoop is a libuv loop: everything MapLibre's worker, file-source and render
// threads send to the main thread arrives through it (a uv_async eventfd), and its timers (style
// transitions, symbol fades, tile expiry) are uv timers. A libuv loop exposes one epoll descriptor that
// turns readable whenever any of its watchers has work, plus the time to its next timer, so a GSource
// can poll it like any other fd and run one non-blocking pass of the loop when it fires. Both loops keep
// their own semantics, nothing spins, and MapLibre's platform code stays untouched.
//
// (The alternative, MapLibre's Qt route, replaces RunLoop, Timer and AsyncTask with toolkit-native
// versions for every thread. That needs a patched MapLibre and buys nothing here: the worker threads run
// their own loops and never touch GLib.)
#include "navi-runloop.hpp"

#include <mln/util/run_loop.hpp>

#include <glib.h>
#include <uv.h>

namespace {

struct UvSource {
    GSource base;
    uv_loop_t* loop;
    gpointer fd_tag;
};

gboolean uv_source_prepare(GSource* source, gint* timeout) {
    auto* s = reinterpret_cast<UvSource*>(source);
    // the loop's clock only moves inside uv_run: without this a timer computed from a stale time fires late
    uv_update_time(s->loop);
    int t = uv_backend_timeout(s->loop);
    *timeout = t;
    return t == 0; // 0: pending callbacks, closing handles or a due timer: run now
}

gboolean uv_source_check(GSource* source) {
    auto* s = reinterpret_cast<UvSource*>(source);
    if (g_source_query_unix_fd(source, s->fd_tag) & (G_IO_IN | G_IO_ERR | G_IO_HUP)) return TRUE;
    uv_update_time(s->loop);
    return uv_backend_timeout(s->loop) == 0;
}

gboolean uv_source_dispatch(GSource* source, GSourceFunc, gpointer) {
    auto* s = reinterpret_cast<UvSource*>(source);
    uv_run(s->loop, UV_RUN_NOWAIT);
    return G_SOURCE_CONTINUE;
}

GSourceFuncs uv_source_funcs = {uv_source_prepare, uv_source_check, uv_source_dispatch, nullptr, nullptr, nullptr};

mln::util::RunLoop* main_loop = nullptr;

} // namespace

namespace navi {

void ensure_main_run_loop() {
    if (main_loop) return;
    // a loop of its own (not uv_default_loop): nothing else in the process can run it behind our back
    main_loop = new mln::util::RunLoop(mln::util::RunLoop::Type::New);
    auto* loop = static_cast<uv_loop_t*>(mln::util::RunLoop::getLoopHandle());

    // one pass now: libuv adds descriptors created outside uv_run (the RunLoop's async eventfd) to its
    // epoll set only at the start of a pass, and until then the backend fd would never turn readable
    uv_run(loop, UV_RUN_NOWAIT);

    GSource* source = g_source_new(&uv_source_funcs, sizeof(UvSource));
    auto* s = reinterpret_cast<UvSource*>(source);
    s->loop = loop;
    s->fd_tag = g_source_add_unix_fd(source, uv_backend_fd(loop), G_IO_IN);
    g_source_set_name(source, "MapLibre run loop");
    // GDK's own event priority, above its redraws: tile results and style transitions feed the next frame
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_attach(source, nullptr);
    g_source_unref(source); // the main context keeps it for the life of the process, as the RunLoop
}

} // namespace navi
