/* NaviMap: a vector map widget for GTK 4, drawn by MapLibre Native with OpenGL ES 3.
 *
 * The widget is a GtkGLArea. MapLibre's own main-thread run loop is driven from the GLib main loop, so
 * the widget needs nothing beyond gtk_init() and a running GMainLoop. All calls are main-thread only.
 *
 * Coordinates are in degrees (latitude, longitude). Screen positions are in widget (logical) pixels,
 * origin top left, the same space as GTK allocations, so an overlay child can be placed with them.
 */
#pragma once

#include <gtk/gtk.h>
#include <math.h>

G_BEGIN_DECLS

#define NAVI_TYPE_MAP (navi_map_get_type ())
G_DECLARE_FINAL_TYPE (NaviMap, navi_map, NAVI, MAP, GtkGLArea)

#define NAVI_MAP_ERROR (navi_map_error_quark ())
GQuark navi_map_error_quark (void);

typedef enum {
  NAVI_MAP_ERROR_PARSE,     /* the JSON or GeoJSON text did not parse */
  NAVI_MAP_ERROR_NOT_FOUND, /* no source or layer with that id */
  NAVI_MAP_ERROR_EXISTS,    /* a source or layer with that id is already there */
  NAVI_MAP_ERROR_STYLE,     /* the style rejected the change */
} NaviMapError;

/* A camera position. Fields set to NAN are left as they are by jump/ease/fly. */
typedef struct {
  double latitude;
  double longitude;
  double zoom;    /* 0 = whole world in 512 px; 16-17 is street level */
  double bearing; /* degrees clockwise from north that point to the top of the view */
  double pitch;   /* degrees of tilt away from straight down, 0 to navi_map_set_max_pitch() */
} NaviCamera;

#define NAVI_CAMERA_UNSET ((NaviCamera) { NAN, NAN, NAN, NAN, NAN })

typedef enum {
  NAVI_EASING_DEFAULT,     /* MapLibre's own ease-out, cubic-bezier(0, 0, 0.25, 1) */
  NAVI_EASING_LINEAR,      /* constant speed: chained eases (a moving car) join without a stop */
  NAVI_EASING_EASE,        /* cubic-bezier(0.25, 0.1, 0.25, 1) */
  NAVI_EASING_EASE_IN,     /* cubic-bezier(0.42, 0, 1, 1) */
  NAVI_EASING_EASE_OUT,    /* cubic-bezier(0, 0, 0.58, 1) */
  NAVI_EASING_EASE_IN_OUT, /* cubic-bezier(0.42, 0, 0.58, 1) */
  NAVI_EASING_EASE_OUT_CUBIC, /* cubic-bezier(0.33, 1, 0.68, 1): fast start, long tail. The re-center
                               * flight back to the car (~650 ms) and the two-finger-tap zoom (700 ms) */
} NaviEasing;

/* the widget */
GtkWidget *navi_map_new (void);

/* Where tiles, glyphs and styles are cached (an SQLite file). Takes effect when the widget is realized;
 * the default is $XDG_CACHE_HOME/vibeos-navi/map-cache.db. */
void navi_map_set_cache_path (NaviMap *map, const char *path);

/* ---- style ---- */

/* Loads a style by URL (https://, file://) or from JSON text. Sources and layers added with the calls
 * below belong to the current style: setting a new style drops them.
 * While a tile source's "bounds" lie far outside the view, the style's layers that draw it are taken
 * out of the style, and put back when the view comes near (MapLibre spends time on every layer every
 * frame, drawn or not, which adds up in a style with one source per region). The layer calls below
 * put them all back first, so they always see the whole style. */
void navi_map_set_style_url (NaviMap *map, const char *url);
void navi_map_set_style_json (NaviMap *map, const char *json);
/* The style at `url` (a file:// one), over the tiles already there when it differs from the style shown only in its
 * layers' paint (Navi's day and night styles do): the style's layers take its colours, and nothing reloads or goes
 * blank. The app's own sources and layers stay, with their paint (navi_map_set_paint_property changes it). Returns
 * FALSE, and sets the style as navi_map_set_style_url () does, where that cannot be: no style shown yet, a style
 * of another kind, or one that differs in more than paint. */
gboolean navi_map_switch_style_url (NaviMap *map, const char *url);
gboolean navi_map_is_style_loaded (NaviMap *map);

/* A GeoJSON source the app owns, for things like the route line and the car. Calls made before the
 * style has loaded are kept and applied once it has. `geojson` may be NULL for an empty source. */
