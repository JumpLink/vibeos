/* The Contacts window (ct-window.h). */
#include "ct-window.h"
#include "ct-editor.h"
#include "ct-page.h"
#include "ct-people.h"
#include "ct-popup.h"
#include "ct-quick.h"
#include "ct-tabs.h"
#include "ct-ui.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

/* ===== CtPager: the pages, the top ones drawn, sliding as activities do ======================================== */

#define CT_TYPE_PAGER (ct_pager_get_type ())
G_DECLARE_FINAL_TYPE (CtPager, ct_pager, CT, PAGER, GtkWidget)

typedef struct {
  GtkWidget *page;
  double x;                     /* the slide, px */
  double alpha;
} Placed;

struct _CtPager {
  GtkWidget parent;
  GArray *pages;                /* Placed, bottom first */
  VibeosAnim *anim;
  GtkWidget *entering, *exiting;
  gboolean closing;
};

G_DEFINE_FINAL_TYPE (CtPager, ct_pager, GTK_TYPE_WIDGET)

static Placed *
placed_of (CtPager *self, GtkWidget *page)
{
  for (guint i = 0; i < self->pages->len; i++)
    if (g_array_index (self->pages, Placed, i).page == page)
      return &g_array_index (self->pages, Placed, i);
  return NULL;
}

/* the lowest page that shows: the top one, the ones under it as far down as the pages above are translucent, and
 * during a slide both the page that comes and the one that goes */
static guint
lowest_shown (CtPager *self)
{
  guint lowest = self->pages->len ? self->pages->len - 1 : 0;

  while (lowest > 0 && ct_page_get_translucent (CT_PAGE (g_array_index (self->pages, Placed, lowest).page)))
    lowest--;
  for (guint i = 0; i < lowest; i++)
    {
      GtkWidget *page = g_array_index (self->pages, Placed, i).page;

      if (page == self->entering || page == self->exiting)
        return i;
    }
  return lowest;
}

static void
update_pages (CtPager *self)
{
  guint lowest = lowest_shown (self);

  for (guint i = 0; i < self->pages->len; i++)
    {
      Placed *placed = &g_array_index (self->pages, Placed, i);
      gboolean top = i == self->pages->len - 1;

      gtk_widget_set_child_visible (placed->page, i >= lowest);
      /* only the top page takes touches, and not while it slides */
      gtk_widget_set_can_target (placed->page, top && !vibeos_anim_is_running (self->anim));
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
ct_pager_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
ct_pager_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtPager *self = CT_PAGER (widget);

  for (guint i = 0; i < self->pages->len; i++)
    {
      Placed *placed = &g_array_index (self->pages, Placed, i);

      if (gtk_widget_get_child_visible (placed->page))
        gtk_widget_allocate (placed->page, width, height, -1,
                             gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) round (placed->x), 0)));
    }
}

static void
ct_pager_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtPager *self = CT_PAGER (widget);

  for (guint i = 0; i < self->pages->len; i++)
    {
      Placed *placed = &g_array_index (self->pages, Placed, i);

      if (!gtk_widget_get_child_visible (placed->page) || placed->alpha <= 0)
        continue;
      gtk_snapshot_push_opacity (snapshot, placed->alpha);
      gtk_widget_snapshot_child (widget, placed->page, snapshot);
      gtk_snapshot_pop (snapshot);
    }
}

