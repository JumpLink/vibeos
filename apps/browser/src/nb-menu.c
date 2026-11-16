/* nb-menu: the browser's menu, the link sheet, "Send to" and the add-shortcut sheet. */
#include "nb.h"
#include "nb-widgets.h"

#include <vibeos.h>
#include <spongy-gtk.h>

#define MENU_MARGIN 16          /* px around the menu's list */
/* the menu grows in from the top: scale 0.9 <-> 1 about the top centre over 220 ms (a quintic deceleration), alpha
 * over 150 ms (a cubic deceleration), both ways */
#define SCALE_MS    220
#define ALPHA_MS    150

/* ==== the popup window: placed under (reach mode: over) the URL bar at the window's end, a tap outside closes it */

#define NB_TYPE_POPUP (nb_popup_get_type ())
G_DECLARE_FINAL_TYPE (NbPopup, nb_popup, NB, POPUP, GtkWidget)

struct _NbPopup {
  GtkWidget  parent;
  GtkWidget *content;
  NbTab     *tab;
  double     t;                 /* the running animation's linear time, 0..1 */
  gboolean   closing;
  VibeosAnim   *anim;
  GtkWidget *indicator;
};

G_DEFINE_FINAL_TYPE (NbPopup, nb_popup, GTK_TYPE_WIDGET)

static void
nb_popup_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

/* the menu's end on the URL bar's end, its top on the bar's bottom, as tall as it wants up to the window's bottom;
 * reach mode: its bottom on the bar's top */
static void
nb_popup_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  NbPopup *self = NB_POPUP (widget);
  graphene_rect_t bar;
  int w, h, min_h;
  gboolean reach = nb_store_get_bool (nb->store, "reachMode", FALSE);
  double top = 64, bottom = height;

  if (gtk_widget_compute_bounds (self->tab->bar, widget, &bar)) {
    top = bar.origin.y + bar.size.height;
    bottom = bar.origin.y;
  }
  gtk_widget_measure (self->content, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
  w = MIN (w, width);
  gtk_widget_measure (self->content, GTK_ORIENTATION_VERTICAL, w, &min_h, &h, NULL, NULL);
  int room = reach ? (int) bottom : height - (int) top;
  h = MAX (MIN (h, room), MIN (min_h, room));
  gtk_widget_allocate (self->content, w, h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (width - w, reach ? bottom - h : top)));
}

static void
nb_popup_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbPopup *self = NB_POPUP (widget);
  graphene_rect_t box;

  if (!gtk_widget_compute_bounds (self->content, widget, &box))
    return;
  double scale = spongy_curve (SPONGY_DECELERATE_QUINT, self->t);
  double alpha = spongy_curve (SPONGY_DECELERATE_CUBIC, MIN (1, self->t * SCALE_MS / ALPHA_MS));
  if (self->closing) {
    scale = 1 - scale;
    alpha = 1 - alpha;
  }
  scale = 0.9 + 0.1 * scale;
  /* the pivot: the window's top centre (the content's margins are the popup window's) */
  double px = box.origin.x + box.size.width / 2, py = box.origin.y - MENU_MARGIN;
  gtk_snapshot_push_opacity (snapshot, alpha);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (px, py));
  gtk_snapshot_scale (snapshot, scale, scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-px, -py));
  gtk_widget_snapshot_child (widget, self->content, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
on_popup_anim (VibeosAnim *anim, double value, gpointer data)
{
  NbPopup *self = data;

  self->t = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (self->closing && !vibeos_anim_is_running (anim)) {
    GtkWidget *overlay = gtk_widget_get_parent (GTK_WIDGET (self));
    if (overlay)
      gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self));
  }
}

static void
nb_popup_dismiss (NbPopup *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  vibeos_anim_stop (self->anim);
  self->t = 0;
  vibeos_anim_start (self->anim, 0, 1, SCALE_MS, 0, SPONGY_LINEAR);
}

static void
on_popup_pressed (GtkGestureClick *click, int n, double x, double y, NbPopup *self)
{
  graphene_rect_t box;

  if (gtk_widget_compute_bounds (self->content, GTK_WIDGET (self), &box) &&
      graphene_rect_contains_point (&box, &GRAPHENE_POINT_INIT (x, y)))
    return;
  /* a touch outside dismisses it, and goes nowhere else */
  gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);
  nb_menu_close (self->tab);
}

