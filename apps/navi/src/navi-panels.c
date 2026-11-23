/* navi-panels: everything over the map. See navi-app.h.
 *
 * Waze's layout, in Navi's own drawing: on a phone a bottom sheet over the map (its handle on top; swiped up it
 * shows more, swiped down less), the menu button top left, the re-centre button bottom left once the map was moved;
 * while driving a black banner on top with the next turn, the speed and the street the car is on at the bottom, and
 * the bar with the arrival time. On a tablet the map fills all of the window but the rail, and the sheet's pages are
 * a card floating at its bottom left, shown only for a place, the routes, the drive and the arrival: browsing the
 * map shows nothing over it, and Search is in the rail. Search opens as a card over the map too.
 *
 * The colours are Waze's own scheme, not the system's: blue buttons, a black banner with the street in cyan; light
 * by day, dark by night (the window's "night" class). */
#include "navi-app.h"
#include "navi-draw.h"
#include "navi-plan.h"
#include "navi-poi.h"
#include "navi-roadinfo.h"
#include "navi-voice.h"
#ifdef HAVE_DICTATION
#include "dictate-engine.h"
#include "dictate-models.h"
#ifdef HAVE_PIPER
#include "dictate-download-dialog.h"
#endif

#include <gst/gst.h>
#endif

#include <glib/gi18n.h>
#include <math.h>
#include <spongy-gtk.h>
#include <string.h>

#define COUNTDOWN_S     10
#define SEARCH_DELAY_MS 250
#define TABLET_CARD_DP  416 /* a tablet's cards over the map: the panel's, the search's (the routes' buttons need 416) */

typedef enum { CHOOSE_NONE, CHOOSE_HOME, CHOOSE_WORK } Choosing;

struct _NaviPanels {
  Navi *navi;
  NaviVoice *voice;
  GtkWidget *sheet;        /* the bottom sheet (phone) or the pane's box (tablet) */
  GtkWidget *handle;
  GtkWidget *stack;
  gboolean expanded;

  /* over the map */
  GtkWidget *menu_button, *recenter_button, *compass, *compass_area, *note;
  GtkWidget *routes_header, *routes_header_to;
  GtkWidget *banner, *banner_turn, *banner_distance, *banner_street, *then_box, *then_turn, *banner_note;
  GtkWidget *banner_side;   /* "Your destination is on the right", the last metres */
  GtkWidget *banner_lanes;  /* the lanes up to the turn */
  NaviLane lanes[16];       /* what it draws */
  guint n_lanes;
  GtkWidget *speedo, *speedo_value, *speedo_unit, *street_pill, *street_label, *sound_button;
  GtkWidget *speed_column;  /* the alert card, the limit sign and the speedometer, bottom left */
  GtkWidget *limit_sign, *alert_card, *alert_title, *alert_detail;
  int limit_kmh;            /* the road's (NAVI_LIMIT_*), shown on the sign */
  double speed;             /* m/s, < 0 unknown */
  gboolean mph;             /* speeds in miles an hour: the units setting's, else the road's country's (else the
                             * locale's); distances follow */
  char units_country[4];    /* the road's country last heard, "" none yet */
  gboolean us_signs;        /* the limit on a US-style board */
  gboolean moving;          /* driving without a route: the speedometer shows while browsing too */
  gint64 slow_since;
  gboolean notice_open;
  double bearing;

  /* browse */
  GtkWidget *where;

  /* the search */
  GtkWidget *search_view, *search_list, *search_status;
  GtkWidget *search_box;   /* what scrolls in the search: room under it for the keyboard */
  GDBusProxy *shell;       /* vibeos.Shell: how much of the window the keyboard covers (KeyboardOverlap) */
  GCancellable *shell_cancel;
  guint keyboard;          /* that: logical px from the window's bottom, 0 when the keyboard is down */
  GtkWidget *search_host;  /* a tablet's: the card over the map the search opens in */
  GtkWidget *place_star;   /* the place card's Save: a star, filled once the place is saved */
  NaviPlace *saved_target; /* the saved place a row's menu was opened for */
  GObject *dictation;      /* the search's microphone (a DictateEngine), NULL without a speech model */
  GtkWidget *search_mic;
  GString *dictated;       /* the sentences heard since the microphone was tapped */
  GCancellable *search_cancel;
  guint search_source;
  NaviCategory category;
  char *category_text;
  Choosing choosing;
  GHashTable *markers;     /* "fuel@30x2" -> GdkTexture: the points of interest's markers, for the widgets */
  struct Along *along;     /* a category's search along the route, while driving */

  /* a place */
  GtkWidget *place_name, *place_detail, *place_distance, *place_go, *place_go_label, *place_icon;
  double place_drive_m, place_drive_s; /* the drive there by road, 0 while unknown */
  NaviPlace *place;

  /* the routes */
  GtkWidget *routes_box, *routes_list, *routes_more, *routes_status, *routes_spinner, *routes_from;
  GtkWidget *leave_note, *leave_label, *leave_cancel; /* the reminder to leave for this place, when there is one */
  GtkWidget *routes_retry;
  GtkWidget *go_button, *go_fill;
  double countdown;        /* 0 to 1, or < 0 when not counting */
  guint countdown_tick;
  gint64 countdown_t0;
  gboolean plan_ahead;          /* the place shown has a reminder still to come: no counting down to Go */
  guint shown_selected;    /* the route selected when the list was last made */

  /* driving */
  GtkWidget *drive_eta, *drive_sub, *drive_main, *drive_free, *drive_free_sub, *drive_more;
  GtkWidget *drive_dest, *drive_via;
  GtkWidget *drive_progress;
  double progress_fraction;

  /* arrived */
  GtkWidget *arrived_name, *arrived_side;

  int sheet_height;
};

static const char *const CSS =
  /* the sheet */
  ".navi-sheet { background: #FFFFFF; border-radius: 18dp 18dp 0 0; box-shadow: 0 -1dp 8dp rgba(0,0,0,0.18); }"
  ".navi-tablet .navi-sheet { border-radius: 22dp; box-shadow: 0 2dp 12dp rgba(0,0,0,0.28); }"
  ".navi-search-card { border-radius: 22dp; box-shadow: 0 2dp 12dp rgba(0,0,0,0.28); }"
  ".night .navi-sheet { background: #1E2228; }"
  ".navi-handle { background: #D5D8DC; min-width: 36dp; min-height: 4dp; border-radius: 2dp; margin: 8dp 0 6dp 0; }"
  ".night .navi-handle { background: #4A5059; }"
  ".navi-sheet label, .navi-card label { color: #191A1C; }"
  ".night .navi-sheet label, .night .navi-card label { color: #F2F4F7; }"
  ".navi-dim { color: #5F656C; }"
  ".night .navi-sheet .navi-dim, .night .navi-card .navi-dim { color: #A9B0B8; }"
  /* "Where to?" */
  ".navi-where { background: #F1F2F6; border-radius: 28dp; min-height: 54dp; margin: 2dp 16dp 18dp 16dp; }"
  ".night .navi-where { background: #2C323A; }"
  ".navi-where label { color: #6B7178; font-size: 18sp; }"
  ".night .navi-where label { color: #A9B0B8; }"
  ".navi-where image { color: #5F656C; margin: 0 14dp 0 18dp; }"
  /* round buttons over the map */
  ".navi-round { background: #FFFFFF; border-radius: 9999px; min-width: 54dp; min-height: 54dp;"
  "  box-shadow: 0 1dp 5dp rgba(0,0,0,0.28); }"
  ".night .navi-round { background: #2A3038; }"
  ".navi-round image { color: #191A1C; }"
  ".night .navi-round image { color: #F2F4F7; }"
  /* the drawn icons on the cards and the round buttons (the crosshairs, Overview): ink by day, light at night */
  ".navi-ink { color: #3A3F45; }"
  ".night .navi-ink { color: #E0E4E9; }"
  ".navi-square { border-radius: 14dp; }"
  /* the note: no GPS */
  ".navi-note { background: rgba(0,0,0,0.82); border-radius: 14dp; padding: 6dp 14dp; margin: 12dp; }"
  ".navi-note label { color: #FFD43B; font-size: 14sp; font-weight: 600; }"
  /* the banner */
  ".navi-banner { background: #000000; padding: 10dp 16dp 12dp 12dp; }"
  ".navi-banner-distance { color: #FFFFFF; font-size: 30sp; font-weight: 700; }"
  ".navi-banner-street { color: #33CCFF; font-size: 26sp; font-weight: 700; }"
  ".navi-then { background: #141414; padding: 6dp 16dp 8dp 16dp; }"
  ".navi-then label { color: #C9CDD2; font-size: 15sp; }"
  ".navi-banner-side { color: #FFFFFF; font-size: 19sp; font-weight: 600; padding-top: 2dp; }"
  ".navi-lanes { background: #000000; padding: 0 12dp 10dp 12dp; }"
  ".navi-banner-note { color: #FFD43B; font-size: 14sp; font-weight: 600; padding-top: 6dp; }"
  ".navi-tablet .navi-banner, .navi-tablet .navi-then { border-radius: 0 0 16dp 0; }"
  /* the speed and the street */
  ".navi-speedo { background: #2E3339; border: 3dp solid #FFFFFF; border-radius: 9999px; min-width: 62dp;"
  "  min-height: 62dp; box-shadow: 0 1dp 5dp rgba(0,0,0,0.3); }"
  ".navi-speedo.navi-speeding { background: #E03131; }"
  ".navi-speedo-value { color: #FFFFFF; font-size: 24sp; font-weight: 700; }"
  ".navi-speedo-unit { color: #FFFFFF; font-size: 10sp; font-weight: 600; }"
  /* a camera ahead */
  ".navi-alert { background: #FFFFFF; border-radius: 18dp; padding: 10dp 16dp 10dp 10dp;"
  "  box-shadow: 0 1dp 6dp rgba(0,0,0,0.3); }"
  ".navi-alert-disc { background: #F08C00; border-radius: 9999px; min-width: 40dp; min-height: 40dp; }"
  ".navi-alert-title { color: #191A1C; font-size: 16sp; font-weight: 700; }"
  ".navi-alert-detail { color: #4A4F55; font-size: 14sp; }"
  ".night .navi-alert { background: #2A3038; }"
  ".night .navi-alert-title { color: #F1F3F5; }"
  ".night .navi-alert-detail { color: #B8BEC6; }"
  ".navi-street-pill { background: #FFFFFF; border-radius: 26dp; padding: 8dp 20dp; min-height: 36dp;"
  "  box-shadow: 0 1dp 5dp rgba(0,0,0,0.28); }"
  ".navi-street-pill label { color: #191A1C; font-size: 17sp; font-weight: 600; }"
  ".night .navi-street-pill { background: #2A3038; }"
  ".night .navi-street-pill label { color: #F2F4F7; }"
  /* the drive's bar */
  ".navi-eta { font-size: 28sp; font-weight: 700; }"
  ".navi-sub { font-size: 18sp; }"
  ".navi-tonal { background: #F1F2F6; border-radius: 9999px; min-width: 48dp; min-height: 48dp; }"
  ".night .navi-tonal { background: #2C323A; }"
  ".navi-tonal image { color: #3A3F45; }"
  ".night .navi-tonal image { color: #E0E4E9; }"
  ".navi-link { color: #0099FF; font-size: 17sp; font-weight: 700; }"
  ".navi-sheet .navi-link { color: #0099FF; }"
  /* buttons */
  ".navi-button { border-radius: 9999px; min-height: 52dp; }"
  ".navi-button label { font-size: 17sp; font-weight: 700; }"
  ".navi-go { background: #0099FF; }"
  ".navi-sheet .navi-go label { color: #FFFFFF; }"
  ".navi-later { background: #F1F2F6; }"
  ".night .navi-later { background: #2C323A; }"
  ".navi-sheet .navi-later label { color: #0099FF; }"
  ".navi-stop { background: #FF5353; }"
  ".navi-sheet .navi-stop label { color: #FFFFFF; }"
  /* the routes */
  ".navi-route-row { padding: 12dp 20dp 12dp 16dp; border-left: 4dp solid transparent; }"
  ".navi-route-row.selected { border-left-color: #0099FF; background: rgba(0,153,255,0.06); }"
  ".navi-route-time { font-size: 24sp; font-weight: 700; }"
  ".navi-route-row.selected .navi-route-time { color: #0099FF; }"
  ".navi-route-via { font-size: 16sp; font-weight: 600; }"
  ".navi-route-note { font-size: 14sp; }"
  ".navi-card { background: #FFFFFF; border-radius: 18dp; margin: 10dp 12dp; padding: 6dp 8dp;"
  "  box-shadow: 0 1dp 6dp rgba(0,0,0,0.22); }"
  ".night .navi-card { background: #1E2228; }"
  ".navi-card image { color: #3A3F45; }"
  ".night .navi-card image { color: #E0E4E9; }"
  ".navi-from-dot { background: #0099FF; border: 3dp solid #FFFFFF; border-radius: 9999px; min-width: 10dp;"
  "  min-height: 10dp; box-shadow: 0 0 0 1px rgba(0,0,0,0.25); }"
  ".navi-header-text { font-size: 16sp; font-weight: 600; }"
  /* the time bubbles */
  ".navi-bubble { background: #FFFFFF; border-radius: 10dp; padding: 4dp 9dp; box-shadow: 0 1dp 4dp rgba(0,0,0,0.3); }"
  ".navi-bubble label { color: #191A1C; }"
  ".navi-bubble.selected { background: #6C18D2; }"
  ".navi-bubble.selected label { color: #FFFFFF; }"
  ".night .navi-bubble { background: #2A3038; }"
  ".night .navi-bubble label { color: #F2F4F7; }"
  ".night .navi-bubble.selected { background: #2CDDFF; }"
  ".night .navi-bubble.selected label { color: #04202A; }"
  ".navi-bubble-time { font-size: 16sp; font-weight: 700; }"
  ".navi-bubble-note { font-size: 13sp; font-weight: 600; }"
  /* the place */
  ".navi-place-name { font-size: 22sp; font-weight: 700; }"
  ".navi-chip { background: #F1F2F6; border-radius: 12dp; padding: 4dp 10dp; }"
  ".night .navi-chip { background: #2C323A; }"
  ".navi-chip label { font-size: 14sp; font-weight: 600; }"
  ".navi-action { background: #F1F2F6; border-radius: 14dp; min-height: 64dp; }"
  ".night .navi-action { background: #2C323A; }"
  ".navi-action label { color: #0099FF; font-size: 13sp; font-weight: 600; }"
  ".navi-sheet .navi-action label { color: #0099FF; }"
  ".navi-action image { color: #0099FF; }"
  ".navi-action image.navi-starred, image.navi-starred { color: #FFB300; }"
  /* the search's chips */
  ".navi-category { background: #F1F2F6; border-radius: 14dp; min-height: 64dp; }"
  ".night .navi-category { background: #2C323A; }"
  ".navi-category label { font-size: 14sp; font-weight: 600; }"
  ".navi-arrived-title { font-size: 24sp; font-weight: 700; }";

/* ---- small parts ---- */

static GtkWidget *
label_new (const char *text, const char *css_class, float xalign)
{
  GtkWidget *l = gtk_label_new (text);
  if (css_class)
    gtk_widget_add_css_class (l, css_class);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  return l;
}

static GtkWidget *
image_new (const char *icon, int size)
{
  GtkWidget *i = gtk_image_new_from_icon_name (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (i), size);
  return i;
}

/* a button with a ripple: `child` in a pressable with these classes */
static GtkWidget *
pressable (GtkWidget *child, const char *class1, const char *class2, GCallback clicked, gpointer data)
{
  GtkWidget *p = vibeos_pressable_new (child);
  if (class1)
    gtk_widget_add_css_class (p, class1);
  if (class2)
    gtk_widget_add_css_class (p, class2);
  gtk_widget_set_overflow (p, GTK_OVERFLOW_HIDDEN);
  if (clicked)
    g_signal_connect (p, "clicked", clicked, data);
  return p;
}

static GtkWidget *
round_button (const char *icon, GtkWidget *custom, const char *label, GCallback clicked, gpointer data)
{
  GtkWidget *child = custom ? custom : image_new (icon, 24);
  gtk_widget_set_halign (child, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (child, GTK_ALIGN_CENTER);
  GtkWidget *b = pressable (child, "navi-round", NULL, clicked, data);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (b), VIBEOS_RIPPLE_CIRCLE, 27);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return b;
}

static GtkWidget *
text_button (const char *text, const char *css_class, GCallback clicked, gpointer data)
{
  GtkWidget *l = gtk_label_new (text);
  GtkWidget *b = pressable (l, "navi-button", css_class, clicked, data);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (b), VIBEOS_RIPPLE_BOUNDED, 26);
  gtk_widget_set_hexpand (b, TRUE);
  return b;
}

static char *
km_text (double metres)
{
  return navi_format_distance (metres);
}

static void
set_text (GtkWidget *label, const char *text)
{
  if (g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), text ? text : ""))
    gtk_label_set_text (GTK_LABEL (label), text ? text : "");
}

/* ---- points of interest ---- */

const char *
navi_poi_label (const char *group)
{
  static const struct {
    const char *group, *label;
  } labels[] = {
    { "fuel", N_ ("Gas station") }, { "charging", N_ ("EV charging") }, { "parking", N_ ("Parking") },
    { "food", N_ ("Restaurant") },  { "fast_food", N_ ("Fast food") },  { "cafe", N_ ("Café") },
    { "grocery", N_ ("Supermarket") }, { "pharmacy", N_ ("Pharmacy") }, { "hospital", N_ ("Hospital") },
    { "atm", N_ ("ATM") },          { "bank", N_ ("Bank") },            { "hotel", N_ ("Hotel") },
    { "toilets", N_ ("Toilets") },
  };
  for (guint i = 0; group && i < G_N_ELEMENTS (labels); i++)
    if (!strcmp (labels[i].group, group))
      return _(labels[i].label);
  return NULL;
}