/* a page opening or closing: 450 ms of the emphasized curve for the slides, the fades linear in their windows */
static void
on_slide (VibeosAnim *anim, double t, gpointer data)
{
  CtPager *self = data;
  double ms = t * CT_ACTIVITY_ANIM_MS;
  double slide = spongy_curve (SPONGY_EMPHASIZED, (float) t) * CT_ACTIVITY_SLIDE_DP;
  Placed *entering = self->entering ? placed_of (self, self->entering) : NULL;
  Placed *exiting = self->exiting ? placed_of (self, self->exiting) : NULL;

  if (!self->closing)
    {
      if (entering)
        {
          entering->x = CT_ACTIVITY_SLIDE_DP - slide;
          entering->alpha = CLAMP ((ms - CT_ACTIVITY_OPEN_FADE_DELAY_MS) / CT_ACTIVITY_FADE_MS, 0, 1);
        }
      if (exiting)
        exiting->x = -slide;
    }
  else
    {
      if (entering)
        entering->x = slide - CT_ACTIVITY_SLIDE_DP;
      if (exiting)
        {
          exiting->x = slide;
          exiting->alpha = 1 - CLAMP ((ms - CT_ACTIVITY_CLOSE_FADE_DELAY_MS) / CT_ACTIVITY_FADE_MS, 0, 1);
        }
    }
  if (t >= 1)
    {
      GtkWidget *gone = self->closing ? self->exiting : NULL;

      if (entering)
        entering->x = 0, entering->alpha = 1;
      if (exiting)
        exiting->x = 0, exiting->alpha = 1;
      self->entering = self->exiting = NULL;
      if (gone)
        {
          for (guint i = 0; i < self->pages->len; i++)
            if (g_array_index (self->pages, Placed, i).page == gone)
              g_array_remove_index (self->pages, i);
          gtk_widget_unparent (gone);
        }
    }
  update_pages (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
ct_pager_dispose (GObject *object)
{
  CtPager *self = CT_PAGER (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  if (self->pages)
    for (guint i = 0; i < self->pages->len; i++)
      gtk_widget_unparent (g_array_index (self->pages, Placed, i).page);
  g_clear_pointer (&self->pages, g_array_unref);
  G_OBJECT_CLASS (ct_pager_parent_class)->dispose (object);
}

static void
ct_pager_class_init (CtPagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ct_pager_dispose;
  widget_class->measure = ct_pager_measure;
  widget_class->size_allocate = ct_pager_size_allocate;
  widget_class->snapshot = ct_pager_snapshot;
  gtk_widget_class_set_css_name (widget_class, "ct-pager");
}

static void
ct_pager_init (CtPager *self)
{
  self->pages = g_array_new (FALSE, TRUE, sizeof (Placed));
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_slide, self);
}

static GtkWidget *
pager_top (CtPager *self)
{
  return self->pages->len ? g_array_index (self->pages, Placed, self->pages->len - 1).page : NULL;
}

/* a slide still running ends where it was going before the next one starts */
static void
finish_slide (CtPager *self)
{
  if (vibeos_anim_is_running (self->anim))
    vibeos_anim_finish (self->anim);
}

static void
pager_push (CtPager *self, GtkWidget *page)
{
  Placed placed = { .page = page, .x = 0, .alpha = 1 };
  GtkWidget *below;

  finish_slide (self);
  below = pager_top (self);
  gtk_widget_set_parent (page, GTK_WIDGET (self));
  g_array_append_val (self->pages, placed);
  if (below && !ct_page_get_translucent (CT_PAGE (page)))
    {
      self->closing = FALSE;
      self->entering = page;
      self->exiting = below;
      vibeos_anim_start (self->anim, 0, 1, CT_ACTIVITY_ANIM_MS, 0, SPONGY_LINEAR);
    }
  update_pages (self);
}

static void
pager_pop (CtPager *self, GtkWidget *page)
{
  gboolean slide;
  guint index;

  finish_slide (self);
  for (index = 0; index < self->pages->len; index++)
    if (g_array_index (self->pages, Placed, index).page == page)
      break;
  if (index == 0 || index >= self->pages->len)
    return;                     /* not here, or the list, which never goes */
  /* the pages above it go with it, at once */
  while (self->pages->len > index + 1)
    {
      GtkWidget *above = pager_top (self);

      g_array_remove_index (self->pages, self->pages->len - 1);
      gtk_widget_unparent (above);
    }
  slide = !ct_page_get_translucent (CT_PAGE (page));
  if (slide)
    {
      self->closing = TRUE;
      self->exiting = page;
      self->entering = g_array_index (self->pages, Placed, index - 1).page;
      vibeos_anim_start (self->anim, 0, 1, CT_ACTIVITY_ANIM_MS, 0, SPONGY_LINEAR);
    }
  else
    {
      g_array_remove_index (self->pages, index);
      gtk_widget_unparent (page);
    }
  update_pages (self);
}

/* ===== the window ============================================================================================= */

typedef struct {
  GtkWidget *window;
  CtStore *store;
  GSettings *settings;
  CtPager *pager;
  GtkWidget *home, *tabs, *list_detail, *detail_empty, *people, *highlights, *organize;
  GtkWidget *nav_bar, *nav_rail, *rail_fab, *back_marker;
  GtkWidget *area;              /* beside the rail (a tablet): the pages, and a search over them */
  VibeosLayout *layout;
  GtkWidget *toast, *toast_label;
  VibeosAnim *toast_anim;
  guint toast_timeout;
  GtkWidget *snackbar;
} Shell;

static Shell *
shell_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (gtk_widget_get_root (window)), "ct-shell");
}

