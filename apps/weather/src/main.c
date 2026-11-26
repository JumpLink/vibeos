/* VibeOS Weather: the page behind the home screen's weather widget, laid out like One UI's Weather: the place and the
 * temperature over the sky, the next 24 hours as a curve, today against yesterday, the week, and the details.
 * GTK4, so scrolling is the GPU moving what is already drawn; the page scrolls in libspongy's view (a fling, and
 * a stretch at the ends), like the other VibeOS apps. Its window, ⋮ menu, Set City and About are libvibeos's, as
 * theirs, and so are its colours (the theme's, light or dark) and its text, which follows Font size and Bold text.
 *
 * On a phone the page is one column. On a tablet, and on a phone turned on its side, it is a column of at most
 * COLUMN_MAX dp in the middle of the window; on a window as wide as a turned tablet, two columns side by side.
 *
 * Where: the city chosen in Set City, if there is one (kept in vibeos.Weather's city; Open-Meteo's geocoding finds
 * the cities as their name is typed); otherwise the first city in GNOME Weather, if it has one; otherwise where the
 * phone is, from GeoClue at city accuracy, named by OpenStreetMap's Nominatim. "Use my location" in Set City goes back
 * from a chosen city to the last two. The place shown is kept in ~/.cache/vibeos-launcher/weather.json (lat, lon,
 * place), so the page opens on the last place at once and moves when the phone has, and the home screen's widget can
 * show the same place.
 * What: one Open-Meteo forecast (no key) with yesterday in it (past_days=1), fetched in curl and kept in
 * ~/.cache/vibeos-launcher/forecast.json; the page shows that at once and asks again when it is 15 minutes old. */
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <spongy-gtk.h>
#include <vibeos.h>

#include "wx-menu.h"

#define STALE_S (15 * 60)
#define UA "vibeos-weather/1.0 (VibeOS)"
#define HOURS 24
#define COL_W 66              /* dp, one hour in the hourly strip at a font size of 1; it grows with the text */
#define COLUMN_MAX 600        /* dp, the page's one column on a wider window */
#define PAGE_MAX 1200         /* dp, the two columns together */
#define BAR_CLEAR 64          /* dp, the second column's top: under reload and the menu, which float over the page */

typedef struct {
  GtkWidget       *win, *sky, *clamp, *body, *spinner;
  JsonNode        *forecast;     /* the forecast shown, to lay it out again (a turn, a font-size change) */
  const char      *sky_name;     /* the sky shown (one of skies[]) */
  gboolean         two_columns;
  double           lat, lon;
  char            *place;
  gboolean         have_place;
  gboolean         shown;        /* the body shows a forecast (not a message) */
  char            *city;         /* the city chosen in Set City, while there is one */
  GSettings       *settings;     /* vibeos.Weather, where the chosen city is kept; NULL without the schema */
  GDBusConnection *system;
  char            *geo_client;   /* the GeoClue client object, while it looks */
  guint            geo_signal, geo_timeout;
  gboolean         geo_asked;
} Page;

/* ---- what the weather codes (WMO) look like and are called ---- */

static const char *
wx_icon (int code, gboolean day)
{
  if (code == 0)
    return day ? "weather-clear-symbolic" : "weather-clear-night-symbolic";
  if (code <= 2)
    return day ? "weather-few-clouds-symbolic" : "weather-few-clouds-night-symbolic";
  if (code == 3)
    return "weather-overcast-symbolic";
  if (code == 45 || code == 48)
    return "weather-fog-symbolic";
  if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82))
    return "weather-showers-scattered-symbolic";
  if (code >= 61 && code <= 67)
    return "weather-showers-symbolic";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "weather-snow-symbolic";
  if (code >= 95)
    return "weather-storm-symbolic";
  return "weather-overcast-symbolic";
}

static const char *
wx_name (int code)
{
  static const struct { int upto; const char *name; } names[] = {
    /* TRANSLATORS: the sky, as the page names it under the temperature */
    { 0, NC_("sky", "Clear") }, { 1, N_("Mostly clear") }, { 2, N_("Partly cloudy") }, { 3, N_("Cloudy") },
    { 48, N_("Fog") }, { 55, N_("Drizzle") }, { 57, N_("Freezing drizzle") }, { 63, N_("Rain") },
    { 65, N_("Heavy rain") }, { 67, N_("Freezing rain") }, { 75, N_("Snow") }, { 77, N_("Snow grains") },
    { 81, N_("Showers") }, { 82, N_("Heavy showers") }, { 86, N_("Snow showers") }, { 99, N_("Thunderstorm") },
  };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (code <= names[i].upto)
      return i == 0 ? g_dpgettext2 (NULL, "sky", names[i].name) : _(names[i].name);
  return _("Cloudy");
}

/* The sky behind the page, top to bottom: four colours and where each sits (0..1). Each colour is a stop of one of
 * the theme's tonal palettes (VibeosPalette), one set of stops for a light theme and one for a dark one: the sky
 * follows the theme's colour and its light or dark, and the text over it is the theme's text colour.
 *
 * It is a texture, not a CSS linear-gradient on the window. Scrolling redraws the whole window every frame, and on the
 * POCO X3 (Adreno 618, 1080x2400 at 120 Hz) GTK's gradient over the full window took a frame every ~24 ms, a third
 * of the refresh rate, where a flat background or a texture keeps every 8.3 ms vblank (measured 2026-11-12). The
 * texture is one pixel wide and 256 tall, made once per sky and theme, and stretched over the window with linear
 * filtering. */
typedef struct {
  const char   *name;
  VibeosPalette palette;
  int           light[4], dark[4];   /* palette stops: 0 (white), 10, 50, 100 ... 900, 1000 (black) */
  float         at[4];
} Sky;

static const Sky skies[] = {
  { "cloudy", VIBEOS_PALETTE_NEUTRAL2, { 300, 100, 200, 50 }, { 700, 600, 800, 900 }, { 0, .30f, .55f, 1 } },
  { "fog",    VIBEOS_PALETTE_NEUTRAL1, { 200, 50, 100, 50 },  { 700, 500, 700, 900 }, { 0, .30f, .55f, 1 } },
  { "clear",  VIBEOS_PALETTE_ACCENT1,  { 300, 100, 200, 50 }, { 600, 500, 700, 800 }, { 0, .32f, .58f, 1 } },
  { "night",  VIBEOS_PALETTE_ACCENT2,  { 400, 200, 300, 100 }, { 900, 800, 900, 1000 }, { 0, .32f, .60f, 1 } },
  { "rain",   VIBEOS_PALETTE_ACCENT2,  { 300, 200, 300, 100 }, { 700, 600, 800, 900 }, { 0, .30f, .58f, 1 } },
  { "snow",   VIBEOS_PALETTE_NEUTRAL1, { 100, 50, 100, 10 },  { 600, 500, 700, 800 }, { 0, .30f, .58f, 1 } },
  { "storm",  VIBEOS_PALETTE_ACCENT3,  { 400, 300, 400, 200 }, { 800, 700, 900, 1000 }, { 0, .30f, .58f, 1 } },
};

#define SKY_H 256

static GdkTexture *
sky_texture (const Sky *s)
{
  gboolean dark = vibeos_theme_get_dark (vibeos_theme_get_default ());
  guint8 *px = g_malloc (SKY_H * 3);
  GdkRGBA stops[4];

  for (int i = 0; i < 4; i++)
    stops[i] = vibeos_palette_stop (s->palette, dark ? s->dark[i] : s->light[i]);
  for (int y = 0; y < SKY_H; y++) {
    float t = y / (float) (SKY_H - 1);
    int i = 0;
    while (i < 2 && t > s->at[i + 1])
      i++;
    float f = CLAMP ((t - s->at[i]) / MAX (s->at[i + 1] - s->at[i], 1e-6f), 0, 1);
    GdkRGBA c = vibeos_rgba_mix (&stops[i], &stops[i + 1], f);
    px[y * 3] = (guint8) lroundf (c.red * 255);
    px[y * 3 + 1] = (guint8) lroundf (c.green * 255);
    px[y * 3 + 2] = (guint8) lroundf (c.blue * 255);
  }
  g_autoptr (GBytes) bytes = g_bytes_new_take (px, SKY_H * 3);
  return gdk_memory_texture_new (1, SKY_H, GDK_MEMORY_R8G8B8, bytes, 3);
}

static const char *
wx_sky (int code, gboolean day)
{
  if (!day)
    return "night";
  if (code <= 1)
    return "clear";
  if (code == 45 || code == 48)
    return "fog";
  if (code >= 95)
    return "storm";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "snow";
  if (code >= 51)
    return "rain";
  return "cloudy";
}

/* the sun and the moon in the theme's accents, everything else in its text colour (style.css) */
static GtkWidget *
wx_image (const char *icon, int size)
{
  GtkWidget *img = gtk_image_new_from_icon_name (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (img), size);
  if (g_str_has_prefix (icon, "weather-clear-symbolic") || g_str_has_prefix (icon, "daytime-"))
    gtk_widget_add_css_class (img, "wx-sun");
  else if (strstr (icon, "night"))
    gtk_widget_add_css_class (img, "wx-moon");
  return img;
}

/* ---- the forecast, read ---- */