/* a point of interest's marker as on the map, `size` points across, for a widget; NULL for no known group */
static GtkWidget *
poi_marker (NaviPanels *p, const char *group, int size)
{
  const NaviPoiKind *k = navi_poi_kind (group);
  if (!k)
    return NULL;
  int scale = gtk_widget_get_scale_factor (p->navi->win);
  g_autofree char *key = g_strdup_printf ("%s@%dx%d", k->group, size, scale);
  if (!p->markers)
    p->markers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  GdkTexture *t = g_hash_table_lookup (p->markers, key);
  if (!t) {
    g_autofree char *file = g_strdup_printf ("%s.svg", k->icon);
    g_autofree char *path = g_build_filename (p->navi->data_dir, "poi", file, NULL);
    t = navi_poi_marker_texture (path, k->color, size, scale);
    if (!t)
      return NULL;
    g_hash_table_insert (p->markers, g_steal_pointer (&key), t);
  }
  GtkWidget *image = gtk_image_new_from_paintable (GDK_PAINTABLE (t));
  gtk_image_set_pixel_size (GTK_IMAGE (image), size + 4);
  return image;
}

static void
stop_countdown (NaviPanels *p)
{
  g_clear_handle_id (&p->countdown_tick, g_source_remove);
  p->countdown = -1;
  if (p->go_fill)
    gtk_widget_queue_draw (p->go_fill);
}

/* ---- the sheet: expanding and collapsing ---- */

static void
set_expanded (NaviPanels *p, gboolean expanded)
{
  p->expanded = expanded;
  if (p->routes_more)
    gtk_revealer_set_reveal_child (GTK_REVEALER (p->routes_more), expanded && p->navi->mode == NAVI_MODE_ROUTES);
  if (p->drive_more)
    gtk_revealer_set_reveal_child (GTK_REVEALER (p->drive_more), expanded && p->navi->mode == NAVI_MODE_DRIVE);
}

static void
sheet_drag_end (GtkGestureDrag *drag, double dx, double dy, NaviPanels *p)
{
  if (fabs (dy) < 30 || fabs (dx) > fabs (dy))
    return;
  if (dy < 0)
    set_expanded (p, TRUE);
  else if (p->expanded)
    set_expanded (p, FALSE);
  else if (p->navi->mode == NAVI_MODE_PLACE)
    navi_back_to_browse (p->navi);
  stop_countdown (p);
}

static void
toggle_sheet (NaviPanels *p)
{
  set_expanded (p, !p->expanded);
  stop_countdown (p);
}

/* the handle: a click gesture's "released" */
static void
handle_released (GtkGestureClick *click, int n, double x, double y, NaviPanels *p)
{
  toggle_sheet (p);
}

/* the drive bar's middle (the arrival time): a pressable's "clicked" */
static void
eta_clicked (GtkWidget *button, NaviPanels *p)
{
  toggle_sheet (p);
}

/* the expanded drive card's Continue: back to the drive */
static void
continue_clicked (GtkWidget *button, NaviPanels *p)
{
  set_expanded (p, FALSE);
  stop_countdown (p);
}

/* ---- the search ---- */

static void open_search (NaviPanels *p, const char *text);

static void
close_search (NaviPanels *p)
{
  if (p->search_view)
    vibeos_search_view_close (VIBEOS_SEARCH_VIEW (p->search_view));
}

static void
pick_place (NaviPanels *p, const NaviPlace *place)
{
  Navi *n = p->navi;
  Choosing choosing = p->choosing;
  p->choosing = CHOOSE_NONE;
  close_search (p);
  if (choosing == CHOOSE_HOME || choosing == CHOOSE_WORK) {
    NaviPlace **slot = choosing == CHOOSE_HOME ? &n->settings->home : &n->settings->work;
    g_clear_pointer (slot, navi_place_free);
    *slot = navi_place_copy (place);
    navi_settings_save (n->settings);
    navi_preview_routes (n, place);
    return;
  }
  if (n->mode == NAVI_MODE_DRIVE) {
    navi_panels_ask_route_change (p, place);
    return;
  }
  navi_show_place (n, place);
}

static void
row_clicked (GtkWidget *row, NaviPanels *p)
{
  NaviPlace *place = g_object_get_data (G_OBJECT (row), "navi-place");
  if (place) {
    g_autoptr (NaviPlace) copy = navi_place_copy (place);
    pick_place (p, copy);
  }
}

static void
saved_clicked (GtkWidget *row, NaviPanels *p)
{
  Navi *n = p->navi;
  gboolean home = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "navi-home"));
  NaviPlace *place = home ? n->settings->home : n->settings->work;
  if (place) {
    g_autoptr (NaviPlace) copy = navi_place_copy (place);
    close_search (p);
    if (n->mode == NAVI_MODE_DRIVE)
      navi_panels_ask_route_change (p, copy);
    else
      navi_preview_routes (n, copy);
    return;
  }
  /* "Set once and go": the next place picked is it */
  p->choosing = home ? CHOOSE_HOME : CHOOSE_WORK;
  vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (p->search_view), "");
  set_text (p->search_status, home ? _("Search for your home address") : _("Search for your work address"));
  gtk_widget_set_visible (p->search_status, TRUE);
}

static const char *
place_icon (const NaviPlace *place)
{
  switch (place->kind) {
  case NAVI_PLACE_LOCALITY: return "find-location-symbolic";
  default: return "mark-location-symbolic";
  }
}

static GtkWidget *place_row_full (NaviPanels *p, const NaviPlace *place, const char *icon, const char *subtitle,
                                  const char *value);

static GtkWidget *
place_row (NaviPanels *p, const NaviPlace *place, const char *icon, gboolean distance)
{
  g_autofree char *d = distance ? km_text (navi_distance_from_car (p->navi, place->lat, place->lng)) : NULL;
  return place_row_full (p, place, icon, place->detail, d);
}

/* a place in a list: its marker or `icon`, its name over `subtitle`, `value` at the end */
static GtkWidget *
place_row_full (NaviPanels *p, const NaviPlace *place, const char *icon, const char *subtitle, const char *value)
{
  GtkWidget *row = vibeos_list_row_new (place->name, subtitle);
  GtkWidget *marker = place->kind == NAVI_PLACE_POI ? poi_marker (p, place->category, 26) : NULL;
  if (marker) {
    vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (row), marker);
  } else {
    vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), icon ? icon : place_icon (place));
  }
  if (value)
    vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), value);
  g_object_set_data_full (G_OBJECT (row), "navi-place", navi_place_copy (place), (GDestroyNotify) navi_place_free);
  g_signal_connect (row, "clicked", G_CALLBACK (row_clicked), p);
  return row;
}

/* ---- the saved places: starred on their cards, listed in the search, renamed and removed from there ---- */

static void fill_saved (NaviPanels *p);
static void update_floating (NaviPanels *p);

static void
saved_place_clicked (GtkWidget *row, NaviPanels *p)
{
  Navi *n = p->navi;
  const NaviSaved *saved = g_object_get_data (G_OBJECT (row), "navi-saved");
  g_autoptr (NaviPlace) copy = navi_place_copy (saved->place);
  /* the drive is to it by the name it was given */
  if (saved->label) {
    g_free (copy->name);
    copy->name = g_strdup (saved->label);
  }
  close_search (p);
  if (n->mode == NAVI_MODE_DRIVE)
    navi_panels_ask_route_change (p, copy);
  else
    navi_preview_routes (n, copy);
}

/* the saved place a menu was opened for, by where it is (the list may have changed under the menu), or -1 */
static int
saved_target_index (NaviPanels *p)
{
  return p->saved_target ? navi_settings_find_saved (p->navi->settings, p->saved_target) : -1;
}

static void
saved_list_changed (NaviPanels *p)
{
  if (p->search_view && p->choosing == CHOOSE_NONE &&
      !*vibeos_search_view_get_text (VIBEOS_SEARCH_VIEW (p->search_view)))
    fill_saved (p);
}

static void
rename_answered (VibeosDialog *d, int response, NaviPanels *p)
{
  int i = saved_target_index (p);
  if (response != VIBEOS_RESPONSE_OK || i < 0)
    return;
  GtkWidget *entry = g_object_get_data (G_OBJECT (d), "navi-entry");
  navi_settings_rename_saved (p->navi->settings, i, gtk_editable_get_text (GTK_EDITABLE (entry)));
  saved_list_changed (p);
}

static void
rename_activated (GtkWidget *entry, VibeosDialog *d)
{
  vibeos_dialog_respond (d, VIBEOS_RESPONSE_OK);
}

static gboolean
rename_focus (gpointer data)
{
  GtkWidget *entry = data;
  if (gtk_widget_get_root (entry)) {
    gtk_widget_grab_focus (entry);
    gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
  }
  g_object_unref (entry);
  return G_SOURCE_REMOVE;
}

static void
rename_saved (gpointer data)
{
  NaviPanels *p = data;
  int i = saved_target_index (p);
  if (i < 0)
    return;
  const NaviSaved *saved = p->navi->settings->saved->pdata[i];
  VibeosDialog *d = vibeos_dialog_new (_("Rename"));
  GtkWidget *entry = gtk_entry_new ();
  gtk_widget_add_css_class (entry, "vibeos-entry");
  gtk_editable_set_text (GTK_EDITABLE (entry), navi_saved_name (saved));
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), saved->place->name);
  gtk_widget_set_margin_top (entry, 8);
  vibeos_dialog_set_content (d, entry);
  vibeos_dialog_add_button (d, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (d, _("Save"), VIBEOS_RESPONSE_OK);
  g_object_set_data (G_OBJECT (d), "navi-entry", entry);
  g_signal_connect (entry, "activate", G_CALLBACK (rename_activated), d);
  g_signal_connect (d, "response", G_CALLBACK (rename_answered), p);
  vibeos_dialog_present (d, p->navi->win);
  /* after the dialog's own focus */
  g_idle_add (rename_focus, g_object_ref (entry));
}

static void
remove_saved (gpointer data)
{
  NaviPanels *p = data;
  int i = saved_target_index (p);
  if (i < 0)
    return;
  navi_settings_remove_saved (p->navi->settings, i);
  saved_list_changed (p);
  if (p->place && p->place_star)
    navi_panels_show_place (p, p->place);
}

static void
saved_more_clicked (GtkWidget *button, NaviPanels *p)
{
  const NaviSaved *saved = g_object_get_data (G_OBJECT (button), "navi-saved");
  g_clear_pointer (&p->saved_target, navi_place_free);
  p->saved_target = navi_place_copy (saved->place);
  VibeosMenu *menu = vibeos_menu_new ();
  vibeos_menu_append (menu, _("Rename"), rename_saved, p);
  vibeos_menu_append (menu, _("Remove"), remove_saved, p);
  vibeos_menu_present (menu, button);
}

/* a saved place in the search's list: its name over where it is (its own name too, once renamed), its menu at the
 * end */
static GtkWidget *
saved_row (NaviPanels *p, const NaviSaved *saved)
{
  const NaviPlace *place = saved->place;
  g_autofree char *subtitle = !saved->label ? g_strdup (place->detail)
                              : place->detail ? g_strdup_printf ("%s, %s", place->name, place->detail)
                                              : g_strdup (place->name);
  GtkWidget *row = vibeos_list_row_new (navi_saved_name (saved), subtitle);
  GtkWidget *star = image_new ("starred-symbolic", 24);
  gtk_widget_add_css_class (star, "navi-starred");
  vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (row), star);
  GtkWidget *more = vibeos_icon_button_new ("view-more-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  /* the rows are made again whenever the list changes, so the place itself is safe to point at */
  g_object_set_data (G_OBJECT (more), "navi-saved", (gpointer) saved);
  g_signal_connect (more, "clicked", G_CALLBACK (saved_more_clicked), p);
  vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (row), more);
  g_object_set_data (G_OBJECT (row), "navi-saved", (gpointer) saved);
  g_signal_connect (row, "clicked", G_CALLBACK (saved_place_clicked), p);
  return row;
}

/* the planned drive's row: the routes there; its button cancels the reminder */
static void
plan_row_clicked (GtkWidget *row, NaviPanels *p)
{
  g_autoptr (NaviPlan) plan = navi_plan_load ();
  if (!plan)
    return;
  close_search (p);
  if (p->navi->mode == NAVI_MODE_DRIVE)
    navi_panels_ask_route_change (p, plan->place);
  else
    navi_preview_routes (p->navi, plan->place);
}

static void
plan_cancel_clicked (GtkWidget *button, NaviPanels *p)
{
  navi_plan_cancel ();
  fill_saved (p);
}

static GtkWidget *
plan_row (NaviPanels *p, const NaviPlan *plan)
{
  g_autofree char *subtitle = navi_plan_describe (plan->time, vibeos_clock_uses_24h ());
  GtkWidget *row = vibeos_list_row_new (plan->place->name, subtitle);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "alarm-symbolic");
  GtkWidget *cancel = vibeos_icon_button_new ("window-close-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (cancel), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Cancel reminder"), -1);
  g_signal_connect (cancel, "clicked", G_CALLBACK (plan_cancel_clicked), p);
  vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (row), cancel);
  g_signal_connect (row, "clicked", G_CALLBACK (plan_row_clicked), p);
  return row;
}

/* the search's first view: Home, Work, the planned drive, the saved places, the places driven to lately */
static void
fill_saved (NaviPanels *p)
{
  Navi *n = p->navi;
  VibeosCardList *list = VIBEOS_CARD_LIST (p->search_list);
  vibeos_card_list_remove_all (list);
  for (int k = 0; k < 2; k++) {
    NaviPlace *place = k == 0 ? n->settings->home : n->settings->work;
    GtkWidget *row = vibeos_list_row_new (k == 0 ? _("Home") : _("Work"),
                                          place ? (place->detail ? place->detail : place->name)
                                                : _("Set once and go"));
    if (k == 0) {
      vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "user-home-symbolic");
    } else {
      GtkWidget *i = navi_icon_new (NAVI_ICON_WORK, 24);
      GdkRGBA c = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
      navi_icon_set_color (i, &c);
      vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (row), i);
    }
    g_object_set_data (G_OBJECT (row), "navi-home", GINT_TO_POINTER (k == 0));
    g_signal_connect (row, "clicked", G_CALLBACK (saved_clicked), p);
    vibeos_card_list_append (list, row);
  }
  /* until an hour after its time: the reminder may have come while the phone lay unseen */
  g_autoptr (NaviPlan) plan = navi_plan_load ();
  if (plan && plan->time > g_get_real_time () / G_USEC_PER_SEC - 3600) {
    vibeos_card_list_add_group (list, _("Planned drive"));
    vibeos_card_list_append (list, plan_row (p, plan));
  }
  if (n->settings->saved->len) {
    vibeos_card_list_add_group (list, _("Saved"));
    for (guint i = 0; i < n->settings->saved->len; i++)
      vibeos_card_list_append (list, saved_row (p, n->settings->saved->pdata[i]));
  }
  if (n->settings->recent->len) {
    vibeos_card_list_add_group (list, _("Recent"));
    for (guint i = 0; i < n->settings->recent->len && i < 10; i++)
      vibeos_card_list_append (list, place_row (p, n->settings->recent->pdata[i], "document-open-recent-symbolic",
                                                TRUE));
  }
}

static void
search_done (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviPanels *p = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) places = navi_search_search_finish (NAVI_SEARCH (src), res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&p->search_cancel);
  VibeosCardList *list = VIBEOS_CARD_LIST (p->search_list);
  vibeos_card_list_remove_all (list);
  gboolean down = navi_search_photon_is_down (NAVI_SEARCH (src));
  if (!places || !places->len) {
    set_text (p->search_status, error || down ? _("Search is unavailable right now") : _("No results"));
    gtk_widget_set_visible (p->search_status, TRUE);
    return;
  }
  /* the map's own places around the car stand in */
  if (down)
    set_text (p->search_status, _("Search is unavailable right now. These are from the map, nearby."));
  gtk_widget_set_visible (p->search_status, down || p->choosing != CHOOSE_NONE);
  for (guint i = 0; i < places->len; i++)
    vibeos_card_list_append (list, place_row (p, places->pdata[i], NULL, TRUE));
}

/* ---- a category along the route, while driving ---- */

#define ALONG_OFF_M 1500 /* how far from the route a place may be */

struct Along {
  NaviPanels *p;
  int pending;      /* the searches still out */
  GPtrArray *found; /* NaviPlace */
  gboolean down;
};

typedef struct {
  NaviPlace *place;
  double off, ahead; /* metres from the route; along it from the car */
} AlongHit;

static int
by_detour (gconstpointer a, gconstpointer b)
{
  const AlongHit *x = a, *y = b;
  /* within a few hundred metres of each other off the route, the nearer one ahead first */
  int ox = (int) (x->off / 300), oy = (int) (y->off / 300);
  if (ox != oy)
    return ox - oy;
  return (x->ahead > y->ahead) - (x->ahead < y->ahead);
}