static void
nb_popup_dispose (GObject *object)
{
  NbPopup *self = NB_POPUP (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  g_clear_pointer (&self->content, gtk_widget_unparent);
  G_OBJECT_CLASS (nb_popup_parent_class)->dispose (object);
}

static void
nb_popup_class_init (NbPopupClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = nb_popup_dispose;
  widget_class->measure = nb_popup_measure;
  widget_class->size_allocate = nb_popup_size_allocate;
  widget_class->snapshot = nb_popup_snapshot;
  gtk_widget_class_set_css_name (widget_class, "nb-popup");
}

static void
nb_popup_init (NbPopup *self)
{
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_popup_anim, self);
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_popup_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

/* ==== the menu ==== */

typedef enum {
  OPT_BACK, OPT_FORWARD, OPT_REFRESH, OPT_ADD_TO_FAVORITE, OPT_SHARE, OPT_NEW_TAB, OPT_NEW_PRIVATE_TAB,
  OPT_FAVORITES, OPT_HISTORY, OPT_DOWNLOADS, OPT_ADD_TO_HOME_SCREEN, OPT_FIND_IN_PAGE, OPT_DESKTOP_VIEW, OPT_PRINT,
  OPT_DESKTOP_SHORTCUTS, OPT_SETTINGS,
} Option;

static gboolean
share_later (gpointer data)
{
  NbTab *t = data;

  if (!t->closing && t->view && webkit_web_view_get_uri (t->view))
    nb_send_to (t, webkit_web_view_get_uri (t->view));
  return G_SOURCE_REMOVE;
}

/* a menu row: its action, then the menu goes */
static void
run_option (NbTab *t, Option o)
{
  WebKitWebView *v = t->view;

  switch (o) {
  case OPT_BACK:
    if (v)
      webkit_web_view_go_back (v);
    break;
  case OPT_FORWARD:
    if (v)
      webkit_web_view_go_forward (v);
    break;
  case OPT_REFRESH:
    if (v)
      webkit_web_view_reload (v);
    break;
  case OPT_ADD_TO_FAVORITE:
    if (v && webkit_web_view_get_uri (v))
      nb_tab_add_favorite (t, webkit_web_view_get_title (v), webkit_web_view_get_uri (v));
    break;
  case OPT_SHARE:
    /* delayed a bit for the popup's exit */
    g_timeout_add (300, share_later, t);
    break;
  case OPT_NEW_TAB:
    nb_open_tab (NULL, FALSE, NULL, NULL);
    break;
  case OPT_NEW_PRIVATE_TAB:
    nb_open_tab (NULL, TRUE, NULL, NULL);
    break;
  case OPT_FAVORITES:
    nb_tab_push_page (t, nb_favorites_page_new (t));
    break;
  case OPT_HISTORY:
    nb_tab_push_page (t, nb_history_page_new (t));
    break;
  case OPT_DOWNLOADS:
    nb_tab_push_page (t, nb_downloads_page_new (t));
    break;
  case OPT_ADD_TO_HOME_SCREEN:
    nb_shortcut_sheet (t);
    break;
  case OPT_FIND_IN_PAGE:
    nb_urlbar_set_finding (t, TRUE);
    break;
  case OPT_DESKTOP_VIEW:
    nb_tab_set_desktop (t, !t->desktop);
    break;
  case OPT_PRINT:
    nb_tab_print (t);
    break;
  case OPT_DESKTOP_SHORTCUTS:
    nb_tab_push_page (t, nb_desktop_shortcuts_page_new (t));
    break;
  case OPT_SETTINGS:
    nb_tab_push_page (t, nb_settings_page_new (t));
    break;
  }
}

static void
on_option (GtkWidget *button, NbTab *t)
{
  Option o = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "nb-option"));

  nb_menu_close (t);
  run_option (t, o);
}

static void
on_desktop_switch (VibeosSwitch *sw, GParamSpec *pspec, NbTab *t)
{
  if (vibeos_switch_get_active (sw) == t->desktop)
    return;
  nb_menu_close (t);
  run_option (t, OPT_DESKTOP_VIEW);
}

