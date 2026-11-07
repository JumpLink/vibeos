/* The Clock's window: see clock-window.h. Measured on still:clock-alarm: the app bar y 116…308 px, the title
 * 72…252 × 168…256, the FAB 408…672 × 1728…1992, the navigation bar y 2040…2280. */
#include "clock-window.h"
#include "alarm-page.h"
#include "clock-page.h"
#include "clock-tab.h"
#include "stopwatch-page.h"
#include "timer-page.h"

#include <glib/gi18n.h>
#include <neo.h>

#define FAB_ROW       120       /* the FAB container: 88 dp and a 16 dp margin over and under it */
#define SIDE_BUTTON   56        /* the left and right buttons: default-size FABs */
#define LEFT_BIAS     0.6       /* where each sits in the space beside the FAB (L/desk_clock.xml) */
#define RIGHT_BIAS    0.4
#define SCALE_MS      200       /* SHORT: the buttons' scale animations */

typedef enum { TAB_ALARM, TAB_CLOCK, TAB_TIMER, TAB_STOPWATCH, N_TABS } Tab;

typedef struct
{
  GtkWidget *widget;
  NeoAnim *scale;
  gboolean grow_back;           /* this shrink is the first half of a shrink-and-expand */
} Button;

typedef struct
{
  GtkWidget *window;
  GSettings *settings;
  GtkWidget *title, *stack, *content, *nav;
  NeoSnackbar *snackbar;
  GtkWidget *pages[N_TABS];
  Tab tab;
  Button buttons[CLOCK_N_BUTTONS];
} Shell;

/* bottom_navigation_menu.xml */
static const struct
{
  const char *name, *icon;
} tabs[N_TABS] = {
  [TAB_ALARM] = { N_("Alarm"), "clock-tab-alarm-symbolic" },
  [TAB_CLOCK] = { N_("Clock"), "clock-tab-clock-symbolic" },
  [TAB_TIMER] = { N_("Timer"), "clock-tab-timer-symbolic" },
  [TAB_STOPWATCH] = { N_("Stopwatch"), "clock-tab-stopwatch-symbolic" },
};

/* --- the row of buttons (DeskClock's FabContainer) */

/* the selected tab's buttons as they are now, on the row (onUpdateFab, onUpdateFabButtons) */
static void
apply (Shell *shell, gboolean fab, gboolean sides)
{
  ClockButton buttons[CLOCK_N_BUTTONS];

  clock_tab_get_buttons (CLOCK_TAB (shell->pages[shell->tab]), buttons);
  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    {
      GtkWidget *widget = shell->buttons[i].widget;

      if (i == CLOCK_FAB ? !fab : !sides)
        continue;
      gtk_widget_set_visible (widget, buttons[i].icon != NULL);
      if (!buttons[i].icon)
        continue;
      neo_fab_set_icon (NEO_PRESSABLE (widget), buttons[i].icon);
      gtk_accessible_update_property (GTK_ACCESSIBLE (widget), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      buttons[i].description, -1);
    }
}