static void
along_show (NaviPanels *p, struct Along *a)
{
  Navi *n = p->navi;
  if (!p->search_list || !n->guidance)
    return;
  NaviRoute *r = navi_guidance_get_route (n->guidance);
  g_autoptr (GArray) hits = g_array_new (FALSE, FALSE, sizeof (AlongHit));
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < a->found->len; i++) {
    NaviPlace *place = a->found->pdata[i];
    double s, bearing;
    double off = navi_path_project (r->points, r->n_points, place->lat, place->lng, n->car_s, G_MAXDOUBLE, &s, &bearing);
    if (off > ALONG_OFF_M || s < n->car_s)
      continue;
    /* the samples' boxes overlap: each place once */
    g_autofree char *key = g_strdup_printf ("%s:%d:%d", place->name, (int) floor (place->lat / 0.0005),
                                            (int) floor (place->lng / 0.0008));
    if (!g_hash_table_add (seen, g_steal_pointer (&key)))
      continue;
    AlongHit h = { place, off, s - n->car_s };
    g_array_append_val (hits, h);
  }
  g_array_sort (hits, by_detour);
  VibeosCardList *list = VIBEOS_CARD_LIST (p->search_list);
  vibeos_card_list_remove_all (list);
  if (!hits->len) {
    set_text (p->search_status, a->down ? _("Search is unavailable right now") : _("None along the route"));
    gtk_widget_set_visible (p->search_status, TRUE);
    return;
  }
  set_text (p->search_status, _("Along the route"));
  gtk_widget_set_visible (p->search_status, TRUE);
  for (guint i = 0; i < hits->len && i < 15; i++) {
    AlongHit *h = &g_array_index (hits, AlongHit, i);
    g_autofree char *ahead = km_text (h->ahead);
    g_autofree char *sub = h->place->detail ? g_strdup_printf (_("%s ahead · %s"), ahead, h->place->detail)
                                            : g_strdup_printf (_("%s ahead"), ahead);
    /* the way there and back, by road about 1.4 times the straight line, at town speed: an estimate */
    int minutes = (int) lround (2 * h->off * 1.4 / 8.3 / 60);
    g_autofree char *detour = h->off < 60 ? g_strdup (_("On the route")) : g_strdup_printf (_("+%d min"), MAX (1, minutes));
    vibeos_card_list_append (list, place_row_full (p, h->place, NULL, sub, detour));
  }
}

static void
along_found (GObject *src, GAsyncResult *res, gpointer data)
{
  struct Along *a = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) places = navi_search_search_finish (NAVI_SEARCH (src), res, &error);
  for (guint i = 0; places && i < places->len; i++)
    g_ptr_array_add (a->found, navi_place_copy (places->pdata[i]));
  if (navi_search_photon_is_down (NAVI_SEARCH (src)))
    a->down = TRUE;
  if (--a->pending > 0)
    return;
  if (a->p && a->p->along == a) {
    a->p->along = NULL;
    along_show (a->p, a);
  }
  g_ptr_array_unref (a->found);
  g_free (a);
}

/* the category near points ahead on the route: soon, in a few kilometres, further on */
static void
search_along (NaviPanels *p, NaviCategory category)
{
  Navi *n = p->navi;
  NaviRoute *r = navi_guidance_get_route (n->guidance);
  static const double ahead[] = { 800, 4000, 10000, 25000 };
  struct Along *a = p->along = g_new0 (struct Along, 1);
  a->p = p;
  a->found = g_ptr_array_new_with_free_func ((GDestroyNotify) navi_place_free);
  guint hint = 0;
  for (guint i = 0; i < G_N_ELEMENTS (ahead); i++) {
    double s = MIN (n->car_s + ahead[i], r->length), lat, lng;
    navi_path_point_at (r->points, r->n_points, s, &lat, &lng, &hint);
    a->pending++;
    navi_search_search_async (n->search, "", category, lat, lng, p->search_cancel, along_found, a);
    if (s >= r->length)
      break;
  }
}

static gboolean
run_search (gpointer data)
{
  NaviPanels *p = data;
  p->search_source = 0;
  const char *text = vibeos_search_view_get_text (VIBEOS_SEARCH_VIEW (p->search_view));
  NaviCategory category = NAVI_CATEGORY_NONE;
  if (p->category_text && !g_strcmp0 (text, p->category_text))
    category = p->category, text = "";
  g_cancellable_cancel (p->search_cancel);
  g_clear_object (&p->search_cancel);
  p->search_cancel = g_cancellable_new ();
  if (p->along) {
    p->along->p = NULL; /* what it finds now shows nowhere */
    p->along = NULL;
  }
  if (category != NAVI_CATEGORY_NONE && p->navi->mode == NAVI_MODE_DRIVE && p->navi->guidance) {
    search_along (p, category);
    return G_SOURCE_REMOVE;
  }
  double lat, lng;
  gboolean approx;
  navi_where_am_i (p->navi, &lat, &lng, &approx);
  navi_search_search_async (p->navi->search, text, category, lat, lng, p->search_cancel, search_done, p);
  return G_SOURCE_REMOVE;
}

static void
search_changed (VibeosSearchView *view, const char *text, NaviPanels *p)
{
  g_clear_handle_id (&p->search_source, g_source_remove);
  g_autofree char *trimmed = g_strstrip (g_strdup (text));
  if (!*trimmed) {
    g_cancellable_cancel (p->search_cancel);
    gtk_widget_set_visible (p->search_status, p->choosing != CHOOSE_NONE);
    if (p->choosing == CHOOSE_NONE)
      fill_saved (p);
    else
      vibeos_card_list_remove_all (VIBEOS_CARD_LIST (p->search_list));
    return;
  }
  p->search_source = g_timeout_add (SEARCH_DELAY_MS, run_search, p);
}

static void
search_activated (VibeosSearchView *view, const char *text, NaviPanels *p)
{
  g_clear_handle_id (&p->search_source, g_source_remove);
  run_search (p);
}

/* ---- the search's microphone: a place said instead of typed ---- */

#ifdef HAVE_DICTATION
/* the language heard: the app's (the one Valhalla speaks) where its model is installed, else Dictate's choice (the
 * user's first language with one, else English) */
static char *
dictation_language (void)
{
  char code[3];
  g_strlcpy (code, navi_route_language (), sizeof code);
  g_autoptr (DictateModel) model = dictate_model_find (code);
  return model ? g_strdup (code) : dictate_language_default ();
}

static void
mic_text (GObject *engine, GStrv finished, const char *partial, NaviPanels *p)
{
  if (!p->search_view)
    return;
  for (guint i = 0; finished && finished[i]; i++) {
    if (p->dictated->len)
      g_string_append_c (p->dictated, ' ');
    g_string_append (p->dictated, finished[i]);
  }
  g_autoptr (GString) text = g_string_new (p->dictated->str);
  if (partial && *partial)
    g_string_append_printf (text, "%s%s", text->len ? " " : "", partial);
  /* a place, not a sentence: Moonshine's full stop goes */
  while (text->len && strchr (".!?,;: ", text->str[text->len - 1]))
    g_string_truncate (text, text->len - 1);
  if (text->len)
    vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (p->search_view), text->str);
}

static void
mic_state (GObject *engine, NaviPanels *p)
{
  DictateState state = dictate_engine_get_state (DICTATE_ENGINE (engine));
  gboolean on = state == DICTATE_STARTING || state == DICTATE_LISTENING;
  if (p->search_mic)
    gtk_image_set_from_icon_name (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (p->search_mic))),
                                  on ? "media-playback-stop-symbolic" : "audio-input-microphone-symbolic");
  if (!p->search_status)
    return;
  if (on) {
    set_text (p->search_status, state == DICTATE_STARTING ? _("Getting ready…") : _("Say where to go"));
    gtk_widget_set_visible (p->search_status, TRUE);
  } else if (p->choosing == CHOOSE_NONE && !g_strcmp0 (gtk_label_get_text (GTK_LABEL (p->search_status)),
                                                         _("Say where to go"))) {
    gtk_widget_set_visible (p->search_status, FALSE);
  }
}

static void
mic_error (GObject *engine, const char *message, NaviPanels *p)
{
  if (!p->search_status)
    return;
  set_text (p->search_status, message);
  gtk_widget_set_visible (p->search_status, TRUE);
}

static void
mic_clicked (GtkWidget *button, NaviPanels *p)
{
  DictateEngine *engine = DICTATE_ENGINE (p->dictation);
  if (dictate_engine_get_state (engine) != DICTATE_IDLE) {
    dictate_engine_stop (engine);
    return;
  }
  g_autofree char *reason = NULL;
  if (!dictate_engine_available (engine, &reason)) {
    mic_error (p->dictation, reason, p);
    return;
  }
  g_string_truncate (p->dictated, 0);
  dictate_engine_start (engine);
}

/* the microphone for the search's bar, NULL where no speech model is installed */
static GtkWidget *
search_mic (NaviPanels *p)
{
  if (!p->dictation) {
    g_autofree char *language = dictation_language ();
    if (!language)
      return NULL; /* no model at all */
    gst_init (NULL, NULL); /* the microphone is read through GStreamer (the voice may not have started it yet) */
    p->dictation = G_OBJECT (dictate_engine_new ());
    p->dictated = g_string_new (NULL);
    dictate_engine_set_language (DICTATE_ENGINE (p->dictation), language);
    /* one turn: it stops by itself once the place has been said */
    dictate_engine_set_turns (DICTATE_ENGINE (p->dictation), TRUE);
    g_signal_connect (p->dictation, "text", G_CALLBACK (mic_text), p);
    g_signal_connect (p->dictation, "state-changed", G_CALLBACK (mic_state), p);
    g_signal_connect (p->dictation, "error", G_CALLBACK (mic_error), p);
  }
  g_autofree char *reason = NULL;
  if (!dictate_engine_available (DICTATE_ENGINE (p->dictation), &reason)) {
    g_debug ("search: no microphone: %s", reason);
    return NULL;
  }
  GtkWidget *mic = vibeos_icon_button_new ("audio-input-microphone-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (mic), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Search by voice"), -1);
  g_signal_connect (mic, "clicked", G_CALLBACK (mic_clicked), p);
  return mic;
}
#endif

static void
search_closed (VibeosSearchView *view, NaviPanels *p)
{
#ifdef HAVE_DICTATION
  if (p->dictation && dictate_engine_get_state (DICTATE_ENGINE (p->dictation)) != DICTATE_IDLE)
    dictate_engine_stop (DICTATE_ENGINE (p->dictation));
  p->search_mic = NULL;
#endif
  g_clear_handle_id (&p->search_source, g_source_remove);
  g_cancellable_cancel (p->search_cancel);
  if (p->along) {
    p->along->p = NULL;
    p->along = NULL;
  }
  p->search_view = p->search_list = p->search_status = p->search_box = NULL;
  p->choosing = CHOOSE_NONE;
  if (p->search_host)
    gtk_widget_set_visible (p->search_host, FALSE);
}

static void
category_clicked (GtkWidget *chip, NaviPanels *p)
{
  p->category = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (chip), "navi-category"));
  g_free (p->category_text);
  p->category_text = g_strdup (g_object_get_data (G_OBJECT (chip), "navi-label"));
  vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (p->search_view), p->category_text);
}

static GtkWidget *
category_chip (NaviPanels *p, NaviCategory category, const char *label, GtkWidget *icon)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label_new (label, NULL, 0.5));
  GtkWidget *chip = pressable (box, "navi-category", NULL, G_CALLBACK (category_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (chip), VIBEOS_RIPPLE_BOUNDED, 14);
  gtk_widget_set_size_request (chip, 80, -1);
  g_object_set_data (G_OBJECT (chip), "navi-category", GINT_TO_POINTER (category));
  g_object_set_data_full (G_OBJECT (chip), "navi-label", g_strdup (label), g_free);
  return chip;
}

/* The keyboard comes up over Navi's window, not with the window squeezed above it (the desktop file's
 * X-Vibeos-Keyboard=overlay): the map stays as it is, and the search's results get room under them to scroll clear of
 * the keys. On a tablet the search's card is short of the keys anyway, mostly. */
static void
keyboard_room (NaviPanels *p)
{
  if (p->search_box)
    gtk_widget_set_margin_bottom (p->search_box, p->keyboard);
}

static void
shell_properties_changed (GDBusProxy *shell, GVariant *changed, GStrv invalidated, NaviPanels *p)
{
  g_autoptr (GVariant) v = g_dbus_proxy_get_cached_property (shell, "KeyboardOverlap");
  guint overlap = v && g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32 (v) : 0;
  if (overlap == p->keyboard)
    return;
  p->keyboard = overlap;
  keyboard_room (p);
}

static void
shell_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GDBusProxy *shell = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (!shell)
    return; /* cancelled, or no session bus: the keyboard then never covers anything Navi knows of */
  NaviPanels *p = data;
  p->shell = shell;
  g_signal_connect (shell, "g-properties-changed", G_CALLBACK (shell_properties_changed), p);
  shell_properties_changed (shell, NULL, NULL, p);
}

static void
open_search (NaviPanels *p, const char *text)
{
  if (p->search_view)
    return;
  Navi *n = p->navi;
  GtkWidget *view = p->search_view = vibeos_search_view_new (_("Where to?"));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  /* the categories near the car (along the route while driving), in a row that scrolls sideways */
  static const struct {
    NaviCategory category;
    const char *group, *label;
  } chip_kinds[] = {
    { NAVI_CATEGORY_FUEL, "fuel", N_ ("Gas") },         { NAVI_CATEGORY_CHARGING, "charging", N_ ("Charging") },
    { NAVI_CATEGORY_PARKING, "parking", N_ ("Parking") }, { NAVI_CATEGORY_FOOD, "food", N_ ("Food") },
    { NAVI_CATEGORY_COFFEE, "cafe", N_ ("Coffee") },    { NAVI_CATEGORY_GROCERY, "grocery", N_ ("Groceries") },
    { NAVI_CATEGORY_PHARMACY, "pharmacy", N_ ("Pharmacy") }, { NAVI_CATEGORY_HOSPITAL, "hospital", N_ ("Hospital") },
    { NAVI_CATEGORY_ATM, "atm", N_ ("ATM") },           { NAVI_CATEGORY_HOTEL, "hotel", N_ ("Hotel") },
    { NAVI_CATEGORY_TOILETS, "toilets", N_ ("Toilets") },
  };
  GtkWidget *chips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (chips, 16);
  gtk_widget_set_margin_end (chips, 16);
  for (guint i = 0; i < G_N_ELEMENTS (chip_kinds); i++) {
    GtkWidget *icon = poi_marker (p, chip_kinds[i].group, 28);
    if (!icon)
      icon = image_new ("mark-location-symbolic", 28);
    gtk_box_append (GTK_BOX (chips), category_chip (p, chip_kinds[i].category, _(chip_kinds[i].label), icon));
  }
  GtkWidget *chip_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (chip_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (chip_scroll), chips);
  gtk_widget_set_margin_top (chip_scroll, 8);
  gtk_box_append (GTK_BOX (box), chip_scroll);
  p->search_status = label_new ("", "navi-dim", 0);
  gtk_widget_set_margin_start (p->search_status, 24);
  gtk_widget_set_margin_top (p->search_status, 8);
  gtk_widget_set_visible (p->search_status, FALSE);
  gtk_box_append (GTK_BOX (box), p->search_status);
  p->search_list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (p->search_list), 8, 24);
  gtk_box_append (GTK_BOX (box), p->search_list);
  p->search_box = box;
  keyboard_room (p);
  GtkWidget *scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), box);
  gtk_widget_set_vexpand (scroll, TRUE);
  vibeos_search_view_set_content (VIBEOS_SEARCH_VIEW (view), scroll);
  g_signal_connect (view, "search-changed", G_CALLBACK (search_changed), p);
  g_signal_connect (view, "search-activated", G_CALLBACK (search_activated), p);
  g_signal_connect (view, "closed", G_CALLBACK (search_closed), p);
#ifdef HAVE_DICTATION
  p->search_mic = search_mic (p);
  if (p->search_mic)
    vibeos_search_view_set_end (VIBEOS_SEARCH_VIEW (view), p->search_mic);
#endif
  fill_saved (p);
  if (p->search_host) {
    gtk_widget_set_visible (p->search_host, TRUE);
    vibeos_search_view_open_in (VIBEOS_SEARCH_VIEW (view), GTK_OVERLAY (p->search_host));
  } else
    vibeos_search_view_open (VIBEOS_SEARCH_VIEW (view), n->win);
  if (text)
    vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (view), text);
}

void
navi_panels_open_search (NaviPanels *p, const char *text)
{
  open_search (p, text);
}

static void
where_clicked (GtkWidget *w, NaviPanels *p)
{
  open_search (p, NULL);
}

/* ---- a route to change or cancel ---- */

enum { RESPONSE_ADD_STOP = 2 };

typedef struct {
  NaviPanels *p;
  NaviPlace *place; /* the new destination, or NULL for Back */
} RouteAsk;

static void
route_ask_answered (VibeosDialog *d, int response, RouteAsk *a)
{
  NaviPanels *p = a->p;
  Navi *n = p->navi;
  gboolean active = n->mode == NAVI_MODE_DRIVE || n->mode == NAVI_MODE_ROUTES;
  if (active && response == RESPONSE_ADD_STOP && a->place && n->mode == NAVI_MODE_DRIVE) {
    navi_add_stop (n, a->place);
  } else if (active && response == VIBEOS_RESPONSE_OK) {
    if (n->mode == NAVI_MODE_DRIVE)
      navi_stop_drive (n);
    else
      navi_back_to_browse (n);
    if (a->place)
      navi_preview_routes (n, a->place);
    else
      open_search (p, NULL); /* a new address can be typed at once */
  }
  g_clear_pointer (&a->place, navi_place_free);
  g_free (a);
}

static void
add_stop_clicked (GtkWidget *button, VibeosDialog *d)
{
  vibeos_dialog_respond (d, RESPONSE_ADD_STOP);
}

