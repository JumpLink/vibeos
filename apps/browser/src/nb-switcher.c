/* nb-switcher: the browser's one window, and its tab switcher.
 *
 * Jelly opened every tab as a window of its own, switched in the system's recent apps; on this phone that showed
 * "New tab" as a second browser window and nothing to switch with. Here the tabs live in the browser: the window
 * shows one tab at a time (nb->tab_stack), and the tab count on the URL line opens the switcher over them: a grid
 * of cards, two across, each with the tab's favicon, its title, a × that closes it and a picture of its page; a tap
 * on a card shows that tab. The toolbar's + opens a new tab, the incognito one an incognito tab; its up arrow
 * (named "Back" for the shell's gesture) goes back to the tab that was shown.
 *
 * A card's picture is the page as it was when its tab was last left (or the switcher last opened over it), scaled
 * down to the card once, on the GPU: a tab in the background is not drawn again, and a picture costs about a
 * megabyte. */
#include "nb.h"
#include "nb-widgets.h"

#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define THUMB_WIDTH  480        /* px: a card is about half the screen, at the phone's scale */
#define THUMB_ASPECT 1.2        /* a card's picture: height / width, the top of the page */
#define GRID_SPACING 12         /* px around and between the cards */

static guint rebuild_id;
static GtkWidget *switcher_scroll;

static void rebuild (void);

/* ---- the cards' pictures ---- */

/* the top of the page, THUMB_WIDTH wide and cropped to the card's shape, drawn by the window's renderer */
static GdkTexture *
scale_down (GdkTexture *full)
{
  int w = gdk_texture_get_width (full), h = gdk_texture_get_height (full);
  GskRenderer *renderer = nb->win && gtk_widget_get_realized (nb->win)
                        ? gtk_native_get_renderer (GTK_NATIVE (nb->win)) : NULL;

  if (w <= 0 || h <= 0 || !renderer)
    return NULL;
  double scale = MIN (1.0, (double) THUMB_WIDTH / w);
  int tw = MAX (1, (int) round (w * scale));
  int sh = MAX (1, (int) round (h * scale));
  int th = MIN (sh, (int) round (tw * THUMB_ASPECT));
  GtkSnapshot *snap = gtk_snapshot_new ();
  gtk_snapshot_push_clip (snap, &GRAPHENE_RECT_INIT (0, 0, tw, th));
  gtk_snapshot_append_scaled_texture (snap, full, GSK_SCALING_FILTER_TRILINEAR, &GRAPHENE_RECT_INIT (0, 0, tw, sh));
  gtk_snapshot_pop (snap);
  GskRenderNode *node = gtk_snapshot_free_to_node (snap);
  if (!node)
    return NULL;
  GdkTexture *small = gsk_renderer_render_texture (renderer, node, &GRAPHENE_RECT_INIT (0, 0, tw, th));
  gsk_render_node_unref (node);
  return small;
}

static gboolean
on_rebuild (gpointer data)
{
  rebuild_id = 0;
  if (nb->switcher_open)
    rebuild ();
  return G_SOURCE_REMOVE;
}

static void
queue_rebuild (void)
{
  if (!rebuild_id)
    rebuild_id = g_idle_add (on_rebuild, NULL);
}

static void
on_snapshot (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) full = webkit_web_view_get_snapshot_finish (WEBKIT_WEB_VIEW (src), res, &error);

  /* cancelled: the tab may be gone, `data` is not touched */
  if (!full)
    return;
  NbTab *t = data;
  GdkTexture *small = scale_down (full);
  g_clear_object (&t->thumb_cancel);
  if (!small)
    return;
  g_clear_object (&t->thumb);
  t->thumb = small;
  if (nb->switcher_open)
    queue_rebuild ();
}

void
nb_tab_take_thumb (NbTab *t)
{
  if (!t->view || t->closing)
    return;
  if (t->thumb_cancel)
    g_cancellable_cancel (t->thumb_cancel);
  g_clear_object (&t->thumb_cancel);
  t->thumb_cancel = g_cancellable_new ();
  webkit_web_view_get_snapshot (t->view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
                                t->thumb_cancel, on_snapshot, t);
}

/* ---- the cards ---- */

/* a card's tab, if it is still open: a card can outlive its tab until the grid is built again */
static NbTab *
card_tab (GtkWidget *w)
{
  NbTab *t = g_object_get_data (G_OBJECT (w), "nb-tab");
  return t && g_ptr_array_find (nb->tabs, t, NULL) && !t->closing ? t : NULL;
}

static void
on_card (GtkWidget *card, gpointer data)
{
  NbTab *t = card_tab (card);
  if (t)
    nb_tab_present (t);
}