CtStore *
ct_window_get_store (GtkWidget *window)
{
  return shell_of (window)->store;
}

GSettings *
ct_window_get_settings (GtkWidget *window)
{
  return shell_of (window)->settings;
}

void
ct_window_push (GtkWidget *window, GtkWidget *page)
{
  Shell *shell = shell_of (window);

  ct_popup_dismiss (window);
  /* a page opens with nothing focused, as an activity does unless it asks (no keyboard coming up uninvited) */
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  pager_push (shell->pager, page);
}

void
ct_window_pop (GtkWidget *window, GtkWidget *page)
{
  Shell *shell = shell_of (window);

  ct_popup_dismiss (window);
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  /* a contact beside the list: the pane goes back to empty */
  if (g_object_get_data (G_OBJECT (page), "ct-detail"))
    {
      if (gtk_widget_get_parent (page))
        vibeos_list_detail_close_detail (VIBEOS_LIST_DETAIL (shell->list_detail));
      return;
    }
  pager_pop (shell->pager, page);
}

GtkWidget *
ct_window_top (GtkWidget *window)
{
  return pager_top (shell_of (window)->pager);
}

GtkWidget *
ct_window_home (GtkWidget *window)
{
  return shell_of (window)->home;
}

GtkOverlay *
ct_window_search_overlay (GtkWidget *window)
{
  Shell *shell = shell_of (window);

  if (vibeos_layout_is_tablet (shell->layout))
    return GTK_OVERLAY (shell->area);
  return vibeos_window_get_overlay (shell->window);
}

GtkWidget *
ct_window_get_people (GtkWidget *window)
{
  return shell_of (window)->people;
}

static int
current_tab (Shell *shell)
{
  GtkWidget *child = gtk_stack_get_visible_child (GTK_STACK (shell->tabs));

  return child == shell->highlights ? 1 : child == shell->organize ? 2 : 0;
}

/* beside the list when the Contacts tab is two panes; else over everything, as a sheet */
void
ct_window_show_contact (GtkWidget *window, const char *uid)
{
  Shell *shell = shell_of (window);
  gboolean beside = vibeos_list_detail_is_split (VIBEOS_LIST_DETAIL (shell->list_detail)) && current_tab (shell) == 0;
  GtkWidget *page = beside ? ct_quick_new_embedded (window, uid) : ct_quick_new (window, uid);

  if (!page)
    return;
  if (g_strcmp0 (uid, CT_PROFILE_UID) != 0)
    ct_recents_add (shell->settings, uid);
  if (!beside)
    {
      ct_window_push (window, page);
      return;
    }
  ct_popup_dismiss (window);
  g_object_set_data (G_OBJECT (page), "ct-detail", GINT_TO_POINTER (TRUE));
  vibeos_list_detail_show (VIBEOS_LIST_DETAIL (shell->list_detail), page);
  ct_people_set_current (shell->people, uid);
}

