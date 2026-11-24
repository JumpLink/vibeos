/* VibeOS Settings' building blocks (see ns-ui.h). */
#include "ns-ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SPONGY
#include <spongy-gtk.h>
#endif

static VibeosListDetail *ld;
static gint64 tapped;          /* when a row was tapped: NS_TIMING logs how long its page took to build */
static gboolean opening_entry; /* the next page pushed is a main-menu entry's */
static GtkWidget *entry_row;   /* the entry's row */
static GtkWidget *first_level; /* the entry's page now open (weak) */

/* ---- navigation ---- */

static void
set_bar_back (NsPage *page, gboolean back)
{
  GtkWidget *bar = g_object_get_data (G_OBJECT (page), "ns-bar");
  if (bar)
    vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), back);
}

/* an entry's page beside the menu has no back arrow (Back there leaves the app); over the menu it has one */
static void
split_changed (GObject *o, GParamSpec *ps, gpointer data)
{
  if (first_level)
    set_bar_back (first_level, !vibeos_list_detail_is_split (ld));
}

void
ns_ui_init (VibeosListDetail *l)
{
  ld = l;
  g_signal_connect (ld, "notify::split", G_CALLBACK (split_changed), NULL);
}

VibeosListDetail *
ns_ui_get_list_detail (void)
{
  return ld;
}

void
ns_push (NsPage *page)
{
  if (tapped && g_getenv ("NS_TIMING"))
    g_message ("%s: built in %.1f ms", ns_page_get_title (page), (g_get_monotonic_time () - tapped) / 1000.0);
  tapped = 0;
  if (opening_entry) {
    opening_entry = FALSE;
    g_set_weak_pointer (&first_level, page);
    set_bar_back (page, !vibeos_list_detail_is_split (ld));
    if (entry_row)
      vibeos_list_detail_show_from (ld, page, entry_row);
    else
      vibeos_list_detail_show (ld, page);
    return;
  }
  vibeos_list_detail_push (ld, page);
}

void
ns_open_entry (NsActivate page, GtkWidget *row, gpointer data)
{
  opening_entry = TRUE;
  entry_row = row;
  tapped = g_get_monotonic_time ();
  page (row, data);
  opening_entry = FALSE;
  entry_row = NULL;
}

NsPage *
ns_top_page (void)
{
  VibeosPageStack *detail = vibeos_list_detail_get_detail_stack (ld);
  if (vibeos_list_detail_is_split (ld) && vibeos_page_stack_get_depth (detail) > 0)
    return vibeos_page_stack_get_top (detail);
  return vibeos_page_stack_get_top (vibeos_list_detail_get_list_stack (ld));
}

static void
back_on (NsPage *page)
{
  NsPageBack cb = page ? g_object_get_data (G_OBJECT (page), "ns-on-back") : NULL;
  if (cb)
    cb (page);
}

gboolean
ns_back (void)
{
  if (ns_busy ())
    return TRUE;
  if (!vibeos_list_detail_back (ld))
    return FALSE;
  back_on (ns_top_page ());
  return TRUE;
}

NsPage *
ns_page_of (GtkWidget *widget)
{
  for (GtkWidget *w = widget; w; w = gtk_widget_get_parent (w))
    if (g_object_get_data (G_OBJECT (w), "ns-is-page"))
      return w;
  return NULL;
}

void
ns_pop (GtkWidget *widget)
{
  NsPage *page = ns_page_of (widget);
  if (page && page == ns_top_page ())
    ns_back ();
}

void
ns_pop_to (NsPage *page)
{
  NsPage *top;
  while ((top = ns_top_page ()) && top != page) {
    if (!vibeos_list_detail_back (ld) || ns_top_page () == top)
      break;
  }
  back_on (ns_top_page ());
}

void
ns_pop_all (void)
{
  vibeos_list_detail_close_detail (ld);
}

const char *
ns_page_get_title (NsPage *page)
{
  return g_object_get_data (G_OBJECT (page), "ns-page-title");
}

void
ns_page_set_busy (NsPage *page, gboolean busy)
{
  g_object_set_data (G_OBJECT (page), "ns-busy", GINT_TO_POINTER (busy));
  GtkWidget *bar = g_object_get_data (G_OBJECT (page), "ns-bar");
  if (bar)
    gtk_widget_set_sensitive (bar, !busy);
}

gboolean
ns_busy (void)
{
  NsPage *top = ld ? ns_top_page () : NULL;
  return top && g_object_get_data (G_OBJECT (top), "ns-busy");
}

void
ns_page_on_back (NsPage *page, NsPageBack back)
{
  g_object_set_data (G_OBJECT (page), "ns-on-back", back);
}

/* ---- pages ---- */

GtkWidget *
ns_scroller_new (GtkWidget *child)
{
#ifdef HAVE_SPONGY
  GtkWidget *sc = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (sc), child);
#else
  GtkWidget *sc = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sc), child);
#endif
  return sc;
}

/* the bar's back arrow: the page before, or out of Settings */
static void
bar_back (VibeosTopBar *bar, gpointer data)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (bar));
  if (!ns_back () && GTK_IS_WINDOW (root))
    gtk_window_close (GTK_WINDOW (root));
}

static NsPage *
page_with (const char *title, GtkWidget *child, gboolean scroll)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (page, "ns-page");
  GtkWidget *bar = vibeos_top_bar_new (title);
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (bar_back), NULL);
  gtk_box_append (GTK_BOX (page), bar);
  GtkWidget *sc = scroll ? ns_scroller_new (child) : child;
  gtk_widget_set_vexpand (sc, TRUE);
  gtk_box_append (GTK_BOX (page), sc);
  g_object_set_data (G_OBJECT (page), "ns-is-page", GINT_TO_POINTER (1));
  g_object_set_data (G_OBJECT (page), "ns-bar", bar);
  g_object_set_data_full (G_OBJECT (page), "ns-page-title", g_strdup (title), g_free);
  return page;
}

NsPage *
ns_page_new (const char *title, GtkWidget **content)
{
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (col, "ns-column");
  *content = col;
  return page_with (title, col, TRUE);
}

