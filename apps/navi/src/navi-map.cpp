// NaviMap: the GtkGLArea subclass, its C API and its gestures. MapLibre's objects (backend, frontend,
// map) live from realize to unrealize, when there is a GL context; everything the app set (style, its
// own sources and layers, camera, insets) is kept here and replayed onto a new map.
#include "navi-map.h"
#include "navi-backend.hpp"
#include "navi-runloop.hpp"

#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/mode.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/conversion/layer.hpp>
#include <mln/style/image.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/sources/tile_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/tile_server_options.hpp>
#include <mln/util/unitbezier.hpp>

#include <GLES3/gl3.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---- MapLibre's log ----

// MapLibre logs a tile it could not load by its z/x/y (in the message and the URL): where the map was. Outside a
// debug build those become "z/x/y", and the rest of the message stays.
std::string scrub_tiles(const std::string& msg) {
#ifdef NAVI_DEBUG
    return msg;
#else
    std::string out;
    size_t i = 0;
    while (i < msg.size()) {
        // a run of digits, '/', digits, '/', digits
        size_t j = i, parts = 0;
        while (j < msg.size()) {
            size_t k = j;
            while (k < msg.size() && g_ascii_isdigit(msg[k])) k++;
            if (k == j) break;
            parts++;
            j = k;
            if (parts == 3 || j >= msg.size() || msg[j] != '/') break;
            j++;
        }
        if (parts == 3 && (i == 0 || !g_ascii_isdigit(msg[i - 1]))) {
            out += "z/x/y";
            i = j;
        } else {
            out += msg[i++];
        }
    }
    return out;
#endif
}

class NaviLog : public mln::Log::Observer {
    bool onRecord(mln::EventSeverity severity, mln::Event event, int64_t code, const std::string& msg) override {
        const char* level = severity == mln::EventSeverity::Error     ? "error"
                            : severity == mln::EventSeverity::Warning ? "warning"
                                                                      : "info";
        if (severity == mln::EventSeverity::Debug) return true;
        g_printerr("maplibre %s (%d): %s\n", level, int(event), scrub_tiles(msg).c_str());
        return true;
    }
};

// ---- Web Mercator in [0, 1] world units: panning is done on the ground plane in these ----

struct Merc {
    double x, y;
};

Merc to_merc(const mln::LatLng& ll) {
    double lat = std::clamp(ll.latitude(), -85.05112878, 85.05112878) * M_PI / 180.0;
    return {(ll.longitude() + 180.0) / 360.0, 0.5 - std::log(std::tan(M_PI / 4 + lat / 2)) / (2 * M_PI)};
}

mln::LatLng from_merc(Merc m) {
    double lat = (2 * std::atan(std::exp((0.5 - m.y) * 2 * M_PI)) - M_PI / 2) * 180.0 / M_PI;
    return mln::LatLng(std::clamp(lat, -85.05112878, 85.05112878), m.x * 360.0 - 180.0);
}

// world size in widget pixels at a zoom (MapLibre's tiles are 512 px)
double world_px(double zoom) {
    return 512.0 * std::exp2(zoom);
}

mln::util::UnitBezier easing_curve(NaviEasing e) {
    switch (e) {
        case NAVI_EASING_LINEAR: return {0, 0, 1, 1};
        case NAVI_EASING_EASE: return {0.25, 0.1, 0.25, 1};
        case NAVI_EASING_EASE_IN: return {0.42, 0, 1, 1};
        case NAVI_EASING_EASE_OUT: return {0, 0, 0.58, 1};
        case NAVI_EASING_EASE_IN_OUT: return {0.42, 0, 0.58, 1};
        case NAVI_EASING_EASE_OUT_CUBIC: return {0.33, 1, 0.68, 1};
        case NAVI_EASING_DEFAULT:
        default: return {0, 0, 0.25, 1};
    }
}

enum {
    SIGNAL_CAMERA_CHANGED,
    SIGNAL_USER_MOVED,
    SIGNAL_DOUBLE_TAP,
    SIGNAL_STYLE_LOADED,
    SIGNAL_LOAD_ERROR,
    SIGNAL_FRAME_RENDERED,
    N_SIGNALS
};
guint signals[N_SIGNALS];

constexpr double PAN_SLOP = 6;           // px a finger moves before a press becomes a pan
constexpr double FLING_MIN = 250;        // px/s at release below which the map just stops
constexpr double FLING_MAX = 6000;       // px/s
constexpr double FLING_SECONDS = 0.3;    // a fling coasts this long, slowing to a stop
constexpr double ROTATE_THRESHOLD = 10;  // degrees of twist before two fingers start turning the map
constexpr double TWO_FINGER_SLOP = 10;   // px (and 6 %) the two-finger centre/spread moves before it counts
constexpr gint64 TWO_FINGER_TAP_US = 300000;

} // namespace

struct NaviMapImpl;

struct _NaviMap {
    GtkGLArea parent_instance;
    NaviMapImpl* impl;
};

G_DEFINE_FINAL_TYPE(NaviMap, navi_map, GTK_TYPE_GL_AREA)
G_DEFINE_QUARK(navi-map-error-quark, navi_map_error)

struct NaviMapImpl final : public mln::MapObserver {
    NaviMap* self;
    std::unique_ptr<navi::Backend> backend;
    std::unique_ptr<navi::Frontend> frontend;
    std::unique_ptr<mln::Map> map;
    float pixel_ratio = 1;    // device pixels MapLibre draws per widget pixel: the display's scale x render_scale
    double render_scale = 1;
    std::string cache_path;

    // the style and what the app added to it, replayed after every style load
    enum class StyleKind { None, Url, Json } style_kind = StyleKind::None;
    std::string style;
    bool style_loaded = false;

    // Style layers taken out while their tile source's bounds are far outside the view. A style with one
    // source per region repeats every layer per region, and MapLibre spends time on every layer every
    // frame, with tiles or without, even hidden: 1300 layers cost 12 ms a frame on an SDM670, the same
    // layers with all but one region's hidden 9.7 ms, the 28 of one region 4 ms.
    std::vector<std::string> style_order; // the style's own layers, bottom to top, as it loaded
    std::vector<std::string> culled_sources;
    std::vector<std::pair<size_t, std::unique_ptr<mln::style::Layer>>> parked; // index in style_order
    bool cull = g_strcmp0(g_getenv("NAVI_MAP_NO_CULL"), "1") != 0; // NAVI_MAP_NO_CULL=1: a measuring aid
    struct SourceRec {
        std::string id;
        std::optional<mln::GeoJSON> data;
    };
    std::vector<SourceRec> sources;
    struct LayerRec {
        std::string id, json, before;
    };
    std::vector<LayerRec> layers;
    struct ImageRec {
        std::string id;
        mln::PremultipliedImage image;
        float pixel_ratio;
    };
    std::vector<ImageRec> images;

    // camera state kept while there is no map
    mln::CameraOptions camera;
    double max_pitch = 60, min_zoom = 0, max_zoom = 22;
    // tile level of detail (MapLibre's defaults: 3 tiles, 1, 60 degrees, 0)
    double lod_radius = 3, lod_scale = 1, lod_pitch = 60, lod_zoom_shift = 0;
    bool use_focus = false;
    double focus_x = 0.5, focus_y = 0.5;
    mln::EdgeInsets padding;
    // the camera move under way (easeTo, flyTo): a new padding (the widget resized) stops it, and it goes on
    std::optional<mln::CameraOptions> moving_to;
    gint64 moving_until = 0;
    bool moving_fly = false;

    guint camera_tick = 0;

    // MapLibre asks for its next frame from inside render() (an animation, a fade, tiles arriving), and
    // GtkGLArea clears its "needs render" flag only after "render" returns, which would swallow that
    // request: it is held here and re-issued at the next frame clock tick
    bool in_render = false, render_again = false;
    guint render_tick = 0;