static void
on_card_close (GtkWidget *button, gpointer data)
{
  NbTab *t = card_tab (button);
  if (t)
    nb_tab_close (t);
}

static char *
card_title (NbTab *t)
{
  const char *title = nb_tab_title (t);
  if (*title)
    return g_strdup (title);
  const char *uri = nb_tab_uri (t);
  g_autoptr (GUri) u = *uri ? g_uri_parse (uri, G_URI_FLAGS_NONE, NULL) : NULL;
  if (u && g_uri_get_host (u))
    return g_strdup (g_uri_get_host (u));
  return g_strdup (t->incognito ? "Incognito tab" : "New tab");
}

static GtkWidget *
card_new (NbTab *t, int width)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GdkTexture *favicon = nb_tab_favicon (t);
  GtkWidget *icon;

  gtk_widget_add_css_class (card, "nb-tab-card");
  if (t == nb->last_active)
    gtk_widget_add_css_class (card, "current");
  if (t->incognito)
    gtk_widget_add_css_class (card, "incognito");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);

  gtk_widget_add_css_class (head, "nb-tab-card-head");
  if (t->incognito) {
    icon = nb_icon ("jelly-incognito-symbolic", 16);
  } else if (favicon) {
    icon = gtk_picture_new_for_paintable (GDK_PAINTABLE (favicon));
    gtk_picture_set_content_fit (GTK_PICTURE (icon), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink (GTK_PICTURE (icon), TRUE);
  } else {
    icon = gtk_image_new_from_icon_name ("vibeos.Browser");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);
  }
  gtk_widget_set_size_request (icon, 16, 16);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "nb-tab-card-icon");
  gtk_box_append (GTK_BOX (head), icon);
  g_autofree char *title = card_title (t);
  GtkWidget *label = nb_label (title, 14, "nb-tab-card-title", TRUE);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_box_append (GTK_BOX (head), label);
  GtkWidget *x = vibeos_pressable_new (nb_icon ("jelly-clear-symbolic", 18));
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (x), VIBEOS_RIPPLE_CIRCLE, 16);
  gtk_widget_add_css_class (x, "nb-tab-card-close");
  gtk_widget_set_size_request (x, 32, 32);
  gtk_widget_set_valign (x, GTK_ALIGN_CENTER);
  g_autofree char *close_name = g_strdup_printf ("Close %s", title);
  gtk_accessible_update_property (GTK_ACCESSIBLE (x), GTK_ACCESSIBLE_PROPERTY_LABEL, close_name, -1);
  g_object_set_data (G_OBJECT (x), "nb-tab", t);
  g_signal_connect (x, "clicked", G_CALLBACK (on_card_close), NULL);
  gtk_box_append (GTK_BOX (head), x);
  gtk_box_append (GTK_BOX (card), head);

  GtkWidget *picture;
  if (t->thumb) {
    picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (t->thumb));
    gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  } else {
    /* a tab not shown since the start: its page is not loaded, there is no picture */
    picture = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class (picture, "nb-tab-card-blank");
  }
  gtk_widget_add_css_class (picture, "nb-tab-card-thumb");
  gtk_widget_set_size_request (picture, -1, (int) round (width * THUMB_ASPECT));
  gtk_box_append (GTK_BOX (card), picture);

  GtkWidget *press = vibeos_pressable_new (card);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (press), VIBEOS_RIPPLE_BOUNDED, 16);
  g_object_set_data (G_OBJECT (press), "nb-tab", t);
  g_autofree char *name = g_strdup_printf ("%s%s", title, t == nb->last_active ? ", current tab" : "");
  gtk_accessible_update_property (GTK_ACCESSIBLE (press), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect (press, "clicked", G_CALLBACK (on_card), NULL);
  return press;
}

/* ---- the switcher ---- */

/* the current tab's card in view, once the grid is laid out */
static gboolean
scroll_to_current (gpointer data)
{
  GtkWidget *current = NULL;
  for (GtkWidget *c = gtk_widget_get_first_child (nb->switcher_grid); c; c = gtk_widget_get_next_sibling (c))
    if (g_object_get_data (G_OBJECT (c), "nb-tab") == nb->last_active)
      current = c;
  graphene_rect_t b;
  if (!current || !gtk_widget_compute_bounds (current, nb->switcher_grid, &b))
    return G_SOURCE_REMOVE;
  int view = gtk_widget_get_height (switcher_scroll);
  double offset = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (switcher_scroll));
  if (b.origin.y < offset || b.origin.y + b.size.height > offset + view)
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (switcher_scroll),
                                  MAX (0, b.origin.y + b.size.height - view + GRID_SPACING));
  return G_SOURCE_REMOVE;
}