NsPage *
ns_page_new_list (const char *title, GtkWidget *list)
{
  gtk_widget_add_css_class (list, "ns-list");
  return page_with (title, list, TRUE);
}

NsPage *
ns_page_new_fixed (const char *title, GtkWidget *child)
{
  return page_with (title, child, FALSE);
}

/* ---- groups and rows ---- */

GtkWidget *
ns_group (GtkWidget *content, const char *title)
{
  GtkWidget *list = vibeos_card_list_new ();
  /* a card under another without a title has the gap between groups over it; a title brings its own */
  gboolean first = gtk_widget_get_first_child (content) == NULL;
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), first || title ? 0 : 16, 0);
  if (title) {
    vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), title);
    g_object_set_data_full (G_OBJECT (list), "ns-group-title", g_strdup (title), g_free);
  }
  gtk_widget_add_css_class (list, "ns-group");
  gtk_box_append (GTK_BOX (content), list);
  return list;
}

static void
row_clicked (VibeosListRow *row, gpointer data)
{
  NsActivate cb = g_object_get_data (G_OBJECT (row), "ns-cb");
  tapped = g_get_monotonic_time ();
  if (cb)
    cb (GTK_WIDGET (row), g_object_get_data (G_OBJECT (row), "ns-data"));
}

GtkWidget *
ns_row (GtkWidget *group, const char *icon, const char *title, const char *subtitle)
{
  GtkWidget *row = vibeos_list_row_new (title, subtitle && *subtitle ? subtitle : NULL);
  if (icon)
    vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), icon);
  vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), FALSE);
  g_signal_connect (row, "clicked", G_CALLBACK (row_clicked), NULL);
  vibeos_card_list_append (VIBEOS_CARD_LIST (group), row);
  return row;
}

void
ns_row_on_activate (GtkWidget *row, NsActivate cb, gpointer data)
{
  vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), TRUE);
  g_object_set_data (G_OBJECT (row), "ns-cb", cb);
  g_object_set_data (G_OBJECT (row), "ns-data", data);
}

void
ns_row_suffix (GtkWidget *row, GtkWidget *suffix)
{
  /* a button takes its taps; a mark, a label or a spinner only shows, and a tap on it is the row's */
  if (!GTK_IS_BUTTON (suffix) && !VIBEOS_IS_PRESSABLE (suffix))
    gtk_widget_set_can_target (suffix, FALSE);
  vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (row), suffix);
}

/* ---- the loading indicator: Material 3's indeterminate circular progress indicator, at a row's size. An arc in the
 * primary colour turns steadily; its head runs ahead (10° to 270°) in the first half of each cycle and its tail
 * catches up in the second, so the arc grows and shrinks as it goes round. It draws only while mapped. */

#define SPINNER_SIZE     24
#define SPINNER_STROKE   3
#define SPINNER_TURN_MS  1568.0    /* the whole turns once in this */
#define SPINNER_CYCLE_MS 1333.0    /* the head runs ahead and the tail catches up once in this */
#define SPINNER_MIN_DEG  10.0
#define SPINNER_MAX_DEG  270.0

struct _NsSpinner {
  GtkWidget parent;
  guint     tick;
  gint64    start;
};

G_DECLARE_FINAL_TYPE (NsSpinner, ns_spinner, NS, SPINNER, GtkWidget)
G_DEFINE_FINAL_TYPE (NsSpinner, ns_spinner, GTK_TYPE_WIDGET)

static gboolean
spinner_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer unused)
{
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
ns_spinner_map (GtkWidget *widget)
{
  NsSpinner *self = NS_SPINNER (widget);
  GTK_WIDGET_CLASS (ns_spinner_parent_class)->map (widget);
  self->tick = gtk_widget_add_tick_callback (widget, spinner_tick, NULL, NULL);
}

static void
ns_spinner_unmap (GtkWidget *widget)
{
  NsSpinner *self = NS_SPINNER (widget);
  if (self->tick)
    gtk_widget_remove_tick_callback (widget, self->tick);
  self->tick = 0;
  GTK_WIDGET_CLASS (ns_spinner_parent_class)->unmap (widget);
}

static void
ns_spinner_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                    int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = SPINNER_SIZE;
}

static double
spinner_ease (double x)
{
  return x * x * (3 - 2 * x);
}

static void
ns_spinner_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NsSpinner *self = NS_SPINNER (widget);
  GdkFrameClock *clock = gtk_widget_get_frame_clock (widget);
  gint64 now = clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  if (!self->start)
    self->start = now;
  double ms = (now - self->start) / 1000.0;
  double cycles = floor (ms / SPINNER_CYCLE_MS), p = fmod (ms, SPINNER_CYCLE_MS) / SPINNER_CYCLE_MS;
  double run = SPINNER_MAX_DEG - SPINNER_MIN_DEG;
  /* each cycle leaves the tail `run` further on */
  double base = fmod (cycles * run + ms / SPINNER_TURN_MS * 360.0, 360.0);
  double tail = base, head = base + SPINNER_MAX_DEG;
  if (p < 0.5)
    head = base + SPINNER_MIN_DEG + run * spinner_ease (p * 2);
  else
    tail = base + run * spinner_ease ((p - 0.5) * 2);

  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  double r = (MIN (w, h) - SPINNER_STROKE) / 2.0, cx = w / 2.0, cy = h / 2.0;
  double a0 = tail * G_PI / 180.0, a1 = head * G_PI / 180.0;   /* 0 at the top, clockwise */
  g_autoptr (GskPathBuilder) b = gsk_path_builder_new ();
  gsk_path_builder_move_to (b, cx + r * sin (a0), cy - r * cos (a0));
  gsk_path_builder_svg_arc_to (b, r, r, 0, head - tail > 180, TRUE, cx + r * sin (a1), cy - r * cos (a1));
  g_autoptr (GskPath) path = gsk_path_builder_to_path (b);
  GskStroke *stroke = gsk_stroke_new (SPINNER_STROKE);
  gsk_stroke_set_line_cap (stroke, GSK_LINE_CAP_ROUND);
  GdkRGBA ink = vibeos_color (VIBEOS_PRIMARY);
  gtk_snapshot_append_stroke (snapshot, path, stroke, &ink);
  gsk_stroke_free (stroke);
}