static double
num (JsonArray *a, guint i)
{
  if (!a || i >= json_array_get_length (a))
    return NAN;
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_VALUE (n) ? json_node_get_double (n) : NAN;
}

static const char *
str (JsonArray *a, guint i)
{
  if (!a || i >= json_array_get_length (a))
    return NULL;
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_VALUE (n) ? json_node_get_string (n) : NULL;
}

static JsonArray *
arr (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) ? json_object_get_array_member (o, name) : NULL;
}

static double
cur (JsonObject *c, const char *name)
{
  return c && json_object_has_member (c, name) ? json_object_get_double_member (c, name) : NAN;
}

static GDateTime *
parse_local (const char *t)
{
  if (!t)
    return NULL;
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autofree char *full = strlen (t) == 16 ? g_strconcat (t, ":00", NULL) : g_strdup (t);
  return g_date_time_new_from_iso8601 (full, tz);
}

/* degrees Fahrenheit, where the user's region tells the weather in them (the forecast comes in Celsius); asked once,
 * since an app keeps the language and region it started in */
static gboolean
fahrenheit (void)
{
  static int f = -1;
  if (f < 0)
    f = vibeos_locale_uses_fahrenheit ();
  return f;
}

/* a temperature given in °C, in the region's degrees, whole, with the degree sign alone ("22°", "72°") */
static char *
fmt_temp (double t)
{
  if (isnan (t))
    return g_strdup ("–");
  double r = round (fahrenheit () ? t * 9 / 5 + 32 : t);
  return g_strdup_printf ("%.0f°", r == 0 ? 0 : r);     /* never "-0°" */
}

static GtkWidget *
label (const char *text, const char *cls, float xalign)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  if (cls)
    gtk_widget_add_css_class (l, cls);
  return l;
}

/* ---- the hourly strip: a column per slot (time, icon, value, chance of rain) over the temperature curve ---- */

#define CURVE_H 34

typedef struct {
  GArray *temps;               /* double per slot */
  GArray *dots;                /* gboolean per slot */
  double  lo, hi;
  int     col_w;               /* px, one slot */
} Curve;

/* one slot of the hourly strip: COL_W at a font size of 1, wider as the text grows, so that "12:00" and "Sunrise"
 * still fit their column */
static int
col_width (void)
{
  return (int) ceil (COL_W * MAX (1.0, vibeos_sp (14) / 14));
}

static void
curve_free (Curve *c)
{
  g_array_unref (c->temps);
  g_array_unref (c->dots);
  g_free (c);
}

static void
draw_curve (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Curve *c = data;
  guint n = c->temps->len;
  double span = MAX (c->hi - c->lo, 1);
  GdkRGBA line = vibeos_color_alpha (VIBEOS_ON_SURFACE, 0.75), dot = vibeos_color (VIBEOS_ON_SURFACE);
  #define CY(t) (h - 6 - ((t) - c->lo) / span * (h - 12))
  #define CX(i) ((i) * c->col_w + c->col_w / 2.0)
  cairo_set_line_width (cr, 1.6);
  gdk_cairo_set_source_rgba (cr, &line);
  for (guint i = 0; i < n; i++) {
    double t = g_array_index (c->temps, double, i);
    if (i == 0) {
      cairo_move_to (cr, CX (i), CY (t));
      continue;
    }
    /* smooth: each segment leaves and arrives level */
    double p = g_array_index (c->temps, double, i - 1), mx = (CX (i - 1) + CX (i)) / 2;
    cairo_curve_to (cr, mx, CY (p), mx, CY (t), CX (i), CY (t));
  }
  cairo_stroke (cr);
  gdk_cairo_set_source_rgba (cr, &dot);
  for (guint i = 0; i < n; i++)
    if (g_array_index (c->dots, gboolean, i)) {
      cairo_arc (cr, CX (i), CY (g_array_index (c->temps, double, i)), 4, 0, 2 * G_PI);
      cairo_fill (cr);
    }
}

static GtkWidget *
drop_label (double pop, gboolean small)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  if (isnan (pop))
    return b;
  GtkWidget *d = gtk_image_new_from_icon_name ("weather-showers-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (d), small ? 10 : 12);
  gtk_widget_add_css_class (d, "wx-dim");
  g_autofree char *t = g_strdup_printf ("%.0f%%", pop);
  gtk_box_append (GTK_BOX (b), d);
  gtk_box_append (GTK_BOX (b), label (t, "wx-small", 0));
  return b;
}

typedef struct {
  GtkWidget *row;              /* the columns */
  Curve     *curve;
  GPtrArray *pops;
} Strip;

static void
add_slot (Strip *st, const char *time, const char *icon, const char *value, gboolean big, double temp, double pop,
          gboolean dot)
{
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request (col, st->curve->col_w, -1);
  gtk_box_append (GTK_BOX (col), label (time, "wx-hour", 0.5));
  GtkWidget *img = wx_image (icon, 28);
  gtk_box_append (GTK_BOX (col), img);
  gtk_box_append (GTK_BOX (col), label (value, big ? "wx-hour-temp" : "wx-hour-event", 0.5));
  gtk_box_append (GTK_BOX (st->row), col);
  g_ptr_array_add (st->pops, drop_label (pop, TRUE));
  g_array_append_val (st->curve->temps, temp);
  g_array_append_val (st->curve->dots, dot);
  if (!isnan (temp)) {
    st->curve->lo = MIN (st->curve->lo, temp);
    st->curve->hi = MAX (st->curve->hi, temp);
  }
}

/* the next 24 hours from `first`, with today's and tomorrow's sunrise and sunset in between */
static GtkWidget *
hourly_strip (JsonObject *hourly, guint first, JsonArray *sunrise, JsonArray *sunset)
{
  Strip st = { gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0), g_new0 (Curve, 1), g_ptr_array_new () };
  st.curve->temps = g_array_new (FALSE, FALSE, sizeof (double));
  st.curve->dots = g_array_new (FALSE, FALSE, sizeof (gboolean));
  st.curve->lo = G_MAXDOUBLE;
  st.curve->hi = -G_MAXDOUBLE;
  st.curve->col_w = col_width ();
  JsonArray *time = arr (hourly, "time"), *temp = arr (hourly, "temperature_2m");
  JsonArray *code = arr (hourly, "weather_code"), *pop = arr (hourly, "precipitation_probability");
  JsonArray *isday = arr (hourly, "is_day");

  g_autoptr (GPtrArray) events = g_ptr_array_new_with_free_func ((GDestroyNotify) g_date_time_unref);
  g_autoptr (GPtrArray) kinds = g_ptr_array_new ();
  for (guint d = 0; sunrise && d < json_array_get_length (sunrise); d++) {
    GDateTime *r = parse_local (str (sunrise, d)), *s = parse_local (str (sunset, d));
    if (r) { g_ptr_array_add (events, r); g_ptr_array_add (kinds, N_("Sunrise")); }
    if (s) { g_ptr_array_add (events, s); g_ptr_array_add (kinds, N_("Sunset")); }
  }

  for (guint i = first; time && i < first + HOURS && i < json_array_get_length (time); i++) {
    g_autoptr (GDateTime) t = parse_local (str (time, i));
    if (!t)
      continue;
    double tc = num (temp, i);
    g_autofree char *hl = vibeos_format_time (t);
    g_autofree char *value = fmt_temp (tc);
    add_slot (&st, hl, wx_icon ((int) num (code, i), num (isday, i) != 0), value, TRUE, tc, num (pop, i), TRUE);
    /* a sunrise or sunset within this hour follows it */
    g_autoptr (GDateTime) next = g_date_time_add_hours (t, 1);
    for (guint e = 0; e < events->len; e++) {
      GDateTime *at = events->pdata[e];
      if (g_date_time_compare (at, t) > 0 && g_date_time_compare (at, next) < 0) {
        g_autofree char *el = vibeos_format_time (at);
        double frac = g_date_time_difference (at, t) / (double) G_TIME_SPAN_HOUR, tn = num (temp, i + 1);
        gboolean rise = !g_strcmp0 (kinds->pdata[e], "Sunrise");
        add_slot (&st, el, rise ? "daytime-sunrise-symbolic" : "daytime-sunset-symbolic", _(kinds->pdata[e]), FALSE,
                  isnan (tn) ? tc : tc + (tn - tc) * frac, NAN, FALSE);
      }
    }
  }

  GtkWidget *curve = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (curve), CURVE_H);
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (curve), st.curve->temps->len * st.curve->col_w);
  int col_w = st.curve->col_w;
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (curve), draw_curve, st.curve, (GDestroyNotify) curve_free);
  GtkWidget *pops = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  for (guint i = 0; i < st.pops->len; i++) {
    gtk_widget_set_size_request (st.pops->pdata[i], col_w, -1);
    gtk_box_append (GTK_BOX (pops), st.pops->pdata[i]);
  }
  g_ptr_array_unref (st.pops);

  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (v), st.row);
  gtk_box_append (GTK_BOX (v), curve);
  gtk_box_append (GTK_BOX (v), pops);
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), v);
  return sc;
}

/* ---- the page ---- */

static GtkWidget *
card (void)
{
  GtkWidget *c = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (c, "wx-card");
  return c;
}

