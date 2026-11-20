/* Call history: see call-history.h. */
#include "call-history.h"
#include "call-stats.h"
#include "calls-client.h"
#include "popup-menu.h"
#include "recents-page.h"

#include <glib/gi18n.h>
#include <math.h>

#define BAR_HEIGHT      56      /* the action bar */
#define UP_SIZE         56      /* the Up button, at the start */
#define TITLE_START     72      /* the title's start, past Up */
#define OVERFLOW_WIDTH  40      /* the ⋮, flush with the end */
#define OVERFLOW_HEIGHT 48
#define TAB_HEIGHT      48
#define TAB_PAD         10      /* a tab label's side padding */
#define UNDERLINE       2       /* under the selected tab */
#define UNDERLINE_COLOR VIBEOS_PRIMARY  /* the theme's, read as the strip draws */
#define PAGE_MS         200     /* to the next page, from rest: (1 + 1) × 100 ms */
#define FLING_VELOCITY  400     /* the slowest fling that turns the page, px/s */

enum { PAGE_ALL, PAGE_MISSED, PAGE_STATS, N_PAGES };

/* ---- the tab strip: the tabs, and the underline under the selected one -------------------------------------- */

#define HISTORY_TYPE_TABS (history_tabs_get_type ())
G_DECLARE_FINAL_TYPE (HistoryTabs, history_tabs, HISTORY, TABS, GtkWidget)

struct _HistoryTabs
{
  GtkWidget parent;
  GtkWidget *tabs[N_PAGES];     /* the pressables; their labels carry "selected" */
  VibeosAnim *anim;
  double position;              /* the underline's page: between two while the pages slide */
};

G_DEFINE_FINAL_TYPE (HistoryTabs, history_tabs, GTK_TYPE_WIDGET)

enum { TABS_SELECTED, N_TABS_SIGNALS };
static guint tabs_signals[N_TABS_SIGNALS];

/* the underline spans the selected tab, both its ends moving toward the next tab's
 * while the pager scrolls */
static void
history_tabs_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  HistoryTabs *self = HISTORY_TABS (widget);
  int from = CLAMP ((int) floor (self->position), 0, N_PAGES - 1), to = MIN (from + 1, N_PAGES - 1);
  double t = self->position - from;
  GdkRGBA color = vibeos_color (UNDERLINE_COLOR);
  graphene_rect_t a, b;
  double left, right;

  GTK_WIDGET_CLASS (history_tabs_parent_class)->snapshot (widget, snapshot);
  if (!gtk_widget_compute_bounds (self->tabs[from], widget, &a)
      || !gtk_widget_compute_bounds (self->tabs[to], widget, &b))
    return;
  left = a.origin.x + (b.origin.x - a.origin.x) * t;
  right = a.origin.x + a.size.width + (b.origin.x + b.size.width - a.origin.x - a.size.width) * t;
  gtk_snapshot_append_color (snapshot, &color,
                             &GRAPHENE_RECT_INIT (left, gtk_widget_get_height (widget) - UNDERLINE, right - left,
                                                  UNDERLINE));
}

static void
on_tabs_anim (VibeosAnim *anim, double value, gpointer data)
{
  HistoryTabs *self = data;

  self->position = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* the selected tab's text takes the accent colour at once; the underline follows the page as it slides */
static void
history_tabs_select (HistoryTabs *self, int page)
{
  for (int i = 0; i < N_PAGES; i++)
    {
      GtkWidget *label = vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->tabs[i]));

      if (i == page)
        gtk_widget_add_css_class (label, "selected");
      else
        gtk_widget_remove_css_class (label, "selected");
    }
  vibeos_anim_start (self->anim, self->position, page, PAGE_MS, 0, SPONGY_DECELERATE_QUINT);
}

static void
on_tab_clicked (GtkWidget *tab, HistoryTabs *self)
{
  g_signal_emit (self, tabs_signals[TABS_SELECTED], 0, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tab), "page")));
}

static void
history_tabs_dispose (GObject *object)
{
  HistoryTabs *self = HISTORY_TABS (object);

  g_clear_pointer (&self->anim, vibeos_anim_free);
  for (int i = 0; i < N_PAGES; i++)
    g_clear_pointer (&self->tabs[i], gtk_widget_unparent);
  G_OBJECT_CLASS (history_tabs_parent_class)->dispose (object);
}