static void
ns_spinner_class_init (NsSpinnerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->map = ns_spinner_map;
  widget_class->unmap = ns_spinner_unmap;
  widget_class->measure = ns_spinner_measure;
  widget_class->snapshot = ns_spinner_snapshot;
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_PROGRESS_BAR);
}

static void
ns_spinner_init (NsSpinner *self)
{
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Loading"), -1);
}

GtkWidget *
ns_spinner_new (void)
{
  return g_object_new (ns_spinner_get_type (), NULL);
}

void
ns_row_set_title (GtkWidget *row, const char *title)
{
  vibeos_list_row_set_title (VIBEOS_LIST_ROW (row), title);
}

void
ns_row_set_subtitle (GtkWidget *row, const char *subtitle)
{
  vibeos_list_row_set_summary (VIBEOS_LIST_ROW (row), subtitle && *subtitle ? subtitle : NULL);
}

GtkWidget *
ns_switch_new (void)
{
  return vibeos_switch_new ();
}

gboolean
ns_switch_get_active (GtkWidget *sw)
{
  return vibeos_switch_get_active (VIBEOS_SWITCH (sw));
}

void
ns_switch_set_active (GtkWidget *sw, gboolean on)
{
  vibeos_switch_set_active (VIBEOS_SWITCH (sw), on);
}

GtkWidget *
ns_row_add_switch (GtkWidget *row, gboolean split)
{
  GtkWidget *sw = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), split);
  /* a whole-row switch: a tap anywhere toggles it */
  if (!split)
    vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), TRUE);
  g_object_set_data (G_OBJECT (row), "ns-switch", sw);
  return sw;
}

GtkWidget *
ns_row_switch (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key)
{
  GtkWidget *row = ns_row (group, NULL, title, subtitle);
  GtkWidget *sw = ns_row_add_switch (row, FALSE);
  if (s)
    g_settings_bind (s, key, sw, "active", G_SETTINGS_BIND_DEFAULT);
  return row;
}

/* a card of the app's own: the card list rounds its corners where its group starts and ends */
static GtkWidget *
card_item (void)
{
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (v, "ns-card-item");
  return v;
}

static GtkWidget *
row_title (const char *title)
{
  GtkWidget *t = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (t), 0);
  gtk_label_set_wrap (GTK_LABEL (t), TRUE);
  gtk_widget_add_css_class (t, "vibeos-list-row-title");
  return t;
}

GtkWidget *
ns_row_widget (GtkWidget *group, const char *title, GtkWidget *child)
{
  GtkWidget *v = card_item ();
  if (title)
    gtk_box_append (GTK_BOX (v), row_title (title));
  gtk_box_append (GTK_BOX (v), child);
  vibeos_card_list_append (VIBEOS_CARD_LIST (group), v);
  return v;
}

/* ---- a stepper for an integer key ---- */

typedef struct {
  GSettings *s;
  char      *key;
  int        lo, hi;
  GtkWidget *value, *minus, *plus;
} Spin;

static void
spin_show (Spin *sp)
{
  int v = g_settings_get_int (sp->s, sp->key);
  g_autofree char *t = g_strdup_printf ("%d", v);
  gtk_label_set_text (GTK_LABEL (sp->value), t);
  gtk_widget_set_sensitive (sp->minus, v > sp->lo);
  gtk_widget_set_sensitive (sp->plus, v < sp->hi);
}

static void
spin_step (GtkWidget *b, Spin *sp)
{
  int d = b == sp->plus ? 1 : -1;
  g_settings_set_int (sp->s, sp->key, CLAMP (g_settings_get_int (sp->s, sp->key) + d, sp->lo, sp->hi));
}

static void
spin_changed (GSettings *s, const char *key, Spin *sp)
{
  spin_show (sp);
}

static void
spin_free (Spin *sp)
{
  g_signal_handlers_disconnect_by_data (sp->s, sp);
  g_free (sp->key);
  g_free (sp);
}

GtkWidget *
ns_row_spin (GtkWidget *group, const char *title, GSettings *s, const char *key, int lo, int hi)
{
  GtkWidget *row = ns_row (group, NULL, title, NULL);
  Spin *sp = g_new0 (Spin, 1);
  sp->s = s;
  sp->key = g_strdup (key);
  sp->lo = lo;
  sp->hi = hi;
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  sp->minus = vibeos_icon_button_new ("list-remove-symbolic");
  sp->plus = vibeos_icon_button_new ("list-add-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (sp->minus), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Less"), -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (sp->plus), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More"), -1);
  sp->value = gtk_label_new ("");
  gtk_label_set_width_chars (GTK_LABEL (sp->value), 2);
  gtk_widget_add_css_class (sp->value, "vibeos-list-row-title");
  for (GtkWidget **b = (GtkWidget *[]) { sp->minus, sp->plus, NULL }; *b; b++) {
    gtk_widget_add_css_class (*b, "ns-step");
    g_signal_connect (*b, "clicked", G_CALLBACK (spin_step), sp);
  }
  gtk_box_append (GTK_BOX (h), sp->minus);
  gtk_box_append (GTK_BOX (h), sp->value);
  gtk_box_append (GTK_BOX (h), sp->plus);
  vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (row), h);
  g_autofree char *sig = g_strconcat ("changed::", key, NULL);
  g_signal_connect (s, sig, G_CALLBACK (spin_changed), sp);
  g_object_set_data_full (G_OBJECT (row), "ns-own", sp, (GDestroyNotify) spin_free);
  spin_show (sp);
  return row;
}

/* ---- check marks of a picker ---- */

GtkWidget *
ns_mark_new (gboolean on)
{
  GtkWidget *mark = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
  gtk_widget_add_css_class (mark, "ns-mark");
  gtk_widget_set_opacity (mark, on);
  return mark;
}