    // Frame pacing. With pace N > 1 the map shows a new frame every N display refreshes, so a map that
    // cannot make every refresh moves at an even rhythm instead of an uneven mix of one and two. Each
    // frame is drawn a cycle ahead, into the backend's own buffer, in the frame clock's layout phase
    // (after every tick callback has moved the camera), and shown in the next cycle, where it costs the
    // GPU one copy: how long the GPU took over it no longer decides which refresh it makes. Drawn and
    // shown in one cycle, a frame that takes about one refresh lands on the first or the second at
    // random. (Drawn straight after the cycle that showed the one before, its GPU work holds up the
    // compositor's frame instead: more frames come late.) Auto (0) starts at one frame per refresh up to 60 Hz and two on a 120 Hz display, and
    // goes one slower when over two seconds of drawing more than a quarter of the frames come late.
    int pace_setting = 0, pace = 1;
    bool pace_known = false;      // auto: the display's rate seen
    bool paced_pending = false;   // MapLibre wants a frame
    bool ahead = false;           // a frame drawn ahead waits to be shown
    bool presenting = false;      // the next "render" shows it
    bool in_layout = false, want_cycle = false;
    gint64 last_present_us = 0;   // the frame clock's time of the cycle that last showed a frame
    GdkFrameClock* clock = nullptr;
    gulong clock_layout = 0, clock_after_paint = 0;
    // auto pacing's measure: the frame counters of the cycles that showed a map frame, not yet presented
    std::vector<gint64> shown_frames;
    gint64 last_shown_us = 0, pace_measure_from = 0;
    int pace_frames = 0, pace_late = 0, pace_late_windows = 0;

    // gestures
    bool gestures_enabled = true;
    GtkGesture *drag = nullptr, *pinch = nullptr, *click = nullptr;
    int active_gestures = 0;
    bool moved_this_gesture = false;
    // one finger
    bool pan_started = false;
    double drag_x0 = 0, drag_y0 = 0, drag_px = 0, drag_py = 0;
    struct Sample {
        gint64 t;
        Merc c;
    };
    std::vector<Sample> samples;
    // two fingers
    enum class Two { Off, Undecided, Transform } two = Two::Off;
    GdkEventSequence *seq_a = nullptr, *seq_b = nullptr;
    double c0x = 0, c0y = 0, d0 = 1, a0 = 0, pcx = 0, pcy = 0, pd = 1, pa = 0;
    gint64 two_t0 = 0;
    bool rotating = false;
    // fling
    guint fling_tick = 0;
    gint64 fling_t0 = 0;
    Merc fling_from{}, fling_by{};
    // pointer (scroll-wheel zoom anchor)
    double pointer_x = -1, pointer_y = -1;

    explicit NaviMapImpl(NaviMap* self_)
        : self(self_) {
        const char* dir = g_get_user_cache_dir();
        cache_path = std::string(dir) + "/vibeos-navi/map-cache.db";
    }

    GtkWidget* widget() { return GTK_WIDGET(self); }

    void request_frame() {
        if (in_render)
            render_again = true;
        else if (pace > 1 && clock)
            request_paced();
        else
            gtk_gl_area_queue_render(GTK_GL_AREA(self));
    }

    void request_paced() {
        paced_pending = true;
        next_cycle();
    }