void
ct_window_edit_contact (GtkWidget *window, const char *uid)
{
  ct_window_push (window, ct_editor_new (window, uid, NULL));
}

void
ct_window_new_contact (GtkWidget *window, EContact *prefill)
{
  ct_window_push (window, ct_editor_new (window, NULL, prefill));
}

/* --- the toast: the app's icon and the text in a pill near the bottom */

#define TOAST_FADE_MS 250

static void
on_toast_anim (VibeosAnim *anim, double value, gpointer data)
{
  Shell *shell = data;

  gtk_widget_set_opacity (shell->toast, value);
  gtk_widget_set_visible (shell->toast, value > 0);
}

static gboolean
on_toast_timeout (gpointer data)
{
  Shell *shell = data;

  shell->toast_timeout = 0;
  vibeos_anim_start (shell->toast_anim, vibeos_anim_get_value (shell->toast_anim), 0, TOAST_FADE_MS, 0, SPONGY_LINEAR);
  return G_SOURCE_REMOVE;
}

void
ct_window_toast (GtkWidget *window, const char *text)
{
  Shell *shell = shell_of (window);

  gtk_label_set_label (GTK_LABEL (shell->toast_label), text);
  g_clear_handle_id (&shell->toast_timeout, g_source_remove);
  vibeos_anim_start (shell->toast_anim, vibeos_anim_get_value (shell->toast_anim), 1, TOAST_FADE_MS, 0, SPONGY_LINEAR);
  shell->toast_timeout = g_timeout_add (TOAST_FADE_MS + CT_TOAST_SHORT_MS, on_toast_timeout, shell);
}

static GtkWidget *
make_toast (Shell *shell)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("vibeos.Contacts");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "ct-toast-icon");
  gtk_box_append (GTK_BOX (box), icon);
  shell->toast_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (shell->toast_label), TRUE);
  gtk_label_set_lines (GTK_LABEL (shell->toast_label), 2);
  gtk_label_set_ellipsize (GTK_LABEL (shell->toast_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (shell->toast_label), 30);
  gtk_label_set_xalign (GTK_LABEL (shell->toast_label), 0);
  gtk_widget_add_css_class (shell->toast_label, "ct-toast-text");
  gtk_box_append (GTK_BOX (box), shell->toast_label);
  gtk_widget_add_css_class (box, "ct-toast");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_END);
  gtk_widget_set_can_target (box, FALSE);
  gtk_widget_set_visible (box, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Message"), -1);
  shell->toast = box;
  shell->toast_anim = vibeos_anim_new (box, on_toast_anim, shell);
  return box;
}

void
ct_window_snackbar (GtkWidget *window, const char *text, const char *action, VibeosSnackbarAction callback,
                    gpointer data)
{
  vibeos_snackbar_show (VIBEOS_SNACKBAR (shell_of (window)->snackbar), text, action, callback, data, VIBEOS_SNACKBAR_LONG);
}

VibeosSnackbar *
ct_window_get_snackbar (GtkWidget *window)
{
  return VIBEOS_SNACKBAR (shell_of (window)->snackbar);
}

/* --- Back: a menu first, then the top page (which may ask, or leave search or a drawer first), then the window */

