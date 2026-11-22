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
#include <mln/renderer/query.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/conversion/layer.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/geo.hpp>
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
    float pixel_ratio = 1;
    std::string cache_path;

    // the style and what the app added to it, replayed after every style load
    enum class StyleKind { None, Url, Json } style_kind = StyleKind::None;
    std::string style;
    bool style_loaded = false;
    struct SourceRec {
        std::string id;
        std::optional<mln::GeoJSON> data;
    };
    std::vector<SourceRec> sources;
    struct LayerRec {
        std::string id, json, before;
    };
    std::vector<LayerRec> layers;

    // camera state kept while there is no map
    mln::CameraOptions camera;
    double max_pitch = 60, min_zoom = 0, max_zoom = 22;
    bool use_focus = false;
    double focus_x = 0.5, focus_y = 0.5;
    mln::EdgeInsets padding;

    guint camera_tick = 0;

    // MapLibre asks for its next frame from inside render() (an animation, a fade, tiles arriving), and
    // GtkGLArea clears its "needs render" flag only after "render" returns, which would swallow that
    // request: it is held here and re-issued at the next frame clock tick
    bool in_render = false, render_again = false;
    guint render_tick = 0;

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
        else
            gtk_gl_area_queue_render(GTK_GL_AREA(self));
    }

    void after_render() {
        in_render = false;
        if (!render_again || render_tick) return;
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
        pixel_ratio = surface ? static_cast<float>(gdk_surface_get_scale(surface)) : 1.0f;
        uint32_t width = std::max(1, gtk_widget_get_width(w));
        uint32_t height = std::max(1, gtk_widget_get_height(w));

        gchar* dir = g_path_get_dirname(cache_path.c_str());
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);

        backend = std::make_unique<navi::Backend>(GTK_GL_AREA(w));
        backend->setFramebufferSize({static_cast<uint32_t>(std::ceil(width * pixel_ratio)),
                                     static_cast<uint32_t>(std::ceil(height * pixel_ratio))});
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
        if (map) map->jumpTo(mln::CameraOptions().withPadding(padding));
    }

    void load_style() {
        if (!map) return;
        style_loaded = false;
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

    void schedule_camera_changed() {
        if (camera_tick) return;
        camera_tick = gtk_widget_add_tick_callback(
            widget(),
            [](GtkWidget* w, GdkFrameClock*, gpointer) -> gboolean {
                NaviMap* m = NAVI_MAP(w);
                m->impl->camera_tick = 0;
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
            double zoom = map->getCameraOptions().zoom.value_or(0);
            mln::AnimationOptions anim{std::chrono::milliseconds(700)};
            anim.easing.emplace(easing_curve(NAVI_EASING_EASE_OUT_CUBIC));
            map->easeTo(mln::CameraOptions().withZoom(std::max(min_zoom, zoom - 1)), anim);
        }
    }

    void zoom_in_at(double x, double y) {
        if (!map) return;
        double zoom = map->getCameraOptions().zoom.value_or(0);
        mln::AnimationOptions anim{std::chrono::milliseconds(300)};
        anim.easing.emplace(easing_curve(NAVI_EASING_EASE_OUT_CUBIC));
        map->easeTo(mln::CameraOptions().withZoom(std::min(max_zoom, zoom + 1)).withAnchor(mln::ScreenCoordinate{x, y}),
                    anim);
    }

    void on_scroll(double dy, bool wheel) {
        if (!map) return;
        stop_fling();
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
        for (const auto& s : sources) push_source(s);
        for (const auto& l : layers) {
            GError* error = nullptr;
            if (!push_layer(l, &error)) {
                g_warning("NaviMap: %s", error->message);
                g_error_free(error);
            }
        }
        g_signal_emit(self, signals[SIGNAL_STYLE_LOADED], 0);
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
    NAVI_MAP(widget)->impl->create_map();
}

static void navi_map_unrealize(GtkWidget* widget) {
    NaviMapImpl* impl = NAVI_MAP(widget)->impl;
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
    gint64 t0 = g_get_monotonic_time();
    impl->backend->beginFrame();
    impl->in_render = true;
    bool drew = impl->frontend->render();
    impl->after_render();
    if (!drew) {
        glClearColor(0.93f, 0.93f, 0.91f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }
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
    if (!impl->push_layer(rec, error)) return FALSE;
    impl->layers.push_back(std::move(rec));
    return TRUE;
}

gboolean navi_map_remove_layer(NaviMap* self, const char* id, GError** error) {
    g_return_val_if_fail(NAVI_IS_MAP(self) && id, FALSE);
    NaviMapImpl* impl = self->impl;
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
}

void navi_map_cancel_transitions(NaviMap* self) {
    g_return_if_fail(NAVI_IS_MAP(self));
    self->impl->stop_fling();
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