static GtkWidget *
mark_row (GtkWidget *g, const char *label, gboolean on)
{
  GtkWidget *r = ns_row (g, NULL, label, NULL);
  GtkWidget *mark = ns_mark_new (on);
  ns_row_suffix (r, mark);
  g_object_set_data (G_OBJECT (r), "ns-mark", mark);
  return r;
}

static void
mark_only (GtkWidget *row)
{
  GtkWidget *box = gtk_widget_get_parent (row);
  for (GtkWidget *r = gtk_widget_get_first_child (box); r; r = gtk_widget_get_next_sibling (r)) {
    GtkWidget *mark = g_object_get_data (G_OBJECT (r), "ns-mark");
    if (mark)
      gtk_widget_set_opacity (mark, r == row ? 1 : 0);
  }
}

/* ---- a choice from a list ---- */

typedef struct {
  GSettings          *s;
  char               *key;
  const char *const  *values, *const *labels;
  GtkWidget          *row;
} Choice;

static const char *
choice_label (Choice *c)
{
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  for (guint i = 0; c->values[i]; i++)
    if (!g_strcmp0 (c->values[i], v))
      return *c->labels[i] ? _(c->labels[i]) : "";
  return "";
}

static void
choice_picked (GtkWidget *row, gpointer data)
{
  Choice *c = g_object_get_data (G_OBJECT (row), "ns-choice");
  g_settings_set_string (c->s, c->key, data);
  ns_row_set_subtitle (c->row, choice_label (c));
  mark_only (row);
}

static void
choice_open (GtkWidget *row, gpointer data)
{
  Choice *c = data;
  GtkWidget *content;
  const char *title = g_object_get_data (G_OBJECT (row), "ns-title");
  NsPage *page = ns_page_new (title, &content);
  /* the row (and with it the choice) lives as long as its picker */
  g_object_set_data_full (G_OBJECT (page), "ns-pick-row", g_object_ref (row), g_object_unref);
  GtkWidget *g = ns_group (content, NULL);
  g_autofree char *v = g_settings_get_string (c->s, c->key);
  for (guint i = 0; c->values[i]; i++) {
    GtkWidget *r = mark_row (g, _(c->labels[i]), !g_strcmp0 (v, c->values[i]));
    g_object_set_data (G_OBJECT (r), "ns-choice", c);
    ns_row_on_activate (r, choice_picked, (gpointer) c->values[i]);
  }
  ns_push (page);
}

static void
choice_free (Choice *c)
{
  g_free (c->key);
  g_free (c);
}

/* the key changed elsewhere (the shell's ringer sets feedbackd's profile, its keyboard menu the keyboard): the row
 * says the new choice */
static void
choice_changed (GSettings *s, const char *key, GtkWidget *row)
{
  Choice *c = g_object_get_data (G_OBJECT (row), "ns-own");
  if (c)
    ns_row_set_subtitle (row, choice_label (c));
}

GtkWidget *
ns_row_choice (GtkWidget *group, const char *title, GSettings *s, const char *key,
               const char *const *values, const char *const *labels)
{
  Choice *c = g_new0 (Choice, 1);
  c->s = s;
  c->key = g_strdup (key);
  c->values = values;
  c->labels = labels;
  c->row = ns_row (group, NULL, title, NULL);
  g_object_set_data_full (G_OBJECT (c->row), "ns-title", g_strdup (title), g_free);
  ns_row_set_subtitle (c->row, choice_label (c));
  ns_row_on_activate (c->row, choice_open, c);
  g_object_set_data_full (G_OBJECT (c->row), "ns-own", c, (GDestroyNotify) choice_free);
  g_autofree char *signal = g_strconcat ("changed::", key, NULL);
  g_signal_connect_object (s, signal, G_CALLBACK (choice_changed), c->row, 0);
  return c->row;
}

GtkWidget *
ns_row_toggle (GtkWidget *group, const char *icon, const char *title, const char *subtitle)
{
  GtkWidget *r = ns_row (group, icon, title, subtitle);
  ns_row_add_switch (r, FALSE);
  return r;
}

void
ns_switch_quiet (GtkWidget *sw, gboolean on)
{
  g_object_set_data (G_OBJECT (sw), "ns-syncing", GINT_TO_POINTER (1));
  ns_switch_set_active (sw, on);
  g_object_set_data (G_OBJECT (sw), "ns-syncing", NULL);
}

gboolean
ns_switch_syncing (GtkWidget *sw)
{
  return g_object_get_data (G_OBJECT (sw), "ns-syncing") != NULL;
}

GtkWidget *
ns_row_get_switch (GtkWidget *row)
{
  return g_object_get_data (G_OBJECT (row), "ns-switch");
}

GtkWidget *
ns_row_app (GtkWidget *group, GAppInfo *app, const char *subtitle)
{
  GtkWidget *row = ns_row (group, NULL, g_app_info_get_display_name (app), subtitle);
  GIcon *gi = g_app_info_get_icon (app);
  GtkWidget *img = gi ? gtk_image_new_from_gicon (gi) : gtk_image_new_from_icon_name ("application-x-executable");
  gtk_image_set_pixel_size (GTK_IMAGE (img), 40);
  vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (row), img);
  g_object_set_data (G_OBJECT (row), "ns-icon", img);
  return row;
}

GtkWidget *
ns_row_info (GtkWidget *group, const char *title, const char *value)
{
  return ns_row (group, NULL, title, value && *value ? value : "–");
}

GtkWidget *
ns_row_button (GtkWidget *group, const char *title, gboolean destructive, NsActivate cb, gpointer data)
{
  GtkWidget *r = ns_row (group, NULL, title, NULL);
  gtk_widget_add_css_class (r, destructive ? "ns-danger" : "ns-accent");
  ns_row_on_activate (r, cb, data);
  return r;
}