/* the grid's columns: two on a phone, more as the window widens (a tablet upright, then turned) */
static int
columns (void)
{
  VibeosLayout *layout = nb->win ? vibeos_layout_get (nb->win) : NULL;

  if (!layout)
    return 2;
  switch (vibeos_layout_get_width_class (layout))
    {
    case VIBEOS_WIDTH_EXPANDED:
      return 4;
    case VIBEOS_WIDTH_MEDIUM:
      return 3;
    default:
      return 2;
    }
}

static void
rebuild (void)
{
  GtkWidget *child;
  guint n = nb->tabs->len;
  int cols = columns ();
  int win_width = nb->win ? gtk_widget_get_width (nb->win) : 0;
  int width = ((win_width > 0 ? win_width : 360) - (cols + 1) * GRID_SPACING) / cols;

  while ((child = gtk_widget_get_first_child (nb->switcher_grid)))
    gtk_grid_remove (GTK_GRID (nb->switcher_grid), child);
  for (guint i = 0; i < n; i++)
    gtk_grid_attach (GTK_GRID (nb->switcher_grid), card_new (nb->tabs->pdata[i], width), i % cols, i / cols, 1, 1);
  /* a wide window's few tabs keep to their columns' width rather than spreading over the row (a phone's one tab
   * takes the row, as it always has) */
  for (int i = n; cols > 2 && i > 0 && i < cols; i++)
    gtk_grid_attach (GTK_GRID (nb->switcher_grid), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), i, 0, 1, 1);
  g_autofree char *title = n == 1 ? g_strdup ("1 tab") : g_strdup_printf ("%u tabs", n);
  gtk_label_set_text (GTK_LABEL (nb->switcher_title), n ? title : "Tabs");
  gtk_widget_set_visible (switcher_scroll, n > 0);
  gtk_widget_set_visible (nb->switcher_empty, n == 0);
}

static void
on_up (gpointer data)
{
  if (nb->last_active)
    nb_tab_present (nb->last_active);
  else if (nb->tabs->len)
    nb_tab_present (nb->tabs->pdata[nb->tabs->len - 1]);
  else
    /* every tab closed: back leaves the browser, which has nothing to show */
    gtk_window_close (GTK_WINDOW (nb->win));
}

static void
on_new_tab (gpointer data)
{
  nb_open_tab (NULL, GPOINTER_TO_INT (data), NULL, NULL);
}