static const char *
uv_word (double uv)
{
  /* TRANSLATORS: the UV index's levels */
  return uv < 3 ? C_("uv", "Low") : uv < 6 ? C_("uv", "Moderate") : uv < 8 ? C_("uv", "High")
       : uv < 11 ? C_("uv", "Very high") : C_("uv", "Extreme");
}

static GtkWidget *
detail (const char *title, const char *value, const char *sub)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class (b, "wx-detail");
  gtk_widget_set_hexpand (b, TRUE);
  gtk_box_append (GTK_BOX (b), label (title, "wx-dim", 0));
  gtk_box_append (GTK_BOX (b), label (value, "wx-big", 0));
  if (sub)
    gtk_box_append (GTK_BOX (b), label (sub, "wx-small", 0));
  return b;
}

/* the skies made for the theme in effect: each is made the first time it shows */
static GdkTexture *made_skies[G_N_ELEMENTS (skies)];

static void
set_sky (Page *p, const char *sky)
{
  for (guint i = 0; i < G_N_ELEMENTS (skies); i++)
    if (!strcmp (skies[i].name, sky)) {
      if (!made_skies[i])
        made_skies[i] = sky_texture (&skies[i]);
      gtk_picture_set_paintable (GTK_PICTURE (p->sky), GDK_PAINTABLE (made_skies[i]));
      p->sky_name = skies[i].name;
      return;
    }
}

/* another theme colour, or light for dark: the skies are made again, in the new palettes */
static void
on_theme_changed (VibeosTheme *theme, Page *p)
{
  for (guint i = 0; i < G_N_ELEMENTS (made_skies); i++)
    g_clear_object (&made_skies[i]);
  set_sky (p, p->sky_name ? p->sky_name : "cloudy");
}

/* what the page shows goes; a forecast shown is laid out again from p->forecast */
static void
clear_body (Page *p)
{
  for (GtkWidget *ch; (ch = gtk_widget_get_first_child (p->body));)
    gtk_box_remove (GTK_BOX (p->body), ch);
}

/* ---- WxFit: the first of its layouts that fits the width ----
 *
 * The same content laid out more than one way, the roomiest first: WxFit shows the first whose least width fits,
 * and the last when none does. A large font size takes the week's rows and the details to their narrower forms
 * rather than past the window's edge. */

#define WX_TYPE_FIT (wx_fit_get_type ())
G_DECLARE_FINAL_TYPE (WxFit, wx_fit, WX, FIT, GtkWidget)

struct _WxFit {
  GtkWidget parent;
};

G_DEFINE_FINAL_TYPE (WxFit, wx_fit, GTK_TYPE_WIDGET)

/* the layout for a width: the first that fits, else the last */
static GtkWidget *
wx_fit_choose (GtkWidget *widget, int width)
{
  GtkWidget *child, *last = NULL;

  for (child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child)) {
    int min;

    gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &min, NULL, NULL, NULL);
    if (min <= width)
      return child;
    last = child;
  }
  return last;
}

static void
wx_fit_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                int *minimum_baseline, int *natural_baseline)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);

  *minimum = *natural = 0;
  if (!child)
    return;
  if (orientation == GTK_ORIENTATION_VERTICAL) {
    child = wx_fit_choose (widget, for_size < 0 ? G_MAXINT : for_size);
    gtk_widget_measure (child, orientation, for_size, minimum, natural, NULL, NULL);
    return;
  }
  /* as narrow as the narrowest, as wide as the roomiest would like */
  gtk_widget_measure (child, orientation, -1, NULL, natural, NULL, NULL);
  *minimum = G_MAXINT;
  for (; child; child = gtk_widget_get_next_sibling (child)) {
    int min;

    gtk_widget_measure (child, orientation, -1, &min, NULL, NULL, NULL);
    *minimum = MIN (*minimum, min);
  }
  *natural = MAX (*natural, *minimum);
}

static void
wx_fit_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  GtkWidget *chosen = wx_fit_choose (widget, width);

  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child)) {
    gtk_widget_set_child_visible (child, child == chosen);
    if (child == chosen)
      gtk_widget_allocate (child, width, height, -1, NULL);
  }
}

static GtkSizeRequestMode
wx_fit_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
wx_fit_dispose (GObject *object)
{
  for (GtkWidget *child; (child = gtk_widget_get_first_child (GTK_WIDGET (object)));)
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (wx_fit_parent_class)->dispose (object);
}

static void
wx_fit_class_init (WxFitClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = wx_fit_dispose;
  widget_class->measure = wx_fit_measure;
  widget_class->size_allocate = wx_fit_size_allocate;
  widget_class->get_request_mode = wx_fit_get_request_mode;
}

static void
wx_fit_init (WxFit *self)
{
}

/* the layouts, the roomiest first; NULL-terminated */
static GtkWidget *
wx_fit_new (GtkWidget *first, ...)
{
  GtkWidget *self = g_object_new (WX_TYPE_FIT, NULL);
  va_list args;

  va_start (args, first);
  for (GtkWidget *child = first; child; child = va_arg (args, GtkWidget *))
    gtk_widget_set_parent (child, self);
  va_end (args);
  return self;
}