GtkWidget *
ns_row_scale (GtkWidget *group, const char *title, const char *icon, double lo, double hi, double step,
              GtkWidget **scale)
{
  /* the icon at the start, the title and the slider under it after it */
  GtkWidget *item = card_item ();
  gtk_orientable_set_orientation (GTK_ORIENTABLE (item), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (item, "ns-scale-row");
  if (icon) {
    GtkWidget *img = gtk_image_new_from_icon_name (icon);
    gtk_image_set_pixel_size (GTK_IMAGE (img), 24);
    gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (img, "vibeos-list-row-icon");
    gtk_widget_add_css_class (img, "ns-scale-icon");
    gtk_box_append (GTK_BOX (item), img);
  } else {
    gtk_widget_add_css_class (item, "ns-no-icon");
  }
  GtkWidget *v = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand (v, TRUE);
  if (title)
    gtk_box_append (GTK_BOX (v), row_title (title));
  GtkWidget *sc = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
  gtk_scale_set_draw_value (GTK_SCALE (sc), FALSE);
  gtk_widget_set_hexpand (sc, TRUE);
  if (title)
    gtk_accessible_update_property (GTK_ACCESSIBLE (sc), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (v), sc);
  gtk_box_append (GTK_BOX (item), v);
  vibeos_card_list_append (VIBEOS_CARD_LIST (group), item);
  *scale = sc;
  return item;
}

GtkWidget *
ns_row_volume (GtkWidget *group, const char *title, const char *icon, double lo, double hi, double step,
               GtkWidget **scale)
{
  GtkWidget *item = card_item ();
  gtk_widget_add_css_class (item, "ns-volume-row");
  gtk_box_append (GTK_BOX (item), row_title (title));
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *sc = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
  gtk_scale_set_draw_value (GTK_SCALE (sc), FALSE);
  gtk_widget_set_hexpand (sc, TRUE);
  gtk_widget_set_valign (sc, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (sc), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (line), sc);
  GtkWidget *img = gtk_image_new_from_icon_name (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (img), 24);
  gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (img, "ns-volume-icon");
  gtk_box_append (GTK_BOX (line), img);
  gtk_box_append (GTK_BOX (item), line);
  vibeos_card_list_append (VIBEOS_CARD_LIST (group), item);
  *scale = sc;
  return item;
}

/* ---- a choice of steps on a slider ---- */

static void
steps_step (GtkWidget *b, GtkWidget *sc)
{
  double v = gtk_range_get_value (GTK_RANGE (sc));
  gtk_range_set_value (GTK_RANGE (sc), round (v) + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "ns-by")));
}

static void
steps_ends (GtkRange *sc, GtkWidget *row)
{
  GtkAdjustment *a = gtk_range_get_adjustment (sc);
  double v = round (gtk_adjustment_get_value (a));
  GtkWidget *minus = g_object_get_data (G_OBJECT (row), "ns-minus"), *plus = g_object_get_data (G_OBJECT (row), "ns-plus");
  gtk_widget_set_sensitive (minus, v > gtk_adjustment_get_lower (a));
  gtk_widget_set_sensitive (plus, v < gtk_adjustment_get_upper (a));
}

GtkWidget *
ns_row_steps (GtkWidget *group, const char *title, guint n, guint selected, GtkWidget **scale)
{
  GtkWidget *item = card_item ();
  gtk_widget_add_css_class (item, "ns-steps-row");
  gtk_box_append (GTK_BOX (item), row_title (title));
  GtkWidget *value = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (value), 0);
  gtk_label_set_wrap (GTK_LABEL (value), TRUE);
  gtk_widget_add_css_class (value, "vibeos-list-row-summary");
  gtk_widget_set_visible (value, FALSE);
  gtk_box_append (GTK_BOX (item), value);
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *minus = vibeos_icon_button_new ("list-remove-symbolic");
  GtkWidget *plus = vibeos_icon_button_new ("list-add-symbolic");
  /* TRANSLATORS: the − and + buttons' names for a screen reader; %s is the setting ("Font size") */
  g_autofree char *less = g_strdup_printf (_("%s smaller"), title);
  /* TRANSLATORS: %s is the setting ("Font size") */
  g_autofree char *more = g_strdup_printf (_("%s larger"), title);
  gtk_accessible_update_property (GTK_ACCESSIBLE (minus), GTK_ACCESSIBLE_PROPERTY_LABEL, less, -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (plus), GTK_ACCESSIBLE_PROPERTY_LABEL, more, -1);
  GtkWidget *sc = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, MAX (n, 2) - 1, 1);
  gtk_scale_set_draw_value (GTK_SCALE (sc), FALSE);
  gtk_range_set_round_digits (GTK_RANGE (sc), 0);
  gtk_range_set_value (GTK_RANGE (sc), MIN (selected, MAX (n, 1) - 1));
  gtk_widget_set_hexpand (sc, TRUE);
  gtk_widget_set_valign (sc, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (sc), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_object_set_data (G_OBJECT (minus), "ns-by", GINT_TO_POINTER (-1));
  g_object_set_data (G_OBJECT (plus), "ns-by", GINT_TO_POINTER (1));
  for (GtkWidget **b = (GtkWidget *[]) { minus, plus, NULL }; *b; b++) {
    gtk_widget_add_css_class (*b, "ns-step");
    gtk_widget_set_valign (*b, GTK_ALIGN_CENTER);
    g_signal_connect (*b, "clicked", G_CALLBACK (steps_step), sc);
  }
  gtk_box_append (GTK_BOX (h), minus);
  gtk_box_append (GTK_BOX (h), sc);
  gtk_box_append (GTK_BOX (h), plus);
  gtk_box_append (GTK_BOX (item), h);
  g_object_set_data (G_OBJECT (item), "ns-minus", minus);
  g_object_set_data (G_OBJECT (item), "ns-plus", plus);
  g_object_set_data (G_OBJECT (item), "ns-value", value);
  g_signal_connect (sc, "value-changed", G_CALLBACK (steps_ends), item);
  steps_ends (GTK_RANGE (sc), item);
  vibeos_card_list_append (VIBEOS_CARD_LIST (group), item);
  *scale = sc;
  return item;
}

void
ns_row_steps_set_value_text (GtkWidget *row, const char *text)
{
  GtkWidget *value = g_object_get_data (G_OBJECT (row), "ns-value");
  gtk_label_set_text (GTK_LABEL (value), text ? text : "");
  gtk_widget_set_visible (value, text && *text);
}