static void
on_scale (NeoAnim *anim, double value, gpointer data)
{
  Shell *shell = data;
  Button *button = NULL;

  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    if (shell->buttons[i].scale == anim)
      button = &shell->buttons[i];
  neo_pressable_set_scale (NEO_PRESSABLE (button->widget), value);
  if (neo_anim_is_running (anim) || value > 0)
    return;

  /* shrunk to nothing: its new state goes on now, at the end of the shrink (hideFabAnimation's and
   * leftHideAnimation's listeners), and it grows back if this was the first half */
  if (button == &shell->buttons[CLOCK_FAB])
    apply (shell, TRUE, FALSE);
  else if (button == &shell->buttons[CLOCK_LEFT])
    apply (shell, FALSE, TRUE);
  if (button->grow_back)
    {
      button->grow_back = FALSE;
      neo_anim_start (anim, 0, 1, SCALE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
    }
}

static void
scale (Button *button, double to, gboolean grow_back)
{
  button->grow_back = grow_back;
  neo_anim_start (button->scale, neo_pressable_get_scale (NEO_PRESSABLE (button->widget)), to, SCALE_MS, 0,
                  SPONGY_ACCELERATE_DECELERATE);
}

/* FabContainer.updateFab */
static void
update_buttons (Shell *shell, ClockUpdate how)
{
  Button *fab = &shell->buttons[CLOCK_FAB], *left = &shell->buttons[CLOCK_LEFT];
  Button *right = &shell->buttons[CLOCK_RIGHT];

  if (how & CLOCK_FAB_IMMEDIATE)
    apply (shell, TRUE, FALSE);
  if (how & CLOCK_BUTTONS_IMMEDIATE)
    apply (shell, FALSE, TRUE);
  if (how & CLOCK_FAB_SHRINK_AND_EXPAND)
    scale (fab, 0, TRUE);
  if (how & CLOCK_BUTTONS_SHRINK_AND_EXPAND)
    {
      scale (left, 0, TRUE);
      scale (right, 0, TRUE);
    }
  if (how & CLOCK_ALL_SHRINK)
    for (int i = 0; i < CLOCK_N_BUTTONS; i++)
      scale (&shell->buttons[i], 0, FALSE);
  if (how & CLOCK_ALL_EXPAND)
    for (int i = 0; i < CLOCK_N_BUTTONS; i++)
      scale (&shell->buttons[i], 1, FALSE);
}

static void
on_update_buttons (ClockTab *tab, int how, Shell *shell)
{
  if (GTK_WIDGET (tab) == shell->pages[shell->tab])
    update_buttons (shell, how);
}

static void
on_button (NeoPressable *widget, Shell *shell)
{
  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    if (shell->buttons[i].widget == GTK_WIDGET (widget))
      clock_tab_clicked (CLOCK_TAB (shell->pages[shell->tab]), i);
}

/* --- tabs */

static void
select_tab (Shell *shell, Tab tab)
{
  gboolean had = clock_tab_has_fab (CLOCK_TAB (shell->pages[shell->tab]));
  gboolean has = clock_tab_has_fab (CLOCK_TAB (shell->pages[tab]));

  shell->tab = tab;
  g_settings_set_enum (shell->settings, "selected-tab", tab);
  gtk_stack_set_visible_child (GTK_STACK (shell->stack), shell->pages[tab]);
  gtk_label_set_text (GTK_LABEL (shell->title), _(tabs[tab].name));

  /* (the navigation listener and TabChangeWatcher) A FAB coming or going takes all three buttons with it, scaling
   * over 200 ms; going, the new tab's buttons go on once they have shrunk. Otherwise the icons change at once. */
  if (had && !has)
    update_buttons (shell, CLOCK_ALL_SHRINK);
  else
    {
      update_buttons (shell, CLOCK_ALL_IMMEDIATE);
      if (!had && has)
        update_buttons (shell, CLOCK_ALL_EXPAND);
    }
}

static void
on_nav (NeoNavBar *nav, int index, Shell *shell)
{
  select_tab (shell, index);
}

/* keys go to the tab on screen first (DeskClock.onKeyDown): the timer's keypad takes digits */
static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, Shell *shell)
{
  return clock_tab_key_pressed (CLOCK_TAB (shell->pages[shell->tab]), keyval);
}

static void
show_tab (Shell *shell, Tab tab)
{
  neo_nav_bar_set_selected (NEO_NAV_BAR (shell->nav), tab);
  select_tab (shell, tab);
}

/* --- layout */

/* The FAB is centred in a 120 dp row at the bottom of the content, raised by the snackbar while it shows
 * (SnackbarSlidingBehavior); the side buttons are centred on the same line, each placed in the space beside the
 * FAB by its bias. They are placed on their own, not in a full-width box, so that touches between them still reach
 * the list under them. */
static gboolean
on_child_position (GtkOverlay *overlay, GtkWidget *child, GdkRectangle *rect, Shell *shell)
{
  int width = gtk_widget_get_width (GTK_WIDGET (overlay)), height = gtk_widget_get_height (GTK_WIDGET (overlay));
  int fab, size, centre_y, fab_x;

  if (child != shell->buttons[CLOCK_FAB].widget && child != shell->buttons[CLOCK_LEFT].widget
      && child != shell->buttons[CLOCK_RIGHT].widget)
    return FALSE;
  gtk_widget_measure (shell->buttons[CLOCK_FAB].widget, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &fab, NULL, NULL);
  gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &size, NULL, NULL);
  centre_y = height - FAB_ROW / 2 - (int) neo_snackbar_get_lift (shell->snackbar);
  fab_x = (width - fab) / 2;
  if (child == shell->buttons[CLOCK_FAB].widget)
    rect->x = fab_x;
  else if (child == shell->buttons[CLOCK_LEFT].widget)
    rect->x = (int) round (LEFT_BIAS * (fab_x - size));
  else
    rect->x = (int) round (fab_x + fab + RIGHT_BIAS * (width - fab_x - fab - size));
  rect->y = centre_y - size / 2;
  rect->width = rect->height = size;
  return TRUE;
}