void
navi_panels_ask_route_change (NaviPanels *p, const NaviPlace *place)
{
  gboolean drive = p->navi->mode == NAVI_MODE_DRIVE;
  RouteAsk *a = g_new0 (RouteAsk, 1);
  a->p = p;
  a->place = place ? navi_place_copy (place) : NULL;
  VibeosDialog *d;
  if (place) {
    /* Waze's two choices; a tap outside or Back keeps the drive as it is (three buttons do not fit a phone) */
    g_autofree char *title = g_strdup_printf (_("Drive to %s?"), place->name ? place->name : _("this place"));
    d = vibeos_dialog_new (title);
    vibeos_dialog_set_message (d, _("Add it as a stop on the way, or make it the new destination and cancel the "
                                    "current route."));
    if (drive) {
      GtkWidget *stop = vibeos_dialog_add_neutral_button (d, _("Add a stop"));
      g_signal_connect (stop, "clicked", G_CALLBACK (add_stop_clicked), d);
    }
    vibeos_dialog_add_button (d, _("New destination"), VIBEOS_RESPONSE_OK);
  } else {
    d = vibeos_dialog_new (_("Cancel the current route?"));
    vibeos_dialog_add_button (d, drive ? _("Keep driving") : _("Keep route"), VIBEOS_RESPONSE_CANCEL);
    vibeos_dialog_add_button (d, _("Cancel route"), VIBEOS_RESPONSE_OK);
  }
  g_signal_connect (d, "response", G_CALLBACK (route_ask_answered), a);
  vibeos_dialog_present (d, p->navi->win);
}

gboolean
navi_panels_back (NaviPanels *p)
{
  if (p->search_view) {
    close_search (p);
    return TRUE;
  }
  return FALSE;
}

/* ---- the menu and settings ---- */

static const VibeosAboutSection about_credits[] = {
  { "Font Awesome", "The app's icon is Font Awesome's \"diamond-turn-right\" by Fonticons, Inc., under "
                    "<a href=\"https://creativecommons.org/licenses/by/4.0/\">CC BY 4.0</a>." },
  { "Maki", "The points of interest's icons are <a href=\"https://github.com/mapbox/maki\">Maki</a> by Mapbox, "
            "dedicated to the public domain under "
            "<a href=\"https://creativecommons.org/publicdomain/zero/1.0/\">CC0 1.0</a>." },
  { NULL, NULL },
};
static const VibeosAboutSection about_legal[] = {
  { "OpenStreetMap", "Map data © <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a> "
                     "contributors, under the ODbL. Map tiles in the OpenMapTiles schema. The traffic signals and "
                     "speed cameras, and the speed limits, are OpenStreetMap's too." },
  { "MapLibre Native", "The map is drawn by <a href=\"https://maplibre.org/\">MapLibre Native</a>, under the BSD "
                       "2-Clause License." },
  { "Valhalla and Photon", "Routes by <a href=\"https://github.com/valhalla/valhalla\">Valhalla</a> (MIT) and "
                           "places by <a href=\"https://github.com/komoot/photon\">Photon</a> (Apache-2.0), on "
                           "VibeOS's own server, which keeps no logs." },
  { NULL, NULL },
};
static const VibeosAboutInfo about_info = {
  .name = "Navi", .icon_name = "vibeos.Navi", .credits = about_credits, .legal = about_legal,
};

typedef struct {
  NaviPanels *p;
  GtkWidget *sheet;
  GtkWidget *dark_row, *view_row, *voice_row, *home_row, *work_row, *speeding_row, *vehicle_row, *units_row;
  GtkWidget *language_row;
} SettingsPage;

static GtkWidget *settings_sheet; /* weak: the Settings sheet while it is open */

static const char *
dark_name (NaviDarkMode m)
{
  switch (m) {
  case NAVI_DARK_ALWAYS: return _("Always on");
  case NAVI_DARK_NEVER: return _("Off");
  case NAVI_DARK_SYSTEM: return _("As the system");
  default: return _("Only at night");
  }
}

static const char *
view_name (NaviMapView v)
{
  switch (v) {
  case NAVI_VIEW_3D: return _("3D");
  case NAVI_VIEW_2D: return _("2D");
  default: return _("Auto");
  }
}

static const char *
voice_name (NaviVoiceMode v)
{
  switch (v) {
  case NAVI_VOICE_ALERTS: return _("Alerts only");
  case NAVI_VOICE_OFF: return _("Off");
  default: return _("On");
  }
}

static gboolean units_mph; /* the speedometer's units now (navi_panels_set_units) */

static const char *
vehicle_name (NaviVehicle v)
{
  return v == NAVI_VEHICLE_MOTORBIKE ? _("Motorcycle") : _("Car");
}

static const char *
units_name (NaviUnits u)
{
  switch (u) {
  case NAVI_UNITS_KM: return _("Kilometers");
  case NAVI_UNITS_MI: return _("Miles");
  default: return _("Auto");
  }
}

static const char *
speeding_name (NaviSpeeding v)
{
  gboolean mph = units_mph;
  switch (v) {
  case NAVI_SPEEDING_AT_LIMIT: return _("At the limit");
  case NAVI_SPEEDING_PLUS_10: return mph ? _("5 mph over") : _("10 km/h over");
  /* xgettext:no-c-format */
  case NAVI_SPEEDING_PERCENT_10: return _("10 % over");
  default: return mph ? _("3 mph over") : _("5 km/h over");
  }
}

static void
settings_refresh (SettingsPage *s)
{
  NaviSettings *st = s->p->navi->settings;
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->dark_row), dark_name (st->dark));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->view_row), view_name (st->view));
  vibeos_list_row_set_summary (VIBEOS_LIST_ROW (s->view_row),
                               st->view == NAVI_VIEW_3D   ? _("Tilted, always")
                               : st->view == NAVI_VIEW_2D ? _("Flat, always")
                                                          : _("Flat while browsing, 3D while driving"));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->voice_row), voice_name (st->voice));
  vibeos_list_row_set_summary (VIBEOS_LIST_ROW (s->voice_row), navi_voice_describe (s->p->voice, navi_route_language ()));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->language_row), navi_spoken_language_name (navi_route_spoken_code ()));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->speeding_row), speeding_name (st->speeding));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->vehicle_row), vehicle_name (st->vehicle));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (s->units_row), units_name (st->units));
  vibeos_list_row_set_summary (VIBEOS_LIST_ROW (s->units_row),
                               st->units == NAVI_UNITS_AUTO ? _("Miles in the UK and the US, else kilometers") : NULL);
  /* a saved place has a clear button at its end, and a tap asks before forgetting it; an unset one opens the
   * search to set it */
  for (int k = 0; k < 2; k++) {
    NaviPlace *place = k == 0 ? st->home : st->work;
    GtkWidget *row = k == 0 ? s->home_row : s->work_row;
    vibeos_list_row_set_summary (VIBEOS_LIST_ROW (row), place ? place->name : _("Not set: tap to search for it"));
    if (place)
      vibeos_list_row_set_end_icon (VIBEOS_LIST_ROW (row), "edit-clear-symbolic");
    else
      vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
    vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), TRUE);
  }
}

static void
choice_clicked (GtkWidget *row, SettingsPage *s)
{
  NaviSettings *st = s->p->navi->settings;
  int *field = g_object_get_data (G_OBJECT (row), "navi-field");
  *field = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "navi-value"));
  navi_settings_save (st);
  settings_refresh (s);
  navi_update_night (s->p->navi);
  navi_apply_view (s->p->navi); /* the map view at once, not at the camera's next move */
  navi_panels_sync_sound (s->p);
  navi_panels_set_units (s->p, NULL);
  vibeos_sheet_back (VIBEOS_SHEET (s->sheet));
}

static void
push_choices (SettingsPage *s, const char *title, int *field, const char *(*name) (int), int count)
{
  GtkWidget *list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 8, 16);
  for (int i = 0; i < count; i++) {
    GtkWidget *row = vibeos_list_row_new (name (i), NULL);
    if (*field == i) {
      GtkWidget *check = image_new ("object-select-symbolic", 20);
      gtk_widget_set_can_target (check, FALSE);
      vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (row), check);
    }
    g_object_set_data (G_OBJECT (row), "navi-field", field);
    g_object_set_data (G_OBJECT (row), "navi-value", GINT_TO_POINTER (i));
    g_signal_connect (row, "clicked", G_CALLBACK (choice_clicked), s);
    vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
  }
  vibeos_sheet_push (VIBEOS_SHEET (s->sheet), title, list);
}

static void
dark_clicked (GtkWidget *row, SettingsPage *s)
{
  push_choices (s, _("Dark mode"), (int *) &s->p->navi->settings->dark, (const char *(*) (int)) dark_name, 4);
}

static void
view_clicked (GtkWidget *row, SettingsPage *s)
{
  push_choices (s, _("Map view"), (int *) &s->p->navi->settings->view, (const char *(*) (int)) view_name, 3);
}

static void
speeding_clicked (GtkWidget *row, SettingsPage *s)
{
  push_choices (s, _("Speeding warning"), (int *) &s->p->navi->settings->speeding,
                (const char *(*) (int)) speeding_name, 4);
}

static void
vehicle_clicked (GtkWidget *row, SettingsPage *s)
{
  push_choices (s, _("Vehicle type"), (int *) &s->p->navi->settings->vehicle,
                (const char *(*) (int)) vehicle_name, 2);
}

static void
units_clicked (GtkWidget *row, SettingsPage *s)
{
  push_choices (s, _("Units"), (int *) &s->p->navi->settings->units, (const char *(*) (int)) units_name, 3);
}

static void
cameras_changed (GObject *sw, GParamSpec *pspec, SettingsPage *s)
{
  NaviPanels *p = s->p;
  p->navi->settings->cameras = vibeos_switch_get_active (VIBEOS_SWITCH (sw));
  navi_settings_save (p->navi->settings);
  if (!p->navi->settings->cameras)
    navi_panels_show_alert (p, NULL, NULL);
  navi_window_update_overlay (p->navi);
}

static void
voice_clicked (GtkWidget *row, SettingsPage *s)
{
  push_choices (s, _("Spoken directions"), (int *) &s->p->navi->settings->voice,
                (const char *(*) (int)) voice_name, 3);
}

/* The voice's language: Valhalla's narrative, the voice and Navi's own words in it from the next route on. Its voice
 * downloaded first when it is not on the phone (Agent's, dictate-download-dialog.h: the same voices, in the same
 * place), or the language stays as it was. */
static void
voice_language_ready (const char *language, gpointer data)
{
  NaviPanels *p = data;
  NaviSettings *st = p->navi->settings;
  g_free (st->voice_language);
  st->voice_language = g_strdup (language);
  navi_settings_save (st);
  navi_route_set_language (language);
  navi_voice_rescan (p->voice);
  g_message ("voice: speaking %s", language);
  SettingsPage *s = settings_sheet ? g_object_get_data (G_OBJECT (settings_sheet), "navi-settings") : NULL;
  if (s) {
    settings_refresh (s);
    vibeos_sheet_back (VIBEOS_SHEET (s->sheet));
  }
}

static void
language_chosen (GtkWidget *row, SettingsPage *s)
{
  const char *code = g_object_get_data (G_OBJECT (row), "navi-language");
#ifdef HAVE_PIPER
  dictate_download_ensure_voice (s->p->navi->win, code, "Navi", voice_language_ready, s->p);
#else
  voice_language_ready (code, s->p);
#endif
}

static void
language_clicked (GtkWidget *row, SettingsPage *s)
{
  GtkWidget *list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 8, 16);
  const char *now = navi_route_spoken_code ();
  const char *code;
  for (guint i = 0; (code = navi_spoken_language_code (i)); i++) {
#ifdef HAVE_PIPER
    /* what is not on the phone yet: "60.3 MB to download" */
    g_autofree char *note = dictate_download_voice_note (code);
#else
    const char *note = NULL;
#endif
    GtkWidget *item = vibeos_list_row_new (navi_spoken_language_name (code), note);
    if (!strcmp (code, now)) {
      GtkWidget *check = image_new ("object-select-symbolic", 20);
      gtk_widget_set_can_target (check, FALSE);
      vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (item), check);
    }
    g_object_set_data (G_OBJECT (item), "navi-language", (gpointer) code);
    g_signal_connect (item, "clicked", G_CALLBACK (language_chosen), s);
    vibeos_card_list_append (VIBEOS_CARD_LIST (list), item);
  }
  vibeos_sheet_push (VIBEOS_SHEET (s->sheet), _("Voice language"), list);
}

static void
switch_changed (GObject *sw, GParamSpec *pspec, SettingsPage *s)
{
  gboolean *field = g_object_get_data (sw, "navi-field");
  *field = vibeos_switch_get_active (VIBEOS_SWITCH (sw));
  navi_settings_save (s->p->navi->settings);
}

static void
forget_answered (VibeosDialog *d, int response, SettingsPage *s)
{
  if (response != VIBEOS_RESPONSE_OK)
    return;
  NaviSettings *st = s->p->navi->settings;
  NaviPlace **slot = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (d), "navi-home")) ? &st->home : &st->work;
  g_clear_pointer (slot, navi_place_free);
  navi_settings_save (st);
  settings_refresh (s);
}

static void
place_setting_clicked (GtkWidget *row, SettingsPage *s)
{
  NaviPanels *p = s->p;
  NaviSettings *st = p->navi->settings;
  gboolean home = row == s->home_row;
  NaviPlace *place = home ? st->home : st->work;
  if (!place) {
    /* not set: the search, ready to set it ("Set once and go") */
    vibeos_sheet_close (VIBEOS_SHEET (s->sheet));
    open_search (p, NULL);
    p->choosing = home ? CHOOSE_HOME : CHOOSE_WORK;
    set_text (p->search_status, home ? _("Search for your home address") : _("Search for your work address"));
    gtk_widget_set_visible (p->search_status, TRUE);
    return;
  }
  VibeosDialog *d = vibeos_dialog_new (home ? _("Forget Home?") : _("Forget Work?"));
  vibeos_dialog_set_message (d, place->name);
  vibeos_dialog_add_button (d, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (d, _("Forget"), VIBEOS_RESPONSE_OK);
  g_object_set_data (G_OBJECT (d), "navi-home", GINT_TO_POINTER (home));
  g_signal_connect (d, "response", G_CALLBACK (forget_answered), s);
  vibeos_dialog_present (d, p->navi->win);
}

static void
open_settings (gpointer data)
{
  NaviPanels *p = data;
  NaviSettings *st = p->navi->settings;
  SettingsPage *s = g_new0 (SettingsPage, 1);
  s->p = p;
  s->sheet = vibeos_sheet_new (_("Settings"));
  vibeos_sheet_set_full_height (VIBEOS_SHEET (s->sheet), TRUE);
  g_object_set_data_full (G_OBJECT (s->sheet), "navi-settings", s, g_free);
  g_set_weak_pointer (&settings_sheet, s->sheet);
  GtkWidget *list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 8, 24);
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), _("Map display"));
  s->dark_row = vibeos_list_row_new (_("Dark mode"), NULL);
  g_signal_connect (s->dark_row, "clicked", G_CALLBACK (dark_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->dark_row);
  s->view_row = vibeos_list_row_new (_("Map view"), _("Auto: flat while browsing, 3D while driving"));
  g_signal_connect (s->view_row, "clicked", G_CALLBACK (view_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->view_row);
  struct { const char *title, *summary; gboolean *field; } switches[] = {
    { _("Keep map north up"), NULL, &st->north_up },
    { _("Auto zoom"), _("Zoom out as the car goes faster"), &st->auto_zoom },
  };
  for (guint i = 0; i < G_N_ELEMENTS (switches); i++) {
    GtkWidget *row = vibeos_list_row_new (switches[i].title, switches[i].summary);
    GtkWidget *sw = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
    vibeos_switch_set_active (VIBEOS_SWITCH (sw), *switches[i].field);
    g_object_set_data (G_OBJECT (sw), "navi-field", switches[i].field);
    g_signal_connect (sw, "notify::active", G_CALLBACK (switch_changed), s);
    vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
  }
  s->units_row = vibeos_list_row_new (_("Units"), NULL);
  g_signal_connect (s->units_row, "clicked", G_CALLBACK (units_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->units_row);
  /* Waze's Navigation: the next route, and a new one on the way, follow them */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), _("Navigation"));
  s->vehicle_row = vibeos_list_row_new (_("Vehicle type"), NULL);
  g_signal_connect (s->vehicle_row, "clicked", G_CALLBACK (vehicle_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->vehicle_row);
  struct { const char *title; gboolean *field; } avoids[] = {
    { _("Avoid toll roads"), &st->avoid_tolls },
    { _("Avoid highways"), &st->avoid_highways },
    { _("Avoid ferries"), &st->avoid_ferries },
  };
  for (guint i = 0; i < G_N_ELEMENTS (avoids); i++) {
    GtkWidget *row = vibeos_list_row_new (avoids[i].title, NULL);
    GtkWidget *sw = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
    vibeos_switch_set_active (VIBEOS_SWITCH (sw), *avoids[i].field);
    g_object_set_data (G_OBJECT (sw), "navi-field", avoids[i].field);
    g_signal_connect (sw, "notify::active", G_CALLBACK (switch_changed), s);
    vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
  }
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), _("Voice and sound"));
  s->voice_row = vibeos_list_row_new (_("Spoken directions"), navi_voice_describe (p->voice, navi_route_language ()));
  g_signal_connect (s->voice_row, "clicked", G_CALLBACK (voice_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->voice_row);
  s->language_row = vibeos_list_row_new (_("Voice language"), NULL);
  g_signal_connect (s->language_row, "clicked", G_CALLBACK (language_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->language_row);
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), _("Alerts"));
  GtkWidget *cams = vibeos_list_row_new (_("Speed camera alerts"),
                                         _("Where the law allows them: never in Switzerland, only danger zones in "
                                           "France, and in Germany not for the driver while driving"));
  GtkWidget *cams_sw = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (cams), FALSE);
  vibeos_switch_set_active (VIBEOS_SWITCH (cams_sw), st->cameras);
  g_signal_connect (cams_sw, "notify::active", G_CALLBACK (cameras_changed), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), cams);
  s->speeding_row = vibeos_list_row_new (_("Speeding warning"), _("The speedometer turns red above the limit"));
  g_signal_connect (s->speeding_row, "clicked", G_CALLBACK (speeding_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->speeding_row);
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), _("Saved places"));
  s->home_row = vibeos_list_row_new (_("Home"), NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (s->home_row), "user-home-symbolic");
  g_signal_connect (s->home_row, "clicked", G_CALLBACK (place_setting_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->home_row);
  s->work_row = vibeos_list_row_new (_("Work"), NULL);
  /* the briefcase the search's Work row shows */
  GtkWidget *briefcase = navi_icon_new (NAVI_ICON_WORK, 24);
  navi_icon_follow_ink (briefcase);
  vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (s->work_row), briefcase);
  g_signal_connect (s->work_row, "clicked", G_CALLBACK (place_setting_clicked), s);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), s->work_row);
  settings_refresh (s);
  GtkWidget *scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_widget_set_vexpand (scroll, TRUE);
  vibeos_sheet_set_content (VIBEOS_SHEET (s->sheet), scroll);
  vibeos_sheet_present (VIBEOS_SHEET (s->sheet), p->navi->win);
}