static GtkWidget *
top_icon (NbTab *t, const char *icon, const char *name, Option o)
{
  GtkWidget *image = nb_icon (icon, 24);
  GtkWidget *b = vibeos_pressable_new (image);

  /* a borderless, round ripple */
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (b), VIBEOS_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (b, "nb-menu-top-icon");
  gtk_widget_set_size_request (b, 24, 24);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_object_set_data (G_OBJECT (b), "nb-option", GINT_TO_POINTER (o));
  g_signal_connect (b, "clicked", G_CALLBACK (on_option), t);
  return b;
}

/* a menu row: the icon, then the title */
static GtkWidget *
item (NbTab *t, const char *icon, const char *title, Option o)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *image = nb_icon (icon, 24);
  GtkWidget *label = nb_label (title, 16, "nb-menu-text", TRUE);

  gtk_widget_add_css_class (box, "nb-menu-item");
  gtk_widget_add_css_class (image, "nb-menu-icon");
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), image);
  gtk_box_append (GTK_BOX (box), label);
  GtkWidget *row = vibeos_pressable_new (box);
  g_object_set_data (G_OBJECT (row), "nb-option", GINT_TO_POINTER (o));
  g_signal_connect (row, "clicked", G_CALLBACK (on_option), t);
  return row;
}

/* the Desktop site row: the same row, not tappable itself; the title takes the room up to the switch, whose own
 * listener runs the option */
static GtkWidget *
desktop_item (NbTab *t)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *image = nb_icon ("jelly-desktop-symbolic", 24);
  GtkWidget *label = nb_label ("Desktop site", 16, "nb-menu-text", TRUE);
  GtkWidget *sw = vibeos_switch_new ();

  gtk_widget_add_css_class (box, "nb-menu-item");
  gtk_widget_add_css_class (box, "nb-menu-switch-item");
  gtk_widget_add_css_class (image, "nb-menu-icon");
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, 16);
  gtk_widget_set_margin_end (label, 12);
  vibeos_switch_set_thumb_icons (VIBEOS_SWITCH (sw), FALSE);
  vibeos_switch_set_active (VIBEOS_SWITCH (sw), t->desktop);
  gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
  g_signal_connect (sw, "notify::active", G_CALLBACK (on_desktop_switch), t);
  gtk_box_append (GTK_BOX (box), image);
  gtk_box_append (GTK_BOX (box), label);
  gtk_box_append (GTK_BOX (box), sw);
  return box;
}

static GtkWidget *
divider (void)
{
  GtkWidget *d = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (d, "nb-menu-divider");
  return d;
}

void
nb_menu_close (NbTab *t)
{
  if (!t->menu)
    return;
  nb_popup_dismiss (NB_POPUP (t->menu));
  t->menu = NULL;
  nb_tab_sync_back (t);
}