gboolean navi_map_add_geojson_source (NaviMap *map, const char *source_id, const char *geojson,
                                      GError **error);
/* The same, measuring its lines as it cuts them into tiles, which a line layer's "line-gradient" needs (its
 * ["line-progress"]: 0 at a line's start to 1 at its end, along the Web Mercator projection) */
gboolean navi_map_add_geojson_source_with_line_metrics (NaviMap *map, const char *source_id, const char *geojson,
                                                        GError **error);
/* Replaces a GeoJSON source's data (a Feature, FeatureCollection or bare geometry). */
gboolean navi_map_set_geojson (NaviMap *map, const char *source_id, const char *geojson, GError **error);
/* The same without JSON: one point, or a line through `n_points` (latitude, longitude) pairs. */
gboolean navi_map_set_geojson_point (NaviMap *map, const char *source_id, double latitude, double longitude,
                                     GError **error);
gboolean navi_map_set_geojson_line (NaviMap *map, const char *source_id, const double *lat_lng_pairs,
                                    gsize n_points, GError **error);
gboolean navi_map_remove_source (NaviMap *map, const char *source_id, GError **error);

/* A style layer as JSON, exactly as in a style's "layers" array, drawn below `before_layer_id` (NULL: on
 * top of everything). */
gboolean navi_map_add_layer_json (NaviMap *map, const char *layer_json, const char *before_layer_id,
                                  GError **error);
gboolean navi_map_remove_layer (NaviMap *map, const char *layer_id, GError **error);
/* One paint property of a layer the app added, as JSON ("\"#2CDDFF\"", an expression): kept with the layer, so a
 * new style or map has it too. Changing paint redraws the layer's tiles as they are, which is much cheaper than new
 * data for its source (that is cut into tiles again). */
gboolean navi_map_set_paint_property (NaviMap *map, const char *layer_id, const char *property, const char *json,
                                      GError **error);
/* An image for the style's symbols ("icon-image": `id`), kept across style loads: `width` x `height` pixels of
 * premultiplied RGBA, rows `stride` bytes apart, drawn at `pixel_ratio` pixels to a point. The same id again
 * replaces it. */
void navi_map_add_image (NaviMap *map, const char *id, const guint8 *rgba, int width, int height, int stride,
                         double pixel_ratio);
/* The style's layer ids, bottom to top, of one type ("symbol", "line", "fill", ...) or of all (NULL).
 * NULL before the style has loaded. The first "symbol" layer is where a route goes to stay under the
 * labels. g_strfreev() it. */
char **navi_map_list_layers (NaviMap *map, const char *type);

/* Draws the map at this fraction of the display's resolution (0.25 to 1) and lets the GPU stretch it
 * over the widget: 0.75 fills 56 % of the pixels, for a GPU that cannot fill a large display at its
 * refresh rate. 1, the default, draws every device pixel. Changing it on a shown map reloads the map. */
void navi_map_set_render_scale (NaviMap *map, double scale);

/* Shows a new frame every `refreshes_per_frame` display refreshes (1 to 4), or 0, the default: one a
 * refresh up to 60 Hz, one every other refresh at 120 Hz, and one refresh slower whenever more than a
 * quarter of the frames miss for two seconds. A map that needs a little more than one refresh per
 * frame otherwise alternates between one and two, and that uneven rhythm looks rougher than a steady
 * half rate. Above 1 each frame is drawn a frame-clock cycle ahead and shown in the next one. */
void navi_map_set_frame_pacing (NaviMap *map, int refreshes_per_frame);
/* At most `fps` new frames a second (0: no limit, the default), by the pacing above: one every second refresh at 60
 * Hz for 30, every fourth at 120 Hz. Auto pacing (0) only; a set navi_map_set_frame_pacing () wins. */
void navi_map_set_max_fps (NaviMap *map, double fps);
/* Whether the map draws (TRUE, the default): FALSE while its window is not seen, so that MapLibre's wish for a
 * frame (a tile come in, a camera moved) is only noted, and drawn once it draws again. No frame is drawn meanwhile,
 * the fling stops; what the map knows (the camera, the sources, the style) keeps changing as before. */
void navi_map_set_drawing (NaviMap *map, gboolean drawing);

/* Coarser tiles toward the horizon of a tilted map, which is where most of a tilted map's tiles are:
 * beyond `min_radius` tiles from the center, tiles drop zoom levels as their distance x `scale` grows,
 * once the pitch is over `pitch_threshold` degrees; a negative `zoom_shift` lowers the detail
 * everywhere (-1 is a quarter of the tiles). MapLibre's defaults, (3, 1, 60, 0), leave a camera tilted
 * less than 60 degrees at full detail. */