/* Exit: Navi goes, nothing of it stays behind. The window's end stops the drive, the voice, the position and the
 * keep-awake (navi-main.c's on_destroy), then the application quits */
static void
exit_navi (Navi *n)
{
  GApplication *app = G_APPLICATION (n->app);
  gtk_window_destroy (GTK_WINDOW (n->win));
  g_application_quit (app);
}

static void
exit_answered (VibeosDialog *d, int response, Navi *n)
{
  if (response == VIBEOS_RESPONSE_OK)
    exit_navi (n);
}

static void
exit_clicked (gpointer data)
{
  NaviPanels *p = data;
  if (p->navi->mode != NAVI_MODE_DRIVE) {
    exit_navi (p->navi);
    return;
  }
  VibeosDialog *d = vibeos_dialog_new (NULL);
  vibeos_dialog_set_message (d, _("Stop navigation and exit?"));
  vibeos_dialog_add_button (d, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (d, _("Exit"), VIBEOS_RESPONSE_OK);
  g_signal_connect (d, "response", G_CALLBACK (exit_answered), p->navi);
  vibeos_dialog_present (d, p->navi->win);
}

static void
menu_clicked (GtkWidget *button, NaviPanels *p)
{
  VibeosMenu *menu = vibeos_menu_new ();
  vibeos_menu_append (menu, _("Settings"), open_settings, p);
  vibeos_menu_append_about (menu, &about_info);
  vibeos_menu_append (menu, _("Exit"), exit_clicked, p);
  vibeos_menu_present (menu, button);
}

void
navi_panels_open_menu (NaviPanels *p, GtkWidget *anchor)
{
  menu_clicked (anchor, p);
}

/* ---- the buttons over the map ---- */

static void
recenter_clicked (GtkWidget *w, NaviPanels *p)
{
  navi_recenter (p->navi);
}

static void
draw_compass (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  NaviPanels *p = data;
  GdkRGBA c = p->navi->night ? (GdkRGBA) { 0.9, 0.92, 0.95, 1 } : (GdkRGBA) { 0.35, 0.37, 0.4, 1 };
  navi_draw_compass (cr, MIN (w, h), p->bearing, &c);
}

static void
compass_clicked (GtkWidget *w, NaviPanels *p)
{
  NaviCamera cam = NAVI_CAMERA_UNSET;
  cam.bearing = 0;
  navi_map_ease_to (p->navi->map, &cam, 400, NAVI_EASING_EASE_OUT_CUBIC);
}

static const char *
sound_icon (NaviVoiceMode mode)
{
  return mode == NAVI_VOICE_OFF      ? "audio-volume-muted-symbolic"
         : mode == NAVI_VOICE_ALERTS ? "audio-volume-low-symbolic"
                                     : "audio-volume-high-symbolic";
}

/* the sound button shows the Spoken directions setting, wherever it was changed */
void
navi_panels_sync_sound (NaviPanels *p)
{
  NaviVoiceMode mode = p->navi->settings->voice;
  GtkWidget *img = vibeos_pressable_get_child (VIBEOS_PRESSABLE (p->sound_button));
  gtk_image_set_from_icon_name (GTK_IMAGE (img), sound_icon (mode));
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->sound_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  mode == NAVI_VOICE_OFF      ? _("Sound off")
                                  : mode == NAVI_VOICE_ALERTS ? _("Alerts only")
                                                              : _("Sound on"), -1);
  if (mode != NAVI_VOICE_ON)
    navi_voice_stop (p->voice);
}

/* Waze's three: sound on, alerts only, sound off */
static void
sound_clicked (GtkWidget *w, NaviPanels *p)
{
  NaviSettings *st = p->navi->settings;
  st->voice = st->voice == NAVI_VOICE_ON ? NAVI_VOICE_ALERTS : st->voice == NAVI_VOICE_ALERTS ? NAVI_VOICE_OFF
                                                                                              : NAVI_VOICE_ON;
  navi_settings_save (st);
  navi_panels_sync_sound (p);
}

/* ---- the pages ---- */

static GtkWidget *
build_browse (NaviPanels *p)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (box), image_new ("edit-find-symbolic", 22));
  gtk_box_append (GTK_BOX (box), label_new (_("Where to?"), NULL, 0));
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  p->where = pressable (box, "navi-where", NULL, G_CALLBACK (where_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (p->where), VIBEOS_RIPPLE_BOUNDED, 28);
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (page), p->where);
  return page;
}

static void
place_go_clicked (GtkWidget *w, NaviPanels *p)
{
  if (p->place)
    navi_preview_routes (p->navi, p->place);
}

static void
place_close_clicked (GtkWidget *w, NaviPanels *p)
{
  navi_back_to_browse (p->navi);
}

static void
save_as_clicked (GtkWidget *w, NaviPanels *p)
{
  if (!p->place)
    return;
  gboolean home = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (w), "navi-home"));
  NaviPlace **slot = home ? &p->navi->settings->home : &p->navi->settings->work;
  g_clear_pointer (slot, navi_place_free);
  *slot = navi_place_copy (p->place);
  navi_settings_save (p->navi->settings);
  GtkWidget *label = g_object_get_data (G_OBJECT (w), "navi-label");
  set_text (label, home ? _("Saved as Home") : _("Saved as Work"));
}

/* the star: the place saved, or no longer */
static void
star_clicked (GtkWidget *w, NaviPanels *p)
{
  if (!p->place)
    return;
  NaviSettings *st = p->navi->settings;
  int i = navi_settings_find_saved (st, p->place);
  if (i >= 0)
    navi_settings_remove_saved (st, i);
  else
    navi_settings_add_saved (st, p->place);
  navi_panels_show_place (p, p->place);
}

static GtkWidget *
action_button_full (NaviPanels *p, GtkWidget *icon, const char *text, GCallback clicked)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), icon);
  GtkWidget *label = label_new (text, NULL, 0.5);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), label);
  GtkWidget *b = pressable (box, "navi-action", NULL, clicked, p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (b), VIBEOS_RIPPLE_BOUNDED, 14);
  gtk_widget_set_hexpand (b, TRUE);
  /* three to a row, each a third whatever its label */
  gtk_widget_set_size_request (b, 1, -1);
  g_object_set_data (G_OBJECT (b), "navi-label", label);
  g_object_set_data (G_OBJECT (b), "navi-icon", icon);
  return b;
}

static GtkWidget *
action_button (NaviPanels *p, GtkWidget *icon, const char *text, gboolean home)
{
  GtkWidget *b = action_button_full (p, icon, text, G_CALLBACK (save_as_clicked));
  g_object_set_data (G_OBJECT (b), "navi-home", GINT_TO_POINTER (home));
  return b;
}

static GtkWidget *
build_place (NaviPanels *p)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (page, 20);
  gtk_widget_set_margin_end (page, 20);
  gtk_widget_set_margin_bottom (page, 18);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  /* a point of interest's marker */
  p->place_icon = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (p->place_icon, GTK_ALIGN_START);
  gtk_widget_set_visible (p->place_icon, FALSE);
  gtk_box_append (GTK_BOX (top), p->place_icon);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (texts, TRUE);
  p->place_name = label_new ("", "navi-place-name", 0);
  gtk_label_set_wrap (GTK_LABEL (p->place_name), TRUE);
  p->place_detail = label_new ("", "navi-dim", 0);
  gtk_label_set_wrap (GTK_LABEL (p->place_detail), TRUE);
  gtk_box_append (GTK_BOX (texts), p->place_name);
  gtk_box_append (GTK_BOX (texts), p->place_detail);
  gtk_box_append (GTK_BOX (top), texts);
  GtkWidget *close = pressable (image_new ("window-close-symbolic", 20), "navi-tonal", NULL,
                                G_CALLBACK (place_close_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (close), VIBEOS_RIPPLE_CIRCLE, 24);
  gtk_widget_set_valign (close, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (close), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Close"), -1);
  gtk_box_append (GTK_BOX (top), close);
  gtk_box_append (GTK_BOX (page), top);
  GtkWidget *chip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (chip, "navi-chip");
  gtk_widget_set_halign (chip, GTK_ALIGN_START);
  p->place_distance = label_new ("", NULL, 0);
  gtk_box_append (GTK_BOX (chip), p->place_distance);
  gtk_box_append (GTK_BOX (page), chip);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top (actions, 6);
  GtkWidget *work = navi_icon_new (NAVI_ICON_WORK, 22);
  GdkRGBA blue = { 0, 0.6, 1, 1 };
  navi_icon_set_color (work, &blue);
  gtk_box_set_homogeneous (GTK_BOX (actions), TRUE);
  p->place_star = action_button_full (p, image_new ("non-starred-symbolic", 22), _("Save"), G_CALLBACK (star_clicked));
  gtk_box_append (GTK_BOX (actions), p->place_star);
  gtk_box_append (GTK_BOX (actions), action_button (p, image_new ("user-home-symbolic", 22), _("Set as Home"), TRUE));
  gtk_box_append (GTK_BOX (actions), action_button (p, work, _("Set as Work"), FALSE));
  gtk_box_append (GTK_BOX (page), actions);
  p->place_go = text_button (_("View routes"), "navi-go", G_CALLBACK (place_go_clicked), p);
  p->place_go_label = vibeos_pressable_get_child (VIBEOS_PRESSABLE (p->place_go));
  gtk_widget_set_margin_top (p->place_go, 8);
  gtk_box_append (GTK_BOX (page), p->place_go);
  return page;
}

/* Go now's fill: the auto-start counting down */
static void
draw_go_fill (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  NaviPanels *p = data;
  if (p->countdown <= 0)
    return;
  cairo_rectangle (cr, 0, 0, w * MIN (p->countdown, 1.0), h);
  cairo_set_source_rgba (cr, 0, 0.32, 0.62, 0.45);
  cairo_fill (cr);
}

static gboolean
countdown_step (gpointer data)
{
  NaviPanels *p = data;
  p->countdown = (g_get_monotonic_time () - p->countdown_t0) / (COUNTDOWN_S * 1e6);
  gtk_widget_queue_draw (p->go_fill);
  if (p->countdown < 1)
    return G_SOURCE_CONTINUE;
  p->countdown_tick = 0;
  p->countdown = -1;
  navi_start_drive (p->navi);
  return G_SOURCE_REMOVE;
}

static void
start_countdown (NaviPanels *p)
{
  stop_countdown (p);
  p->countdown = 0;
  p->countdown_t0 = g_get_monotonic_time ();
  p->countdown_tick = g_timeout_add (33, countdown_step, p);
}

static void
go_clicked (GtkWidget *w, NaviPanels *p)
{
  stop_countdown (p);
  navi_start_drive (p->navi);
}

/* the reminder to leave for the place shown, when there is one still to come: "Reminder at 08:15", and Cancel */
static void
update_leave_note (NaviPanels *p)
{
  Navi *n = p->navi;
  g_autoptr (NaviPlan) plan = navi_plan_load ();
  gboolean show = plan && n->dest && plan->time > g_get_real_time () / G_USEC_PER_SEC &&
                  navi_geo_distance (plan->place->lat, plan->place->lng, n->dest->lat, n->dest->lng) < 30;
  p->plan_ahead = show;
  if (show) {
    g_autofree char *text = navi_plan_describe (plan->time, vibeos_clock_uses_24h ());
    set_text (p->leave_label, text);
  }
  gtk_widget_set_visible (p->leave_cancel, TRUE);
  gtk_widget_set_visible (p->leave_note, show);
  update_floating (p);
}

static void
plan_armed (gboolean armed, gpointer data)
{
  NaviPanels *p = data;
  if (armed) {
    update_leave_note (p);
    return;
  }
  /* no systemd user session to keep the timer (Navi in a sandbox, say): no reminder, and no plan */
  navi_plan_cancel ();
  p->plan_ahead = FALSE;
  set_text (p->leave_label, _("Reminders cannot be set here"));
  gtk_widget_set_visible (p->leave_cancel, FALSE);
  gtk_widget_set_visible (p->leave_note, TRUE);
  update_floating (p);
}

/* Leave later: a reminder at the time picked (today, or tomorrow when that time has gone by today) */
static void
leave_chosen (int hour, int minute, gpointer data)
{
  NaviPanels *p = data;
  Navi *n = p->navi;
  if (!n->dest)
    return;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) today = g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                                       g_date_time_get_day_of_month (now), hour, minute, 0);
  g_autoptr (GDateTime) at = g_date_time_compare (today, now) > 0 ? g_date_time_ref (today)
                                                                   : g_date_time_add_days (today, 1);
  p->plan_ahead = TRUE;
  navi_plan_set (n->dest, g_date_time_to_unix (at), plan_armed, p);
}

static void
leave_cancel_clicked (GtkWidget *w, NaviPanels *p)
{
  navi_plan_cancel ();
  update_leave_note (p);
}

static void
later_clicked (GtkWidget *w, NaviPanels *p)
{
  stop_countdown (p);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) soon = g_date_time_add_minutes (now, 30);
  vibeos_time_picker_dialog (p->navi->win, g_date_time_get_hour (soon), g_date_time_get_minute (soon), leave_chosen,
                             p, NULL);
}

static void
header_back_clicked (GtkWidget *w, NaviPanels *p)
{
  stop_countdown (p);
  if (p->navi->dest)
    navi_show_place (p->navi, p->navi->dest);
  else
    navi_back_to_browse (p->navi);
}

static GtkWidget *
build_routes_header (NaviPanels *p)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (card, "navi-card");
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  GtkWidget *back = vibeos_icon_button_new ("go-previous-symbolic");
  g_signal_connect (back, "clicked", G_CALLBACK (header_back_clicked), p);
  gtk_widget_set_valign (back, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), back);
  GtkWidget *rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (rows, 8);
  gtk_widget_set_margin_bottom (rows, 8);
  gtk_widget_set_hexpand (rows, TRUE);
  GtkWidget *from = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (dot, "navi-from-dot");
  gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (dot, 4);
  gtk_box_append (GTK_BOX (from), dot);
  p->routes_from = label_new (_("Your location"), "navi-header-text", 0);
  gtk_box_append (GTK_BOX (from), p->routes_from);
  GtkWidget *to = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *pin = navi_icon_new (NAVI_ICON_PIN, 20);
  gtk_box_append (GTK_BOX (to), pin);
  p->routes_header_to = label_new ("", "navi-header-text", 0);
  gtk_label_set_ellipsize (GTK_LABEL (p->routes_header_to), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (to), p->routes_header_to);
  gtk_box_append (GTK_BOX (rows), from);
  gtk_box_append (GTK_BOX (rows), to);
  gtk_box_append (GTK_BOX (card), rows);
  return card;
}

static void draw_limit_sign (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data);

static void
retry_clicked (GtkWidget *w, NaviPanels *p)
{
  if (p->navi->dest && p->navi->mode == NAVI_MODE_ROUTES) {
    g_autoptr (NaviPlace) dest = navi_place_copy (p->navi->dest);
    navi_preview_routes (p->navi, dest);
  }
}