void
nb_menu_open (NbTab *t)
{
  if (t->menu)
    return;
  nb_urlbar_stop_editing (t);
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, MENU_MARGIN);
  gtk_widget_add_css_class (outer, "nb-menu");

  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_add_css_class (top, "nb-menu-top");
  gtk_widget_set_halign (top, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (top), top_icon (t, "jelly-back-symbolic", "Go back", OPT_BACK));
  gtk_box_append (GTK_BOX (top), top_icon (t, "jelly-forward-symbolic", "Go forward", OPT_FORWARD));
  gtk_box_append (GTK_BOX (top), top_icon (t, "jelly-refresh-symbolic", "Refresh page", OPT_REFRESH));
  gtk_box_append (GTK_BOX (top), top_icon (t, "jelly-heart-plus-symbolic", "Add to favorites", OPT_ADD_TO_FAVORITE));
  gtk_box_append (GTK_BOX (top), top_icon (t, "jelly-share-symbolic", "Share", OPT_SHARE));
  gtk_box_append (GTK_BOX (outer), top);

  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (list, "nb-menu-list");
  gtk_box_append (GTK_BOX (list), item (t, "jelly-new-tab-symbolic", "New tab", OPT_NEW_TAB));
  gtk_box_append (GTK_BOX (list), item (t, "jelly-new-private-tab-symbolic", "Incognito tab", OPT_NEW_PRIVATE_TAB));
  gtk_box_append (GTK_BOX (list), divider ());
  gtk_box_append (GTK_BOX (list), item (t, "jelly-favorite-symbolic", "Favorites", OPT_FAVORITES));
  gtk_box_append (GTK_BOX (list), item (t, "jelly-history-symbolic", "History", OPT_HISTORY));
  gtk_box_append (GTK_BOX (list), item (t, "jelly-file-download-symbolic", "Downloads", OPT_DOWNLOADS));
  gtk_box_append (GTK_BOX (list), divider ());
  gtk_box_append (GTK_BOX (list), item (t, "jelly-apps-symbolic", "Add shortcut", OPT_ADD_TO_HOME_SCREEN));
  gtk_box_append (GTK_BOX (list), item (t, "jelly-find-in-page-symbolic", "Find in page", OPT_FIND_IN_PAGE));
  gtk_box_append (GTK_BOX (list), desktop_item (t));
  gtk_box_append (GTK_BOX (list), item (t, "jelly-print-symbolic", "Print", OPT_PRINT));
  gtk_box_append (GTK_BOX (list), divider ());
  gtk_box_append (GTK_BOX (list), item (t, "jelly-desktop-symbolic", "Desktop shortcuts", OPT_DESKTOP_SHORTCUTS));
  gtk_box_append (GTK_BOX (list), item (t, "jelly-settings-symbolic", "Settings", OPT_SETTINGS));

  /* the scroller: the list at its natural height, scrolled when the window is shorter */
  GtkWidget *scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  GtkWidget *card = gtk_overlay_new ();
  gtk_widget_add_css_class (card, "nb-menu-card");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
  gtk_overlay_set_child (GTK_OVERLAY (card), scroll);
  GtkWidget *indicator = nb_indicator_new (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (scroll)));
  gtk_overlay_add_overlay (GTK_OVERLAY (card), indicator);
  gtk_widget_set_vexpand (card, TRUE);
  gtk_box_append (GTK_BOX (outer), card);
  gtk_widget_set_margin_start (outer, MENU_MARGIN);
  gtk_widget_set_margin_end (outer, MENU_MARGIN);
  gtk_widget_set_margin_top (outer, MENU_MARGIN);
  gtk_widget_set_margin_bottom (outer, MENU_MARGIN);

  NbPopup *p = g_object_new (NB_TYPE_POPUP, NULL);
  p->tab = t;
  p->content = outer;
  p->indicator = indicator;
  gtk_widget_set_parent (outer, GTK_WIDGET (p));
  GtkOverlay *overlay = vibeos_window_get_overlay (t->win);
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (p));
  gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (p), FALSE);
  gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (p), TRUE);
  vibeos_anim_start (p->anim, 0, 1, SCALE_MS, 0, SPONGY_LINEAR);
  nb_indicator_awaken (indicator);
  t->menu = GTK_WIDGET (p);
  nb_tab_sync_back (t);
}

/* ==== the link sheet ==== */

typedef struct {
  NbTab   *t;
  char    *url;
  NbSheet *sheet;
} LinkAction;

static void
link_free (gpointer data)
{
  LinkAction *a = data;
  g_free (a->url);
  g_free (a);
}

static void
on_link_new_tab (GtkWidget *row, LinkAction *a)
{
  nb_open_tab (a->url, a->t->incognito, NULL, NULL);
  nb_sheet_dismiss (a->sheet);
}

static void
on_link_share (GtkWidget *row, LinkAction *a)
{
  g_autofree char *url = g_strdup (a->url);
  NbTab *t = a->t;
  nb_sheet_dismiss (a->sheet);
  nb_send_to (t, url);
}

static void
on_link_favorite (GtkWidget *row, LinkAction *a)
{
  /* a favourite with the URL as its title */
  nb_tab_add_favorite (a->t, a->url, a->url);
  nb_sheet_dismiss (a->sheet);
}

static void
on_link_download (GtkWidget *row, LinkAction *a)
{
  nb_tab_ask_download (a->t, a->url, NULL);
  nb_sheet_dismiss (a->sheet);
}