/* ---- one flag of a flags key ---- */

typedef struct {
  GSettings *s;
  char      *key, *flag;
  GtkWidget *sw;
  gboolean   syncing;
} Flag;

static void
flag_show (GSettings *s, const char *key, Flag *f)
{
  g_auto (GStrv) v = g_settings_get_strv (f->s, f->key);
  f->syncing = TRUE;
  ns_switch_set_active (f->sw, g_strv_contains ((const char *const *) v, f->flag));
  f->syncing = FALSE;
}

static void
flag_toggled (GtkWidget *sw, GParamSpec *ps, Flag *f)
{
  if (f->syncing)
    return;
  g_auto (GStrv) v = g_settings_get_strv (f->s, f->key);
  GStrvBuilder *b = g_strv_builder_new ();
  for (guint i = 0; v[i]; i++)
    if (g_strcmp0 (v[i], f->flag))
      g_strv_builder_add (b, v[i]);
  if (ns_switch_get_active (sw))
    g_strv_builder_add (b, f->flag);
  g_auto (GStrv) nv = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_settings_set_strv (f->s, f->key, (const char *const *) nv);
}

static void
flag_free (Flag *f)
{
  g_signal_handlers_disconnect_by_data (f->s, f);
  g_free (f->key);
  g_free (f->flag);
  g_free (f);
}

GtkWidget *
ns_row_flag (GtkWidget *group, const char *title, const char *subtitle, GSettings *s, const char *key,
             const char *flag)
{
  GtkWidget *row = ns_row_switch (group, title, subtitle, NULL, NULL);
  Flag *f = g_new0 (Flag, 1);
  f->s = s;
  f->key = g_strdup (key);
  f->flag = g_strdup (flag);
  f->sw = ns_row_get_switch (row);
  flag_show (s, key, f);
  g_signal_connect (f->sw, "notify::active", G_CALLBACK (flag_toggled), f);
  g_autofree char *sig = g_strconcat ("changed::", key, NULL);
  g_signal_connect (s, sig, G_CALLBACK (flag_show), f);
  g_object_set_data_full (G_OBJECT (row), "ns-own", f, (GDestroyNotify) flag_free);
  return row;
}

/* ---- a choice by position ---- */

/* a copy of `labels`, translated (a static list of N_() strings is translated here); an empty label stays empty,
 * since gettext's translation of "" is the catalog's header */
static char **
translated_strv (const char *const *labels)
{
  GStrvBuilder *b = g_strv_builder_new ();
  for (guint i = 0; labels && labels[i]; i++)
    g_strv_builder_add (b, *labels[i] ? _(labels[i]) : "");
  char **out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return out;
}

typedef struct {
  char     **labels;
  char     **summaries;         /* or NULL: the labels */
  guint      selected;
  NsPicked   picked;
  gpointer   data;
  GtkWidget *row;
} Pick;

static void
pick_picked (GtkWidget *row, gpointer index)
{
  Pick *p = g_object_get_data (G_OBJECT (row), "ns-pick");
  mark_only (row);
  ns_row_pick_select (p->row, GPOINTER_TO_UINT (index));
  if (p->picked)
    p->picked (p->row, GPOINTER_TO_UINT (index), p->data);
}

static void
pick_open (GtkWidget *row, gpointer data)
{
  Pick *p = data;
  GtkWidget *content;
  NsPage *page = ns_page_new (g_object_get_data (G_OBJECT (row), "ns-title"), &content);
  /* the row (and with it the labels and the choice) lives as long as its picker: a card filled again behind the
   * picker drops the row it was opened from */
  g_object_set_data_full (G_OBJECT (page), "ns-pick-row", g_object_ref (row), g_object_unref);
  GtkWidget *g = ns_group (content, NULL);
  for (guint i = 0; p->labels[i]; i++) {
    GtkWidget *r = mark_row (g, p->labels[i], i == p->selected);
    g_object_set_data (G_OBJECT (r), "ns-pick", p);
    ns_row_on_activate (r, pick_picked, GUINT_TO_POINTER (i));
  }
  ns_push (page);
}

static void
pick_free (Pick *p)
{
  g_strfreev (p->labels);
  g_strfreev (p->summaries);
  g_free (p);
}

GtkWidget *
ns_row_pick (GtkWidget *group, const char *title, const char *const *labels, guint selected, NsPicked picked,
             gpointer data)
{
  Pick *p = g_new0 (Pick, 1);
  p->labels = translated_strv (labels);
  p->picked = picked;
  p->data = data;
  p->row = ns_row (group, NULL, title, NULL);
  g_object_set_data_full (G_OBJECT (p->row), "ns-title", g_strdup (title), g_free);
  g_object_set_data (G_OBJECT (p->row), "ns-pick", p);
  ns_row_pick_select (p->row, selected);
  ns_row_on_activate (p->row, pick_open, p);
  g_object_set_data_full (G_OBJECT (p->row), "ns-own", p, (GDestroyNotify) pick_free);
  return p->row;
}

void
ns_row_pick_select (GtkWidget *row, guint selected)
{
  Pick *p = g_object_get_data (G_OBJECT (row), "ns-pick");
  char **says = p->summaries ? p->summaries : p->labels;

  p->selected = selected;
  ns_row_set_subtitle (row, selected < g_strv_length (says) ? says[selected] : NULL);
}

void
ns_row_pick_set_summaries (GtkWidget *row, const char *const *summaries)
{
  Pick *p = g_object_get_data (G_OBJECT (row), "ns-pick");

  g_strfreev (p->summaries);
  p->summaries = translated_strv (summaries);
  ns_row_pick_select (row, p->selected);
}

/* ---- a typed value ---- */

typedef struct {
  GSettings *s;
  char      *key;
  GtkWidget *row;
} Entry;

static gboolean
entry_is_int (Entry *e)
{
  g_autoptr (GVariant) v = g_settings_get_value (e->s, e->key);
  return g_variant_is_of_type (v, G_VARIANT_TYPE_INT32);
}