static GtkWidget *
build_routes (NaviPanels *p)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (status, 20);
  gtk_widget_set_margin_top (status, 8);
  gtk_widget_set_margin_bottom (status, 16);
  p->routes_spinner = gtk_spinner_new ();
  gtk_box_append (GTK_BOX (status), p->routes_spinner);
  p->routes_status = label_new ("", "navi-sub", 0);
  gtk_label_set_wrap (GTK_LABEL (p->routes_status), TRUE);
  gtk_widget_set_hexpand (p->routes_status, TRUE);
  gtk_box_append (GTK_BOX (status), p->routes_status);
  /* after a failure: ask again */
  p->routes_retry = text_button (_("Retry"), "navi-later", G_CALLBACK (retry_clicked), p);
  gtk_widget_set_valign (p->routes_retry, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (p->routes_retry, 16);
  gtk_widget_set_visible (p->routes_retry, FALSE);
  gtk_box_append (GTK_BOX (status), p->routes_retry);
  g_object_set_data (G_OBJECT (page), "navi-status", status);
  gtk_box_append (GTK_BOX (page), status);
  /* the selected route, then (swiped up) the others */
  p->routes_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (page), p->routes_list);
  p->routes_more = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (p->routes_more), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration (GTK_REVEALER (p->routes_more), 250);
  gtk_box_append (GTK_BOX (page), p->routes_more);
  p->leave_note = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_start (p->leave_note, 20);
  gtk_widget_set_margin_end (p->leave_note, 16);
  gtk_widget_set_margin_bottom (p->leave_note, 2);
  GtkWidget *alarm = image_new ("alarm-symbolic", 20);
  gtk_widget_add_css_class (alarm, "navi-dim");
  gtk_box_append (GTK_BOX (p->leave_note), alarm);
  p->leave_label = label_new ("", "navi-dim", 0);
  gtk_widget_set_hexpand (p->leave_label, TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (p->leave_label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (p->leave_note), p->leave_label);
  p->leave_cancel = text_button (_("Cancel"), "navi-later", G_CALLBACK (leave_cancel_clicked), p);
  gtk_widget_set_valign (p->leave_cancel, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (p->leave_note), p->leave_cancel);
  gtk_widget_set_visible (p->leave_note, FALSE);
  gtk_box_append (GTK_BOX (page), p->leave_note);
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  gtk_widget_set_margin_start (buttons, 16);
  gtk_widget_set_margin_end (buttons, 16);
  gtk_widget_set_margin_top (buttons, 8);
  gtk_widget_set_margin_bottom (buttons, 18);
  gtk_box_append (GTK_BOX (buttons), text_button (_("Leave later"), "navi-later", G_CALLBACK (later_clicked), p));
  GtkWidget *go = gtk_overlay_new ();
  p->go_fill = gtk_drawing_area_new ();
  /* weak: the window's "destroy" comes after its widgets went, and Exit's quiet () stops the countdown then, which
   * drew a fill already gone (a Gtk-CRITICAL on every Exit) */
  g_object_add_weak_pointer (G_OBJECT (p->go_fill), (gpointer *) &p->go_fill);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (p->go_fill), draw_go_fill, p, NULL);
  gtk_overlay_set_child (GTK_OVERLAY (go), p->go_fill);
  GtkWidget *go_label = gtk_label_new (_("Go now"));
  gtk_overlay_add_overlay (GTK_OVERLAY (go), go_label);
  p->go_button = pressable (go, "navi-button", "navi-go", G_CALLBACK (go_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (p->go_button), VIBEOS_RIPPLE_BOUNDED, 26);
  gtk_widget_set_hexpand (p->go_button, TRUE);
  gtk_box_append (GTK_BOX (buttons), p->go_button);
  g_object_set_data (G_OBJECT (page), "navi-buttons", buttons);
  gtk_box_append (GTK_BOX (page), buttons);
  p->routes_box = page;
  return page;
}

static void
route_row_clicked (GtkWidget *row, NaviPanels *p)
{
  stop_countdown (p);
  navi_select_route (p->navi, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "navi-index")));
}

static GtkWidget *
route_row (NaviPanels *p, NaviRoute *r, guint i, gboolean selected, gboolean best)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  g_autofree char *time = navi_format_duration (r->time);
  GtkWidget *t = label_new (time, "navi-route-time", 0);
  gtk_widget_set_hexpand (t, TRUE);
  gtk_box_append (GTK_BOX (top), t);
  g_autofree char *dist = km_text (r->length);
  gtk_box_append (GTK_BOX (top), label_new (dist, "navi-sub", 1));
  gtk_box_append (GTK_BOX (box), top);
  if (r->via) {
    g_autofree char *via = g_strdup_printf (_("Via %s"), r->via);
    GtkWidget *v = label_new (via, "navi-route-via", 0);
    gtk_label_set_ellipsize (GTK_LABEL (v), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (box), v);
  }
  GString *note = g_string_new (best ? _("Best route") : "");
  if (r->toll)
    g_string_append_printf (note, "%s%s", note->len ? " · " : "", _("Toll road"));
  if (r->ferry)
    g_string_append_printf (note, "%s%s", note->len ? " · " : "", _("Ferry"));
  if (note->len)
    gtk_box_append (GTK_BOX (box), label_new (note->str, "navi-dim", 0));
  g_string_free (note, TRUE);
  GtkWidget *row = pressable (box, "navi-route-row", selected ? "selected" : NULL, G_CALLBACK (route_row_clicked), p);
  g_object_set_data (G_OBJECT (row), "navi-index", GUINT_TO_POINTER (i));
  return row;
}

static void
clear_box (GtkWidget *box)
{
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (box)))
    gtk_widget_unparent (c);
}

static GtkWidget *
build_drive (NaviPanels *p)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  /* following: search, the arrival, the routes */
  p->drive_main = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (p->drive_main, 16);
  gtk_widget_set_margin_end (p->drive_main, 16);
  gtk_widget_set_margin_bottom (p->drive_main, 14);
  GtkWidget *search = pressable (image_new ("edit-find-symbolic", 22), "navi-tonal", NULL,
                                 G_CALLBACK (where_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (search), VIBEOS_RIPPLE_CIRCLE, 24);
  gtk_widget_set_valign (search, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (search), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Search"), -1);
  gtk_widget_set_visible (search, !p->navi->tablet); /* a tablet has it in the rail */
  gtk_box_append (GTK_BOX (p->drive_main), search);
  GtkWidget *mid = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (mid, TRUE);
  p->drive_eta = label_new ("", "navi-eta", 0.5);
  p->drive_sub = label_new ("", "navi-sub", 0.5);
  gtk_box_append (GTK_BOX (mid), p->drive_eta);
  gtk_box_append (GTK_BOX (mid), p->drive_sub);
  GtkWidget *mid_press = pressable (mid, NULL, NULL, G_CALLBACK (eta_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (mid_press), VIBEOS_RIPPLE_NONE, 0);
  gtk_widget_set_hexpand (mid_press, TRUE);
  gtk_box_append (GTK_BOX (p->drive_main), mid_press);
  GtkWidget *routes_icon = navi_icon_new (NAVI_ICON_ROUTES, 24);
  navi_icon_follow_ink (routes_icon);
  GtkWidget *overview = pressable (routes_icon, "navi-tonal", NULL, NULL, NULL);
  g_signal_connect_swapped (overview, "clicked", G_CALLBACK (navi_overview), p->navi);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (overview), VIBEOS_RIPPLE_CIRCLE, 24);
  gtk_widget_set_valign (overview, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (overview), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Overview"), -1);
  gtk_box_append (GTK_BOX (p->drive_main), overview);
  gtk_box_append (GTK_BOX (page), p->drive_main);
  /* free: Re-center with the time and distance left, and Overview */
  p->drive_free = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (p->drive_free, 16);
  gtk_widget_set_margin_end (p->drive_free, 16);
  gtk_widget_set_margin_bottom (p->drive_free, 14);
  GtkWidget *rc = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *cross = navi_icon_new (NAVI_ICON_CROSSHAIR, 34);
  navi_icon_follow_ink (cross);
  gtk_box_append (GTK_BOX (rc), cross);
  GtkWidget *rc_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (rc_text), label_new (_("Re-center"), "navi-route-via", 0));
  p->drive_free_sub = label_new ("", "navi-dim", 0);
  gtk_box_append (GTK_BOX (rc_text), p->drive_free_sub);
  gtk_box_append (GTK_BOX (rc), rc_text);
  GtkWidget *rc_press = pressable (rc, NULL, NULL, G_CALLBACK (recenter_clicked), p);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (rc_press), VIBEOS_RIPPLE_BOUNDED, 12);
  gtk_widget_set_hexpand (rc_press, TRUE);
  gtk_box_append (GTK_BOX (p->drive_free), rc_press);
  GtkWidget *ov = pressable (label_new (_("Overview"), "navi-link", 0.5), NULL, NULL, NULL, NULL);
  g_signal_connect_swapped (ov, "clicked", G_CALLBACK (navi_overview), p->navi);
  gtk_widget_set_valign (ov, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (p->drive_free), ov);
  gtk_widget_set_visible (p->drive_free, FALSE);
  gtk_box_append (GTK_BOX (page), p->drive_free);
  /* swiped up: where to, the way, the route's progress, Stop */
  p->drive_more = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (p->drive_more), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration (GTK_REVEALER (p->drive_more), 250);
  GtkWidget *more = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (more, 20);
  gtk_widget_set_margin_end (more, 20);
  gtk_widget_set_margin_bottom (more, 18);
  p->drive_dest = label_new ("", "navi-route-via", 0.5);
  gtk_label_set_wrap (GTK_LABEL (p->drive_dest), TRUE);
  gtk_label_set_justify (GTK_LABEL (p->drive_dest), GTK_JUSTIFY_CENTER);
  p->drive_via = label_new ("", "navi-dim", 0.5);
  gtk_label_set_wrap (GTK_LABEL (p->drive_via), TRUE);
  gtk_label_set_justify (GTK_LABEL (p->drive_via), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (more), p->drive_dest);
  gtk_box_append (GTK_BOX (more), p->drive_via);
  p->drive_progress = gtk_progress_bar_new ();
  gtk_widget_set_margin_top (p->drive_progress, 10);
  gtk_widget_set_margin_bottom (p->drive_progress, 10);
  gtk_box_append (GTK_BOX (more), p->drive_progress);
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  GtkWidget *stop = text_button (_("Stop"), "navi-stop", NULL, NULL);
  g_signal_connect_swapped (stop, "clicked", G_CALLBACK (navi_stop_drive), p->navi);
  gtk_box_append (GTK_BOX (buttons), stop);
  GtkWidget *resume = text_button (_("Continue"), "navi-go", NULL, NULL);
  g_signal_connect (resume, "clicked", G_CALLBACK (continue_clicked), p);
  gtk_box_append (GTK_BOX (buttons), resume);
  gtk_box_append (GTK_BOX (more), buttons);
  gtk_revealer_set_child (GTK_REVEALER (p->drive_more), more);
  gtk_box_append (GTK_BOX (page), p->drive_more);
  return page;
}

static void
arrived_done_clicked (GtkWidget *w, NaviPanels *p)
{
  navi_stop_drive (p->navi);
}

static GtkWidget *
build_arrived (NaviPanels *p)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (page, 20);
  gtk_widget_set_margin_end (page, 20);
  gtk_widget_set_margin_bottom (page, 18);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_box_append (GTK_BOX (top), navi_icon_new (NAVI_ICON_FINISH, 44));
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_append (GTK_BOX (texts), label_new (_("You have arrived"), "navi-arrived-title", 0));
  p->arrived_name = label_new ("", "navi-dim", 0);
  gtk_label_set_wrap (GTK_LABEL (p->arrived_name), TRUE);
  gtk_box_append (GTK_BOX (texts), p->arrived_name);
  p->arrived_side = label_new ("", NULL, 0);
  gtk_label_set_wrap (GTK_LABEL (p->arrived_side), TRUE);
  gtk_widget_set_visible (p->arrived_side, FALSE);
  gtk_box_append (GTK_BOX (texts), p->arrived_side);
  gtk_box_append (GTK_BOX (top), texts);
  gtk_box_append (GTK_BOX (page), top);
  GtkWidget *done = text_button (_("Done"), "navi-go", G_CALLBACK (arrived_done_clicked), p);
  gtk_widget_set_margin_top (done, 8);
  gtk_box_append (GTK_BOX (page), done);
  return page;
}

/* ---- the banner ---- */

/* "Your destination is on the right" for a destination on the right or left of the road, else NULL */
static const char *
destination_side (NaviTurn type)
{
  if (type == NAVI_TURN_DESTINATION_RIGHT)
    return _("Your destination is on the right");
  if (type == NAVI_TURN_DESTINATION_LEFT)
    return _("Your destination is on the left");
  return NULL;
}

static void
draw_lanes (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  NaviPanels *p = data;
  navi_draw_lanes (cr, width, height, p->lanes, p->n_lanes);
}

static GtkWidget *
build_banner (NaviPanels *p)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (box, GTK_ALIGN_START);
  GtkWidget *main = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (main, "navi-banner");
  p->banner_turn = navi_icon_new (NAVI_ICON_TURN, 64);
  gtk_widget_set_valign (p->banner_turn, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (main), p->banner_turn);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (texts, TRUE);
  p->banner_distance = label_new ("", "navi-banner-distance", 0);
  p->banner_street = label_new ("", "navi-banner-street", 0);
  gtk_label_set_wrap (GTK_LABEL (p->banner_street), TRUE);
  gtk_label_set_lines (GTK_LABEL (p->banner_street), 2);
  gtk_label_set_ellipsize (GTK_LABEL (p->banner_street), PANGO_ELLIPSIZE_END);
  p->banner_side = label_new ("", "navi-banner-side", 0);
  gtk_label_set_wrap (GTK_LABEL (p->banner_side), TRUE);
  gtk_widget_set_visible (p->banner_side, FALSE);
  p->banner_note = label_new ("", "navi-banner-note", 0);
  gtk_widget_set_visible (p->banner_note, FALSE);
  gtk_box_append (GTK_BOX (texts), p->banner_distance);
  gtk_box_append (GTK_BOX (texts), p->banner_street);
  gtk_box_append (GTK_BOX (texts), p->banner_side);
  gtk_box_append (GTK_BOX (texts), p->banner_note);
  gtk_box_append (GTK_BOX (main), texts);
  gtk_box_append (GTK_BOX (box), main);
  /* the lanes up to the turn, the ones to take lit, as Waze's: only where the map has them */
  p->banner_lanes = gtk_drawing_area_new ();
  gtk_widget_add_css_class (p->banner_lanes, "navi-lanes");
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (p->banner_lanes), 46);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (p->banner_lanes), draw_lanes, p, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->banner_lanes), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Lanes"), -1);
  gtk_widget_set_visible (p->banner_lanes, FALSE);
  gtk_box_append (GTK_BOX (box), p->banner_lanes);
  /* "and then": the turn right after this one */
  p->then_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (p->then_box, "navi-then");
  gtk_widget_set_halign (p->then_box, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (p->then_box), label_new (_("and then"), NULL, 0));
  p->then_turn = navi_icon_new (NAVI_ICON_TURN, 22);
  GdkRGBA grey = { 0.8, 0.81, 0.83, 1 };
  navi_icon_set_color (p->then_turn, &grey);
  gtk_box_append (GTK_BOX (p->then_box), p->then_turn);
  gtk_widget_set_visible (p->then_box, FALSE);
  gtk_box_append (GTK_BOX (box), p->then_box);
  return box;
}

/* ---- the public part ---- */

#define STREET_GAP 10 /* dp between the speed and the street */

/* The street the car is on: centred under the map as Waze has it, but never over the speed at the left. A name too
 * long to sit centred clear of it starts right of the speed instead, and one longer still ends in "…" short of the
 * other edge (the label ellipsizes in what it is given). */