static void
history_tabs_class_init (HistoryTabsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = history_tabs_dispose;
  widget_class->snapshot = history_tabs_snapshot;
  /* the tabs: each its text and padding, plus an equal share of what is left over
   * (so they are not equally wide), which is how a box shares out room among children that expand */
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "history-tabs");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_TAB_LIST);
  tabs_signals[TABS_SELECTED] = g_signal_new ("selected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                              NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
history_tabs_init (HistoryTabs *self)
{
  const char *titles[N_PAGES] = { _("All"), _("Missed"), _("Statistics") };

  gtk_widget_set_size_request (GTK_WIDGET (self), -1, TAB_HEIGHT);
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_tabs_anim, self);
  for (int i = 0; i < N_PAGES; i++)
    {
      GtkWidget *label = gtk_label_new (titles[i]);

      gtk_widget_add_css_class (label, "history-tab");
      gtk_widget_set_margin_start (label, TAB_PAD);
      gtk_widget_set_margin_end (label, TAB_PAD);
      /* a bounded ripple on each tab */
      self->tabs[i] = vibeos_pressable_new (label);
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->tabs[i]), VIBEOS_RIPPLE_BOUNDED, 0);
      gtk_widget_set_hexpand (self->tabs[i], TRUE);
      g_object_set_data (G_OBJECT (self->tabs[i]), "page", GINT_TO_POINTER (i));
      g_signal_connect (self->tabs[i], "clicked", G_CALLBACK (on_tab_clicked), self);
      gtk_widget_set_parent (self->tabs[i], GTK_WIDGET (self));
    }
  history_tabs_select (self, PAGE_ALL);
  vibeos_anim_finish (self->anim);
}

/* ---- the screen ---------------------------------------------------------------------------------------------- */

struct _CallHistory
{
  GtkWidget parent;
  GtkWidget *box;               /* the bar, the tabs, the pages */
  GtkWidget *overflow;
  HistoryTabs *tabs;
  GtkWidget *stack, *pages[N_PAGES];
  int page;
};

G_DEFINE_FINAL_TYPE (CallHistory, call_history, GTK_TYPE_WIDGET)

enum { CLOSED, EDIT_NUMBER, SHOW_DETAILS, SHOW_SCREEN, N_SIGNALS };
static guint signals[N_SIGNALS];

static void update_overflow (CallHistory *self);

static void
show_page (CallHistory *self, int page)
{
  if (page < 0 || page >= N_PAGES || page == self->page)
    return;
  /* a page picked: going to All or Missed marks the missed calls seen and takes their notification away (only a
   * change of page does; opening the screen does not); going to Statistics does not */
  if (page != PAGE_STATS)
    calls_client_mark_missed_read (calls_client_get ());
  gtk_stack_set_transition_type (GTK_STACK (self->stack), page > self->page ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
                                                                            : GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
  self->page = page;
  gtk_stack_set_visible_child (GTK_STACK (self->stack), self->pages[page]);
  history_tabs_select (self->tabs, page);
  update_overflow (self);
}

static void
on_tab_selected (HistoryTabs *tabs, int page, CallHistory *self)
{
  show_page (self, page);
}

/* a fling sideways goes to the page next to this one (the lists give a sideways drag up, spongy-gtk.c) */
static void
on_swipe (GtkGestureSwipe *swipe, double vx, double vy, CallHistory *self)
{
  if (fabs (vx) < FLING_VELOCITY || fabs (vx) < 2 * fabs (vy))
    return;
  show_page (self, self->page + (vx < 0 ? 1 : -1));
}

/* Up starts the main screen again: this one goes */
static void
on_up (CallHistory *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CallHistory *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  g_signal_emit (self, signals[CLOSED], 0);
  return TRUE;
}

static void
on_clear_response (VibeosDialog *dialog, int response, gpointer data)
{
  /* OK: the missed-call notifications go, then every row (the service does both). The "Clearing call history…"
   * progress dialog is left out: it is one DELETE here, and would only flash */
  if (response == VIBEOS_RESPONSE_OK)
    calls_client_clear_call_log (calls_client_get ());
}

/* "Clear call history?" */
static void
on_clear (gpointer data)
{
  CallHistory *self = data;
  VibeosDialog *dialog = vibeos_dialog_new (_("Clear call history?"));

  vibeos_dialog_set_message (dialog, _("This will delete all calls from your history"));
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_clear_response), NULL);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_sort (gpointer data)
{
  CallHistory *self = data;
  CallStats *stats = CALL_STATS (self->pages[PAGE_STATS]);

  call_stats_set_sort_by_duration (stats, !call_stats_get_sort_by_duration (stats));
}

static gboolean
has_calls (CallHistory *self)
{
  return recents_page_has_calls (RECENTS_PAGE (self->pages[PAGE_ALL]));
}

/* the ⋮ menu: on Statistics first the sort it is not sorted by, then "Clear call history" while there are calls.
 * "Adjust time range" and "Reset time range" are left out (no time range) */
static void
on_overflow (CallHistory *self)
{
  DialerPopupMenu *menu = dialer_popup_menu_new ();

  if (self->page == PAGE_STATS)
    dialer_popup_menu_append (menu, call_stats_get_sort_by_duration (CALL_STATS (self->pages[PAGE_STATS]))
                                      ? _("Sort by call count") : _("Sort by call duration"), on_sort, self);
  if (has_calls (self))
    dialer_popup_menu_append (menu, _("Clear call history"), on_clear, self);
  dialer_popup_menu_present (menu, self->overflow, POPUP_MENU_OVERFLOW);
}

/* "Clear call history" only while the log has calls; Statistics always has its sort; with no
 * item there is no overflow */