    // the layout phase of this cycle, or of the next when this one's is under way (a layout phase that
    // asks for itself again runs again in the same cycle, and GDK gives up after four)
    void next_cycle() {
        if (in_layout)
            want_cycle = true;
        else
            gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_LAYOUT);
    }

    // the display's refresh period, from the frames presented so far (the current frame's own timings
    // do not have it yet; GDK assumes 60 Hz until a frame has been presented)
    static gint64 refresh_us(GdkFrameClock* c) {
        gint64 r = 0, presented = 0;
        gdk_frame_clock_get_refresh_info(c, gdk_frame_clock_get_frame_time(c), &r, &presented);
        return r > 0 ? r : 16667;
    }

    void on_clock_layout() {
        if (!paced_pending && !ahead) return;
        if (pace < 2) { // pacing went off with a frame waiting
            paced_pending = ahead = false;
            gtk_gl_area_queue_render(GTK_GL_AREA(self));
            return;
        }
        in_layout = true;
        gint64 now = gdk_frame_clock_get_frame_time(clock);
        double since = double(now - last_present_us) / refresh_us(clock); // refreshes since a frame was shown
        if (ahead) {
            if (since < pace - 0.5) {
                want_cycle = true;
            } else { // show it: GtkGLArea's "render" in this cycle's paint phase copies it over
                ahead = false;
                presenting = true;
                last_present_us = now;
                gtk_gl_area_queue_render(GTK_GL_AREA(self));
                if (paced_pending) want_cycle = true;
            }
        } else if (since < pace - 1.5) {
            want_cycle = true;
        } else { // the cycle before the one that shows it
            paced_pending = false;
            draw_ahead();
            if (ahead) want_cycle = true;
        }
        in_layout = false;
    }

    void on_clock_after_paint() {
        if (!want_cycle) return;
        want_cycle = false;
        gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_LAYOUT);
    }

    void draw_ahead() {
        if (!frontend || !gtk_widget_get_mapped(widget())) return;
        GtkGLArea* area = GTK_GL_AREA(self);
        gtk_gl_area_make_current(area);
        if (gtk_gl_area_get_error(area)) return;
        gint64 t0 = g_get_monotonic_time();
        backend->beginFrame();
        in_render = true;
        bool drew = frontend->render();
        after_render();
        if (!drew) return;
        glFlush(); // the GPU starts on it now, a cycle before it is shown
        ahead = true;
        g_signal_emit(self, signals[SIGNAL_FRAME_RENDERED], 0, (g_get_monotonic_time() - t0) / 1000.0);
    }

    // a map frame goes to the screen in this cycle (auto pacing measures when it got there)
    void note_shown() {
        if (!clock || pace_setting != 0) return;
        if (shown_frames.size() >= 32) shown_frames.erase(shown_frames.begin());
        shown_frames.push_back(gdk_frame_clock_get_frame_counter(clock));
    }

    void check_pacing() {
        if (!clock || pace_setting != 0) return;
        gint64 history = gdk_frame_clock_get_history_start(clock);
        size_t done = 0;
        for (; done < shown_frames.size(); done++) {
            if (shown_frames[done] < history) continue; // its timings are gone
            GdkFrameTimings* t = gdk_frame_clock_get_timings(clock, shown_frames[done]);
            if (!t || !gdk_frame_timings_get_complete(t)) break;
            gint64 shown = gdk_frame_timings_get_presentation_time(t), r = gdk_frame_timings_get_refresh_interval(t);
            if (!shown || r <= 0) continue;
            if (!pace_known) { // the display's rate: one frame a refresh up to about 60 Hz, two at 120
                pace_known = true;
                set_pace(std::max(1, static_cast<int>(1e6 / r / 60 + 0.25)));
                pace_measure_from = shown + 3 * G_USEC_PER_SEC; // not while the map starts up
            }
            double gap = double(shown - last_shown_us) / r;
            if (last_shown_us && shown >= pace_measure_from && gap < 2.5 * pace + 0.5) { // longer: a pause
                pace_frames++;
                if (gap > pace + 0.5) pace_late++;
            }
            last_shown_us = shown;
        }
        shown_frames.erase(shown_frames.begin(), shown_frames.begin() + done);
        if (pace_frames < 120 / pace) return;
        pace_late_windows = pace_late * 4 > pace_frames ? pace_late_windows + 1 : 0;
        pace_frames = pace_late = 0;
        if (pace_late_windows >= 2 && pace < 4) {
            g_message("NaviMap: frames keep missing every %s refresh; drawing one every %d",
                      pace == 1 ? "single" : pace == 2 ? "second" : "third", pace + 1);
            set_pace(pace + 1);
            pace_late_windows = 0;
        }
    }

    void set_pace(int n) {
        pace = n;
        if (backend) backend->setOffscreen(pace > 1);
        update_area_buffers();
    }

    // MapLibre needs depth and stencil buffers: the GL area's own when it draws there, none when it
    // draws into the backend's
    void update_area_buffers() {
        bool own = render_scale >= 1 && pace < 2;
        GtkGLArea* area = GTK_GL_AREA(self);
        if (gtk_gl_area_get_has_depth_buffer(area) != own) gtk_gl_area_set_has_depth_buffer(area, own);
        if (gtk_gl_area_get_has_stencil_buffer(area) != own) gtk_gl_area_set_has_stencil_buffer(area, own);
    }

    void attach_clock() {
        clock = gtk_widget_get_frame_clock(widget());
        if (!clock) return;
        g_object_ref(clock);
        clock_layout = g_signal_connect_swapped(clock, "layout", G_CALLBACK(+[](NaviMapImpl* impl) {
                                                    impl->on_clock_layout();
                                                }),
                                                this);
        clock_after_paint = g_signal_connect_swapped(clock, "after-paint", G_CALLBACK(+[](NaviMapImpl* impl) {
                                                         impl->on_clock_after_paint();
                                                     }),
                                                     this);
    }

    void detach_clock() {
        if (!clock) return;
        g_signal_handler_disconnect(clock, clock_layout);
        g_signal_handler_disconnect(clock, clock_after_paint);
        g_clear_object(&clock);
        clock_layout = clock_after_paint = 0;
        paced_pending = ahead = presenting = want_cycle = false;
        shown_frames.clear();
        last_shown_us = 0;
    }

    void after_render() {
        in_render = false;
        if (!render_again) return;
        if (pace > 1 && clock) {
            render_again = false;
            request_paced();
            return;
        }
        if (render_tick) return;
        render_again = false;
        render_tick = gtk_widget_add_tick_callback(
            widget(),
            [](GtkWidget* w, GdkFrameClock*, gpointer) -> gboolean {
                NAVI_MAP(w)->impl->render_tick = 0;
                gtk_gl_area_queue_render(GTK_GL_AREA(w));
                return G_SOURCE_REMOVE;
            },
            nullptr, nullptr);
    }

    // ---- map lifetime ----

    void create_map() {
        GtkWidget* w = widget();
        GtkNative* native = gtk_widget_get_native(w);
        GdkSurface* surface = native ? gtk_native_get_surface(native) : nullptr;
        pixel_ratio = static_cast<float>((surface ? gdk_surface_get_scale(surface) : 1.0) * render_scale);
        uint32_t width = std::max(1, gtk_widget_get_width(w));
        uint32_t height = std::max(1, gtk_widget_get_height(w));

        gchar* dir = g_path_get_dirname(cache_path.c_str());
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);

        backend = std::make_unique<navi::Backend>(GTK_GL_AREA(w));
        backend->setRenderScale(render_scale);
        backend->setOffscreen(pace > 1);
        double scale = surface ? gdk_surface_get_scale(surface) : 1.0;
        backend->setFramebufferSize({static_cast<uint32_t>(std::ceil(width * scale)),
                                     static_cast<uint32_t>(std::ceil(height * scale))});
        frontend = std::make_unique<navi::Frontend>(*backend, pixel_ratio, [this] { request_frame(); });

        mln::ResourceOptions resources;
        resources.withCachePath(cache_path)
            .withMaximumCacheSize(128 * 1024 * 1024)
            .withTileServerOptions(mln::TileServerOptions::MapLibreConfiguration());
        mln::MapOptions options;
        options.withMapMode(mln::MapMode::Continuous).withSize({width, height}).withPixelRatio(pixel_ratio);
        map = std::make_unique<mln::Map>(*frontend, *this, options, resources,
                                         mln::ClientOptions().withName("vibeos-navi").withVersion("0.1"));
        apply_bounds();
        apply_tile_lod();
        update_padding();
        mln::CameraOptions start = camera;
        start.padding = padding;
        map->jumpTo(start);
        load_style();
    }

    void destroy_map() {
        if (!map) return;
        stop_fling();
        if (render_tick) gtk_widget_remove_tick_callback(widget(), render_tick);
        render_tick = 0;
        camera = map->getCameraOptions();
        camera.padding.reset();
        map.reset(); // resets the frontend's renderer with the context current
        frontend.reset();
        backend.reset();
        style_loaded = false;
        forget_culled();
    }

    void apply_tile_lod() {
        if (!map) return;
        map->setTileLodMinRadius(lod_radius);
        map->setTileLodScale(lod_scale);
        map->setTileLodPitchThreshold(lod_pitch * M_PI / 180);
        map->setTileLodZoomShift(lod_zoom_shift);
    }

    void apply_bounds() {
        if (!map) return;
        map->setBounds(mln::BoundOptions().withMaxPitch(max_pitch).withMinZoom(min_zoom).withMaxZoom(max_zoom));
    }

    void update_padding() {
        if (use_focus) {
            double w = gtk_widget_get_width(widget()), h = gtk_widget_get_height(widget());
            double dx = (2 * focus_x - 1) * w, dy = (2 * focus_y - 1) * h;
            padding = mln::EdgeInsets(std::max(dy, 0.0), std::max(dx, 0.0), std::max(-dy, 0.0), std::max(-dx, 0.0));
        }
        if (!map) return;
        map->jumpTo(mln::CameraOptions().withPadding(padding));
        // the keyboard leaving resizes the window while a place is being flown to: the move goes on from here
        gint64 left = moving_until - g_get_monotonic_time();
        if (moving_to && left > 0) {
            mln::AnimationOptions anim{std::chrono::milliseconds(left / 1000)};
            if (moving_fly)
                map->flyTo(*moving_to, anim);
            else
                map->easeTo(*moving_to, anim);
        }
    }

    void moving(const mln::CameraOptions& to, guint ms, bool fly) {
        moving_to = to;
        moving_until = g_get_monotonic_time() + gint64(ms) * 1000;
        moving_fly = fly;
    }

    void stopped_moving() {
        moving_to.reset();
        moving_until = 0;
    }

    void load_style() {
        if (!map) return;
        style_loaded = false;
        forget_culled();
        try {
            if (style_kind == StyleKind::Url)
                map->getStyle().loadURL(style);
            else if (style_kind == StyleKind::Json)
                map->getStyle().loadJSON(style);
        } catch (const std::exception& e) {
            g_signal_emit(self, signals[SIGNAL_LOAD_ERROR], 0, e.what());
        }
    }

    // ---- the app's sources and layers ----

    mln::style::GeoJSONSource* style_geojson_source(const std::string& id) {
        if (!map || !style_loaded) return nullptr;
        mln::style::Source* source = map->getStyle().getSource(id);
        return source ? source->as<mln::style::GeoJSONSource>() : nullptr;
    }

    void push_source(const SourceRec& rec) {
        if (!map || !style_loaded) return;
        mln::style::Style& st = map->getStyle();
        if (mln::style::Source* existing = st.getSource(rec.id)) {
            if (auto* gj = existing->as<mln::style::GeoJSONSource>(); gj && rec.data) gj->setGeoJSON(*rec.data);
            return;
        }
        auto source = std::make_unique<mln::style::GeoJSONSource>(rec.id);
        source->setGeoJSON(rec.data ? *rec.data : mln::GeoJSON{mapbox::geojson::feature_collection{}});
        st.addSource(std::move(source));
    }

    bool push_layer(const LayerRec& rec, GError** error) {
        if (!map || !style_loaded) return true;
        mln::style::conversion::Error err;
        auto layer = mln::style::conversion::convertJSON<std::unique_ptr<mln::style::Layer>>(rec.json, err);
        if (!layer) {
            g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_PARSE, "layer: %s", err.message.c_str());
            return false;
        }
        try {
            map->getStyle().addLayer(std::move(*layer), rec.before.empty() ? std::nullopt
                                                                          : std::optional<std::string>(rec.before));
        } catch (const std::exception& e) {
            g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_STYLE, "layer %s: %s", rec.id.c_str(), e.what());
            return false;
        }
        return true;
    }

    void push_image(const ImageRec& rec) {
        if (!map || !style_loaded) return;
        map->getStyle().addImage(std::make_unique<mln::style::Image>(rec.id, rec.image.clone(), rec.pixel_ratio));
    }

    SourceRec* find_source(const std::string& id) {
        for (auto& s : sources)
            if (s.id == id) return &s;
        return nullptr;
    }

    bool set_data(const char* id, mln::GeoJSON data, GError** error) {
        SourceRec* rec = find_source(id);
        if (!rec) {
            // a GeoJSON source the style itself defines: take it over so its data survives a reload
            if (style_loaded && !style_geojson_source(id)) {
                g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_NOT_FOUND, "no GeoJSON source \"%s\"", id);
                return false;
            }
            sources.push_back({id, std::nullopt});
            rec = &sources.back();
        }
        rec->data = std::move(data);
        if (auto* gj = style_geojson_source(id))
            gj->setGeoJSON(*rec->data);
        else
            push_source(*rec);
        return true;
    }

    // ---- camera ----

    mln::CameraOptions current_camera() const { return map ? map->getCameraOptions() : camera; }

    std::optional<mln::CameraOptions> to_options(const NaviCamera* c) {
        mln::CameraOptions o;
        try {
            if (!std::isnan(c->latitude) || !std::isnan(c->longitude)) {
                mln::LatLng now = current_camera().center.value_or(mln::LatLng(0, 0));
                o.center = mln::LatLng(std::isnan(c->latitude) ? now.latitude() : c->latitude,
                                       std::isnan(c->longitude) ? now.longitude() : c->longitude);
            }
        } catch (const std::exception& e) {
            g_warning("NaviMap: bad coordinate (%g, %g): %s", c->latitude, c->longitude, e.what());
            return std::nullopt;
        }
        if (!std::isnan(c->zoom)) o.zoom = std::clamp(c->zoom, min_zoom, max_zoom);
        if (!std::isnan(c->bearing)) o.bearing = c->bearing;
        if (!std::isnan(c->pitch)) o.pitch = std::clamp(c->pitch, 0.0, max_pitch);
        return o;
    }

    void merge_pending(const mln::CameraOptions& o) {
        if (o.center) camera.center = o.center;
        if (o.zoom) camera.zoom = o.zoom;
        if (o.bearing) camera.bearing = o.bearing;
        if (o.pitch) camera.pitch = o.pitch;
    }

    // takes out the style layers of tile sources whose bounds do not reach the view (with a margin of the
    // view's own size, at least 0.1 degrees, so they are back before they come into sight), and puts
    // them back where they were when they do; layers added through the API are never taken out
    void cull_sources() {
        if (!cull || !map || !style_loaded) return;
        mln::LatLngBounds view = map->latLngBoundsForCamera(map->getCameraOptions());
        double dlat = std::max(view.north() - view.south(), 0.1), dlng = std::max(view.east() - view.west(), 0.1);
        mln::LatLngBounds near = mln::LatLngBounds::hull(
            mln::LatLng(std::max(view.south() - dlat, -90.0), view.west() - dlng),
            mln::LatLng(std::min(view.north() + dlat, 90.0), view.east() + dlng));
        auto& style = map->getStyle();
        std::vector<std::string> far;
        for (const mln::style::Source* src : std::as_const(style).getSources()) {
            mln::style::SourceType t = src->getType();
            if (t != mln::style::SourceType::Vector && t != mln::style::SourceType::Raster &&
                t != mln::style::SourceType::RasterDEM)
                continue;
            const auto& u = static_cast<const mln::style::TileSource*>(src)->getURLOrTileset();
            if (!u.is<mln::Tileset>()) continue; // a TileJSON URL: its bounds are not known here
            const auto& bounds = u.get<mln::Tileset>().bounds;
            if (bounds && !bounds->intersects(near)) far.push_back(src->getID());
        }
        std::sort(far.begin(), far.end());
        if (far == culled_sources) return;
        culled_sources = std::move(far);
        auto is_far = [&](const std::string& id) {
            return std::binary_search(culled_sources.begin(), culled_sources.end(), id);
        };
        std::sort(parked.begin(), parked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        decltype(parked) still;
        for (auto& p : parked) { // back in reach, bottom to top
            if (is_far(p.second->getSourceID()))
                still.push_back(std::move(p));
            else
                put_back(p.first, std::move(p.second));
        }
        parked = std::move(still);
        for (size_t i = 0; i < style_order.size(); i++) { // newly out of reach
            mln::style::Layer* layer = style.getLayer(style_order[i]);
            if (!layer || !is_far(layer->getSourceID())) continue;
            if (auto removed = style.removeLayer(style_order[i])) parked.emplace_back(i, std::move(removed));
        }
    }

    // a style layer back in, just above the nearest style layer below it that is in the style
    void put_back(size_t index, std::unique_ptr<mln::style::Layer> layer) {
        auto& style = map->getStyle();
        std::vector<mln::style::Layer*> now = style.getLayers();
        std::optional<std::string> before;
        if (!now.empty()) before = now.front()->getID();
        for (size_t j = index; j-- > 0;) {
            if (!style.getLayer(style_order[j])) continue;
            auto it = std::find_if(now.begin(), now.end(), [&](auto* l) { return l->getID() == style_order[j]; });
            if (it != now.end()) before = it + 1 == now.end() ? std::nullopt : std::optional((*(it + 1))->getID());
            break;
        }
        try {
            style.addLayer(std::move(layer), before);
        } catch (const std::exception& e) {
            g_warning("NaviMap: layer %s: %s", style_order[index].c_str(), e.what());
        }
    }

    // every style layer back in (the layer calls of the API see the whole style)
    void restore_culled() {
        if (!map || !style_loaded || parked.empty()) return;
        std::sort(parked.begin(), parked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& p : parked) put_back(p.first, std::move(p.second));
        parked.clear();
        culled_sources.clear(); // the next camera change takes them out again
    }

    void forget_culled() {
        style_order.clear();
        culled_sources.clear();
        parked.clear();
    }

    void schedule_camera_changed() {
        if (camera_tick) return;
        camera_tick = gtk_widget_add_tick_callback(
            widget(),
            [](GtkWidget* w, GdkFrameClock*, gpointer) -> gboolean {
                NaviMap* m = NAVI_MAP(w);
                m->impl->camera_tick = 0;
                m->impl->cull_sources();
                g_signal_emit(m, signals[SIGNAL_CAMERA_CHANGED], 0);
                return G_SOURCE_REMOVE;
            },
            nullptr, nullptr);
    }

    // ---- gestures ----

    void gesture_begin() {
        if (active_gestures++ == 0) {
            moved_this_gesture = false;
            stop_fling();
            stopped_moving();
            if (map) {
                map->cancelTransitions();
                map->setGestureInProgress(true);
            }
        }
    }

    void gesture_end() {
        if (active_gestures > 0 && --active_gestures == 0 && map && !fling_tick) map->setGestureInProgress(false);
    }

    // the first camera change a gesture makes tells the app the user took over the map
    void note_moved() {
        if (moved_this_gesture) return;
        moved_this_gesture = true;
        g_signal_emit(self, signals[SIGNAL_USER_MOVED], 0);
    }

    // keeps the ground point under (fx, fy) under (tx, ty): with a tilted camera that is a move of the
    // center by the ground distance between the two points, which a flat map makes exact
    void pan_ground(double fx, double fy, double tx, double ty) {
        if (!map || (fx == tx && fy == ty)) return;
        mln::CameraOptions cam = map->getCameraOptions();
        if (!cam.center || !cam.zoom) return;
        Merc a = to_merc(map->latLngForPixel({fx, fy}, mln::LatLng::Unwrapped));
        Merc b = to_merc(map->latLngForPixel({tx, ty}, mln::LatLng::Unwrapped));
        Merc c = to_merc(*cam.center);
        double dx = a.x - b.x, dy = a.y - b.y;
        // near the horizon one pixel spans kilometres: cap a step at 4 screen sizes of the centre's scale
        double limit = 4 * std::max(gtk_widget_get_width(widget()), gtk_widget_get_height(widget())) /
                       world_px(*cam.zoom);
        double len = std::hypot(dx, dy);
        if (!std::isfinite(len)) return;
        if (len > limit) {
            dx *= limit / len;
            dy *= limit / len;
        }
        Merc n{c.x + dx, std::clamp(c.y + dy, 0.0, 1.0)};
        map->jumpTo(mln::CameraOptions().withCenter(from_merc(n)));
        note_moved();
    }

    void record_sample() {
        if (!map) return;
        mln::CameraOptions cam = map->getCameraOptions();
        if (!cam.center) return;
        gint64 now = g_get_monotonic_time();
        samples.push_back({now, to_merc(*cam.center)});
        while (samples.size() > 2 && now - samples.front().t > 120000) samples.erase(samples.begin());
    }

    void stop_fling() {
        if (fling_tick) {
            gtk_widget_remove_tick_callback(widget(), fling_tick);
            fling_tick = 0;
            if (map && active_gestures == 0) map->setGestureInProgress(false);
        }
    }

    // a release with speed coasts FLING_SECONDS on a cubic ease-out whose start speed is the finger's
    void start_fling() {
        if (!map || samples.size() < 2) return;
        gint64 now = g_get_monotonic_time();
        const Sample& last = samples.back();
        if (now - last.t > 60000) return; // the finger rested before lifting
        const Sample* first = &samples.front();
        double dt = (last.t - first->t) / 1e6;
        if (dt < 0.01) return;
        double zoom = map->getCameraOptions().zoom.value_or(0);
        double vx = (last.c.x - first->c.x) / dt, vy = (last.c.y - first->c.y) / dt; // world units per s
        double speed_px = std::hypot(vx, vy) * world_px(zoom);
        if (speed_px < FLING_MIN) return;
        if (speed_px > FLING_MAX) {
            vx *= FLING_MAX / speed_px;
            vy *= FLING_MAX / speed_px;
        }
        // distance of 1 - (1 - t)^3 over T at start speed v: v * T / 3
        fling_by = {vx * FLING_SECONDS / 3, vy * FLING_SECONDS / 3};
        fling_from = last.c;
        fling_t0 = 0;
        map->setGestureInProgress(true);
        fling_tick = gtk_widget_add_tick_callback(
            widget(),
            [](GtkWidget* w, GdkFrameClock* clock, gpointer) -> gboolean {
                NaviMapImpl* impl = NAVI_MAP(w)->impl;
                gint64 t = gdk_frame_clock_get_frame_time(clock);
                if (!impl->fling_t0) impl->fling_t0 = t;
                double k = std::min(1.0, (t - impl->fling_t0) / (FLING_SECONDS * 1e6));
                double e = 1 - std::pow(1 - k, 3);
                if (impl->map) {
                    Merc p{impl->fling_from.x + impl->fling_by.x * e,
                           std::clamp(impl->fling_from.y + impl->fling_by.y * e, 0.0, 1.0)};
                    impl->map->jumpTo(mln::CameraOptions().withCenter(from_merc(p)));
                }
                if (k < 1) return G_SOURCE_CONTINUE;
                impl->fling_tick = 0;
                if (impl->map && impl->active_gestures == 0) impl->map->setGestureInProgress(false);
                return G_SOURCE_REMOVE;
            },
            nullptr, nullptr);
    }

    void on_drag_begin(double x, double y) {
        gesture_begin();
        pan_started = false;
        drag_x0 = drag_px = x;
        drag_y0 = drag_py = y;
        samples.clear();
    }

    void on_drag_update(double ox, double oy) {
        double x = drag_x0 + ox, y = drag_y0 + oy;
        if (two != Two::Off) { // two fingers own the map; take over again from wherever this one is
            drag_px = x;
            drag_py = y;
            samples.clear();
            return;
        }
        if (!pan_started) {
            if (std::hypot(ox, oy) < PAN_SLOP) return;
            pan_started = true;
        }
        pan_ground(drag_px, drag_py, x, y);
        drag_px = x;
        drag_py = y;
        record_sample();
    }

    void on_drag_end() {
        if (pan_started && two == Two::Off) start_fling();
        samples.clear();
        gesture_end();
    }

    bool two_points(double* ax, double* ay, double* bx, double* by) {
        return gtk_gesture_get_point(pinch, seq_a, ax, ay) && gtk_gesture_get_point(pinch, seq_b, bx, by);
    }

    void on_pinch_begin() {
        GList* seqs = gtk_gesture_get_sequences(pinch);
        if (g_list_length(seqs) < 2) {
            g_list_free(seqs);
            return;
        }
        seq_a = static_cast<GdkEventSequence*>(seqs->data);
        seq_b = static_cast<GdkEventSequence*>(seqs->next->data);
        g_list_free(seqs);
        double ax, ay, bx, by;
        if (!two_points(&ax, &ay, &bx, &by)) return;
        gesture_begin();
        two = Two::Undecided;
        rotating = false;
        two_t0 = g_get_monotonic_time();
        c0x = pcx = (ax + bx) / 2;
        c0y = pcy = (ay + by) / 2;
        d0 = pd = std::max(1.0, std::hypot(bx - ax, by - ay));
        a0 = pa = std::atan2(by - ay, bx - ax);
    }

    static double angle_diff(double a, double b) { return std::remainder(a - b, 2 * M_PI); }

    // two fingers move, spread and twist together: the ground under their centre follows it, the zoom
    // follows their spread and, past a small twist, the bearing follows their angle around that centre.
    // A two-finger vertical drag is just a pan: the tilt stays where the app put it.
    void on_pinch_update() {
        if (two == Two::Off || !map) return;
        double ax, ay, bx, by;
        if (!two_points(&ax, &ay, &bx, &by)) return;
        double cx = (ax + bx) / 2, cy = (ay + by) / 2;
        double d = std::max(1.0, std::hypot(bx - ax, by - ay));
        double a = std::atan2(by - ay, bx - ax);

        if (two == Two::Undecided) {
            bool moved = std::hypot(cx - c0x, cy - c0y) > TWO_FINGER_SLOP;
            bool spread = std::abs(d / d0 - 1) > 0.06 && std::abs(d - d0) > TWO_FINGER_SLOP;
            bool twisted = std::abs(angle_diff(a, a0)) * 180 / M_PI > ROTATE_THRESHOLD;
            if (!moved && !spread && !twisted) return;
            two = Two::Transform; // apply everything since the fingers landed: nothing is lost to the slop
        }
        pan_ground(pcx, pcy, cx, cy);
        if (d != pd) map->scaleBy(d / pd, mln::ScreenCoordinate{cx, cy});
        if (!rotating && std::abs(angle_diff(a, a0)) * 180 / M_PI > ROTATE_THRESHOLD) {
            rotating = true;
            pa = a; // start turning from here, so the map does not jump by the threshold
        }
        if (rotating) {
            double bearing = map->getCameraOptions().bearing.value_or(0) - angle_diff(a, pa) * 180 / M_PI;
            map->jumpTo(mln::CameraOptions().withBearing(bearing).withAnchor(mln::ScreenCoordinate{cx, cy}));
        }
        note_moved();
        pcx = cx;
        pcy = cy;
        pd = d;
        pa = a;
    }

    void on_pinch_end() {
        if (two == Two::Off) return;
        bool tap = two == Two::Undecided && g_get_monotonic_time() - two_t0 < TWO_FINGER_TAP_US;
        two = Two::Off;
        seq_a = seq_b = nullptr;
        gesture_end();
        if (tap && map) {
            // a two-finger tap zooms out one step around the focus point: fast start, long tail
            note_moved();
            stopped_moving();
            double zoom = map->getCameraOptions().zoom.value_or(0);
            mln::AnimationOptions anim{std::chrono::milliseconds(700)};
            anim.easing.emplace(easing_curve(NAVI_EASING_EASE_OUT_CUBIC));
            map->easeTo(mln::CameraOptions().withZoom(std::max(min_zoom, zoom - 1)), anim);
        }
    }

    void zoom_in_at(double x, double y) {
        if (!map) return;
        stopped_moving();
        double zoom = map->getCameraOptions().zoom.value_or(0);
        mln::AnimationOptions anim{std::chrono::milliseconds(300)};
        anim.easing.emplace(easing_curve(NAVI_EASING_EASE_OUT_CUBIC));
        map->easeTo(mln::CameraOptions().withZoom(std::min(max_zoom, zoom + 1)).withAnchor(mln::ScreenCoordinate{x, y}),
                    anim);
    }

    void on_scroll(double dy, bool wheel) {
        if (!map) return;
        stop_fling();
        stopped_moving();
        double x = pointer_x >= 0 ? pointer_x : gtk_widget_get_width(widget()) / 2.0;
        double y = pointer_y >= 0 ? pointer_y : gtk_widget_get_height(widget()) / 2.0;
        moved_this_gesture = false;
        note_moved();
        if (wheel) {
            double zoom = map->getCameraOptions().zoom.value_or(0);
            mln::AnimationOptions anim{std::chrono::milliseconds(150)};
            map->easeTo(mln::CameraOptions()
                            .withZoom(std::clamp(zoom - dy * 0.5, min_zoom, max_zoom))
                            .withAnchor(mln::ScreenCoordinate{x, y}),
                        anim);
        } else {
            map->scaleBy(std::exp2(-dy / 100.0), mln::ScreenCoordinate{x, y});
        }
    }

    // ---- mln::MapObserver ----

    void onCameraIsChanging() override { schedule_camera_changed(); }
    void onCameraDidChange(CameraChangeMode) override { schedule_camera_changed(); }

    void onDidFinishLoadingStyle() override {
        style_loaded = true;
        forget_culled();
        for (const mln::style::Layer* layer : std::as_const(map->getStyle()).getLayers())
            style_order.push_back(layer->getID());
        for (const auto& s : sources) push_source(s);
        for (const auto& i : images) push_image(i);
        for (const auto& l : layers) {
            GError* error = nullptr;
            if (!push_layer(l, &error)) {
                g_warning("NaviMap: %s", error->message);
                g_error_free(error);
            }
        }
        cull_sources();
        g_signal_emit(self, signals[SIGNAL_STYLE_LOADED], 0);
    }

    // NAVI_MAP_STATS=1 prints what MapLibre drew, over 60 frames (a measuring aid). The draw calls are
    // counted per frame; the update counters only ever grow, so their growth is what a frame added.
    struct {
        int frames = 0;
        double draws = 0;
        double texture_updates = 0, buffer_bytes = 0; // the counters at the window's start
    } stats;
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        static const bool on = g_strcmp0(g_getenv("NAVI_MAP_STATS"), "1") == 0;
        if (!on || !status.renderingStats) return;
        const mln::gfx::RenderingStats& r = *status.renderingStats;
        if (stats.frames == 0) {
            stats.texture_updates = r.numTextureUpdates;
            stats.buffer_bytes = static_cast<double>(r.bufferUpdateBytes);
        }
        stats.draws += r.numDrawCalls;
        if (++stats.frames < 60) return;
        g_message("NaviMap: per frame %.0f draw calls, %.1f texture updates, %.0f KiB of buffer updates",
                  stats.draws / 60, (r.numTextureUpdates - stats.texture_updates) / 59,
                  (static_cast<double>(r.bufferUpdateBytes) - stats.buffer_bytes) / 59 / 1024);
        stats = {};
    }

    void onDidFailLoadingMap(mln::MapLoadError, const std::string& message) override {
        g_signal_emit(self, signals[SIGNAL_LOAD_ERROR], 0, message.c_str());
    }
};