static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  Shell *shell = data;
  GtkWidget *top = pager_top (shell->pager);
  VibeosListDetail *ld = VIBEOS_LIST_DETAIL (shell->list_detail);

  if (ct_popup_dismiss (shell->window))
    return TRUE;
  if (top != shell->home)
    ct_page_back (CT_PAGE (top));
  /* on the home page: the contact beside the list, then the list's own Back, then the first tab, then out */
  else if (current_tab (shell) == 0 && vibeos_list_detail_has_detail (ld))
    {
      GtkWidget *detail = vibeos_page_stack_get_top (vibeos_list_detail_get_detail_stack (ld));

      if (CT_IS_PAGE (detail))
        ct_page_back (CT_PAGE (detail));
      else
        vibeos_list_detail_back (ld);
    }
  else if (current_tab (shell) == 0 && ct_people_back (shell->people))
    ;
  else if (current_tab (shell) != 0)
    ct_window_select_tab (shell->window, 0);
  else
    gtk_window_close (GTK_WINDOW (shell->window));
  return TRUE;
}

/* ===== the home page: the tabs, the navigation bar or rail ===================================================== */

void
ct_window_select_tab (GtkWidget *window, int tab)
{
  Shell *shell = shell_of (window);
  GtkWidget *child = tab == 1 ? shell->highlights : tab == 2 ? shell->organize : shell->list_detail;

  gtk_stack_set_visible_child (GTK_STACK (shell->tabs), child);
  vibeos_nav_bar_set_selected (VIBEOS_NAV_BAR (shell->nav_bar), tab);
  vibeos_nav_rail_set_selected (VIBEOS_NAV_RAIL (shell->nav_rail), tab);
  /* the back gesture leaves the app unless Back has something to do: here, back to the first tab */
  gtk_widget_set_visible (shell->back_marker, tab != 0);
}

static void
on_nav_selected (GtkWidget *nav, int index, Shell *shell)
{
  ct_window_select_tab (shell->window, index);
}

/* A rail destination tapped over pages (on a tablet the rail stays beside them): back to the home page, and TRUE.
 * An editor among them (a contact's, or Customize view's) is not thrown away: the top page gets a Back, which asks
 * when there are changes to lose, and FALSE */
static gboolean
close_pages (Shell *shell)
{
  CtPager *pager = shell->pager;

  if (pager->pages->len < 2)
    return TRUE;
  for (guint i = 1; i < pager->pages->len; i++)
    {
      GObject *page = G_OBJECT (g_array_index (pager->pages, Placed, i).page);

      if (g_object_get_data (page, "ct-editor") || g_object_get_data (page, "ct-customize"))
        {
          ct_page_back (CT_PAGE (pager_top (pager)));
          return FALSE;
        }
    }
  ct_popup_dismiss (shell->window);
  gtk_window_set_focus (GTK_WINDOW (shell->window), NULL);
  pager_pop (pager, g_array_index (pager->pages, Placed, 1).page);
  return TRUE;
}

/* the rail's destinations, the one showing too: the search and the pages beside the rail close first */
static void
on_rail_selected (VibeosNavRail *rail, int index, Shell *shell)
{
  ct_people_close_search (shell->people);
  if (close_pages (shell))
    ct_window_select_tab (shell->window, index);
  else
    vibeos_nav_rail_set_selected (rail, current_tab (shell));
}

static void
on_rail_fab (VibeosPressable *fab, Shell *shell)
{
  ct_window_new_contact (shell->window, NULL);
}

/* the list pane on a tablet: about a third of the width, as wide as a phone at least */
static void
update_layout (Shell *shell)
{
  gboolean tablet = vibeos_layout_is_tablet (shell->layout);
  int width = vibeos_layout_get_width (shell->layout) - (tablet ? CT_RAIL_W_DP : 0);

  gtk_widget_set_visible (shell->nav_bar, !tablet);
  gtk_widget_set_visible (shell->nav_rail, tablet);
  vibeos_nav_bar_set_horizontal_items (VIBEOS_NAV_BAR (shell->nav_bar), vibeos_layout_is_landscape (shell->layout));
  ct_people_set_fab_shown (shell->people, !tablet);
  vibeos_list_detail_set_list_width (VIBEOS_LIST_DETAIL (shell->list_detail),
                                     (int) CLAMP (width * CT_LIST_PANE_RATIO, CT_LIST_PANE_MIN_DP,
                                                  CT_LIST_PANE_MAX_DP));
}