void
nb_link_sheet (NbTab *t, const char *url, gboolean image)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  LinkAction *a = g_new0 (LinkAction, 1);
  GtkWidget *row;

  gtk_widget_add_css_class (box, "nb-sheet-box");
  a->t = t;
  a->url = g_strdup (url);
  row = nb_sheet_row ("jelly-new-tab-symbolic", NULL, "Open in new tab");
  g_signal_connect (row, "clicked", G_CALLBACK (on_link_new_tab), a);
  gtk_box_append (GTK_BOX (box), row);
  row = nb_sheet_row ("jelly-share-symbolic", NULL, "Share");
  g_signal_connect (row, "clicked", G_CALLBACK (on_link_share), a);
  gtk_box_append (GTK_BOX (box), row);
  row = nb_sheet_row ("jelly-heart-plus-symbolic", NULL, "Add to favorites");
  g_signal_connect (row, "clicked", G_CALLBACK (on_link_favorite), a);
  gtk_box_append (GTK_BOX (box), row);
  if (image) {
    row = nb_sheet_row ("jelly-download-symbolic", NULL, "Download");
    g_signal_connect (row, "clicked", G_CALLBACK (on_link_download), a);
    gtk_box_append (GTK_BOX (box), row);
  }
  a->sheet = nb_sheet_new (box, 0.32);
  g_object_set_data_full (G_OBJECT (a->sheet), "nb-link", a, link_free);
  nb_sheet_present (a->sheet, t->win);
}

/* ==== Send to: the targets this phone has ==== */

typedef struct {
  NbTab    *t;
  char     *url;
  NbSheet  *sheet;
  GAppInfo *app;
  char     *scheme;
} SendTarget;

static void
target_free (gpointer data)
{
  SendTarget *s = data;
  g_free (s->url);
  g_free (s->scheme);
  g_clear_object (&s->app);
  g_free (s);
}

static void
on_send (GtkWidget *row, SendTarget *s)
{
  if (!s->app) {
    gdk_clipboard_set_text (gtk_widget_get_clipboard (s->t->win), s->url);
    nb_tab_snackbar (s->t, "Link copied", NULL, NULL, NULL);
  } else {
    g_autofree char *body = g_uri_escape_string (s->url, NULL, TRUE);
    g_autofree char *uri = g_strdup_printf ("%s:?body=%s", s->scheme, body);
    GList *uris = g_list_append (NULL, uri);
    g_autoptr (GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context (gtk_widget_get_display (s->t->win));
    g_app_info_launch_uris (s->app, uris, G_APP_LAUNCH_CONTEXT (ctx), NULL);
    g_list_free (uris);
  }
  nb_sheet_dismiss (s->sheet);
}

static void
add_target (GtkWidget *box, NbTab *t, NbSheet **sheet_slot, const char *url, GAppInfo *app, const char *scheme,
            GPtrArray *keep)
{
  SendTarget *s = g_new0 (SendTarget, 1);
  GtkWidget *row;

  s->t = t;
  s->url = g_strdup (url);
  s->app = app ? g_object_ref (app) : NULL;
  s->scheme = g_strdup (scheme);
  if (app) {
    GIcon *gicon = g_app_info_get_icon (app);
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (t->win));
    g_autoptr (GtkIconPaintable) icon = gicon ? gtk_icon_theme_lookup_by_gicon (theme, gicon, 24,
                                                                               gtk_widget_get_scale_factor (t->win),
                                                                               GTK_TEXT_DIR_LTR, 0) : NULL;
    row = nb_sheet_row ("jelly-share-symbolic", icon ? GDK_PAINTABLE (icon) : NULL, g_app_info_get_name (app));
  } else {
    row = nb_sheet_row ("jelly-external-symbolic", NULL, "Copy link");
  }
  g_signal_connect (row, "clicked", G_CALLBACK (on_send), s);
  gtk_box_append (GTK_BOX (box), row);
  g_ptr_array_add (keep, s);
  (void) sheet_slot;
}

void
nb_send_to (NbTab *t, const char *url)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GPtrArray *targets = g_ptr_array_new_with_free_func (target_free);
  static const char *const schemes[] = { "sms", "mailto" };

  gtk_widget_add_css_class (box, "nb-sheet-box");
  GtkWidget *title = nb_label ("Send to", 16, "nb-sheet-title", FALSE);
  gtk_widget_set_halign (title, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), title);
  add_target (box, t, NULL, url, NULL, NULL, targets);
  for (guint i = 0; i < G_N_ELEMENTS (schemes); i++) {
    g_autofree char *type = g_strdup_printf ("x-scheme-handler/%s", schemes[i]);
    GList *apps = g_app_info_get_all_for_type (type);
    for (GList *l = apps; l; l = l->next)
      if (g_app_info_should_show (l->data))
        add_target (box, t, NULL, url, l->data, schemes[i], targets);
    g_list_free_full (apps, g_object_unref);
  }
  NbSheet *sheet = nb_sheet_new (box, 0.32);
  for (guint i = 0; i < targets->len; i++)
    ((SendTarget *) targets->pdata[i])->sheet = sheet;
  g_object_set_data_full (G_OBJECT (sheet), "nb-targets", targets, (GDestroyNotify) g_ptr_array_unref);
  nb_sheet_present (sheet, t->win);
}