static void
fill (Page *p, JsonObject *o)
{
  JsonObject *c = o && json_object_has_member (o, "current") ? json_object_get_object_member (o, "current") : NULL;
  JsonObject *h = o && json_object_has_member (o, "hourly") ? json_object_get_object_member (o, "hourly") : NULL;
  JsonObject *d = o && json_object_has_member (o, "daily") ? json_object_get_object_member (o, "daily") : NULL;
  if (!c || !h || !d)
    return;
  clear_body (p);
  p->shown = TRUE;
  /* the first column: now and the next hours; the second: yesterday, the week and the details. One under the other
   * in one column, side by side in two. */
  GtkWidget *col_a = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12), *col_b = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_hexpand (col_a, TRUE);
  gtk_widget_set_hexpand (col_b, TRUE);
  if (p->two_columns)
    gtk_widget_set_margin_top (col_b, BAR_CLEAR);
  gtk_box_append (GTK_BOX (p->body), col_a);
  gtk_box_append (GTK_BOX (p->body), col_b);

  int code = (int) cur (c, "weather_code");
  gboolean day = cur (c, "is_day") != 0;
  set_sky (p, wx_sky (code, day));
  JsonArray *dmax = arr (d, "temperature_2m_max"), *dmin = arr (d, "temperature_2m_min");
  JsonArray *dcode = arr (d, "weather_code"), *dpop = arr (d, "precipitation_probability_max");
  JsonArray *dtime = arr (d, "time"), *rise = arr (d, "sunrise"), *set = arr (d, "sunset"), *uv = arr (d, "uv_index_max");
  /* daily[0] is yesterday, daily[1] today */

  /* the head: place, temperature, what it is, today's high and low, feels like */
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (head, "wx-head");
  GtkWidget *place = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *pin = gtk_image_new_from_icon_name ("mark-location-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (pin), 22);
  gtk_box_append (GTK_BOX (place), pin);
  gtk_box_append (GTK_BOX (place), label (p->place ? p->place : "", "wx-place", 0));
  gtk_box_append (GTK_BOX (head), place);
  g_autofree char *now = fmt_temp (cur (c, "temperature_2m"));
  gtk_box_append (GTK_BOX (head), label (now, "wx-now", 0));
  gtk_box_append (GTK_BOX (head), label (wx_name (code), "wx-cond", 0));
  g_autofree char *hi = fmt_temp (num (dmax, 1)), *lo = fmt_temp (num (dmin, 1));
  g_autofree char *feels = fmt_temp (cur (c, "apparent_temperature"));
  /* TRANSLATORS: today's highest and lowest temperatures, then what it feels like now */
  g_autofree char *range = g_strdup_printf (_("↑%s / ↓%s\nFeels like %s"), hi, lo, feels);
  GtkWidget *rl = label (range, "wx-range", 0);
  gtk_widget_set_margin_top (rl, 70);
  gtk_box_append (GTK_BOX (head), rl);
  gtk_box_append (GTK_BOX (col_a), head);

  /* the next 24 hours */
  GtkWidget *hours = card ();
  /* TRANSLATORS: today's sky ("Partly cloudy"), then its highest and lowest temperatures ("12°") */
  g_autofree char *summary = g_strdup_printf (_("%s. High %s, low %s."), wx_name ((int) num (dcode, 1)), hi, lo);
  gtk_box_append (GTK_BOX (hours), label (summary, "wx-summary", 0));
  gtk_box_append (GTK_BOX (hours), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  JsonArray *htime = arr (h, "time");
  g_autoptr (GDateTime) dnow = g_date_time_new_now_local ();
  g_autofree char *hour_now = g_date_time_format (dnow, "%Y-%m-%dT%H:00");
  guint first = 0;
  for (guint i = 0; htime && i < json_array_get_length (htime); i++)
    if (!g_strcmp0 (str (htime, i), hour_now)) {
      first = i;
      break;
    }
  gtk_box_append (GTK_BOX (hours), hourly_strip (h, first, rise, set));
  gtk_box_append (GTK_BOX (col_a), hours);

  /* now against this hour yesterday */
  double then = num (arr (h, "temperature_2m"), first >= 24 ? first - 24 : G_MAXUINT);
  double diff = cur (c, "temperature_2m") - then;
  if (!isnan (diff)) {
    GtkWidget *cmp = card ();
    GtkWidget *t = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (t), gtk_image_new_from_icon_name ("temperature-symbolic"));
    gtk_box_append (GTK_BOX (t), label (_("Today's temperature"), "wx-dim", 0));
    gtk_box_append (GTK_BOX (cmp), t);
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    const char *say = fabs (diff) < 1 ? _("About the same as yesterday")
                    : diff < 0 ? (diff > -4 ? _("Temperatures a little lower than yesterday")
                                            : _("Much colder than yesterday"))
                               : (diff < 4 ? _("Temperatures a little higher than yesterday")
                                           : _("Much warmer than yesterday"));
    GtkWidget *sl = label (say, "wx-big", 0);
    gtk_widget_set_hexpand (sl, TRUE);
    gtk_box_append (GTK_BOX (row), sl);
    if (fabs (diff) >= 1) {
      g_autofree char *dd = g_strdup_printf ("%.0f°%s", fabs (diff) * (fahrenheit () ? 1.8 : 1), diff < 0 ? "↓" : "↑");
      gtk_box_append (GTK_BOX (row), label (dd, "wx-delta", 1));
    }
    gtk_box_append (GTK_BOX (cmp), row);
    gtk_box_append (GTK_BOX (col_b), cmp);
  }

  /* the week: yesterday dim, then today and on. A day on a row; with no room for that, the chance of rain and the
   * icons on a line of their own under the day */
  GtkWidget *week = card ();
  GtkWidget *grid = gtk_grid_new ();
  GtkWidget *narrow = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 18);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_grid_set_column_spacing (GTK_GRID (narrow), 10);
  for (guint i = 0; dtime && i < json_array_get_length (dtime); i++) {
    g_autofree char *noon = g_strconcat (str (dtime, i) ? str (dtime, i) : "", "T12:00", NULL);
    g_autoptr (GDateTime) dt = parse_local (noon);
    g_autofree char *name = i == 0 ? g_strdup (_("Yesterday")) : i == 1 ? g_strdup (_("Today"))
                          : dt ? vibeos_format_date (dt, "EEE") : g_strdup ("");
    g_autofree char *mx = fmt_temp (num (dmax, i)), *mn = fmt_temp (num (dmin, i));
    g_autofree char *temps = g_strdup_printf ("%s  %s", mx, mn);
    const char *cls = i == 0 ? "wx-yesterday" : "wx-day";
    GtkWidget *n = label (name, cls, 0);
    gtk_widget_set_hexpand (n, TRUE);
    gtk_grid_attach (GTK_GRID (grid), n, 0, i, 1, 1);
    n = label (name, cls, 0);
    gtk_widget_set_hexpand (n, TRUE);
    gtk_widget_set_margin_top (n, i ? 18 : 0);
    gtk_grid_attach (GTK_GRID (narrow), n, 0, 2 * i, 1, 1);
    if (i > 0) {
      int dc = (int) num (dcode, i);
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
      gtk_grid_attach (GTK_GRID (grid), drop_label (num (dpop, i), FALSE), 1, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), wx_image (wx_icon (dc, TRUE), 26), 2, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), wx_image (wx_icon (dc, FALSE), 26), 3, i, 1, 1);
      gtk_box_append (GTK_BOX (line), drop_label (num (dpop, i), FALSE));
      gtk_box_append (GTK_BOX (line), wx_image (wx_icon (dc, TRUE), 26));
      gtk_box_append (GTK_BOX (line), wx_image (wx_icon (dc, FALSE), 26));
      gtk_widget_set_halign (line, GTK_ALIGN_END);
      gtk_widget_set_margin_top (line, 6);
      gtk_grid_attach (GTK_GRID (narrow), line, 0, 2 * i + 1, 2, 1);
    }
    gtk_grid_attach (GTK_GRID (grid), label (temps, cls, 1), 4, i, 1, 1);
    GtkWidget *t = label (temps, cls, 1);
    gtk_widget_set_valign (t, GTK_ALIGN_END);
    gtk_grid_attach (GTK_GRID (narrow), t, 1, 2 * i, 1, 1);
  }
  gtk_box_append (GTK_BOX (week), wx_fit_new (grid, narrow, NULL));
  gtk_box_append (GTK_BOX (col_b), week);

  /* the details: two columns, or one with no room for two */
  GtkWidget *more = card ();
  GtkWidget *dg = gtk_grid_new ();
  GtkWidget *dl = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_grid_set_row_spacing (GTK_GRID (dg), 16);
  gtk_grid_set_column_homogeneous (GTK_GRID (dg), TRUE);
  g_autoptr (GDateTime) r = parse_local (str (rise, 1));
  g_autoptr (GDateTime) s = parse_local (str (set, 1));
  g_autofree char *rs = r ? vibeos_format_time (r) : g_strdup ("–");
  g_autofree char *ss = s ? vibeos_format_time (s) : g_strdup ("–");
  /* TRANSLATORS: under the sunrise's time; %s is the sunset's */
  g_autofree char *ssub = g_strdup_printf (_("Sunset %s"), ss);
  double u = num (uv, 1);
  g_autofree char *uvs = isnan (u) ? g_strdup ("–") : g_strdup_printf ("%.0f", u);
  g_autofree char *hum = g_strdup_printf ("%.0f%%", cur (c, "relative_humidity_2m"));
  /* the forecast's km/h and hPa in the region's units, with their names in the user's language */
  double w = cur (c, "wind_speed_10m"), pa = cur (c, "pressure_msl");
  g_autofree char *wind = isnan (w) ? g_strdup ("–") : vibeos_format_wind_speed (w);
  g_autofree char *pres = isnan (pa) ? g_strdup ("–") : vibeos_format_air_pressure (pa);
  gtk_grid_attach (GTK_GRID (dg), detail (_("Sunrise"), rs, ssub), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail (_("UV index"), uvs, isnan (u) ? NULL : uv_word (u)), 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail (_("Humidity"), hum, NULL), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail (_("Wind"), wind, NULL), 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (dg), detail (_("Pressure"), pres, NULL), 0, 2, 1, 1);
  gtk_box_append (GTK_BOX (dl), detail (_("Sunrise"), rs, ssub));
  gtk_box_append (GTK_BOX (dl), detail (_("UV index"), uvs, isnan (u) ? NULL : uv_word (u)));
  gtk_box_append (GTK_BOX (dl), detail (_("Humidity"), hum, NULL));
  gtk_box_append (GTK_BOX (dl), detail (_("Wind"), wind, NULL));
  gtk_box_append (GTK_BOX (dl), detail (_("Pressure"), pres, NULL));
  gtk_box_append (GTK_BOX (more), wx_fit_new (dg, dl, NULL));
  gtk_box_append (GTK_BOX (col_b), more);

  gtk_box_append (GTK_BOX (col_b), label ("Open-Meteo.com", "wx-credit", 0.5));
}

/* the forecast kept, and shown */
static void
show_forecast (Page *p, JsonNode *root)
{
  if (root != p->forecast) {
    g_clear_pointer (&p->forecast, json_node_unref);
    p->forecast = json_node_ref (root);
  }
  fill (p, json_node_get_object (root));
}

/* the page without a forecast, saying why */
static void
waiting (Page *p, const char *text)
{
  clear_body (p);
  g_clear_pointer (&p->forecast, json_node_unref);
  GtkWidget *message = label (text, "wx-summary", 0);
  gtk_widget_add_css_class (message, "wx-message");
  gtk_box_append (GTK_BOX (p->body), message);
  p->shown = FALSE;
}

/* ---- the forecast, fetched (curl, away from the main loop) and kept ---- */

static char *
cache_path (const char *name)
{
  return g_build_filename (g_get_user_cache_dir (), "vibeos-launcher", name, NULL);
}

static void
fetched (GObject *src, GAsyncResult *res, gpointer data)
{
  Page *p = data;
  g_autofree char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out;
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!ok || !json_parser_load_from_data (jp, out, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp))) {
    g_message ("Open-Meteo gave no answer");
    /* a forecast shown stays; a page waiting for one says it is not coming */
    if (!p->shown)
      waiting (p, _("No forecast: Open-Meteo did not answer. Tap reload to try again."));
    return;
  }
  g_autofree char *path = cache_path ("forecast.json");
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, out, -1, NULL);
  show_forecast (p, json_parser_get_root (jp));
}

static void
fetch (Page *p)
{
  char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (lat, sizeof lat, "%.4f", p->lat);
  g_ascii_formatd (lon, sizeof lon, "%.4f", p->lon);
  g_autofree char *url = g_strdup_printf (
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&past_days=1&forecast_days=8"
    "&current=temperature_2m,apparent_temperature,weather_code,is_day,relative_humidity_2m,wind_speed_10m,pressure_msl"
    "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,"
    "uv_index_max", lat, lon);
  const char *argv[] = { "curl", "-sf", "--max-time", "20", "-A", UA, url, NULL };
  GSubprocess *sp = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!sp)
    return;
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  g_subprocess_communicate_utf8_async (sp, NULL, NULL, fetched, p);
  g_object_unref (sp);
}