static char *
entry_text (Entry *e)
{
  if (entry_is_int (e))
    return g_strdup_printf ("%d", g_settings_get_int (e->s, e->key));
  return g_settings_get_string (e->s, e->key);
}

static void
entry_show (GSettings *s, const char *key, Entry *e)
{
  g_autofree char *t = entry_text (e);
  ns_row_set_subtitle (e->row, *t ? t : _("Not set"));
}

static void
entry_done (const char *text, gpointer data)
{
  Entry *e = data;
  if (!text)
    return;
  if (entry_is_int (e))
    g_settings_set_int (e->s, e->key, atoi (text));
  else
    g_settings_set_string (e->s, e->key, text);
}

static void
entry_open (GtkWidget *row, gpointer data)
{
  Entry *e = data;
  g_autofree char *t = entry_text (e);
  ns_ask (row, g_object_get_data (G_OBJECT (row), "ns-title"), NULL, t, FALSE, _("Save"), entry_done, e);
}

static void
entry_free (Entry *e)
{
  g_signal_handlers_disconnect_by_data (e->s, e);
  g_free (e->key);
  g_free (e);
}

GtkWidget *
ns_row_entry (GtkWidget *group, const char *title, GSettings *s, const char *key)
{
  Entry *e = g_new0 (Entry, 1);
  e->s = s;
  e->key = g_strdup (key);
  e->row = ns_row (group, NULL, title, NULL);
  g_object_set_data_full (G_OBJECT (e->row), "ns-title", g_strdup (title), g_free);
  entry_show (s, key, e);
  g_autofree char *sig = g_strconcat ("changed::", key, NULL);
  g_signal_connect (s, sig, G_CALLBACK (entry_show), e);
  ns_row_on_activate (e->row, entry_open, e);
  g_object_set_data_full (G_OBJECT (e->row), "ns-own", e, (GDestroyNotify) entry_free);
  return e->row;
}

/* a value of the caller's own (a form field): the row shows it, a tap asks for a new one */
typedef struct {
  GtkWidget *row;
  char      *title, *value;
  gboolean   secret;
  NsEntered  changed;
  gpointer   data;
} Value;

static void
value_show (Value *v)
{
  ns_row_set_subtitle (v->row, !v->value || !*v->value ? _("Not set") : v->secret ? "••••••••" : v->value);
}

static void
value_done (const char *text, gpointer data)
{
  Value *v = data;
  if (!text)
    return;
  g_free (v->value);
  v->value = g_strstrip (g_strdup (text));
  value_show (v);
  if (v->changed)
    v->changed (v->value, v->data);
}

static void
value_open (GtkWidget *row, gpointer data)
{
  Value *v = data;
  ns_ask (row, v->title, NULL, v->value, v->secret, _("Save"), value_done, v);
}

static void
value_free (Value *v)
{
  g_free (v->title);
  g_free (v->value);
  g_free (v);
}

GtkWidget *
ns_row_value (GtkWidget *group, const char *title, const char *value, gboolean secret, NsEntered changed,
              gpointer data)
{
  Value *v = g_new0 (Value, 1);
  v->row = ns_row (group, NULL, title, NULL);
  v->title = g_strdup (title);
  v->value = g_strdup (value);
  v->secret = secret;
  v->changed = changed;
  v->data = data;
  value_show (v);
  ns_row_on_activate (v->row, value_open, v);
  g_object_set_data_full (G_OBJECT (v->row), "ns-value", v, (GDestroyNotify) value_free);
  return v->row;
}

const char *
ns_row_value_get (GtkWidget *row)
{
  Value *v = g_object_get_data (G_OBJECT (row), "ns-value");
  return v && v->value ? v->value : "";
}

void
ns_row_value_set (GtkWidget *row, const char *value)
{
  Value *v = g_object_get_data (G_OBJECT (row), "ns-value");
  g_free (v->value);
  v->value = g_strdup (value);
  value_show (v);
}

/* ---- notes and cards ---- */

GtkWidget *
ns_note (GtkWidget *content, const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "ns-note");
  gtk_box_append (GTK_BOX (content), l);
  return l;
}

GtkWidget *
ns_footnote (GtkWidget *content, const char *text)
{
  GtkWidget *foot = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (foot, "ns-footnote");
  GtkWidget *mark = gtk_image_new_from_icon_name ("help-about-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (mark), 20);
  gtk_widget_set_halign (mark, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (foot), mark);
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_box_append (GTK_BOX (foot), l);
  gtk_box_append (GTK_BOX (content), foot);
  return l;
}

void
ns_group_clear (GtkWidget *group)
{
  const char *title = g_object_get_data (G_OBJECT (group), "ns-group-title");
  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (group));
  if (title)
    vibeos_card_list_add_group (VIBEOS_CARD_LIST (group), title);
}

void
ns_group_set_visible (GtkWidget *group, gboolean visible)
{
  gtk_widget_set_visible (group, visible);
}

void
ns_group_remove (GtkWidget *group, GtkWidget *row)
{
  vibeos_card_list_remove (VIBEOS_CARD_LIST (group), row);
}

GtkWidget *
ns_button_new (const char *label, gboolean filled)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, filled ? "ns-filled" : "ns-tonal");
  return b;
}

/* ---- lists that follow a service ---- */

typedef struct {
  guint  timer;
  NsFill fill;
} Later;

static gboolean
later_run (gpointer page)
{
  Later *l = g_object_get_data (page, "ns-later");
  l->timer = 0;
  l->fill (page);
  return G_SOURCE_REMOVE;
}

void
ns_later (NsPage *page)
{
  Later *l = g_object_get_data (G_OBJECT (page), "ns-later");
  if (l->timer)
    g_source_remove (l->timer);
  l->timer = g_timeout_add (250, later_run, page);
}

static void
later_free (Later *l)
{
  if (l->timer)
    g_source_remove (l->timer);
  g_free (l);
}

void
ns_later_init (NsPage *page, NsFill fill)
{
  Later *l = g_new0 (Later, 1);
  l->fill = fill;
  g_object_set_data_full (G_OBJECT (page), "ns-later", l, (GDestroyNotify) later_free);
}