void navi_map_set_tile_lod (NaviMap *map, double min_radius, double scale, double pitch_threshold,
                            double zoom_shift);

/* ---- camera ---- */

void navi_map_get_camera (NaviMap *map, NaviCamera *camera);
void navi_map_jump_to (NaviMap *map, const NaviCamera *camera);
/* Moves there in `duration_ms`. A new jump, ease, fly or gesture replaces a running one. */
void navi_map_ease_to (NaviMap *map, const NaviCamera *camera, guint duration_ms, NaviEasing easing);
/* The same with any cubic-bezier(x1, y1, x2, y2) timing curve. */
void navi_map_ease_to_bezier (NaviMap *map, const NaviCamera *camera, guint duration_ms,
                              double x1, double y1, double x2, double y2);
/* Zooms out, across and back in, like a flight. duration_ms 0 lets MapLibre pick it from the distance. */
void navi_map_fly_to (NaviMap *map, const NaviCamera *camera, guint duration_ms);
void navi_map_cancel_transitions (NaviMap *map);

/* The tilt limit for the API and the gestures, at most 85 degrees. The default is 60. */
void navi_map_set_max_pitch (NaviMap *map, double degrees);
void navi_map_set_zoom_range (NaviMap *map, double min_zoom, double max_zoom);

/* Content insets in widget pixels: the camera's center (and the vanishing point) is the middle of what
 * the insets leave. Clears a focus set with navi_map_set_focus(). */
void navi_map_set_padding (NaviMap *map, double top, double left, double bottom, double right);
/* Puts the camera's center at this fraction of the widget, kept through resizes: (0.5, 0.75) puts the
 * car a quarter of the height above the bottom edge, as navigation apps do. (0.5, 0.5) is plain. */
void navi_map_set_focus (NaviMap *map, double fraction_x, double fraction_y);
/* The point the camera's center is drawn at, in widget pixels. */
void navi_map_get_focus_point (NaviMap *map, double *x, double *y);

/* ---- coordinates ---- */

/* Where a geographic point is drawn, in widget pixels. Points behind the camera give nonsense; with
 * pitch, check that y is inside the widget before using it. */
void navi_map_geo_to_screen (NaviMap *map, double latitude, double longitude, double *x, double *y);
/* The geographic point under a widget pixel (on the ground plane). */
void navi_map_screen_to_geo (NaviMap *map, double x, double y, double *latitude, double *longitude);
/* Ground meters per widget pixel at the camera's center. */
double navi_map_meters_per_pixel (NaviMap *map);

/* The features drawn within `radius` widget pixels of (x, y), topmost first, as a GeoJSON
 * FeatureCollection (each feature's "navi:layer" property names its source layer), or NULL for none.
 * `layer_ids` (NULL-terminated, or NULL for all) limits the search to those style layers. g_free() it. */
char *navi_map_query_features (NaviMap *map, double x, double y, double radius, const char *const *layer_ids);

/* ---- gestures ---- */

/* On by default. One finger pans and a fling coasts ~0.3 s. Two fingers pan (vertical drags too: the
 * tilt stays where the app put it), pinch zooms continuously, and a twist past 10 degrees turns the map
 * around the fingers' centre. A two-finger tap zooms out one step (700 ms ease-out); a double tap emits
 * "double-tap" and, unless a handler returns TRUE, zooms in one step around the finger. */
void navi_map_set_gestures_enabled (NaviMap *map, gboolean enabled);

/* Signals:
 *   "camera-changed" (NaviMap*)  once per frame in which the camera moved, before layout: reposition
 *                                overlays here.
 *   "user-moved"     (NaviMap*)  a gesture started moving the camera (once per gesture): the user took
 *                                the map, so an app leaves follow mode. Not emitted for API calls.
 *   "double-tap"     gboolean (NaviMap*, double x, double y)  on the second tap's release; return TRUE
 *                                when the app handled it (say, opened what navi_map_query_features()
 *                                found there); otherwise the map zooms in, which counts as "user-moved".
 *   "style-loaded"   (NaviMap*)
 *   "load-error"     (NaviMap*, const char *message)
 *   "frame-rendered" (NaviMap*, double render_ms)  after each frame MapLibre drew, with the time the
 *                                CPU spent issuing it.
 */

G_END_DECLS