// ---- GObject ----

static gboolean navi_map_real_double_tap(NaviMap* self, double x, double y) {
    self->impl->note_moved();
    self->impl->zoom_in_at(x, y);
    return TRUE;
}

static void navi_map_realize(GtkWidget* widget) {
    GTK_WIDGET_CLASS(navi_map_parent_class)->realize(widget);
    GtkGLArea* area = GTK_GL_AREA(widget);
    gtk_gl_area_make_current(area);
    if (GError* error = gtk_gl_area_get_error(area)) {
        g_warning("NaviMap: no OpenGL ES 3 context: %s", error->message);
        return;
    }
    navi::ensure_main_run_loop();
    NAVI_MAP(widget)->impl->attach_clock();
    NAVI_MAP(widget)->impl->create_map();
}

static void navi_map_unrealize(GtkWidget* widget) {
    NaviMapImpl* impl = NAVI_MAP(widget)->impl;
    impl->detach_clock();
    if (impl->map) {
        gtk_gl_area_make_current(GTK_GL_AREA(widget));
        impl->destroy_map();
    }
    GTK_WIDGET_CLASS(navi_map_parent_class)->unrealize(widget);
}

static void navi_map_size_allocate(GtkWidget* widget, int width, int height, int baseline) {
    GTK_WIDGET_CLASS(navi_map_parent_class)->size_allocate(widget, width, height, baseline);
    NaviMapImpl* impl = NAVI_MAP(widget)->impl;
    if (!impl->map) return;
    // before this frame's render, so it is drawn at the new size (GtkGLArea's "resize" comes later)
    impl->map->setSize({static_cast<uint32_t>(std::max(1, width)), static_cast<uint32_t>(std::max(1, height))});
    if (impl->use_focus) impl->update_padding();
}