/* the cached forecast, if it is for here; TRUE when it is fresh too */
static gboolean
load_cache (Page *p)
{
  g_autofree char *path = cache_path ("forecast.json");
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!json_parser_load_from_file (jp, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp)))
    return FALSE;
  JsonObject *o = json_node_get_object (json_parser_get_root (jp));
  double lat = json_object_get_double_member_with_default (o, "latitude", 999);
  double lon = json_object_get_double_member_with_default (o, "longitude", 999);
  if (fabs (lat - p->lat) > 0.2 || fabs (lon - p->lon) > 0.2)
    return FALSE;
  show_forecast (p, json_parser_get_root (jp));
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileInfo) info = g_file_query_info (f, G_FILE_ATTRIBUTE_TIME_MODIFIED, 0, NULL, NULL);
  return info && g_get_real_time () / G_USEC_PER_SEC -
                   (gint64) g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) < STALE_S;
}

/* where: what the home screen's widget last showed */
static gboolean
load_place (Page *p)
{
  g_autofree char *path = cache_path ("weather.json");
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (!json_parser_load_from_file (jp, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp)))
    return FALSE;
  JsonObject *o = json_node_get_object (json_parser_get_root (jp));
  p->lat = json_object_get_double_member_with_default (o, "lat", 0);
  p->lon = json_object_get_double_member_with_default (o, "lon", 0);
  p->place = g_strdup (json_object_get_string_member_with_default (o, "place", ""));
  return p->have_place = p->lat || p->lon;
}

/* ---- where: GNOME Weather's first city, or GeoClue (city accuracy) named by Nominatim ---- */

#define GEO_BUS     "org.freedesktop.GeoClue2"
#define GEO_MANAGER "/org/freedesktop/GeoClue2/Manager"
#define GEO_CITY    4            /* GClueAccuracyLevel: city */
#define GEO_WAIT_S  30

static void
save_place (Page *p)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "lat");
  json_builder_add_double_value (b, p->lat);
  json_builder_set_member_name (b, "lon");
  json_builder_add_double_value (b, p->lon);
  json_builder_set_member_name (b, "place");
  json_builder_add_string_value (b, p->place ? p->place : "");
  json_builder_end_object (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  g_autofree char *data = json_generator_to_data (gen, NULL);
  g_autofree char *path = cache_path ("weather.json");
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, data, -1, NULL);
}

/* a place found: kept, and its forecast fetched unless the page shows this place's already (about 2 km) and that is
 * fresh; a new name alone is shown from the forecast on file. name NULL keeps the name, "" clears it. */
static void
use_place (Page *p, double lat, double lon, const char *name)
{
  gboolean moved = !p->have_place || fabs (lat - p->lat) > 0.02 || fabs (lon - p->lon) > 0.02;
  p->lat = lat;
  p->lon = lon;
  if (name) {                     /* NULL: the name stays */
    g_free (p->place);
    p->place = g_strdup (name);
  }
  p->have_place = TRUE;
  save_place (p);
  if (moved || !load_cache (p))
    fetch (p);
}

/* no place at all: say how to get one (a known place keeps its weather) */
static void
nowhere (Page *p)
{
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  if (p->have_place)
    return;
  waiting (p, _("No location. Turn on Location in Settings, then tap reload, or set a city in the ⋮ menu."));
}

typedef struct {
  Page  *p;
  double lat, lon;
} Naming;

static void
named (GObject *src, GAsyncResult *res, gpointer data)
{
  Naming *n = data;
  g_autofree char *out = NULL;
  const char *name = NULL;
  g_autoptr (JsonParser) jp = json_parser_new ();
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out &&
      json_parser_load_from_data (jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp))) {
    JsonObject *o = json_node_get_object (json_parser_get_root (jp));
    JsonObject *a = json_object_has_member (o, "address") ? json_object_get_object_member (o, "address") : NULL;
    const char *keys[] = { "suburb", "city_district", "village", "town", "city", "municipality", "county" };
    for (guint i = 0; a && !name && i < G_N_ELEMENTS (keys); i++)
      if (json_object_has_member (a, keys[i]))
        name = json_object_get_string_member (a, keys[i]);
  }
  /* a city chosen while the phone was being found wins over where it is */
  if (n->p->city) {
    g_free (n);
    return;
  }
  /* unnamed: the old name only if the phone is still there */
  gboolean near = n->p->have_place && fabs (n->lat - n->p->lat) <= 0.02 && fabs (n->lon - n->p->lon) <= 0.02;
  use_place (n->p, n->lat, n->lon, name ? name : near ? NULL : "");
  g_free (n);
}

/* the user's language, two letters, for places' names ("en" in the C locale) */
static char *
user_language (void)
{
  g_autofree char *lang = g_strndup (g_get_language_names ()[0], 2);
  return g_strdup (g_strcmp0 (lang, "C") ? lang : "en");
}

/* the place's name from Nominatim, in the user's language */
static void
name_place (Page *p, double lat, double lon)
{
  char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (la, sizeof la, "%.4f", lat);
  g_ascii_formatd (lo, sizeof lo, "%.4f", lon);
  g_autofree char *lang = user_language ();
  g_autofree char *url = g_strdup_printf ("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=14"
                                          "&lat=%s&lon=%s&accept-language=%s", la, lo, lang);
  const char *argv[] = { "curl", "-sf", "--max-time", "15", "-A", UA, url, NULL };
  GSubprocess *sp = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!sp) {
    use_place (p, lat, lon, NULL);
    return;
  }
  Naming *n = g_new0 (Naming, 1);
  n->p = p;
  n->lat = lat;
  n->lon = lon;
  g_subprocess_communicate_utf8_async (sp, NULL, NULL, named, n);
  g_object_unref (sp);
}

/* GeoClue drops a client whose owner leaves the bus; this ends one the app no longer needs */
static void
geo_stop (Page *p)
{
  if (p->geo_timeout)
    g_source_remove (p->geo_timeout);
  p->geo_timeout = 0;
  if (!p->geo_client)
    return;
  if (p->geo_signal)
    g_dbus_connection_signal_unsubscribe (p->system, p->geo_signal);
  p->geo_signal = 0;
  g_dbus_connection_call (p->system, GEO_BUS, p->geo_client, GEO_BUS ".Client", "Stop", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
  g_dbus_connection_call (p->system, GEO_BUS, GEO_MANAGER, GEO_BUS ".Manager", "DeleteClient",
                          g_variant_new ("(o)", p->geo_client), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
  g_clear_pointer (&p->geo_client, g_free);
}

static void
geo_failed (Page *p, const char *what, GError *error)
{
  g_message ("location: %s: %s", what, error ? error->message : "none");
  geo_stop (p);
  nowhere (p);
}

static void
on_location (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
             GVariant *params, gpointer data)
{
  Page *p = data;
  const char *old_path, *new_path;
  g_autoptr (GError) error = NULL;
  double lat, lon;

  g_variant_get (params, "(&o&o)", &old_path, &new_path);
  g_autoptr (GVariant) all = g_dbus_connection_call_sync (bus, GEO_BUS, new_path, "org.freedesktop.DBus.Properties",
                                                          "GetAll", g_variant_new ("(s)", GEO_BUS ".Location"),
                                                          G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000,
                                                          NULL, &error);
  if (!all) {
    geo_failed (p, "location", error);
    return;
  }
  g_autoptr (GVariant) props = g_variant_get_child_value (all, 0);
  if (!g_variant_lookup (props, "Latitude", "d", &lat) || !g_variant_lookup (props, "Longitude", "d", &lon)) {
    geo_failed (p, "location", NULL);
    return;
  }
  geo_stop (p);                   /* one fix is enough for a forecast */
  name_place (p, lat, lon);
}

static gboolean
geo_waited (gpointer data)
{
  Page *p = data;
  p->geo_timeout = 0;
  geo_failed (p, "no fix", NULL);
  return G_SOURCE_REMOVE;
}

static gboolean
geo_set (Page *p, const char *prop, GVariant *value, GError **error)
{
  g_autoptr (GVariant) ret = g_dbus_connection_call_sync (p->system, GEO_BUS, p->geo_client,
                                                          "org.freedesktop.DBus.Properties", "Set",
                                                          g_variant_new ("(ssv)", GEO_BUS ".Client", prop, value),
                                                          NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, error);
  return ret != NULL;
}

static void
on_geo_started (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!ret)
    geo_failed (data, "start", error);   /* location off, refused, or no agent to ask */
}

static void
on_geo_client (GObject *src, GAsyncResult *res, gpointer data)
{
  Page *p = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);

  p->geo_asked = FALSE;
  if (!ret) {
    geo_failed (p, "client", error);
    return;
  }
  g_variant_get (ret, "(o)", &p->geo_client);
  /* a city was chosen while GeoClue was asked: it is not needed */
  if (p->city) {
    geo_stop (p);
    return;
  }
  if (!geo_set (p, "DesktopId", g_variant_new_string ("vibeos.Weather"), &error) ||
      !geo_set (p, "RequestedAccuracyLevel", g_variant_new_uint32 (GEO_CITY), &error)) {
    geo_failed (p, "client setup", error);
    return;
  }
  p->geo_signal = g_dbus_connection_signal_subscribe (p->system, GEO_BUS, GEO_BUS ".Client", "LocationUpdated",
                                                      p->geo_client, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_location, p,
                                                      NULL);
  p->geo_timeout = g_timeout_add_seconds (GEO_WAIT_S, geo_waited, p);
  g_dbus_connection_call (p->system, GEO_BUS, p->geo_client, GEO_BUS ".Client", "Start", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_geo_started, p);
}