/* ==== the add-shortcut sheet: the shortcut's icon and name, Cancel and Add to home screen ==== */

typedef struct {
  NbTab      *t;
  NbSheet    *sheet;
  char       *name;
  char       *url;
  GdkTexture *icon;
} PinRequest;

static void
pin_free (gpointer data)
{
  PinRequest *p = data;
  g_free (p->name);
  g_free (p->url);
  g_clear_object (&p->icon);
  g_free (p);
}

static void
on_pin_cancel (GtkWidget *b, PinRequest *p)
{
  nb_sheet_dismiss (p->sheet);
}

static void
on_pin_add (GtkWidget *b, PinRequest *p)
{
  g_autoptr (GError) error = NULL;

  if (!nb_add_shortcut (p->name, p->url, p->icon, &error)) {
    g_message ("vibeos-browser: shortcut not written: %s", error->message);
    nb_tab_snackbar (p->t, "Shortcut not added", NULL, NULL, NULL);
  }
  nb_sheet_dismiss (p->sheet);
}

static GtkWidget *
pill (const char *label)
{
  GtkWidget *l = nb_label (label, 14, "nb-pin-button-text", FALSE);
  gtk_widget_set_halign (l, GTK_ALIGN_CENTER);
  GtkWidget *b = vibeos_pressable_new (l);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (b), VIBEOS_RIPPLE_BOUNDED, 24);
  gtk_widget_add_css_class (b, "nb-pin-button");
  return b;
}

void
nb_shortcut_sheet (NbTab *t)
{
  if (!t->view || !webkit_web_view_get_uri (t->view))
    return;
  PinRequest *p = g_new0 (PinRequest, 1);
  const char *title = webkit_web_view_get_title (t->view);
  GdkTexture *favicon = nb_tab_favicon (t);

  p->t = t;
  p->url = g_strdup (webkit_web_view_get_uri (t->view));
  p->name = g_strdup (title && *title ? title : p->url);
  p->icon = favicon ? g_object_ref (favicon) : NULL;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "nb-pin-sheet");
  GtkWidget *app = nb_label ("Browser", 24, "nb-pin-title", FALSE);
  gtk_widget_set_halign (app, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), app);
  GtkWidget *hint = nb_label ("Touch & hold the shortcut to move it around the home screen", 14, "nb-pin-hint",
                              FALSE);
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_justify (GTK_LABEL (hint), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.5);
  gtk_box_append (GTK_BOX (box), hint);

  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (card, "nb-pin-card");
  GtkWidget *icon;
  if (favicon) {
    icon = gtk_image_new_from_paintable (GDK_PAINTABLE (favicon));
    gtk_widget_add_css_class (icon, "nb-pin-favicon");
  } else {
    icon = gtk_image_new_from_icon_name ("vibeos.Browser");
  }
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 56);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), icon);
  GtkWidget *name = nb_label (p->name, 14, "nb-pin-name", TRUE);
  gtk_widget_set_halign (name, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), name);
  GtkWidget *dims = nb_label ("1 × 1", 14, "nb-pin-dims", FALSE);
  gtk_widget_set_halign (dims, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), dims);
  gtk_box_append (GTK_BOX (box), card);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (buttons, "nb-pin-buttons");
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  GtkWidget *cancel = pill ("Cancel"), *add = pill ("Add to home screen");
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_pin_cancel), p);
  g_signal_connect (add, "clicked", G_CALLBACK (on_pin_add), p);
  gtk_box_append (GTK_BOX (buttons), cancel);
  gtk_box_append (GTK_BOX (buttons), add);
  gtk_box_append (GTK_BOX (box), buttons);

  p->sheet = nb_sheet_new (box, 0.6);
  g_object_set_data_full (G_OBJECT (p->sheet), "nb-pin", p, pin_free);
  nb_sheet_present (p->sheet, t->win);
}