static void navi_map_resize(GtkGLArea* area, int width, int height) {
    NaviMapImpl* impl = NAVI_MAP(area)->impl;
    if (impl->backend)
        impl->backend->setFramebufferSize({static_cast<uint32_t>(std::max(1, width)),
                                           static_cast<uint32_t>(std::max(1, height))});
}

static gboolean navi_map_render(GtkGLArea* area, GdkGLContext*) {
    NaviMapImpl* impl = NAVI_MAP(area)->impl;
    if (!impl->frontend) return FALSE;
    impl->check_pacing();
    if (impl->presenting) {
        impl->presenting = false;
        if (impl->backend->presentAhead()) {
            impl->note_shown();
            return TRUE;
        }
        // the area changed size since it was drawn: draw it again
    }
    impl->ahead = false; // drawn now, into the same buffer when there is one
    gint64 t0 = g_get_monotonic_time();
    impl->backend->beginFrame();
    impl->in_render = true;
    bool drew = impl->frontend->render();
    impl->after_render();
    impl->backend->endFrame(drew);
    if (!drew) return TRUE;
    impl->note_shown();
    // NAVI_MAP_FINISH=1 waits for the GPU here, so render_ms measures the frame's GPU time too (a
    // measuring aid: it stalls the pipeline)
    static const bool finish = g_strcmp0(g_getenv("NAVI_MAP_FINISH"), "1") == 0;
    if (finish) glFinish();
    double ms = (g_get_monotonic_time() - t0) / 1000.0;
    g_signal_emit(area, signals[SIGNAL_FRAME_RENDERED], 0, ms);
    return TRUE;
}

