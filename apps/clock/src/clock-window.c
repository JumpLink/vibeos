/* The Clock's window: see clock-window.h. Measured on still:clock-alarm: the app bar y 116…308 px, the title
 * 72…252 × 168…256, the FAB 408…672 × 1728…1992, the navigation bar y 2040…2280. */
#include "clock-window.h"
#include "alarm-page.h"
#include "clock-page.h"

#include <glib/gi18n.h>
#include <neo.h>

#define FAB_ROW       120       /* the FAB container: 88 dp and a 16 dp margin over and under it */
#define FAB_SHOW_MS   200       /* SHORT: the FAB's scale animations */

typedef enum { TAB_ALARM, TAB_CLOCK, N_TABS } Tab;

typedef struct
{
  GtkWidget *window;
  GtkWidget *title, *stack, *content, *fab, *nav;
  NeoSnackbar *snackbar;
  GtkWidget *pages[N_TABS];
  NeoAnim *fab_scale;
} Shell;

static const struct
{
  const char *name, *icon, *fab_icon;
} tabs[N_TABS] = {
  /* bottom_navigation_menu.xml, and each fragment's FAB (spec/clock.md §1.3) */
  [TAB_ALARM] = { N_("Alarm"), "clock-tab-alarm-symbolic", "clock-add-symbolic" },
  [TAB_CLOCK] = { N_("Clock"), "clock-tab-clock-symbolic", NULL },
};

static void
on_fab_scale (NeoAnim *anim, double value, gpointer data)
{
  Shell *shell = data;

  neo_pressable_set_scale (NEO_PRESSABLE (shell->fab), value);
}

static void
select_tab (Shell *shell, Tab tab)
{
  gboolean had_fab = neo_pressable_get_scale (NEO_PRESSABLE (shell->fab)) > 0;
  gboolean has_fab = tabs[tab].fab_icon != NULL;

  gtk_stack_set_visible_child (GTK_STACK (shell->stack), shell->pages[tab]);
  gtk_label_set_text (GTK_LABEL (shell->title), _(tabs[tab].name));
  if (has_fab)
    neo_fab_set_icon (NEO_PRESSABLE (shell->fab), tabs[tab].fab_icon);
  /* the same FAB on both tabs: only its icon changes (FAB_AND_BUTTONS_IMMEDIATE); appearing or going it scales over
   * 200 ms (mShowAnimation, mHideAnimation) */
  if (had_fab != has_fab)
    neo_anim_start (shell->fab_scale, had_fab, has_fab, FAB_SHOW_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

static void
on_nav (NeoNavBar *nav, int index, Shell *shell)
{
  select_tab (shell, index);
}

static void
on_fab (NeoPressable *fab, Shell *shell)
{
  if (gtk_stack_get_visible_child (GTK_STACK (shell->stack)) == shell->pages[TAB_ALARM])
    alarm_page_add_alarm (ALARM_PAGE (shell->pages[TAB_ALARM]));
}

/* the FAB is centred in a 120 dp row at the bottom of the content, raised by the snackbar while it shows
 * (SnackbarSlidingBehavior). It is placed on its own, not in a full-width box, so that touches either side of it
 * still reach the list under it. */
static gboolean
on_child_position (GtkOverlay *overlay, GtkWidget *child, GdkRectangle *rect, Shell *shell)
{
  int width = gtk_widget_get_width (GTK_WIDGET (overlay)), height = gtk_widget_get_height (GTK_WIDGET (overlay));
  int size;

  if (child != shell->fab)
    return FALSE;
  gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &size, NULL, NULL);
  *rect = (GdkRectangle) { (width - size) / 2,
                           height - (FAB_ROW + size) / 2 - (int) neo_snackbar_get_lift (shell->snackbar),
                           size, size };
  return TRUE;
}

static void
on_lift (NeoSnackbar *snackbar, GParamSpec *pspec, Shell *shell)
{
  gtk_widget_queue_allocate (shell->content);
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  neo_anim_free (shell->fab_scale);
  g_free (shell);
}

GtkWidget *
clock_window_new (GtkApplication *app, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = neo_window_new (app);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  shell->window = window;
  g_object_set_data_full (G_OBJECT (window), "clock-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Clock"));

  /* the app bar: the tab's name 24 dp in, no lift, no shadow */
  gtk_widget_add_css_class (bar, "neo-app-bar");
  shell->title = gtk_label_new (NULL);
  gtk_widget_add_css_class (shell->title, "neo-app-bar-title");
  gtk_widget_set_hexpand (shell->title, TRUE);
  gtk_widget_set_valign (shell->title, GTK_ALIGN_CENTER);
  gtk_label_set_xalign (GTK_LABEL (shell->title), 0);
  neo_label_set_font_padding (shell->title, 22);
  gtk_box_append (GTK_BOX (bar), shell->title);
  gtk_box_append (GTK_BOX (box), bar);

  /* the content: the pages, then the FAB row and the snackbar over their bottom */
  shell->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->stack), GTK_STACK_TRANSITION_TYPE_NONE);
  shell->snackbar = NEO_SNACKBAR (neo_snackbar_new ());
  shell->pages[TAB_ALARM] = alarm_page_new (settings, shell->snackbar);
  shell->pages[TAB_CLOCK] = clock_page_new (settings);
  for (int i = 0; i < N_TABS; i++)
    gtk_stack_add_child (GTK_STACK (shell->stack), shell->pages[i]);

  shell->content = gtk_overlay_new ();
  gtk_widget_set_vexpand (shell->content, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (shell->content), shell->stack);

  shell->fab = neo_fab_new (tabs[TAB_ALARM].fab_icon, TRUE);
  g_signal_connect (shell->fab, "clicked", G_CALLBACK (on_fab), shell);
  shell->fab_scale = neo_anim_new (shell->fab, on_fab_scale, shell);
  gtk_overlay_add_overlay (GTK_OVERLAY (shell->content), shell->fab);
  gtk_overlay_add_overlay (GTK_OVERLAY (shell->content), GTK_WIDGET (shell->snackbar));
  g_signal_connect (shell->content, "get-child-position", G_CALLBACK (on_child_position), shell);
  g_signal_connect (shell->snackbar, "notify::lift", G_CALLBACK (on_lift), shell);
  gtk_box_append (GTK_BOX (box), shell->content);

  shell->nav = neo_nav_bar_new ();
  for (int i = 0; i < N_TABS; i++)
    neo_nav_bar_append (NEO_NAV_BAR (shell->nav), tabs[i].icon, _(tabs[i].name));
  g_signal_connect (shell->nav, "selected", G_CALLBACK (on_nav), shell);
  gtk_box_append (GTK_BOX (box), shell->nav);

  neo_window_set_content (GTK_WINDOW (window), box);
  select_tab (shell, TAB_ALARM);
  return window;
}

void
clock_window_show_alarm (GtkWidget *window, guint alarm_id)
{
  Shell *shell = g_object_get_data (G_OBJECT (window), "clock-shell");

  neo_nav_bar_set_selected (NEO_NAV_BAR (shell->nav), TAB_ALARM);
  select_tab (shell, TAB_ALARM);
  alarm_page_show_alarm (ALARM_PAGE (shell->pages[TAB_ALARM]), alarm_id);
}