static void
on_layout_changed (VibeosLayout *layout, Shell *shell)
{
  update_layout (shell);
}

/* one pane again (a display-size change): the contact open beside the list closes rather than covering it */
static void
on_split (GObject *ld, GParamSpec *pspec, Shell *shell)
{
  gboolean split = vibeos_list_detail_is_split (VIBEOS_LIST_DETAIL (ld));

  /* the placeholder's picture and line only with a side to show them on: one pane gives it no width */
  gtk_widget_set_visible (shell->detail_empty, split);
  if (!split)
    vibeos_list_detail_close_detail (VIBEOS_LIST_DETAIL (ld));
}

static void
on_detail_closed (VibeosListDetail *ld, Shell *shell)
{
  ct_people_set_current (shell->people, NULL);
}

static void
on_theme_changed (VibeosTheme *theme, Shell *shell)
{
  ct_fab_update (shell->rail_fab);
}

static GtkWidget *
make_home (Shell *shell)
{
  GtkWidget *home = ct_page_new (shell->window);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *placeholder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  const char *icons[] = { "ct-person-symbolic", "ct-star-border-symbolic", "ct-organize-symbolic" };
  const char *labels[] = { _("Contacts"), _("Highlights"), _("Organize") };

  gtk_widget_add_css_class (home, "ct-home");
  shell->home = home;
  shell->people = ct_people_new (shell->window);
  shell->list_detail = vibeos_list_detail_new ();
  vibeos_list_detail_set_list (VIBEOS_LIST_DETAIL (shell->list_detail), shell->people);
  gtk_widget_add_css_class (placeholder, "ct-detail-empty");
  shell->detail_empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (shell->detail_empty, TRUE);
  gtk_widget_set_valign (shell->detail_empty, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (shell->detail_empty, FALSE);
  GtkWidget *empty_icon = gtk_image_new_from_icon_name ("ct-person-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (empty_icon), CT_DETAIL_EMPTY_ICON_DP);
  gtk_widget_add_css_class (empty_icon, "ct-detail-empty-icon");
  gtk_box_append (GTK_BOX (shell->detail_empty), empty_icon);
  GtkWidget *empty_text = ct_text (_("Pick a contact to see it here"), "ct-detail-empty-text");
  gtk_label_set_xalign (GTK_LABEL (empty_text), 0.5);
  gtk_label_set_wrap (GTK_LABEL (empty_text), TRUE);
  gtk_label_set_justify (GTK_LABEL (empty_text), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (shell->detail_empty), empty_text);
  gtk_box_append (GTK_BOX (placeholder), shell->detail_empty);
  vibeos_list_detail_set_placeholder (VIBEOS_LIST_DETAIL (shell->list_detail), placeholder);
  g_signal_connect (shell->list_detail, "notify::split", G_CALLBACK (on_split), shell);
  g_signal_connect (shell->list_detail, "detail-closed", G_CALLBACK (on_detail_closed), shell);
  shell->highlights = ct_highlights_new (shell->window);
  shell->organize = ct_organize_new (shell->window);

  shell->tabs = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->tabs), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (shell->tabs), CT_TAB_FADE_MS);
  gtk_stack_add_child (GTK_STACK (shell->tabs), shell->list_detail);
  gtk_stack_add_child (GTK_STACK (shell->tabs), shell->highlights);
  gtk_stack_add_child (GTK_STACK (shell->tabs), shell->organize);
  gtk_widget_set_hexpand (shell->tabs, TRUE);
  gtk_widget_set_vexpand (shell->tabs, TRUE);

  shell->nav_rail = vibeos_nav_rail_new ();
  shell->rail_fab = ct_fab_new ();
  g_signal_connect (shell->rail_fab, "clicked", G_CALLBACK (on_rail_fab), shell);
  vibeos_nav_rail_set_header (VIBEOS_NAV_RAIL (shell->nav_rail), shell->rail_fab);
  shell->nav_bar = vibeos_nav_bar_new ();
  for (int i = 0; i < 3; i++)
    {
      vibeos_nav_bar_append (VIBEOS_NAV_BAR (shell->nav_bar), icons[i], labels[i]);
      vibeos_nav_rail_append (VIBEOS_NAV_RAIL (shell->nav_rail), icons[i], labels[i]);
    }
  g_signal_connect (shell->nav_bar, "selected", G_CALLBACK (on_nav_selected), shell);
  g_signal_connect (shell->nav_rail, "selected", G_CALLBACK (on_rail_selected), shell);
  g_signal_connect (shell->nav_rail, "reselected", G_CALLBACK (on_rail_selected), shell);

  /* the rail is the window's, beside the pages (ct_window_new): it stays when a page opens over the home page */
  gtk_box_append (GTK_BOX (row), shell->tabs);
  gtk_widget_set_vexpand (row, TRUE);
  gtk_box_append (GTK_BOX (home), row);
  gtk_box_append (GTK_BOX (home), shell->nav_bar);

  shell->back_marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (shell->back_marker), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Contacts"),
                                  -1);
  gtk_widget_set_can_target (shell->back_marker, FALSE);
  gtk_widget_set_visible (shell->back_marker, FALSE);
  gtk_box_append (GTK_BOX (home), shell->back_marker);
  return home;
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), shell);
  if (shell->layout)
    {
      g_signal_handlers_disconnect_by_data (shell->layout, shell);
      g_object_unref (shell->layout);
    }

  g_clear_handle_id (&shell->toast_timeout, g_source_remove);
  g_clear_pointer (&shell->toast_anim, vibeos_anim_free);
  g_object_unref (shell->store);
  g_object_unref (shell->settings);
  g_free (shell);
}