static void navi_map_dispose(GObject* object) {
    NaviMap* self = NAVI_MAP(object);
    if (self->impl) self->impl->stop_fling();
    G_OBJECT_CLASS(navi_map_parent_class)->dispose(object);
}

static void navi_map_finalize(GObject* object) {
    NaviMap* self = NAVI_MAP(object);
    delete self->impl; // the map went at unrealize
    self->impl = nullptr;
    G_OBJECT_CLASS(navi_map_parent_class)->finalize(object);
}

static void navi_map_class_init(NaviMapClass* klass) {
    mln::Log::setObserver(std::make_unique<NaviLog>());
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    GtkGLAreaClass* area_class = GTK_GL_AREA_CLASS(klass);
    object_class->dispose = navi_map_dispose;
    object_class->finalize = navi_map_finalize;
    widget_class->realize = navi_map_realize;
    widget_class->unrealize = navi_map_unrealize;
    widget_class->size_allocate = navi_map_size_allocate;
    area_class->render = navi_map_render;
    area_class->resize = navi_map_resize;

    signals[SIGNAL_CAMERA_CHANGED] = g_signal_new("camera-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                  nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
    signals[SIGNAL_USER_MOVED] = g_signal_new("user-moved", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                                              nullptr, nullptr, G_TYPE_NONE, 0);
    signals[SIGNAL_DOUBLE_TAP] = g_signal_new_class_handler(
        "double-tap", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_CALLBACK(navi_map_real_double_tap),
        g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 2, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
    signals[SIGNAL_STYLE_LOADED] = g_signal_new("style-loaded", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
    signals[SIGNAL_LOAD_ERROR] = g_signal_new("load-error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                                              nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SIGNAL_FRAME_RENDERED] = g_signal_new("frame-rendered", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                                  nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_DOUBLE);
    gtk_widget_class_set_css_name(widget_class, "navi-map");
}

static void navi_map_init(NaviMap* self) {
    self->impl = new NaviMapImpl(self);
    NaviMapImpl* impl = self->impl;
    GtkGLArea* area = GTK_GL_AREA(self);
    gtk_gl_area_set_allowed_apis(area, GDK_GL_API_GLES);
    gtk_gl_area_set_required_version(area, 3, 0);
    gtk_gl_area_set_has_depth_buffer(area, TRUE);   // fill extrusions, 3D layers
    gtk_gl_area_set_has_stencil_buffer(area, TRUE); // MapLibre clips tiles with the stencil
    gtk_gl_area_set_auto_render(area, FALSE);       // a frame only when MapLibre asks for one
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);

    impl->drag = gtk_gesture_drag_new();
    g_signal_connect(impl->drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag*, double x, double y, NaviMap* m) {
                         m->impl->on_drag_begin(x, y);
                     }),
                     self);
    g_signal_connect(impl->drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag*, double x, double y, NaviMap* m) {
                         m->impl->on_drag_update(x, y);
                     }),
                     self);
    g_signal_connect(impl->drag, "drag-end", G_CALLBACK(+[](GtkGestureDrag*, double, double, NaviMap* m) {
                         m->impl->on_drag_end();
                     }),
                     self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(impl->drag));

    // GtkGestureZoom: it starts with the second finger, and we read both points ourselves
    impl->pinch = gtk_gesture_zoom_new();
    g_signal_connect(impl->pinch, "begin", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, NaviMap* m) {
                         m->impl->on_pinch_begin();
                     }),
                     self);
    g_signal_connect(impl->pinch, "update", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, NaviMap* m) {
                         m->impl->on_pinch_update();
                     }),
                     self);
    g_signal_connect(impl->pinch, "end", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, NaviMap* m) {
                         m->impl->on_pinch_end();
                     }),
                     self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(impl->pinch));

    // on the second tap's release: its press starts the drag gesture, which stops any camera animation
    impl->click = gtk_gesture_click_new();
    g_signal_connect(impl->click, "released", G_CALLBACK(+[](GtkGestureClick*, int n, double x, double y, NaviMap* m) {
                         if (n != 2 || !m->impl->map) return;
                         m->impl->stop_fling();
                         gboolean handled = FALSE;
                         g_signal_emit(m, signals[SIGNAL_DOUBLE_TAP], 0, x, y, &handled);
                     }),
                     self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(impl->click));

    GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(+[](GtkEventControllerScroll* c, double, double dy, NaviMap* m) {
                         bool wheel = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL;
                         m->impl->on_scroll(dy, wheel);
                         return TRUE;
                     }),
                     self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll);

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, NaviMap* m) {
                         m->impl->pointer_x = x;
                         m->impl->pointer_y = y;
                     }),
                     self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);
}