static void
update_overflow (CallHistory *self)
{
  gtk_widget_set_visible (self->overflow, self->page == PAGE_STATS || has_calls (self));
}

static void
on_edit_number (CallHistory *self, const char *number)
{
  g_signal_emit (self, signals[EDIT_NUMBER], 0, number);
}

static void
on_show_details (CallHistory *self, const char *number, int presentation, GArray *ids)
{
  g_signal_emit (self, signals[SHOW_DETAILS], 0, number, presentation, ids);
}

static void
on_show_screen (CallHistory *self, GtkWidget *screen)
{
  g_signal_emit (self, signals[SHOW_SCREEN], 0, screen);
}

static void
call_history_dispose (GObject *object)
{
  CallHistory *self = CALL_HISTORY (object);

  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (call_history_parent_class)->dispose (object);
}

static void
call_history_class_init (CallHistoryClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = call_history_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "call-history");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[EDIT_NUMBER] = g_signal_new ("edit-number", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SHOW_DETAILS] = g_signal_new ("show-details", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_ARRAY);
  signals[SHOW_SCREEN] = g_signal_new ("show-screen", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 1, GTK_TYPE_WIDGET);
}

static void
call_history_init (CallHistory *self)
{
}

static GtkWidget *
make_bar (CallHistory *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *up = vibeos_icon_button_new ("dialer-back-symbolic");
  GtkWidget *title = gtk_label_new (_("Call History"));

  gtk_widget_add_css_class (bar, "dialer-history-bar");
  gtk_widget_set_size_request (bar, -1, BAR_HEIGHT);
  gtk_widget_set_size_request (up, UP_SIZE, UP_SIZE);
  gtk_widget_set_valign (up, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (up, "dialer-history-icon");
  /* named "Back", the button the shell's back gesture clicks; "Navigate up" is its description */
  gtk_accessible_update_property (GTK_ACCESSIBLE (up), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Navigate up"), -1);
  g_signal_connect_swapped (up, "clicked", G_CALLBACK (on_up), self);
  gtk_box_append (GTK_BOX (bar), up);

  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_set_margin_start (title, TITLE_START - UP_SIZE);
  gtk_widget_add_css_class (title, "dialer-history-title");
  gtk_box_append (GTK_BOX (bar), title);

  self->overflow = vibeos_icon_button_new ("dialer-more-symbolic");
  gtk_widget_set_size_request (self->overflow, OVERFLOW_WIDTH, OVERFLOW_HEIGHT);
  gtk_widget_set_valign (self->overflow, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (self->overflow, "dialer-history-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->overflow), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"),
                                  -1);
  gtk_widget_set_visible (self->overflow, FALSE);
  g_signal_connect_swapped (self->overflow, "clicked", G_CALLBACK (on_overflow), self);
  gtk_box_append (GTK_BOX (bar), self->overflow);
  return bar;
}

GtkWidget *
call_history_new (Contacts *contacts, VibeosSnackbar *snackbar)
{
  CallHistory *self = g_object_new (CALL_TYPE_HISTORY, NULL);
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkGesture *swipe = gtk_gesture_swipe_new ();

  gtk_widget_add_css_class (GTK_WIDGET (self), "dialer-history");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_box_append (GTK_BOX (self->box), make_bar (self));

  self->tabs = g_object_new (HISTORY_TYPE_TABS, NULL);
  g_signal_connect (self->tabs, "selected", G_CALLBACK (on_tab_selected), self);
  gtk_box_append (GTK_BOX (self->box), GTK_WIDGET (self->tabs));

  self->stack = gtk_stack_new ();
  gtk_stack_set_transition_duration (GTK_STACK (self->stack), PAGE_MS);
  gtk_widget_set_vexpand (self->stack, TRUE);
  for (int i = 0; i < N_PAGES; i++)
    {
      if (i == PAGE_STATS)
        {
          self->pages[i] = call_stats_new (contacts, snackbar);
          g_signal_connect_swapped (self->pages[i], "show-screen", G_CALLBACK (on_show_screen), self);
        }
      else
        {
          self->pages[i] = recents_page_new_history (contacts, snackbar, i == PAGE_MISSED);
          g_signal_connect_swapped (self->pages[i], "show-details", G_CALLBACK (on_show_details), self);
        }
      g_signal_connect_swapped (self->pages[i], "edit-number", G_CALLBACK (on_edit_number), self);
      gtk_stack_add_child (GTK_STACK (self->stack), self->pages[i]);
    }
  g_signal_connect_swapped (self->pages[PAGE_ALL], "loaded", G_CALLBACK (update_overflow), self);
  gtk_box_append (GTK_BOX (self->box), self->stack);
  self->page = PAGE_ALL;

  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (swipe), FALSE);
  g_signal_connect (swipe, "swipe", G_CALLBACK (on_swipe), self);
  gtk_widget_add_controller (self->stack, GTK_EVENT_CONTROLLER (swipe));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  return GTK_WIDGET (self);
}