GtkWidget *
ct_window_new (GtkApplication *app, CtStore *store, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = vibeos_window_new (app);
  GtkWidget *root = gtk_overlay_new (), *row;
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();

  shell->window = window;
  shell->store = g_object_ref (store);
  shell->settings = g_object_ref (settings);
  g_object_set_data_full (G_OBJECT (window), "ct-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Contacts"));
  gtk_widget_add_css_class (window, "ct-window");

  /* the rail (a tablet) at the start, beside the pages: a page opens over the home page only, the rail staying */
  shell->pager = g_object_new (CT_TYPE_PAGER, NULL);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  shell->area = gtk_overlay_new ();
  /* a page sliding in or out stays off the rail */
  gtk_widget_set_overflow (shell->area, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_hexpand (shell->area, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (shell->area), GTK_WIDGET (shell->pager));
  gtk_box_append (GTK_BOX (row), shell->area);
  gtk_overlay_set_child (GTK_OVERLAY (root), row);
  /* the snackbar is edge to edge on a phone */
  shell->snackbar = vibeos_snackbar_new ();
  vibeos_snackbar_set_margin (VIBEOS_SNACKBAR (shell->snackbar), 0);
  gtk_widget_add_css_class (shell->snackbar, "ct-snackbar");
  gtk_overlay_add_overlay (GTK_OVERLAY (root), shell->snackbar);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), make_toast (shell));
  vibeos_window_set_content (GTK_WINDOW (window), root);

  pager_push (shell->pager, make_home (shell));
  gtk_box_prepend (GTK_BOX (row), shell->nav_rail);
  /* the window's own, which goes with the window: held until the shell goes */
  shell->layout = g_object_ref (vibeos_layout_get_for_window (GTK_WINDOW (window)));
  g_signal_connect (shell->layout, "changed", G_CALLBACK (on_layout_changed), shell);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), shell);
  update_layout (shell);
  ct_window_select_tab (window, 0);

  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);
  return window;
}