// ---- C API ----

GtkWidget* navi_map_new(void) {
    return GTK_WIDGET(g_object_new(NAVI_TYPE_MAP, nullptr));
}

void navi_map_set_cache_path(NaviMap* self, const char* path) {
    g_return_if_fail(NAVI_IS_MAP(self) && path);
    self->impl->cache_path = path;
}

void navi_map_set_style_url(NaviMap* self, const char* url) {
    g_return_if_fail(NAVI_IS_MAP(self) && url);
    NaviMapImpl* impl = self->impl;
    impl->style_kind = NaviMapImpl::StyleKind::Url;
    impl->style = url;
    impl->sources.clear();
    impl->layers.clear();
    impl->load_style();
}

void navi_map_set_style_json(NaviMap* self, const char* json) {
    g_return_if_fail(NAVI_IS_MAP(self) && json);
    NaviMapImpl* impl = self->impl;
    impl->style_kind = NaviMapImpl::StyleKind::Json;
    impl->style = json;
    impl->sources.clear();
    impl->layers.clear();
    impl->load_style();
}

gboolean navi_map_is_style_loaded(NaviMap* self) {
    g_return_val_if_fail(NAVI_IS_MAP(self), FALSE);
    return self->impl->style_loaded;
}

static std::optional<mln::GeoJSON> parse_geojson(const char* text, GError** error) {
    mln::style::conversion::Error err;
    std::optional<mln::GeoJSON> gj = mln::style::conversion::parseGeoJSON(text, err);
    if (!gj) g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_PARSE, "GeoJSON: %s", err.message.c_str());
    return gj;
}

gboolean navi_map_add_geojson_source(NaviMap* self, const char* id, const char* geojson, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id, FALSE);
    NaviMapImpl* impl = self->impl;
    if (impl->find_source(id) || (impl->map && impl->style_loaded && impl->map->getStyle().getSource(id))) {
        g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_EXISTS, "source \"%s\" exists", id);
        return FALSE;
    }
    NaviMapImpl::SourceRec rec{id, std::nullopt};
    if (geojson) {
        rec.data = parse_geojson(geojson, error);
        if (!rec.data) return FALSE;
    }
    impl->sources.push_back(std::move(rec));
    impl->push_source(impl->sources.back());
    return TRUE;
}

gboolean navi_map_set_geojson(NaviMap* self, const char* id, const char* geojson, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id && geojson, FALSE);
    std::optional<mln::GeoJSON> data = parse_geojson(geojson, error);
    return data && self->impl->set_data(id, std::move(*data), error);
}

gboolean navi_map_set_geojson_point(NaviMap* self, const char* id, double latitude, double longitude, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id, FALSE);
    mapbox::geojson::point point{longitude, latitude};
    return self->impl->set_data(id, mln::GeoJSON{mapbox::geojson::geometry{point}}, error);
}

gboolean navi_map_set_geojson_line(NaviMap* self, const char* id, const double* lat_lng, gsize n, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id && (lat_lng || n == 0), FALSE);
    mapbox::geojson::line_string line;
    line.reserve(n);
    for (gsize i = 0; i < n; i++) line.emplace_back(lat_lng[2 * i + 1], lat_lng[2 * i]);
    return self->impl->set_data(id, mln::GeoJSON{mapbox::geojson::geometry{std::move(line)}}, error);
}

gboolean navi_map_remove_source(NaviMap* self, const char* id, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id, FALSE);
    NaviMapImpl* impl = self->impl;
    auto it = std::find_if(impl->sources.begin(), impl->sources.end(), [&](const auto& s) { return s.id == id; });
    bool known = it != impl->sources.end();
    if (known) impl->sources.erase(it);
    if (impl->map && impl->style_loaded) {
        if (!impl->map->getStyle().getSource(id)) {
            if (known) return TRUE;
            g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_NOT_FOUND, "no source \"%s\"", id);
            return FALSE;
        }
        if (!impl->map->getStyle().removeSource(id)) {
            g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_STYLE, "source \"%s\" is still used by a layer", id);
            return FALSE;
        }
    } else if (!known) {
        g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_NOT_FOUND, "no source \"%s\"", id);
        return FALSE;
    }
    return TRUE;
}

gboolean navi_map_add_layer_json(NaviMap* self, const char* json, const char* before, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && json, FALSE);
    NaviMapImpl* impl = self->impl;
    // parse now, so a bad layer fails here rather than at the next style load
    mln::style::conversion::Error err;
    auto layer = mln::style::conversion::convertJSON<std::unique_ptr<mln::style::Layer>>(json, err);
    if (!layer) {
        g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_PARSE, "layer: %s", err.message.c_str());
        return FALSE;
    }
    std::string id = (*layer)->getID();
    for (const auto& l : impl->layers)
        if (l.id == id) {
            g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_EXISTS, "layer \"%s\" exists", id.c_str());
            return FALSE;
        }
    NaviMapImpl::LayerRec rec{id, json, before ? before : ""};
    if (before) impl->restore_culled(); // `before` may be a layer that is out
    if (!impl->push_layer(rec, error)) return FALSE;
    impl->layers.push_back(std::move(rec));
    return TRUE;
}