static void
locate (Page *p)
{
  if (p->geo_client || p->geo_asked)
    return;
  if (!p->system) {
    g_autoptr (GError) error = NULL;
    p->system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!p->system) {
      geo_failed (p, "system bus", error);
      return;
    }
  }
  p->geo_asked = TRUE;
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  g_dbus_connection_call (p->system, GEO_BUS, GEO_MANAGER, GEO_BUS ".Manager", "GetClient", NULL,
                          G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_geo_client, p);
}

/* The first city in GNOME Weather (org.gnome.Weather locations: serialized GWeatherLocations, (uv) with
 * (name, code, current, [(lat, lon)], [(parent lat, lon)]), radians): a place the user chose there comes first */
static gboolean
weather_app_city (Page *p)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = src ? g_settings_schema_source_lookup (src, "org.gnome.Weather", TRUE) : NULL;
  if (!schema || !g_settings_schema_has_key (schema, "locations"))
    return FALSE;
  g_autoptr (GSettings) settings = g_settings_new_full (schema, NULL, NULL);
  g_autoptr (GVariant) all = g_settings_get_value (settings, "locations");
  if (!g_variant_is_of_type (all, G_VARIANT_TYPE ("av")) || g_variant_n_children (all) == 0)
    return FALSE;
  g_autoptr (GVariant) first = g_variant_get_child_value (all, 0);     /* v */
  g_autoptr (GVariant) loc = g_variant_get_variant (first);              /* (uv) */
  if (!g_variant_is_of_type (loc, G_VARIANT_TYPE ("(uv)")))
    return FALSE;
  g_autoptr (GVariant) inner = NULL;
  guint32 version;
  g_variant_get (loc, "(uv)", &version, &inner);
  if (!g_variant_is_of_type (inner, G_VARIANT_TYPE ("(ssba(dd)a(dd))")))
    return FALSE;
  const char *name, *code;
  gboolean current;
  g_autoptr (GVariantIter) own = NULL, parent = NULL;
  g_variant_get (inner, "(&s&sba(dd)a(dd))", &name, &code, &current, &own, &parent);
  double lat, lon;
  if (!g_variant_iter_next (own, "(dd)", &lat, &lon) && !g_variant_iter_next (parent, "(dd)", &lat, &lon))
    return FALSE;
  use_place (p, lat * 180 / G_PI, lon * 180 / G_PI, name);
  return TRUE;
}

/* where the page is for: a chosen city stays, and is shown already; otherwise GNOME Weather's or the phone's */
static void
where (Page *p)
{
  if (p->city)
    return;
  if (!weather_app_city (p))
    locate (p);
}

/* the city chosen in Set City, from vibeos.Weather: the page is for it, at once */
static gboolean
load_city (Page *p)
{
  const char *name;
  double lat, lon;

  if (!p->settings)
    return FALSE;
  g_autoptr (GVariant) city = g_settings_get_value (p->settings, "city");
  g_variant_get (city, "(&sdd)", &name, &lat, &lon);
  if (!*name)
    return FALSE;
  p->city = g_strdup (name);
  g_free (p->place);
  p->place = g_strdup (name);
  p->lat = lat;
  p->lon = lon;
  p->have_place = TRUE;
  save_place (p);
  return TRUE;
}

/* a city chosen in Set City: kept, and shown instead of wherever the page was for; GeoClue stops looking */
static void
choose_city (Page *p, const char *name, double lat, double lon)
{
  gboolean far = !p->have_place || fabs (lat - p->lat) > 0.02 || fabs (lon - p->lon) > 0.02;

  /* the spinner was GeoClue's, if it was looking; the forecast's fetch starts its own */
  geo_stop (p);
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  g_free (p->city);
  p->city = g_strdup (name);
  if (p->settings)
    g_settings_set (p->settings, "city", "(sdd)", name, lat, lon);
  /* another place's forecast does not stay up while this one's comes */
  if (far) {
    g_autofree char *text = g_strdup_printf (_("Getting the weather for %s…"), name);
    waiting (p, text);
  }
  use_place (p, lat, lon, name);
}

/* "Use my location": no chosen city; the page goes back to GNOME Weather's first city or to where the phone is */
static void
use_my_location (Page *p)
{
  g_clear_pointer (&p->city, g_free);
  if (p->settings)
    g_settings_reset (p->settings, "city");
  where (p);
}

static void
page_free (Page *p)
{
  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), p);
  g_signal_handlers_disconnect_by_data (vibeos_text_get_default (), p);
  g_clear_pointer (&p->forecast, json_node_unref);
  geo_stop (p);
  g_clear_object (&p->system);
  g_clear_object (&p->settings);
  g_free (p->city);
  g_free (p->place);
  g_free (p);
}

/* ---- Set City: a sheet with a field, and the cities matching what is typed, from Open-Meteo's geocoding ----
 *
 * A bottom sheet as tall as the window lets it be, the field at its top (a card in the middle on a tablet: see
 * fit_search). The on-screen keyboard shrinks the window it opens over (the shell gives the window the space above
 * the keyboard), and the sheet with it: the field stays where it is, the list of cities under it gets shorter and
 * scrolls. A search waits for a pause in the typing; an answer for anything but the latest search is dropped, and
 * the search it was for stopped. */

#define SEARCH_DELAY_MS 250     /* the pause in the typing that starts a search */
#define CARD_WIDTH      480     /* a larger window (a tablet's): the sheet is a card in the middle, this wide, */
#define CARD_LIST       400     /* with its list this tall: five cities, the card one size whatever is found */
#define CARD_BAR        56      /* the card's bar, a compact VibeosTopBar */
#define SEARCH_MIN      2       /* the characters a search needs: fewer would match half the world */
#define SEARCH_COUNT    8

typedef struct {
  char  *name;
  double lat, lon;
} City;

typedef struct {
  Page        *p;
  GtkWidget   *sheet, *field, *text, *clear, *status, *list, *scroll;
  GArray      *cities;          /* City: the rows', in order */
  guint        delay;           /* the pause before the search */
  guint        serial;          /* the latest search; an answer to an older one is dropped */
  GSubprocess *curl;            /* the latest search, while it runs */
  gboolean     gone;            /* the sheet is: answers are dropped */
} CitySearch;                   /* a g_rc_box: the sheet holds it, and each search running */

typedef struct {
  CitySearch *search;
  guint       serial;
} Query;

static void
city_clear (gpointer data)
{
  g_free (((City *) data)->name);
}

static void
search_clear (gpointer data)
{
  CitySearch *s = data;

  g_array_unref (s->cities);
  g_clear_object (&s->curl);
}

static void
search_release (gpointer data)
{
  g_rc_box_release_full (data, search_clear);
}

/* the search running, or about to, is not: an answer that still comes is an old one */
static void
search_stop (CitySearch *s)
{
  s->serial++;
  g_clear_handle_id (&s->delay, g_source_remove);
  if (s->curl)
    g_subprocess_force_exit (s->curl);
  g_clear_object (&s->curl);
}

/* The sheet's size. A bottom sheet (a phone's) is as tall as the window lets it be. On a larger window libvibeos
 * floats the sheet as a card in the middle, 360 wide and here as tall as the window: a narrow column, empty under
 * the field until something is typed. The card is CARD_WIDTH wide instead and as tall as its list's CARD_LIST,
 * less on a window too short for that (the keyboard is up), and it keeps that size as cities come and go: the
 * status, when it shows, takes its height from the list's. */
static void
fit_search (CitySearch *s)
{
  VibeosLayout *layout = vibeos_layout_get_for_window (GTK_WINDOW (s->p->win));
  int width = vibeos_layout_get_width (layout), height = vibeos_layout_get_height (layout);
  /* libvibeos's floating sheet: on a window over 450 x 360, with these margins at its top and bottom */
  gboolean card = width > 450 && height > 360;
  double ends = 20 + 100 * MAX (0, (height - 720) / 720.0);
  int field = 0, status = 0, room;

  vibeos_sheet_set_full_height (VIBEOS_SHEET (s->sheet), !card);
  vibeos_sheet_set_content_width (VIBEOS_SHEET (s->sheet), card ? CARD_WIDTH : 0);
  if (!card)
    {
      gtk_widget_set_size_request (s->scroll, -1, -1);
      return;
    }
  gtk_widget_measure (s->field, GTK_ORIENTATION_VERTICAL, CARD_WIDTH, NULL, &field, NULL, NULL);
  if (gtk_widget_get_visible (s->status))
    gtk_widget_measure (s->status, GTK_ORIENTATION_VERTICAL, CARD_WIDTH, NULL, &status, NULL, NULL);
  room = MIN (height - (int) (2 * ends) - CARD_BAR - field, CARD_LIST);
  gtk_widget_set_size_request (s->scroll, -1, MAX (room - status, 0));
}

static void
on_search_layout (VibeosLayout *layout, CitySearch *s)
{
  fit_search (s);
}

static void
show_status (CitySearch *s, const char *text)
{
  gtk_label_set_text (GTK_LABEL (s->status), text ? text : "");
  gtk_widget_set_visible (s->status, text != NULL);
  fit_search (s);
}

static void
clear_rows (CitySearch *s)
{
  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (s->list));
  g_array_set_size (s->cities, 0);
}