static GtkWidget *
switcher_new (void)
{
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *incognito = nb_action_button ("jelly-new-private-tab-symbolic", "New incognito tab");
  GtkWidget *add = nb_action_button ("jelly-new-tab-symbolic", "New tab");
  gtk_widget_add_css_class (incognito, "nb-toolbar-action");
  gtk_widget_add_css_class (add, "nb-toolbar-action");
  gtk_widget_set_margin_end (add, 4);
  g_signal_connect_swapped (incognito, "clicked", G_CALLBACK (on_new_tab), GINT_TO_POINTER (TRUE));
  g_signal_connect_swapped (add, "clicked", G_CALLBACK (on_new_tab), GINT_TO_POINTER (FALSE));
  gtk_box_append (GTK_BOX (actions), incognito);
  gtk_box_append (GTK_BOX (actions), add);

  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (page, "nb-page");
  gtk_widget_add_css_class (page, "nb-switcher");
  GtkWidget *toolbar = nb_toolbar_new ("Tabs", 0, G_CALLBACK (on_up), NULL, actions);
  nb->switcher_title = nb_toolbar_get_title (toolbar);
  gtk_box_append (GTK_BOX (page), toolbar);

  GtkWidget *body = gtk_overlay_new ();
  gtk_widget_set_vexpand (body, TRUE);
  nb->switcher_grid = gtk_grid_new ();
  gtk_widget_add_css_class (nb->switcher_grid, "nb-switcher-grid");
  gtk_grid_set_column_homogeneous (GTK_GRID (nb->switcher_grid), TRUE);
  gtk_grid_set_column_spacing (GTK_GRID (nb->switcher_grid), GRID_SPACING);
  gtk_grid_set_row_spacing (GTK_GRID (nb->switcher_grid), GRID_SPACING);
  gtk_widget_set_valign (nb->switcher_grid, GTK_ALIGN_START);
  switcher_scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (switcher_scroll), nb->switcher_grid);
  gtk_overlay_set_child (GTK_OVERLAY (body), switcher_scroll);
  nb->switcher_empty = nb_label ("No open tabs", 14, "nb-empty-text", FALSE);
  gtk_widget_set_valign (nb->switcher_empty, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (nb->switcher_empty, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target (nb->switcher_empty, FALSE);
  gtk_widget_set_visible (nb->switcher_empty, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (body), nb->switcher_empty);
  gtk_box_append (GTK_BOX (page), body);
  return page;
}

void
nb_switcher_open (void)
{
  NbTab *t = nb->last_active;

  if (!nb->win || nb->switcher_open)
    return;
  if (t && t->menu)
    nb_menu_close (t);
  /* the tab shown is pictured as it is now */
  if (t)
    nb_tab_take_thumb (t);
  nb->switcher_open = TRUE;
  rebuild ();
  /* no focus while the stack switches (see nb_tab_present) */
  gtk_window_set_focus (GTK_WINDOW (nb->win), NULL);
  gtk_stack_set_visible_child_name (GTK_STACK (nb->root), "switcher");
  gtk_window_set_title (GTK_WINDOW (nb->win), "Tabs");
  if (t)
    nb_tab_sync_back (t);
  g_idle_add (scroll_to_current, NULL);
}

void
nb_switcher_close (void)
{
  if (!nb->switcher_open)
    return;
  nb->switcher_open = FALSE;
  g_clear_handle_id (&rebuild_id, g_source_remove);
  gtk_window_set_focus (GTK_WINDOW (nb->win), NULL);
  gtk_stack_set_visible_child_name (GTK_STACK (nb->root), "tabs");
}

void
nb_tabs_changed (void)
{
  for (guint i = 0; i < nb->tabs->len; i++)
    nb_urlbar_sync_count (nb->tabs->pdata[i]);
  if (nb->switcher_open)
    queue_rebuild ();
}

/* ---- the window ---- */

static void
on_active (GtkWindow *win, GParamSpec *pspec, gpointer data)
{
  NbTab *t = nb->last_active;
  if (!gtk_window_is_active (win) || !t || t->closing)
    return;
  if (!nb->switcher_open)
    nb_tab_ensure_view (t);
  nb_save_session ();
}

static gboolean
on_close_request (GtkWindow *win, gpointer data)
{
  /* the browser swiped away in Recents: its tabs are kept for the next start (the session, written now), their
   * pages end at once (the shell kills the pid of a window that stays) */
  nb_save_session_now ();
  nb->quitting = TRUE;
  for (guint i = 0; i < nb->tabs->len; i++) {
    NbTab *t = nb->tabs->pdata[i];
    if (t->view)
      webkit_web_view_terminate_web_process (t->view);
  }
  return FALSE;
}

static void
on_window_destroy (GtkWidget *win, gpointer data)
{
  g_clear_handle_id (&rebuild_id, g_source_remove);
  nb->win = nb->root = nb->tab_stack = NULL;
  nb->switcher = nb->switcher_grid = nb->switcher_title = nb->switcher_empty = NULL;
  switcher_scroll = NULL;
  nb->switcher_open = FALSE;
}

gboolean
nb_is_tablet (void)
{
  VibeosLayout *layout = nb->win ? vibeos_layout_get (nb->win) : NULL;

  return layout && vibeos_layout_is_tablet (layout);
}

/* turned, or a display-size change: the switcher's columns, and each page's user agent and zoom (a user agent
 * changed takes effect with the page's next load) */
static void
on_layout_changed (VibeosLayout *layout, gpointer data)
{
  for (guint i = 0; i < nb->tabs->len; i++)
    nb_apply_view_settings (nb->tabs->pdata[i]);
  queue_rebuild ();
}

GtkWidget *
nb_window_get (void)
{
  if (nb->win)
    return nb->win;
  nb->quitting = FALSE;
  nb->win = vibeos_window_new (nb->gapp);
  gtk_widget_add_css_class (nb->win, "nb");
  gtk_window_set_title (GTK_WINDOW (nb->win), "Browser");
  nb->root = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (nb->root), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (nb->root), 150);
  nb->tab_stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (nb->root), nb->tab_stack, "tabs");
  nb->switcher = switcher_new ();
  gtk_stack_add_named (GTK_STACK (nb->root), nb->switcher, "switcher");
  gtk_stack_set_visible_child_name (GTK_STACK (nb->root), "tabs");
  vibeos_window_set_content (GTK_WINDOW (nb->win), nb->root);
  g_signal_connect (nb->win, "notify::is-active", G_CALLBACK (on_active), NULL);
  g_signal_connect (nb->win, "close-request", G_CALLBACK (on_close_request), NULL);
  g_signal_connect (nb->win, "destroy", G_CALLBACK (on_window_destroy), NULL);
  g_signal_connect_object (vibeos_layout_get_for_window (GTK_WINDOW (nb->win)), "changed",
                           G_CALLBACK (on_layout_changed), nb->win, 0);
  return nb->win;
}