static gboolean
place_street (GtkOverlay *overlay, GtkWidget *child, GdkRectangle *alloc, NaviPanels *p)
{
  if (child != p->street_pill)
    return FALSE;
  int w = gtk_widget_get_width (GTK_WIDGET (overlay)), h = gtk_widget_get_height (GTK_WIDGET (overlay));
  int clear = 0, min, nat;
  if (gtk_widget_get_visible (p->speed_column)) {
    gtk_widget_measure (p->speedo, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
    clear = gtk_widget_get_margin_start (p->speed_column) + nat + STREET_GAP;
  }
  gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  /* the pill's own alignment and margins place it inside what it is given: the whole map, or the part of it right
   * of the speed */
  if (w - nat >= 2 * clear)
    *alloc = (GdkRectangle) { 0, 0, w, h };
  else
    *alloc = (GdkRectangle) { clear, 0, MAX (w - clear - STREET_GAP, 0), h };
  return TRUE;
}

NaviPanels *
navi_panels_new (Navi *n)
{
  NaviPanels *p = g_new0 (NaviPanels, 1);
  p->navi = n;
  p->countdown = -1;
  p->voice = navi_voice_new ();
  navi_voice_set_stats (p->voice, n->stats);
  /* a shell without the property (or none) leaves it 0 */
  p->shell_cancel = g_cancellable_new ();
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                            G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "vibeos.Shell", "/vibeos/Shell",
                            "vibeos.Shell", p->shell_cancel, shell_ready, p);
  vibeos_css_load_string (CSS, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
#ifdef HAVE_PIPER
  /* Agent's download dialogs (Voice language) look as they do in Agent */
  vibeos_css_load_resource ("/vibeos/Navi/dictate.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
#endif

  p->stack = gtk_stack_new ();
  gtk_stack_set_vhomogeneous (GTK_STACK (p->stack), FALSE);
  gtk_stack_set_interpolate_size (GTK_STACK (p->stack), TRUE);
  gtk_stack_set_transition_type (GTK_STACK (p->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (p->stack), 200);
  gtk_stack_add_named (GTK_STACK (p->stack), build_browse (p), "browse");
  gtk_stack_add_named (GTK_STACK (p->stack), build_place (p), "place");
  gtk_stack_add_named (GTK_STACK (p->stack), build_routes (p), "routes");
  gtk_stack_add_named (GTK_STACK (p->stack), build_drive (p), "drive");
  gtk_stack_add_named (GTK_STACK (p->stack), build_arrived (p), "arrived");

  p->sheet = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->sheet, "navi-sheet");
  gtk_widget_set_valign (p->sheet, GTK_ALIGN_END);
  if (n->tablet) {
    /* a card at the map's bottom left, the map going on all round it */
    gtk_widget_set_halign (p->sheet, GTK_ALIGN_START);
    gtk_widget_set_size_request (p->sheet, TABLET_CARD_DP, -1);
    gtk_widget_set_margin_start (p->sheet, 16);
    gtk_widget_set_margin_bottom (p->sheet, 16);
  }
  p->handle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (p->handle, "navi-handle");
  gtk_widget_set_halign (p->handle, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (p->sheet), p->handle);
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (handle_released), p);
  gtk_widget_add_controller (p->handle, GTK_EVENT_CONTROLLER (click));
  gtk_widget_set_margin_top (p->handle, 8);
  gtk_box_append (GTK_BOX (p->sheet), p->stack);
  GtkGesture *drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-end", G_CALLBACK (sheet_drag_end), p);
  gtk_widget_add_controller (p->sheet, GTK_EVENT_CONTROLLER (drag));

  /* over the map */
  GtkOverlay *ov = GTK_OVERLAY (n->map_overlay);
  if (!n->tablet) {
    p->menu_button = round_button ("open-menu-symbolic", NULL, _("Menu"), G_CALLBACK (menu_clicked), p);
    gtk_widget_add_css_class (p->menu_button, "navi-square");
    gtk_widget_set_halign (p->menu_button, GTK_ALIGN_START);
    gtk_widget_set_valign (p->menu_button, GTK_ALIGN_START);
    gtk_widget_set_margin_start (p->menu_button, 14);
    gtk_widget_set_margin_top (p->menu_button, 14);
    gtk_overlay_add_overlay (ov, p->menu_button);
  }
  GtkWidget *cross = navi_icon_new (NAVI_ICON_CROSSHAIR, 28);
  navi_icon_follow_ink (cross);
  p->recenter_button = round_button (NULL, cross, _("Re-center"), G_CALLBACK (recenter_clicked), p);
  gtk_widget_set_halign (p->recenter_button, GTK_ALIGN_START);
  gtk_widget_set_valign (p->recenter_button, GTK_ALIGN_END);
  gtk_widget_set_margin_start (p->recenter_button, 14);
  gtk_widget_set_visible (p->recenter_button, FALSE);
  gtk_overlay_add_overlay (ov, p->recenter_button);
  p->compass_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (p->compass_area), 30);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (p->compass_area), 30);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (p->compass_area), draw_compass, p, NULL);
  p->compass = round_button (NULL, p->compass_area, _("North up"), G_CALLBACK (compass_clicked), p);
  gtk_widget_set_halign (p->compass, GTK_ALIGN_END);
  gtk_widget_set_valign (p->compass, GTK_ALIGN_START);
  gtk_widget_set_margin_end (p->compass, 14);
  gtk_widget_set_margin_top (p->compass, 14);
  gtk_widget_set_visible (p->compass, FALSE);
  gtk_overlay_add_overlay (ov, p->compass);
  p->sound_button = round_button (sound_icon (n->settings->voice), NULL, _("Sound on"), G_CALLBACK (sound_clicked), p);
  gtk_widget_set_halign (p->sound_button, GTK_ALIGN_END);
  gtk_widget_set_valign (p->sound_button, GTK_ALIGN_START);
  gtk_widget_set_margin_end (p->sound_button, 14);
  gtk_widget_set_visible (p->sound_button, FALSE);
  gtk_overlay_add_overlay (ov, p->sound_button);

  /* the speed and the street, bottom left and middle, while following a drive; over the speed the road's limit, and
   * over that a camera ahead */
  p->speed_column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (p->speed_column, GTK_ALIGN_START);
  gtk_widget_set_valign (p->speed_column, GTK_ALIGN_END);
  gtk_widget_set_margin_start (p->speed_column, 14);
  gtk_widget_set_can_target (p->speed_column, FALSE);
  p->alert_card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (p->alert_card, "navi-alert");
  gtk_widget_set_halign (p->alert_card, GTK_ALIGN_START);
  GtkWidget *disc = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (disc, "navi-alert-disc");
  gtk_widget_set_valign (disc, GTK_ALIGN_CENTER);
  GtkWidget *cam = navi_icon_new (NAVI_ICON_CAMERA, 24);
  gtk_widget_set_halign (cam, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (cam, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (cam, TRUE);
  gtk_box_append (GTK_BOX (disc), cam);
  gtk_box_append (GTK_BOX (p->alert_card), disc);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  p->alert_title = label_new ("", "navi-alert-title", 0);
  p->alert_detail = label_new ("", "navi-alert-detail", 0);
  gtk_box_append (GTK_BOX (texts), p->alert_title);
  gtk_box_append (GTK_BOX (texts), p->alert_detail);
  gtk_box_append (GTK_BOX (p->alert_card), texts);
  gtk_widget_set_visible (p->alert_card, FALSE);
  gtk_box_append (GTK_BOX (p->speed_column), p->alert_card);
  p->limit_kmh = NAVI_LIMIT_UNKNOWN;
  p->speed = -1;
  /* metric until a road's country says otherwise: a British phone in Berlin showed miles, and asked its first
   * route's directions in them */
  p->mph = units_mph = p->navi->settings->units == NAVI_UNITS_MI ||
                       (p->navi->settings->units == NAVI_UNITS_AUTO && g_getenv ("NAVI_SPEED_UNITS") &&
                        navi_speed_in_mph ());
  p->us_signs = FALSE;
  navi_format_set_miles (p->mph);
  p->limit_sign = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (p->limit_sign), 54);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (p->limit_sign), 54);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (p->limit_sign), draw_limit_sign, p, NULL);
  gtk_widget_set_halign (p->limit_sign, GTK_ALIGN_START);
  gtk_widget_set_margin_start (p->limit_sign, 4);
  gtk_widget_set_visible (p->limit_sign, FALSE);
  gtk_box_append (GTK_BOX (p->speed_column), p->limit_sign);
  p->speedo = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->speedo, "navi-speedo");
  gtk_widget_set_halign (p->speedo, GTK_ALIGN_START);
  GtkWidget *sv = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (sv, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (sv, TRUE);
  p->speedo_value = label_new ("--", "navi-speedo-value", 0.5);
  gtk_box_append (GTK_BOX (sv), p->speedo_value);
  p->speedo_unit = label_new (p->mph ? "mph" : "km/h", "navi-speedo-unit", 0.5);
  gtk_box_append (GTK_BOX (sv), p->speedo_unit);
  gtk_box_append (GTK_BOX (p->speedo), sv);
  gtk_box_append (GTK_BOX (p->speed_column), p->speedo);
  gtk_widget_set_visible (p->speed_column, FALSE);
  gtk_overlay_add_overlay (ov, p->speed_column);
  p->street_pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (p->street_pill, "navi-street-pill");
  gtk_widget_set_halign (p->street_pill, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->street_pill, GTK_ALIGN_END);
  gtk_widget_set_can_target (p->street_pill, FALSE);
  p->street_label = label_new ("", NULL, 0.5);
  gtk_label_set_ellipsize (GTK_LABEL (p->street_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (p->street_label), 22);
  gtk_box_append (GTK_BOX (p->street_pill), p->street_label);
  gtk_widget_set_visible (p->street_pill, FALSE);
  gtk_overlay_add_overlay (ov, p->street_pill);
  g_signal_connect (ov, "get-child-position", G_CALLBACK (place_street), p);

  p->note = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (p->note, "navi-note");
  gtk_widget_set_halign (p->note, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->note, GTK_ALIGN_START);
  gtk_widget_set_margin_top (p->note, 70);
  gtk_widget_set_can_target (p->note, FALSE);
  gtk_box_append (GTK_BOX (p->note), label_new ("", NULL, 0.5));
  gtk_widget_set_visible (p->note, FALSE);
  gtk_overlay_add_overlay (ov, p->note);

  p->routes_header = build_routes_header (p);
  gtk_widget_set_visible (p->routes_header, FALSE);
  if (n->tablet) {
    gtk_widget_set_halign (p->routes_header, GTK_ALIGN_START);
    gtk_widget_set_size_request (p->routes_header, 380, -1);
  }
  gtk_overlay_add_overlay (ov, p->routes_header);
  p->banner = build_banner (p);
  if (n->tablet) {
    gtk_widget_add_css_class (n->win, "navi-tablet");
    gtk_widget_set_halign (p->banner, GTK_ALIGN_START);
    gtk_widget_set_size_request (p->banner, 460, -1);
  }
  gtk_widget_set_visible (p->banner, FALSE);
  gtk_overlay_add_overlay (ov, p->banner);
  gtk_overlay_add_overlay (ov, p->sheet);
  if (n->tablet) {
    /* the search's card: as tall as the map, the card's width, at the left */
    p->search_host = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (p->search_host), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_add_css_class (p->search_host, "navi-search-card");
    gtk_widget_set_overflow (p->search_host, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_halign (p->search_host, GTK_ALIGN_START);
    gtk_widget_set_size_request (p->search_host, TABLET_CARD_DP, -1);
    gtk_widget_set_margin_start (p->search_host, 16);
    gtk_widget_set_margin_top (p->search_host, 16);
    gtk_widget_set_margin_bottom (p->search_host, 16);
    gtk_widget_set_visible (p->search_host, FALSE);
    gtk_overlay_add_overlay (ov, p->search_host);
  }
  return p;
}

void
navi_panels_quiet (NaviPanels *p)
{
  stop_countdown (p);
  navi_voice_stop (p->voice);
  navi_voice_rest (p->voice);
#ifdef HAVE_DICTATION
  if (p->dictation && dictate_engine_get_state (DICTATE_ENGINE (p->dictation)) != DICTATE_IDLE)
    dictate_engine_stop (DICTATE_ENGINE (p->dictation));
#endif
}

void
navi_panels_free (NaviPanels *p)
{
  stop_countdown (p);
  if (p->go_fill)
    g_object_remove_weak_pointer (G_OBJECT (p->go_fill), (gpointer *) &p->go_fill);
  g_clear_pointer (&p->saved_target, navi_place_free);
  if (p->dictation)
    g_signal_handlers_disconnect_by_data (p->dictation, p);
  g_clear_object (&p->dictation);
  if (p->dictated)
    g_string_free (p->dictated, TRUE);
  g_clear_handle_id (&p->search_source, g_source_remove);
  g_cancellable_cancel (p->search_cancel);
  g_clear_object (&p->search_cancel);
  g_cancellable_cancel (p->shell_cancel);
  g_clear_object (&p->shell_cancel);
  if (p->shell)
    g_signal_handlers_disconnect_by_data (p->shell, p);
  g_clear_object (&p->shell);
  g_clear_pointer (&p->place, navi_place_free);
  g_free (p->category_text);
  g_clear_pointer (&p->markers, g_hash_table_unref);
  if (p->along)
    p->along->p = NULL;
  navi_voice_free (p->voice);
  g_free (p);
}

int
navi_panels_bottom_inset (NaviPanels *p)
{
  if (!gtk_widget_get_visible (p->sheet))
    return 0;
  int min, nat, w = p->navi->tablet ? TABLET_CARD_DP : gtk_widget_get_width (p->navi->map_overlay);
  gtk_widget_measure (p->sheet, GTK_ORIENTATION_VERTICAL, w > 0 ? w : -1, &min, &nat, NULL, NULL);
  return nat;
}

int
navi_panels_top_inset (NaviPanels *p)
{
  GtkWidget *top = NULL;
  /* the routes' header is over the map on a tablet too (at its left), above the bubbles and the note */
  if (gtk_widget_get_visible (p->routes_header))
    top = p->routes_header;
  else if (gtk_widget_get_visible (p->banner))
    top = p->banner;
  if (!top)
    return 0;
  int min, nat, w = gtk_widget_get_width (p->navi->map_overlay);
  gtk_widget_measure (top, GTK_ORIENTATION_VERTICAL, w > 0 ? w : -1, &min, &nat, NULL, NULL);
  return nat + 10;
}

/* the floating buttons: which show, and how far above the sheet */
static void
update_floating (NaviPanels *p)
{
  Navi *n = p->navi;
  gboolean drive = n->mode == NAVI_MODE_DRIVE, follow = n->cam == NAVI_CAM_FOLLOW;
  gboolean browse = n->mode == NAVI_MODE_BROWSE;
  int bottom = navi_panels_bottom_inset (p) + 14;
  if (p->menu_button)
    gtk_widget_set_visible (p->menu_button, browse);
  gtk_widget_set_visible (p->recenter_button, browse && !follow);
  gtk_widget_set_margin_bottom (p->recenter_button, bottom);
  gtk_widget_set_visible (p->speed_column, follow && (drive || (browse && p->moving)));
  gtk_widget_set_margin_bottom (p->speed_column, bottom);
  gtk_widget_set_visible (p->street_pill, drive && follow && *gtk_label_get_text (GTK_LABEL (p->street_label)));
  gtk_widget_set_margin_bottom (p->street_pill, bottom + 12);
  gtk_widget_set_visible (p->sound_button, drive && follow);
  gtk_widget_set_margin_top (p->sound_button, navi_panels_top_inset (p) + 10);
  gtk_widget_set_visible (p->compass, !drive && fabs (remainder (p->bearing, 360)) > 1 && !follow &&
                                        n->mode != NAVI_MODE_ROUTES);
  gtk_widget_set_margin_top (p->compass, browse ? 14 : navi_panels_top_inset (p) + 10);
  gtk_widget_set_visible (p->drive_main, follow || !drive);
  gtk_widget_set_visible (p->drive_free, drive && !follow);
  GtkWidget *note = gtk_widget_get_first_child (p->note);
  gboolean has_note = *gtk_label_get_text (GTK_LABEL (note));
  gtk_widget_set_visible (p->note, has_note && !drive);
  gtk_widget_set_margin_top (p->note, browse ? 76 : navi_panels_top_inset (p) + 4);
  gtk_widget_set_visible (p->banner_note, has_note);
}

static gboolean
update_floating_later (gpointer data)
{
  update_floating (data);
  return G_SOURCE_REMOVE;
}

void
navi_panels_set_mode (NaviPanels *p, NaviMode mode)
{
  static const char *const pages[] = { "browse", "place", "routes", "drive", "arrived" };
  if (mode != NAVI_MODE_ROUTES)
    stop_countdown (p);
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack), pages[mode]);
  set_expanded (p, FALSE);
  /* a tablet's map is clean while browsing: Search is in the rail */
  gtk_widget_set_visible (p->sheet, !(p->navi->tablet && mode == NAVI_MODE_BROWSE));
  gtk_widget_set_visible (p->routes_header, mode == NAVI_MODE_ROUTES);
  gtk_widget_set_visible (p->banner, mode == NAVI_MODE_DRIVE);
  if (mode == NAVI_MODE_ROUTES && p->navi->dest)
    set_text (p->routes_header_to, p->navi->dest->name);
  if (mode == NAVI_MODE_ARRIVED && p->navi->dest) {
    set_text (p->arrived_name, p->navi->dest->name);
    NaviRoute *r = p->navi->guidance ? navi_guidance_get_route (p->navi->guidance) : NULL;
    const char *side = r && r->n_maneuvers ? destination_side (r->maneuvers[r->n_maneuvers - 1].type) : NULL;
    set_text (p->arrived_side, side);
    gtk_widget_set_visible (p->arrived_side, side != NULL);
    navi_panels_say (p, navi_guidance_saying (NAVI_SAY_ARRIVED, navi_route_language ()), TRUE);
  }
  update_floating (p);
  /* again once the sheet has its new height */
  g_idle_add (update_floating_later, p);
}

void
navi_panels_set_cam (NaviPanels *p, NaviCam cam)
{
  update_floating (p);
}

static void
place_chip (NaviPanels *p)
{
  if (!p->place)
    return;
  g_autofree char *chip = NULL;
  if (p->place_drive_m > 0) {
    /* by road, as a drive */
    g_autofree char *t = navi_format_duration (p->place_drive_s);
    g_autofree char *d = km_text (p->place_drive_m);
    chip = g_strdup_printf ("🚗 %s · %s", t, d);
  } else {
    /* as the crow flies, until the road's is known: no car on it */
    chip = km_text (navi_distance_from_car (p->navi, p->place->lat, p->place->lng));
  }
  set_text (p->place_distance, chip);
}

void
navi_panels_set_place_drive (NaviPanels *p, double metres, double seconds)
{
  p->place_drive_m = metres;
  p->place_drive_s = seconds;
  place_chip (p);
}

void
navi_panels_show_place (NaviPanels *p, const NaviPlace *place)
{
  if (p->place != place) {
    /* the same place again (its address came): the drive there stays */
    if (!p->place || navi_geo_distance (p->place->lat, p->place->lng, place->lat, place->lng) > 1)
      p->place_drive_m = p->place_drive_s = 0;
    g_clear_pointer (&p->place, navi_place_free);
    p->place = navi_place_copy (place);
  }
  set_text (p->place_name, place->name);
  /* a point of interest: its marker, what it is, then where */
  gboolean poi = place->kind == NAVI_PLACE_POI;
  const char *what = poi ? navi_poi_label (place->category) : NULL;
  if (what && !g_strcmp0 (what, place->name))
    what = NULL; /* unnamed: its kind is the title already */
  g_autofree char *detail = what && place->detail ? g_strdup_printf ("%s · %s", what, place->detail)
                                                  : g_strdup (what ? what : place->detail);
  set_text (p->place_detail, detail);
  gtk_widget_set_visible (p->place_detail, detail != NULL);
  GtkWidget *old = gtk_widget_get_first_child (p->place_icon);
  if (old)
    gtk_box_remove (GTK_BOX (p->place_icon), old);
  GtkWidget *marker = poi ? poi_marker (p, place->category, 34) : NULL;
  if (marker)
    gtk_box_append (GTK_BOX (p->place_icon), marker);
  gtk_widget_set_visible (p->place_icon, marker != NULL);
  place_chip (p);
  set_text (p->place_go_label, place->kind == NAVI_PLACE_POINT ? _("Drive here") : poi ? _("Go") : _("View routes"));
  /* the Set as buttons say what they do again; the place that already is Home (Work) says so instead */
  gboolean starred = navi_settings_find_saved (p->navi->settings, place) >= 0;
  GtkWidget *star = g_object_get_data (G_OBJECT (p->place_star), "navi-icon");
  gtk_image_set_from_icon_name (GTK_IMAGE (star), starred ? "starred-symbolic" : "non-starred-symbolic");
  if (starred)
    gtk_widget_add_css_class (star, "navi-starred");
  else
    gtk_widget_remove_css_class (star, "navi-starred");
  set_text (g_object_get_data (G_OBJECT (p->place_star), "navi-label"), starred ? _("Saved") : _("Save"));
  int k = 0;
  for (GtkWidget *b = gtk_widget_get_next_sibling (p->place_star); b; b = gtk_widget_get_next_sibling (b), k++) {
    NaviPlace *saved = k == 0 ? p->navi->settings->home : p->navi->settings->work;
    gboolean is = saved && navi_geo_distance (saved->lat, saved->lng, place->lat, place->lng) < 30;
    set_text (g_object_get_data (G_OBJECT (b), "navi-label"), is ? (k == 0 ? _("Your Home") : _("Your Work"))
                                                                  : (k == 0 ? _("Set as Home") : _("Set as Work")));
    gtk_widget_set_sensitive (b, !is);
  }
}