static void
on_city_row (VibeosListRow *row, CitySearch *s)
{
  City *c = &g_array_index (s->cities, City, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "wx-city")));

  choose_city (s->p, c->name, c->lat, c->lon);
  vibeos_sheet_close (VIBEOS_SHEET (s->sheet));
}

static void
on_here_row (VibeosListRow *row, CitySearch *s)
{
  use_my_location (s->p);
  vibeos_sheet_close (VIBEOS_SHEET (s->sheet));
}

/* too little typed to search: no cities; while a city is chosen, the way back to where the phone is */
static void
show_idle (CitySearch *s)
{
  clear_rows (s);
  show_status (s, NULL);
  if (s->p->city) {
    /* TRANSLATORS: under "Use my location"; %s is the city chosen now */
    g_autofree char *now = g_strdup_printf (_("Instead of %s"), s->p->city);
    GtkWidget *row = vibeos_list_row_new (_("Use my location"), now);
    vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "find-location-symbolic");
    g_signal_connect (row, "clicked", G_CALLBACK (on_here_row), s);
    vibeos_card_list_append (VIBEOS_CARD_LIST (s->list), row);
  }
}

/* under a city's name, where it is: "region, country", each said once */
static char *
city_where (JsonObject *o, const char *name)
{
  const char *region = json_object_get_string_member_with_default (o, "admin1", NULL);
  const char *country = json_object_get_string_member_with_default (o, "country", NULL);
  GString *where = g_string_new (NULL);

  if (region && *region && g_strcmp0 (region, name))
    g_string_append (where, region);
  if (country && *country && g_strcmp0 (country, region) && g_strcmp0 (country, name))
    g_string_append_printf (where, "%s%s", where->len ? ", " : "", country);
  return g_string_free (where, FALSE);
}

/* the answer: {"results": [{"name", "latitude", "longitude", "admin1", "country", ...}, ...]}, no results when
 * nothing matches; NULL when there was no answer */
static void
show_cities (CitySearch *s, JsonObject *answer)
{
  JsonArray *results = arr (answer, "results");

  clear_rows (s);
  if (!answer) {
    show_status (s, _("No answer. Check the connection, then type again."));
    return;
  }
  for (guint i = 0; results && i < json_array_get_length (results); i++) {
    JsonObject *o = json_array_get_object_element (results, i);
    if (!o || !json_object_has_member (o, "latitude") || !json_object_has_member (o, "longitude"))
      continue;
    const char *name = json_object_get_string_member_with_default (o, "name", "");
    if (!*name)
      continue;
    City c = { g_strdup (name), json_object_get_double_member (o, "latitude"),
               json_object_get_double_member (o, "longitude") };
    g_autofree char *where = city_where (o, name);
    GtkWidget *row = vibeos_list_row_new (name, *where ? where : NULL);
    g_object_set_data (G_OBJECT (row), "wx-city", GUINT_TO_POINTER (s->cities->len));
    g_array_append_val (s->cities, c);
    g_signal_connect (row, "clicked", G_CALLBACK (on_city_row), s);
    vibeos_card_list_append (VIBEOS_CARD_LIST (s->list), row);
  }
  show_status (s, s->cities->len ? NULL : _("No cities found"));
}

static void
on_cities (GObject *src, GAsyncResult *res, gpointer data)
{
  Query *q = data;
  CitySearch *s = q->search;
  g_autofree char *out = NULL;
  g_autoptr (JsonParser) jp = json_parser_new ();
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out &&
                json_parser_load_from_data (jp, out, -1, NULL) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (jp));

  if (!s->gone && q->serial == s->serial) {
    g_clear_object (&s->curl);
    show_cities (s, ok ? json_node_get_object (json_parser_get_root (jp)) : NULL);
  }
  search_release (s);
  g_free (q);
}

static gboolean
search_now (gpointer data)
{
  CitySearch *s = data;
  g_autofree char *typed = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (s->text))));
  g_autofree char *name = g_uri_escape_string (typed, NULL, FALSE);
  g_autofree char *lang = user_language ();
  g_autofree char *url = g_strdup_printf ("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d"
                                          "&language=%s&format=json", name, SEARCH_COUNT, lang);
  const char *argv[] = { "curl", "-sf", "--max-time", "15", "-A", UA, url, NULL };

  s->delay = 0;
  s->curl = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (!s->curl) {
    show_cities (s, NULL);
    return G_SOURCE_REMOVE;
  }
  Query *q = g_new (Query, 1);
  q->search = g_rc_box_acquire (s);
  q->serial = s->serial;
  g_subprocess_communicate_utf8_async (s->curl, NULL, NULL, on_cities, q);
  return G_SOURCE_REMOVE;
}

/* each change: the search before it is stopped; one starts once the typing pauses. The cities shown stay until the
 * new ones come. */
static void
on_typed (GtkEditable *text, CitySearch *s)
{
  g_autofree char *typed = g_strstrip (g_strdup (gtk_editable_get_text (text)));

  gtk_widget_set_visible (s->clear, *gtk_editable_get_text (text) != '\0');
  search_stop (s);
  if (g_utf8_strlen (typed, -1) < SEARCH_MIN) {
    show_idle (s);
    return;
  }
  s->delay = g_timeout_add (SEARCH_DELAY_MS, search_now, s);
}

/* Enter: the first city found */
static void
on_enter (GtkText *text, CitySearch *s)
{
  City *c = s->cities->len > 0 ? &g_array_index (s->cities, City, 0) : NULL;

  if (!c)
    return;
  choose_city (s->p, c->name, c->lat, c->lon);
  vibeos_sheet_close (VIBEOS_SHEET (s->sheet));
}

static void
on_clear (VibeosPressable *button, CitySearch *s)
{
  gtk_editable_set_text (GTK_EDITABLE (s->text), "");
  gtk_widget_grab_focus (s->text);
}

static void
on_search_destroy (GtkWidget *sheet, CitySearch *s)
{
  s->gone = TRUE;
  search_stop (s);
  g_signal_handlers_disconnect_by_func (vibeos_layout_get_for_window (GTK_WINDOW (s->p->win)), on_search_layout, s);
}

/* the ⋮ menu's Set City */
static void
set_city (gpointer data)
{
  Page *p = data;
  CitySearch *s = g_rc_box_new0 (CitySearch);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("system-search-symbolic");
  GtkWidget *scroll = spongy_scroll_view_new ();

  s->p = p;
  s->field = field;
  s->scroll = scroll;
  s->cities = g_array_new (FALSE, TRUE, sizeof (City));
  g_array_set_clear_func (s->cities, city_clear);
  s->sheet = vibeos_sheet_new (_("Set City"));
  g_object_set_data_full (G_OBJECT (s->sheet), "wx-city-search", s, search_release);
  g_signal_connect (s->sheet, "destroy", G_CALLBACK (on_search_destroy), s);

  /* the field: a pill with the magnifier, the text and, once there is text, a button that clears it */
  gtk_widget_add_css_class (field, "wx-city-field");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "wx-city-icon");
  gtk_box_append (GTK_BOX (field), icon);
  s->text = gtk_text_new ();
  gtk_widget_add_css_class (s->text, "vibeos-search-entry");
  gtk_widget_set_hexpand (s->text, TRUE);
  gtk_widget_set_valign (s->text, GTK_ALIGN_CENTER);
  gtk_text_set_placeholder_text (GTK_TEXT (s->text), _("City name"));
  gtk_text_set_input_hints (GTK_TEXT (s->text),
                            GTK_INPUT_HINT_UPPERCASE_WORDS | GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI);
  gtk_accessible_update_property (GTK_ACCESSIBLE (s->text), GTK_ACCESSIBLE_PROPERTY_LABEL, _("City name"), -1);
  g_signal_connect (s->text, "changed", G_CALLBACK (on_typed), s);
  g_signal_connect (s->text, "activate", G_CALLBACK (on_enter), s);
  gtk_box_append (GTK_BOX (field), s->text);
  s->clear = vibeos_icon_button_new ("edit-clear-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (s->clear), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Clear"), -1);
  gtk_widget_set_visible (s->clear, FALSE);
  g_signal_connect (s->clear, "clicked", G_CALLBACK (on_clear), s);
  gtk_box_append (GTK_BOX (field), s->clear);
  gtk_box_append (GTK_BOX (content), field);

  /* why no cities show, when none do */
  s->status = label (NULL, "wx-city-status", 0);
  gtk_widget_set_visible (s->status, FALSE);
  gtk_box_append (GTK_BOX (content), s->status);

  /* the cities, under the field, scrolling */
  s->list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (s->list), 8, 16);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), s->list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (content), scroll);

  vibeos_sheet_set_content (VIBEOS_SHEET (s->sheet), content);
  show_idle (s);
  g_signal_connect (vibeos_layout_get_for_window (GTK_WINDOW (p->win)), "changed", G_CALLBACK (on_search_layout), s);
  vibeos_sheet_present (VIBEOS_SHEET (s->sheet), p->win);
  /* typing at once: the keyboard comes up for a field focused right after a tap */
  gtk_widget_grab_focus (s->text);
}

/* ---- About ---- */