static void
on_lift (NeoSnackbar *snackbar, GParamSpec *pspec, Shell *shell)
{
  gtk_widget_queue_allocate (shell->content);
}

/* --- in front */

/* The service shows no timer or stopwatch notification while the app is in front, as the tabs show them
 * (NotificationModel.isApplicationInForeground). The window being the active one is in front: the lock screen,
 * the launcher or another app takes that from it. */
static void
on_active (GtkWindow *window, GParamSpec *pspec, Shell *shell)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());

  if (bus)
    g_dbus_connection_call (bus, "de.yesman.Clock.Alarms", "/de/yesman/Clock/App", "de.yesman.Clock.App",
                            "SetForeground", g_variant_new ("(b)", gtk_window_is_active (window)), NULL,
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    neo_anim_free (shell->buttons[i].scale);
  g_object_unref (shell->settings);
  g_free (shell);
}

GtkWidget *
clock_window_new (GtkApplication *app, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = neo_window_new (app);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkEventController *key;

  shell->window = window;
  shell->settings = g_object_ref (settings);
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

  /* the content: the pages, then the row of buttons and the snackbar over their bottom */
  shell->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->stack), GTK_STACK_TRANSITION_TYPE_NONE);
  shell->snackbar = NEO_SNACKBAR (neo_snackbar_new ());
  shell->pages[TAB_ALARM] = alarm_page_new (settings, shell->snackbar);
  shell->pages[TAB_CLOCK] = clock_page_new (settings);
  shell->pages[TAB_TIMER] = timer_page_new ();
  shell->pages[TAB_STOPWATCH] = stopwatch_page_new (shell->snackbar);
  for (int i = 0; i < N_TABS; i++)
    {
      gtk_stack_add_child (GTK_STACK (shell->stack), shell->pages[i]);
      g_signal_connect (shell->pages[i], "update-buttons", G_CALLBACK (on_update_buttons), shell);
    }

  shell->content = gtk_overlay_new ();
  gtk_widget_set_vexpand (shell->content, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (shell->content), shell->stack);

  /* the FAB (88 dp, primary) and the side buttons (56 dp, secondary); hidden until a tab sets them */
  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    {
      Button *button = &shell->buttons[i];

      button->widget = neo_fab_new ("clock-add-symbolic", i == CLOCK_FAB);
      gtk_widget_set_visible (button->widget, FALSE);
      g_signal_connect (button->widget, "clicked", G_CALLBACK (on_button), shell);
      button->scale = neo_anim_new (button->widget, on_scale, shell);
      gtk_overlay_add_overlay (GTK_OVERLAY (shell->content), button->widget);
    }
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
  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_active), shell);
  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key), shell);
  gtk_widget_add_controller (window, key);
  /* the tab it was left on (UiDataModel.getSelectedTab) */
  shell->tab = g_settings_get_enum (settings, "selected-tab");
  neo_nav_bar_set_selected (NEO_NAV_BAR (shell->nav), shell->tab);
  select_tab (shell, shell->tab);
  return window;
}

void
clock_window_show_alarm (GtkWidget *window, guint alarm_id)
{
  Shell *shell = g_object_get_data (G_OBJECT (window), "clock-shell");

  show_tab (shell, TAB_ALARM);
  alarm_page_show_alarm (ALARM_PAGE (shell->pages[TAB_ALARM]), alarm_id);
}

void
clock_window_show_timer (GtkWidget *window, guint timer_id)
{
  Shell *shell = g_object_get_data (G_OBJECT (window), "clock-shell");

  show_tab (shell, TAB_TIMER);
  timer_page_show_timer (TIMER_PAGE (shell->pages[TAB_TIMER]), timer_id);
}

void
clock_window_show_stopwatch (GtkWidget *window)
{
  Shell *shell = g_object_get_data (G_OBJECT (window), "clock-shell");

  show_tab (shell, TAB_STOPWATCH);
}