void
navi_panels_set_routes (NaviPanels *p, GPtrArray *routes, guint selected, const char *error)
{
  GtkWidget *status = g_object_get_data (G_OBJECT (p->routes_box), "navi-status");
  GtkWidget *buttons = g_object_get_data (G_OBJECT (p->routes_box), "navi-buttons");
  clear_box (p->routes_list);
  gtk_revealer_set_child (GTK_REVEALER (p->routes_more), NULL);
  if (!routes) {
    /* a new preview: its routes will start Go now's countdown */
    g_object_set_data (G_OBJECT (p->routes_box), "navi-counting", NULL);
    p->shown_selected = 0;
    gtk_widget_set_visible (status, TRUE);
    gtk_widget_set_visible (buttons, FALSE);
    gtk_widget_set_visible (p->leave_note, FALSE);
    gtk_spinner_set_spinning (GTK_SPINNER (p->routes_spinner), !error);
    gtk_widget_set_visible (p->routes_spinner, !error);
    gtk_widget_set_visible (p->routes_retry, error != NULL);
    set_text (p->routes_status, error ? error : _("Finding the best routes…"));
    update_floating (p);
    return;
  }
  /* another route picked (a row, a bubble, a tap on its line): no more counting down */
  if (selected != p->shown_selected)
    stop_countdown (p);
  p->shown_selected = selected;
  gtk_widget_set_visible (status, FALSE);
  gtk_spinner_set_spinning (GTK_SPINNER (p->routes_spinner), FALSE);
  gtk_widget_set_visible (buttons, TRUE);
  GtkWidget *others = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  for (guint i = 0; i < routes->len; i++) {
    GtkWidget *row = route_row (p, routes->pdata[i], i, i == selected, i == 0 && routes->len > 1);
    gtk_box_append (GTK_BOX (i == selected ? p->routes_list : others), row);
  }
  gtk_revealer_set_child (GTK_REVEALER (p->routes_more), others);
  update_leave_note (p);
  /* where the routes start: the car, or without a position the map's centre (navi_where_am_i) */
  Navi *n = p->navi;
  gboolean located = n->have_fix && n->fix.kind != NAVI_FIX_LAST_KNOWN;
  set_text (p->routes_from, located ? _("Your location") : _("The map's centre"));
  /* a fresh preview starts Go now's countdown (not without a position: the drive would start from nowhere);
   * picking another route stops it */
  if (!g_object_get_data (G_OBJECT (p->routes_box), "navi-counting") && p->navi->mode == NAVI_MODE_ROUTES &&
      selected == 0 && !p->plan_ahead && located && !g_getenv ("NAVI_NO_COUNTDOWN")) {
    g_object_set_data (G_OBJECT (p->routes_box), "navi-counting", GINT_TO_POINTER (1));
    start_countdown (p);
  }
  update_floating (p);
  g_idle_add (update_floating_later, p);
}

/* the lanes coming up to the turn: from 1.5 km on a motorway's exits and forks (their lanes are signposted that
 * far ahead), 500 m in town; none where the map has none */
static void
update_lanes (NaviPanels *p, const NaviManeuver *m, double to_next)
{
  gboolean motorway = (m->type >= NAVI_TURN_RAMP_STRAIGHT && m->type <= NAVI_TURN_MERGE) ||
                      m->type == NAVI_TURN_MERGE_RIGHT || m->type == NAVI_TURN_MERGE_LEFT;
  gboolean show = m->n_lanes > 0 && to_next <= (motorway ? 1500 : 500);
  if (show && (p->n_lanes != m->n_lanes || memcmp (p->lanes, m->lanes, m->n_lanes * sizeof (NaviLane)))) {
    p->n_lanes = MIN (m->n_lanes, G_N_ELEMENTS (p->lanes));
    memcpy (p->lanes, m->lanes, p->n_lanes * sizeof (NaviLane));
    gtk_widget_queue_draw (p->banner_lanes);
  }
  if (gtk_widget_get_visible (p->banner_lanes) != show) {
    gtk_widget_set_visible (p->banner_lanes, show);
    update_floating (p);
  }
}

/* the banner: the next turn, its distance and street, and "and then" */
static void
update_banner (NaviPanels *p, const NaviProgress *pr, NaviRoute *r, guint next)
{
  Navi *n = p->navi;
  const NaviManeuver *m = &r->maneuvers[next];
  navi_icon_set_turn (p->banner_turn, m->type, m->roundabout_exit);
  if (!pr->on_route && (n->rerouting || n->reroute_failed)) {
    /* off the route: what the new route is doing, not the old route's next turn */
    set_text (p->banner_distance, "");
    set_text (p->banner_street, n->rerouting ? _("Finding a new route…") : _("No route from here yet. Trying again…"));
    gtk_widget_set_visible (p->then_box, FALSE);
    gtk_widget_set_visible (p->banner_side, FALSE);
    gtk_widget_set_visible (p->banner_lanes, FALSE);
    return;
  }
  g_autofree char *dist = navi_format_distance (pr->to_next);
  set_text (p->banner_distance, dist);
  const char *street = m->street;
  if (m->type == NAVI_TURN_DESTINATION || m->type == NAVI_TURN_DESTINATION_LEFT ||
      m->type == NAVI_TURN_DESTINATION_RIGHT)
    street = n->dest ? n->dest->name : _("Your destination");
  g_autofree char *instruction = NULL;
  if (!street && m->instruction) {
    instruction = g_strdup (m->instruction);
    gsize len = strlen (instruction);
    if (len && instruction[len - 1] == '.')
      instruction[len - 1] = 0;
  }
  set_text (p->banner_street, street ? street : instruction);
  /* the last metres: which side of the road the destination is on, from where the voice says it */
  const char *side = destination_side (m->type);
  double speed = n->have_fix ? n->fix.speed : -1;
  gboolean near = side && pr->to_next <= navi_guidance_pre_distance (speed);
  set_text (p->banner_side, near ? side : NULL);
  gtk_widget_set_visible (p->banner_side, near);
  update_lanes (p, m, pr->to_next);
  /* "and then", when the next two come close together */
  gboolean then = next + 1 < r->n_maneuvers && r->maneuvers[next + 1].s - m->s < 150 && pr->to_next < 600;
  if (then)
    navi_icon_set_turn (p->then_turn, r->maneuvers[next + 1].type, r->maneuvers[next + 1].roundabout_exit);
  gtk_widget_set_visible (p->then_box, then);
}

void
navi_panels_update_drive (NaviPanels *p, const NaviProgress *pr, NaviRoute *r)
{
  Navi *n = p->navi;
  if (!r || !r->n_maneuvers)
    return;
  guint next = MIN (pr->next, r->n_maneuvers - 1);
  update_banner (p, pr, r, next);
  /* the bar */
  g_autofree char *eta = navi_format_eta (pr->time_left);
  g_autofree char *mins = navi_format_duration (pr->time_left);
  g_autofree char *left = navi_format_distance (pr->remaining);
  g_autofree char *sub = g_strdup_printf ("%s • %s", mins, left);
  set_text (p->drive_eta, eta);
  set_text (p->drive_sub, sub);
  set_text (p->drive_free_sub, sub);
  if (n->dest) {
    g_autofree char *d = n->dest->detail ? g_strdup_printf ("%s, %s", n->dest->name, n->dest->detail)
                                         : g_strdup (n->dest->name);
    set_text (p->drive_dest, d);
  }
  if (r->via) {
    g_autofree char *via = g_strdup_printf (_("Via %s"), r->via);
    set_text (p->drive_via, via);
  }
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->drive_progress),
                                 r->length > 0 ? CLAMP (1 - pr->remaining / r->length, 0, 1) : 0);
  /* the street the car is on */
  if (pr->on_route) {
    const char *on = navi_route_street_at (r, pr->s);
    navi_panels_set_street (p, on);
  }
}

/* the speedometer red: faster than the limit and the margin Settings give */
static void
update_speeding (NaviPanels *p)
{
  gboolean over = FALSE;
  if (p->speed >= 0 && p->limit_kmh > 0) {
    double shown = p->speed * (p->mph ? 2.2369363 : 3.6);
    double limit = navi_limit_display (p->limit_kmh, p->mph), margin = 0;
    switch (p->navi->settings->speeding) {
    case NAVI_SPEEDING_AT_LIMIT: margin = 0; break;
    case NAVI_SPEEDING_PLUS_5: margin = p->mph ? 3 : 5; break;
    case NAVI_SPEEDING_PLUS_10: margin = p->mph ? 5 : 10; break;
    case NAVI_SPEEDING_PERCENT_10: margin = limit * 0.1; break;
    }
    over = lround (shown) > limit + margin;
  }
  if (over)
    gtk_widget_add_css_class (p->speedo, "navi-speeding");
  else
    gtk_widget_remove_css_class (p->speedo, "navi-speeding");
}

void
navi_panels_set_speed (NaviPanels *p, double speed)
{
  p->speed = speed;
  /* without a route the speedometer shows while the car moves, and stays two minutes after it stops */
  gint64 now = g_get_monotonic_time ();
  gboolean was = p->moving;
  if (speed > 5.5) {
    p->moving = TRUE;
    p->slow_since = 0;
  } else if (speed < 0 || (p->moving && speed < 1.5 && p->slow_since && now - p->slow_since > 120 * G_USEC_PER_SEC)) {
    p->moving = FALSE;
  } else if (speed < 1.5 && !p->slow_since) {
    p->slow_since = now;
  }
  if (was != p->moving)
    update_floating (p);
  update_speeding (p);
  if (speed < 0) {
    set_text (p->speedo_value, "--");
    return;
  }
  char text[16];
  g_snprintf (text, sizeof text, "%ld", lround (speed * (p->mph ? 2.2369363 : 3.6)));
  set_text (p->speedo_value, text);
}

static void
draw_limit_sign (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  NaviPanels *p = data;
  if (p->limit_kmh == NAVI_LIMIT_UNKNOWN)
    return;
  /* its shadow, then the sign */
  cairo_save (cr);
  cairo_arc (cr, w / 2.0, h / 2.0 + 1, MIN (w, h) / 2.0 - 0.5, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 0, 0, 0, p->mph ? 0 : 0.25);
  cairo_fill (cr);
  cairo_restore (cr);
  navi_draw_limit_sign (cr, MIN (w, h), p->limit_kmh > 0 ? navi_limit_display (p->limit_kmh, p->mph) : -1,
                        p->us_signs);
}

/* the speeds the road's signs give: miles an hour in the UK and the US, else kilometres, whatever the language;
 * unless the units setting says one. Distances follow. */
void
navi_panels_set_units (NaviPanels *p, const char *country)
{
  if (country && *country)
    g_strlcpy (p->units_country, country, sizeof p->units_country);
  country = p->units_country;
  NaviUnits setting = p->navi->settings->units;
  gboolean mph = setting == NAVI_UNITS_MI     ? TRUE
                 : setting == NAVI_UNITS_KM   ? FALSE
                 : g_getenv ("NAVI_SPEED_UNITS") ? navi_speed_in_mph ()
                                              : navi_country_uses_mph (country);
  /* the US's white board where the road is the US's (an mph setting in Germany keeps the round sign) */
  gboolean us = mph && navi_country_uses_us_signs (country);
  navi_format_set_miles (mph);
  if (mph == p->mph && us == p->us_signs)
    return;
  p->mph = units_mph = mph;
  p->us_signs = us;
  set_text (p->speedo_unit, mph ? "mph" : "km/h");
  navi_panels_set_speed (p, p->speed);
  int kmh = p->limit_kmh;
  p->limit_kmh = -99; /* none of NAVI_LIMIT_*: the sign is drawn again, in the new units */
  navi_panels_set_limit (p, kmh);
}

void
navi_panels_set_limit (NaviPanels *p, int kmh)
{
  /* no limit in the US's way of showing them is no sign at all: there is no such sign there */
  if (kmh == NAVI_LIMIT_NONE && p->us_signs)
    kmh = NAVI_LIMIT_UNKNOWN;
  if (kmh == p->limit_kmh)
    return;
  p->limit_kmh = kmh;
  gtk_widget_set_visible (p->limit_sign, kmh != NAVI_LIMIT_UNKNOWN);
  if (kmh > 0) {
    g_autofree char *label = g_strdup_printf (p->mph ? _("Speed limit %d mph") : _("Speed limit %d km/h"),
                                              navi_limit_display (kmh, p->mph));
    gtk_accessible_update_property (GTK_ACCESSIBLE (p->limit_sign), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  } else if (kmh == NAVI_LIMIT_NONE) {
    gtk_accessible_update_property (GTK_ACCESSIBLE (p->limit_sign), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _("No speed limit"), -1);
  }
  gtk_widget_queue_draw (p->limit_sign);
  update_speeding (p);
}

void
navi_panels_show_alert (NaviPanels *p, const char *title, const char *detail)
{
  if (!title) {
    gtk_widget_set_visible (p->alert_card, FALSE);
    return;
  }
  set_text (p->alert_title, title);
  set_text (p->alert_detail, detail ? detail : "");
  gtk_widget_set_visible (p->alert_detail, detail && *detail);
  gtk_widget_set_visible (p->alert_card, TRUE);
}

#define RESPONSE_TURN_OFF 3 /* the notice's Turn off: not CANCEL, which Back and a tap beside the card give too */

static void
notice_answered (VibeosDialog *d, int response, NaviPanels *p)
{
  p->notice_open = FALSE;
  if (response == RESPONSE_TURN_OFF) {
    p->navi->settings->cameras = FALSE;
    navi_panels_show_alert (p, NULL, NULL);
    navi_window_update_overlay (p->navi);
  }
  navi_settings_save (p->navi->settings);
}

/* once, the first time a camera is shown or alerted: where such alerts are not allowed */
void
navi_panels_camera_notice (NaviPanels *p)
{
  NaviSettings *st = p->navi->settings;
  if (st->cameras_notice_seen || p->notice_open)
    return;
  st->cameras_notice_seen = TRUE;
  p->notice_open = TRUE;
  VibeosDialog *d = vibeos_dialog_new (_("Speed camera alerts"));
  vibeos_dialog_set_message (d, _("Not allowed everywhere. In Germany the driver may not use them while driving "
                                  "(§23 (1c) StVO); a passenger may. In Switzerland Navi never shows them, and in "
                                  "France only danger zones.\n\nYou can turn them off in Settings at any time."));
  vibeos_dialog_add_button (d, _("Turn off"), RESPONSE_TURN_OFF);
  vibeos_dialog_add_button (d, _("OK"), VIBEOS_RESPONSE_OK);
  g_signal_connect (d, "response", G_CALLBACK (notice_answered), p);
  vibeos_dialog_present (d, p->navi->win);
  navi_settings_save (st);
}

void
navi_panels_set_street (NaviPanels *p, const char *street)
{
  set_text (p->street_label, street);
  gtk_widget_set_visible (p->street_pill, street && *street && p->navi->mode == NAVI_MODE_DRIVE &&
                                            p->navi->cam == NAVI_CAM_FOLLOW);
}

void
navi_panels_set_note (NaviPanels *p, const char *note)
{
  set_text (gtk_widget_get_first_child (p->note), note);
  set_text (p->banner_note, note);
  update_floating (p);
}

void
navi_panels_set_rerouting (NaviPanels *p, gboolean rerouting)
{
  if (rerouting) {
    set_text (p->banner_distance, "");
    set_text (p->banner_street, _("Finding a new route…"));
    gtk_widget_set_visible (p->then_box, FALSE);
    gtk_widget_set_visible (p->banner_side, FALSE);
    gtk_widget_set_visible (p->banner_lanes, FALSE);
  }
}

void
navi_panels_set_compass (NaviPanels *p, double bearing)
{
  if (fabs (remainder (bearing - p->bearing, 360)) < 0.2)
    return;
  gboolean was = fabs (remainder (p->bearing, 360)) > 1;
  p->bearing = bearing;
  gtk_widget_queue_draw (p->compass_area);
  if (was != (fabs (remainder (bearing, 360)) > 1))
    update_floating (p);
}

void
navi_panels_say (NaviPanels *p, const char *text, gboolean alert)
{
  NaviVoiceMode mode = p->navi->settings->voice;
  NAVI_PRIVATE_DEBUG ("navi: say \"%s\"\n", text);
  /* "Alerts only", as Waze's: the road's alerts (navi_panels_say_alert), no directions */
  if (mode != NAVI_VOICE_ON)
    return;
  navi_voice_say (p->voice, text, navi_route_language ());
}

void
navi_panels_say_alert (NaviPanels *p, const char *text)
{
  if (p->navi->settings->voice == NAVI_VOICE_OFF)
    return;
  navi_voice_say (p->voice, text, navi_route_language ());
}

void
navi_panels_prepare_speech (NaviPanels *p, char **texts)
{
  if (p->navi->settings->voice != NAVI_VOICE_ON)
    return;
  for (guint i = 0; texts && texts[i]; i++)
    navi_voice_prepare (p->voice, texts[i], navi_route_language ());
}

void
navi_panels_rest_voice (NaviPanels *p)
{
  navi_voice_rest (p->voice);
}

void
navi_panels_warm_voice (NaviPanels *p)
{
  if (p->navi->settings->voice != NAVI_VOICE_OFF)
    navi_voice_warm (p->voice, navi_route_language ());
}