/* where the page's data comes from, and on what terms (the About sheet's Legal page) */
static const VibeosAboutSection wx_legal[] = {
  /* TRANSLATORS: keep the links' markup as it is */
  { "Open-Meteo",
    N_("Weather forecasts by <a href=\"https://open-meteo.com/\">Open-Meteo.com</a>, under "
       "<a href=\"https://creativecommons.org/licenses/by/4.0/\">CC BY 4.0</a>.\n\n"
       "Set City finds cities with Open-Meteo's geocoding, on data from "
       "<a href=\"https://www.geonames.org/\">GeoNames</a>, under CC BY 4.0.") },
  { "OpenStreetMap",
    N_("The place's name, where the phone is, from OpenStreetMap's "
       "<a href=\"https://nominatim.openstreetmap.org/\">Nominatim</a>. Data © "
       "<a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap contributors</a>, under the "
       "<a href=\"https://opendatacommons.org/licenses/odbl/\">Open Database License</a> (ODbL).") },
  { NULL },
};

static void
about (gpointer data)
{
  Page *p = data;
  const VibeosAboutInfo info = { .name = N_("Weather"), .icon_name = "vibeos.Weather", .legal = wx_legal };

  vibeos_about_present (p->win, &info);
}

/* ---- the window ---- */

/* ---- the page's width: one column, or two on a wide window ----
 *
 * WxClamp holds the page: as wide as the window up to its max, and in the middle of the window past that. */

#define WX_TYPE_CLAMP (wx_clamp_get_type ())
G_DECLARE_FINAL_TYPE (WxClamp, wx_clamp, WX, CLAMP, GtkWidget)

struct _WxClamp {
  GtkWidget  parent;
  GtkWidget *child;
  int        max;               /* dp; G_MAXINT for none */
};

G_DEFINE_FINAL_TYPE (WxClamp, wx_clamp, GTK_TYPE_WIDGET)

static void
wx_clamp_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  WxClamp *self = WX_CLAMP (widget);

  if (orientation == GTK_ORIENTATION_VERTICAL && for_size >= 0)
    for_size = MIN (for_size, self->max);
  gtk_widget_measure (self->child, orientation, for_size, minimum, natural, NULL, NULL);
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *natural = MAX (*minimum, MIN (*natural, self->max));
}

static void
wx_clamp_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  WxClamp *self = WX_CLAMP (widget);
  int min, w;

  gtk_widget_measure (self->child, GTK_ORIENTATION_HORIZONTAL, -1, &min, NULL, NULL, NULL);
  w = MIN (width, MAX (self->max, min));
  gtk_widget_allocate (self->child, w, height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - w) / 2, 0)));
}

static GtkSizeRequestMode
wx_clamp_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
wx_clamp_dispose (GObject *object)
{
  WxClamp *self = WX_CLAMP (object);

  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (wx_clamp_parent_class)->dispose (object);
}

static void
wx_clamp_class_init (WxClampClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = wx_clamp_dispose;
  widget_class->measure = wx_clamp_measure;
  widget_class->size_allocate = wx_clamp_size_allocate;
  widget_class->get_request_mode = wx_clamp_get_request_mode;
}

static void
wx_clamp_init (WxClamp *self)
{
  self->max = G_MAXINT;
}

static GtkWidget *
wx_clamp_new (GtkWidget *child)
{
  WxClamp *self = g_object_new (WX_TYPE_CLAMP, NULL);

  self->child = child;
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

static void
wx_clamp_set_max (WxClamp *self, int max)
{
  if (self->max == max)
    return;
  self->max = max;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

/* a phone upright: one column, the window's width; wider (a tablet, a phone on its side): one column of at most
 * COLUMN_MAX in the middle; as wide as a turned tablet: two columns. The forecast is laid out again when that
 * changes. */
static void
update_columns (Page *p)
{
  VibeosLayout *layout = vibeos_layout_get_for_window (GTK_WINDOW (p->win));
  VibeosWidthClass width = vibeos_layout_get_width_class (layout);
  gboolean two = width == VIBEOS_WIDTH_EXPANDED;

  wx_clamp_set_max (WX_CLAMP (p->clamp), width == VIBEOS_WIDTH_COMPACT ? G_MAXINT : two ? PAGE_MAX : COLUMN_MAX);
  if (two == p->two_columns)
    return;
  p->two_columns = two;
  gtk_orientable_set_orientation (GTK_ORIENTABLE (p->body), two ? GTK_ORIENTATION_HORIZONTAL
                                                                : GTK_ORIENTATION_VERTICAL);
  gtk_box_set_homogeneous (GTK_BOX (p->body), two);
  gtk_box_set_spacing (GTK_BOX (p->body), two ? 16 : 12);
  /* the second column's top differs between the two */
  if (p->forecast)
    fill (p, json_node_get_object (p->forecast));
}

static void
on_layout_changed (VibeosLayout *layout, Page *p)
{
  update_columns (p);
}

/* a new font size: the hourly strip's columns are as wide as the new text needs */
static void
on_text_changed (VibeosText *text, Page *p)
{
  if (p->forecast)
    fill (p, json_node_get_object (p->forecast));
}

/* reload: the forecast for the place shown, and where the phone is now (a chosen city stays) */
static void
refresh_clicked (GtkWidget *b, Page *p)
{
  if (p->have_place)
    fetch (p);
  where (p);
}

/* ⋮: the menu, over the button */
static void
more_clicked (GtkWidget *button, Page *p)
{
  WxMenu *menu = wx_menu_new ();

  wx_menu_append (menu, _("Set City"), set_city, p);
  wx_menu_append (menu, _("About"), about, p);
  wx_menu_present (menu, button);
}

/* back (Alt+Left from VibeOS's edge swipe) and Escape close the page, back to the home screen; while the menu or a
 * sheet shows, they close that instead (their own keys come first: the focus is in them) */
static void
add_back (GtkWidget *win)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (keys), GTK_SHORTCUT_SCOPE_GLOBAL);
  GtkShortcutAction *close = gtk_named_action_new ("window.close");
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      close));
  gtk_widget_add_controller (win, keys);
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }

  Page *p = g_new0 (Page, 1);
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = schemas ? g_settings_schema_source_lookup (schemas, "vibeos.Weather", TRUE)
                                               : NULL;
  /* without its schema (run from the build tree) a chosen city lasts as long as the app */
  p->settings = schema ? g_settings_new_full (schema, NULL, NULL) : NULL;
  GtkWidget *win = p->win = vibeos_window_new (app);
  g_object_set_data_full (G_OBJECT (win), "page", p, (GDestroyNotify) page_free);
  gtk_window_set_title (GTK_WINDOW (win), _("Weather"));
  gtk_widget_add_css_class (win, "wx");
  add_back (win);

  /* the sky under everything, the page scrolling over it */
  GtkWidget *overlay = gtk_overlay_new ();
  p->sky = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (p->sky), GTK_CONTENT_FIT_FILL);
  gtk_picture_set_can_shrink (GTK_PICTURE (p->sky), TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), p->sky);
  set_sky (p, "cloudy");
  p->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (p->body, "wx-body");
  p->clamp = wx_clamp_new (p->body);
  GtkWidget *sc = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (sc), p->clamp);
  gtk_widget_set_vexpand (sc, TRUE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), sc);

  /* reload and the ⋮ menu float over the sky, top right; back is the system's (the swipe, or closing the app) */
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (bar, GTK_ALIGN_START);
  gtk_widget_set_halign (bar, GTK_ALIGN_END);
  gtk_widget_add_css_class (bar, "wx-bar");
  p->spinner = gtk_spinner_new ();
  gtk_widget_add_css_class (p->spinner, "wx-spinner");
  GtkWidget *reload = vibeos_icon_button_new ("vibeos-refresh-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (reload), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Reload"), -1);
  g_signal_connect (reload, "clicked", G_CALLBACK (refresh_clicked), p);
  GtkWidget *more = vibeos_icon_button_new ("vibeos-more-symbolic");
  /* where every top bar has its ⋮ (vibeos-top-bar.h) */
  gtk_widget_add_css_class (more, "vibeos-top-bar-end");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  g_signal_connect (more, "clicked", G_CALLBACK (more_clicked), p);
  gtk_box_append (GTK_BOX (bar), p->spinner);
  gtk_box_append (GTK_BOX (bar), reload);
  gtk_box_append (GTK_BOX (bar), more);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), bar);
  /* the menu and the sheets go over everything, in the window's root overlay */
  vibeos_window_set_content (GTK_WINDOW (win), overlay);

  /* the layout before the window shows: it starts at the size it will have, in the right columns */
  g_signal_connect (vibeos_layout_get_for_window (GTK_WINDOW (win)), "changed", G_CALLBACK (on_layout_changed), p);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), p);
  g_signal_connect (vibeos_text_get_default (), "changed", G_CALLBACK (on_text_changed), p);
  update_columns (p);

  gtk_window_present (GTK_WINDOW (win));
  /* the chosen city, or the last place, at once; then where the phone is now, without a chosen city */
  if (load_city (p) || load_place (p)) {
    if (!load_cache (p))
      fetch (p);
  } else
    waiting (p, _("Finding where you are…"));
  where (p);
}

/* libvibeos's colours, font, text sizes and icons, for the whole page */
static void
startup (GApplication *app, gpointer data)
{
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_resource ("/vibeos/Weather/style.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  g_autoptr (GtkApplication) app = gtk_application_new ("vibeos.Weather", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "startup", G_CALLBACK (startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