/* ---- dialogs ---- */

typedef struct {
  NsEntered  done;
  NsAnswered answered;
  gpointer   data;
  GtkWidget *entry;
} Ask;

static void
ask_response (VibeosDialog *d, int response, Ask *a)
{
  gboolean ok = response == VIBEOS_RESPONSE_OK;
  if (a->done)
    a->done (ok ? gtk_editable_get_text (GTK_EDITABLE (a->entry)) : NULL, a->data);
  else if (a->answered)
    a->answered (ok, a->data);
}

static void
ask_free (Ask *a)
{
  g_clear_object (&a->entry);
  g_free (a);
}

/* a dialog over the page it belongs to: centred on the detail pane beside the menu */
static VibeosDialog *
alert (GtkWidget *parent, const char *heading, const char *body, const char *ok, Ask *a)
{
  VibeosDialog *d = vibeos_dialog_new (heading);
  if (body)
    vibeos_dialog_set_message (d, body);
  if (a->entry)
    vibeos_dialog_set_content (d, a->entry);
  vibeos_dialog_add_button (d, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (d, ok, VIBEOS_RESPONSE_OK);
  g_signal_connect_data (d, "response", G_CALLBACK (ask_response), a, (GClosureNotify) (void (*) (void)) ask_free, 0);
  if (ld && vibeos_list_detail_is_split (ld) && parent
      && gtk_widget_is_ancestor (parent, vibeos_list_detail_get_detail_pane (ld)))
    vibeos_dialog_set_anchor (d, vibeos_list_detail_get_detail_pane (ld));
  return d;
}

static GtkWidget *
dialog_parent (GtkWidget *parent)
{
  /* a row of a page that has closed has no window: the dialog goes over the window still there */
  if (parent && gtk_widget_get_root (parent))
    return parent;
  return ld ? GTK_WIDGET (ld) : parent;
}

static void
entry_activated (GtkWidget *entry, VibeosDialog *d)
{
  vibeos_dialog_respond (d, VIBEOS_RESPONSE_OK);
}

void
ns_ask (GtkWidget *parent, const char *heading, const char *body, const char *initial, gboolean secret,
        const char *ok, NsEntered done, gpointer data)
{
  Ask *a = g_new0 (Ask, 1);
  a->done = done;
  a->data = data;
  a->entry = g_object_ref_sink (secret ? gtk_password_entry_new () : gtk_entry_new ());
  if (secret)
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (a->entry), TRUE);
  gtk_widget_add_css_class (a->entry, "vibeos-entry");
  gtk_editable_set_text (GTK_EDITABLE (a->entry), initial ? initial : "");
  parent = dialog_parent (parent);
  VibeosDialog *d = alert (parent, heading, body, ok, a);
  g_signal_connect_object (a->entry, "activate", G_CALLBACK (entry_activated), d, 0);
  vibeos_dialog_present (d, parent);
  gtk_widget_grab_focus (a->entry);
}

GtkWidget *
ns_confirm (GtkWidget *parent, const char *heading, const char *body, const char *yes, gboolean destructive,
            NsAnswered done, gpointer data)
{
  Ask *a = g_new0 (Ask, 1);
  a->answered = done;
  a->data = data;
  parent = dialog_parent (parent);
  VibeosDialog *d = alert (parent, heading, body, yes, a);
  vibeos_dialog_present (d, parent);
  return GTK_WIDGET (d);
}

void
ns_dialog_close (GtkWidget *dialog)
{
  vibeos_dialog_respond (VIBEOS_DIALOG (dialog), VIBEOS_RESPONSE_CANCEL);
}

/* ---- the rest ---- */

void
ns_spawn (const char *const *argv)
{
  g_autoptr (GError) err = NULL;
  if (!g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err))
    g_warning ("%s: %s", argv[0], err->message);
}

GSettings *
ns_settings (const char *schema)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  return sc ? g_settings_new_full (sc, NULL, NULL) : NULL;
}

gboolean
ns_settings_exists (const char *schema)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  return sc != NULL;
}

char *
ns_munge_app_id (const char *id)
{
  char *m = g_ascii_strdown (id, -1);
  if (g_str_has_suffix (m, ".desktop"))
    m[strlen (m) - strlen (".desktop")] = 0;
  g_strcanon (m, "0123456789abcdefghijklmnopqrstuvwxyz-", '-');
  /* and no two dashes in a row, as GNOME Shell's own (messageTray.js _canonicalizeId), which is what reads the
   * notification keys: "my--app" would be another app's path to it */
  char *out = m;
  for (const char *in = m; *in; in++)
    if (*in != '-' || out == m || out[-1] != '-')
      *out++ = *in;
  *out = 0;
  return m;
}

GSettings *
ns_page_settings (NsPage *page, const char *schema)
{
  GSettings *s = ns_settings (schema);
  if (s)
    g_object_set_data_full (G_OBJECT (page), schema, s, g_object_unref);
  return s;
}

gboolean
ns_has_key (GSettings *s, const char *key)
{
  g_autoptr (GSettingsSchema) sc = NULL;
  if (!s)
    return FALSE;
  g_object_get (s, "settings-schema", &sc, NULL);
  return g_settings_schema_has_key (sc, key);
}

GdkTexture *
ns_texture_for_pixbuf (GdkPixbuf *pb)
{
  g_autoptr (GBytes) b = gdk_pixbuf_read_pixel_bytes (pb);
  return gdk_memory_texture_new (gdk_pixbuf_get_width (pb), gdk_pixbuf_get_height (pb),
                                 gdk_pixbuf_get_has_alpha (pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, b,
                                 gdk_pixbuf_get_rowstride (pb));
}

char *
ns_run_line (const char *const *argv)
{
  g_autofree char *out = NULL;
  if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, NULL, NULL) || !out)
    return NULL;
  char *nl = strchr (out, '\n');
  if (nl)
    *nl = 0;
  g_strstrip (out);
  return *out ? g_strdup (out) : NULL;
}