void navi_map_add_image(NaviMap* self, const char* id, const guint8* rgba, int width, int height, int stride,
                        double pixel_ratio) {
    g_return_if_fail(NAVI_IS_MAP(self) && id && rgba && width > 0 && height > 0 && stride >= 4 * width);
    NaviMapImpl* impl = self->impl;
    mln::PremultipliedImage image({static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    for (int y = 0; y < height; y++) memcpy(image.data.get() + size_t(y) * width * 4, rgba + size_t(y) * stride, width * 4);
    NaviMapImpl::ImageRec rec{id, std::move(image), static_cast<float>(pixel_ratio > 0 ? pixel_ratio : 1)};
    impl->push_image(rec);
    for (auto& i : impl->images)
        if (i.id == rec.id) {
            i = std::move(rec);
            return;
        }
    impl->images.push_back(std::move(rec));
}

gboolean navi_map_remove_layer(NaviMap* self, const char* id, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id, FALSE);
    NaviMapImpl* impl = self->impl;
    impl->restore_culled();
    auto it = std::find_if(impl->layers.begin(), impl->layers.end(), [&](const auto& l) { return l.id == id; });
    bool known = it != impl->layers.end();
    if (known) impl->layers.erase(it);
    bool removed = impl->map && impl->style_loaded && impl->map->getStyle().removeLayer(id);
    if (!known && !removed) {
        g_set_error(error, NAVI_MAP_ERROR, NAVI_MAP_ERROR_NOT_FOUND, "no layer \"%s\"", id);
        return FALSE;
    }
    return TRUE;
}

char** navi_map_list_layers(NaviMap* self, const char* type) {
    g_return_val_if_fail(NAVI_IS_MAP(self), nullptr);
    NaviMapImpl* impl = self->impl;
    if (!impl->map || !impl->style_loaded) return nullptr;
    impl->restore_culled();
    GStrvBuilder* ids = g_strv_builder_new();
    for (const mln::style::Layer* layer : impl->map->getStyle().getLayers())
        if (!type || g_strcmp0(layer->getTypeInfo()->type, type) == 0) g_strv_builder_add(ids, layer->getID().c_str());
    char** out = g_strv_builder_end(ids);
    g_strv_builder_unref(ids);
    return out;
}

void navi_map_get_camera(NaviMap* self, NaviCamera* out) {
    g_return_if_fail(NAVI_IS_MAP(self) && out);
    mln::CameraOptions c = self->impl->current_camera();
    out->latitude = c.center ? c.center->latitude() : NAN;
    out->longitude = c.center ? c.center->longitude() : NAN;
    out->zoom = c.zoom.value_or(NAN);
    out->bearing = c.bearing.value_or(NAN);
    out->pitch = c.pitch.value_or(NAN);
}

void navi_map_jump_to(NaviMap* self, const NaviCamera* camera) {
    g_return_if_fail(NAVI_IS_MAP(self) && camera);
    NaviMapImpl* impl = self->impl;
    auto o = impl->to_options(camera);
    if (!o) return;
    impl->stop_fling();
    impl->stopped_moving();
    if (impl->map)
        impl->map->jumpTo(*o);
    else
        impl->merge_pending(*o);
}

static void ease(NaviMap* self, const NaviCamera* camera, guint ms, mln::util::UnitBezier curve) {
    NaviMapImpl* impl = self->impl;
    auto o = impl->to_options(camera);
    if (!o) return;
    impl->stop_fling();
    if (!impl->map) {
        impl->merge_pending(*o);
        return;
    }
    mln::AnimationOptions anim{std::chrono::milliseconds(ms)};
    anim.easing.emplace(curve); // UnitBezier has const members: no assignment
    impl->map->easeTo(*o, anim);
    impl->moving(*o, ms, false);
}

void navi_map_ease_to(NaviMap* self, const NaviCamera* camera, guint ms, NaviEasing easing) {
    g_return_if_fail(NAVI_IS_MAP(self) && camera);
    ease(self, camera, ms, easing_curve(easing));
}

void navi_map_ease_to_bezier(NaviMap* self, const NaviCamera* camera, guint ms, double x1, double y1, double x2,
                             double y2) {
    g_return_if_fail(NAVI_IS_MAP(self) && camera);
    ease(self, camera, ms, mln::util::UnitBezier(std::clamp(x1, 0.0, 1.0), y1, std::clamp(x2, 0.0, 1.0), y2));
}

void navi_map_fly_to(NaviMap* self, const NaviCamera* camera, guint ms) {
    g_return_if_fail(NAVI_IS_MAP(self) && camera);
    NaviMapImpl* impl = self->impl;
    auto o = impl->to_options(camera);
    if (!o) return;
    impl->stop_fling();
    if (!impl->map) {
        impl->merge_pending(*o);
        return;
    }
    mln::AnimationOptions anim;
    if (ms) anim.duration = std::chrono::milliseconds(ms);
    impl->map->flyTo(*o, anim);
    impl->moving(*o, ms ? ms : 1500, true);
}

void navi_map_cancel_transitions(NaviMap* self) {
    g_return_if_fail(NAVI_IS_MAP(self));
    self->impl->stop_fling();
    self->impl->stopped_moving();
    if (self->impl->map) self->impl->map->cancelTransitions();
}

void navi_map_set_max_pitch(NaviMap* self, double degrees) {
    g_return_if_fail(NAVI_IS_MAP(self));
    self->impl->max_pitch = std::clamp(degrees, 0.0, 85.0);
    self->impl->apply_bounds();
}

void navi_map_set_zoom_range(NaviMap* self, double min_zoom, double max_zoom) {
    g_return_if_fail(NAVI_IS_MAP(self) && min_zoom <= max_zoom);
    self->impl->min_zoom = std::clamp(min_zoom, 0.0, 25.5);
    self->impl->max_zoom = std::clamp(max_zoom, 0.0, 25.5);
    self->impl->apply_bounds();
}

void navi_map_set_padding(NaviMap* self, double top, double left, double bottom, double right) {
    g_return_if_fail(NAVI_IS_MAP(self));
    self->impl->use_focus = false;
    self->impl->padding = mln::EdgeInsets(top, left, bottom, right);
    self->impl->update_padding();
}

void navi_map_set_focus(NaviMap* self, double fx, double fy) {
    g_return_if_fail(NAVI_IS_MAP(self));
    self->impl->use_focus = true;
    self->impl->focus_x = std::clamp(fx, 0.0, 1.0);
    self->impl->focus_y = std::clamp(fy, 0.0, 1.0);
    self->impl->update_padding();
}

void navi_map_get_focus_point(NaviMap* self, double* x, double* y) {
    g_return_if_fail(NAVI_IS_MAP(self));
    const mln::EdgeInsets& p = self->impl->padding;
    double w = gtk_widget_get_width(GTK_WIDGET(self)), h = gtk_widget_get_height(GTK_WIDGET(self));
    if (x) *x = p.left() + (w - p.left() - p.right()) / 2;
    if (y) *y = p.top() + (h - p.top() - p.bottom()) / 2;
}

void navi_map_geo_to_screen(NaviMap* self, double latitude, double longitude, double* x, double* y) {
    g_return_if_fail(NAVI_IS_MAP(self));
    double sx = NAN, sy = NAN;
    if (self->impl->map) {
        try {
            mln::ScreenCoordinate p = self->impl->map->pixelForLatLng(mln::LatLng(latitude, longitude));
            sx = p.x;
            sy = p.y;
        } catch (const std::exception&) {
        }
    }
    if (x) *x = sx;
    if (y) *y = sy;
}

void navi_map_screen_to_geo(NaviMap* self, double x, double y, double* latitude, double* longitude) {
    g_return_if_fail(NAVI_IS_MAP(self));
    double lat = NAN, lng = NAN;
    if (self->impl->map) {
        mln::LatLng ll = self->impl->map->latLngForPixel({x, y});
        lat = ll.latitude();
        lng = ll.longitude();
    }
    if (latitude) *latitude = lat;
    if (longitude) *longitude = lng;
}

double navi_map_meters_per_pixel(NaviMap* self) {
    g_return_val_if_fail(NAVI_IS_MAP(self), NAN);
    mln::CameraOptions c = self->impl->current_camera();
    if (!c.center || !c.zoom) return NAN;
    return mln::Projection::getMetersPerPixelAtLatitude(c.center->latitude(), *c.zoom);
}

char* navi_map_query_features(NaviMap* self, double x, double y, double radius, const char* const* layer_ids) {
    g_return_val_if_fail(NAVI_IS_MAP(self), nullptr);
    NaviMapImpl* impl = self->impl;
    if (!impl->frontend || !impl->frontend->getRenderer()) return nullptr;
    mln::RenderedQueryOptions options;
    if (layer_ids) {
        std::vector<std::string> ids;
        for (const char* const* l = layer_ids; *l; l++) ids.emplace_back(*l);
        options.layerIDs = std::move(ids);
    }
    std::vector<mln::Feature> features;
    if (radius > 0)
        features = impl->frontend->getRenderer()->queryRenderedFeatures(
            mln::ScreenBox{{x - radius, y - radius}, {x + radius, y + radius}}, options);
    else
        features = impl->frontend->getRenderer()->queryRenderedFeatures(mln::ScreenCoordinate{x, y}, options);
    if (features.empty()) return nullptr;
    mapbox::geojson::feature_collection collection;
    for (auto& f : features) {
        mapbox::geojson::feature out(static_cast<const mln::GeoJSONFeature&>(f));
        out.properties["navi:layer"] = f.sourceLayer.empty() ? std::string(f.source) : f.sourceLayer;
        collection.push_back(std::move(out));
    }
    std::string text = mapbox::geojson::stringify(collection);
    return g_strdup(text.c_str());
}

void navi_map_set_tile_lod(NaviMap* self, double min_radius, double scale, double pitch_threshold,
                           double zoom_shift) {
    g_return_if_fail(NAVI_IS_MAP(self));
    NaviMapImpl* impl = self->impl;
    impl->lod_radius = std::max(0.0, min_radius);
    impl->lod_scale = std::max(0.1, scale);
    impl->lod_pitch = std::clamp(pitch_threshold, 0.0, 180.0);
    impl->lod_zoom_shift = std::min(0.0, zoom_shift);
    impl->apply_tile_lod();
}

void navi_map_set_frame_pacing(NaviMap* self, int refreshes_per_frame) {
    g_return_if_fail(NAVI_IS_MAP(self));
    NaviMapImpl* impl = self->impl;
    impl->pace_setting = std::clamp(refreshes_per_frame, 0, 4);
    impl->pace_known = false;
    impl->pace_frames = impl->pace_late = impl->pace_late_windows = 0;
    impl->set_pace(std::max(impl->pace_setting, 1));
}

void navi_map_set_render_scale(NaviMap* self, double scale) {
    g_return_if_fail(NAVI_IS_MAP(self));
    NaviMapImpl* impl = self->impl;
    scale = std::clamp(scale, 0.25, 1.0);
    if (scale == impl->render_scale) return;
    impl->render_scale = scale;
    impl->update_area_buffers();
    GtkGLArea* area = GTK_GL_AREA(self);
    if (impl->map) { // the pixel ratio is fixed at creation: make the map again (style and sources come back)
        gtk_gl_area_make_current(area);
        impl->destroy_map();
        impl->create_map();
    }
}

void navi_map_set_gestures_enabled(NaviMap* self, gboolean enabled) {
    g_return_if_fail(NAVI_IS_MAP(self));
    NaviMapImpl* impl = self->impl;
    impl->gestures_enabled = enabled;
    GtkPropagationPhase phase = enabled ? GTK_PHASE_BUBBLE : GTK_PHASE_NONE;
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(impl->drag), phase);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(impl->pinch), phase);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(impl->click), phase);
    if (!enabled) impl->stop_fling();
}
