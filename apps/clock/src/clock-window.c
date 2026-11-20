/* The Clock's window: see clock-window.h. */
#include "clock-window.h"
#include "alarm-page.h"
#include "city-picker.h"
#include "clock-navigator.h"
#include "clock-page.h"
#include "clock-tab.h"
#include "ringtone-picker.h"
#include "screensaver.h"
#include "settings-page.h"
#include "stopwatch-page.h"
#include "timer-page.h"

#include <glib/gi18n.h>
#include <vibeos.h>

#define FAB_ROW       120       /* the row the FAB sits in, with a margin over and under it */
#define SIDE_BUTTON   56        /* the left and right buttons, smaller than the FAB */
#define LEFT_BIAS     0.6       /* where each sits in the space beside the FAB */
#define RIGHT_BIAS    0.4
#define FAB_COLUMN    180       /* a tablet in landscape: the buttons' column at the end, beside the pages */
#define COLUMN_GAP    24        /* in the column, between the FAB and the buttons over and under it */
#define SCALE_MS      200       /* SHORT: the buttons' scale animations */

typedef enum { TAB_ALARM, TAB_CLOCK, TAB_TIMER, TAB_STOPWATCH, N_TABS } Tab;

typedef struct
{
  GtkWidget *widget;
  VibeosAnim *scale;
  gboolean grow_back;           /* this shrink is the first half of a shrink-and-expand */
} Button;

typedef struct
{
  GtkWidget *window;
  GSettings *settings;
  GtkWidget *navigator;         /* the window's pages: this shell, then the cities picker, Settings ... */
  GtkWidget *title, *stack, *content, *nav;
  VibeosSnackbar *snackbar;
  GtkWidget *pages[N_TABS];
  Tab tab;
  Button buttons[CLOCK_N_BUTTONS];
  VibeosLayout *layout;
  gboolean column;              /* the buttons in a column at the end rather than a row at the bottom */
} Shell;

/* the bottom navigation bar's tabs */
static const struct
{
  const char *name, *icon;
} tabs[N_TABS] = {
  [TAB_ALARM] = { N_("Alarm"), "clock-tab-alarm-symbolic" },
  [TAB_CLOCK] = { N_("Clock"), "clock-tab-clock-symbolic" },
  [TAB_TIMER] = { N_("Timer"), "clock-tab-timer-symbolic" },
  [TAB_STOPWATCH] = { N_("Stopwatch"), "clock-tab-stopwatch-symbolic" },
};

/* --- the row of buttons */

/* the selected tab's buttons as they are now, on the row */
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
      vibeos_fab_set_icon (VIBEOS_PRESSABLE (widget), buttons[i].icon);
      gtk_accessible_update_property (GTK_ACCESSIBLE (widget), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      buttons[i].description, -1);
    }
}

static void
on_scale (VibeosAnim *anim, double value, gpointer data)
{
  Shell *shell = data;
  Button *button = NULL;

  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    if (shell->buttons[i].scale == anim)
      button = &shell->buttons[i];
  vibeos_pressable_set_scale (VIBEOS_PRESSABLE (button->widget), value);
  if (vibeos_anim_is_running (anim) || value > 0)
    return;

  /* shrunk to nothing: its new state goes on now, at the end of the shrink, and it grows back if this was the first
   * half */
  if (button == &shell->buttons[CLOCK_FAB])
    apply (shell, TRUE, FALSE);
  else if (button == &shell->buttons[CLOCK_LEFT])
    apply (shell, FALSE, TRUE);
  if (button->grow_back)
    {
      button->grow_back = FALSE;
      vibeos_anim_start (anim, 0, 1, SCALE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
    }
}

static void
scale (Button *button, double to, gboolean grow_back)
{
  button->grow_back = grow_back;
  vibeos_anim_start (button->scale, vibeos_pressable_get_scale (VIBEOS_PRESSABLE (button->widget)), to, SCALE_MS, 0,
                  SPONGY_ACCELERATE_DECELERATE);
}

/* the row brought up to date with the tab, as @how says */
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
on_button (VibeosPressable *widget, Shell *shell)
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

  /* A FAB coming or going takes all three buttons with it, scaling over 200 ms; going, the new tab's buttons go on once
   * they have shrunk. Otherwise the icons change at once. */
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
on_nav (VibeosNavBar *nav, int index, Shell *shell)
{
  select_tab (shell, index);
}

/* keys go to the tab on screen first: the timer's keypad takes digits. Not while a page is open over the tabs */
static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, Shell *shell)
{
  if (clock_navigator_get_depth (CLOCK_NAVIGATOR (shell->navigator)) > 1)
    return FALSE;
  return clock_tab_key_pressed (CLOCK_TAB (shell->pages[shell->tab]), keyval);
}

/* --- the pages over the tabs */

static void
close_page (GtkWidget *page, Shell *shell)
{
  /* a page closing itself goes only if it is the one on top */
  if (clock_navigator_get_top (CLOCK_NAVIGATOR (shell->navigator)) == page)
    clock_navigator_pop (CLOCK_NAVIGATOR (shell->navigator));
}

static void
show_ringtone_picker (Shell *shell, GtkWidget *picker)
{
  g_signal_connect (picker, "closed", G_CALLBACK (close_page), shell);
  clock_navigator_push (CLOCK_NAVIGATOR (shell->navigator), picker);
}

static void
on_pick_timer_sound (GtkWidget *page, Shell *shell)
{
  show_ringtone_picker (shell, ringtone_picker_new_for_timer (shell->settings));
}

static void
on_pick_ringtone (AlarmPage *page, guint alarm_id, Shell *shell)
{
  show_ringtone_picker (shell, ringtone_picker_new_for_alarm (shell->settings, alarm_id));
}

/* Settings ▸ Screen saver ▸ Preview, or a long press on the Clock tab */
static void
on_show_screensaver (GtkWidget *page, Shell *shell)
{
  screensaver_present (gtk_window_get_application (GTK_WINDOW (shell->window)), shell->settings);
}

/* Settings ▸ Screen saver ▸ Settings: the screen saver's settings, over Settings */
static void
on_screensaver_settings (GtkWidget *settings_page, Shell *shell)
{
  GtkWidget *page = settings_page_new_screensaver (shell->settings);

  g_signal_connect (page, "closed", G_CALLBACK (close_page), shell);
  clock_navigator_push (CLOCK_NAVIGATOR (shell->navigator), page);
}

/* Settings, from the gear or the cities picker's menu */
static void
show_settings (Shell *shell)
{
  GtkWidget *page = settings_page_new (shell->settings);

  g_signal_connect (page, "closed", G_CALLBACK (close_page), shell);
  g_signal_connect (page, "pick-timer-sound", G_CALLBACK (on_pick_timer_sound), shell);
  g_signal_connect (page, "show-screensaver", G_CALLBACK (on_show_screensaver), shell);
  g_signal_connect (page, "show-screensaver-settings", G_CALLBACK (on_screensaver_settings), shell);
  clock_navigator_push (CLOCK_NAVIGATOR (shell->navigator), page);
}

static void
on_settings_button (VibeosPressable *button, Shell *shell)
{
  show_settings (shell);
}

static void
on_picker_settings (GtkWidget *picker, Shell *shell)
{
  show_settings (shell);
}

/* the Clock tab's FAB: the cities picker */
static void
on_show_cities (ClockPage *page, Shell *shell)
{
  GtkWidget *picker = city_picker_new (shell->settings);

  g_signal_connect (picker, "closed", G_CALLBACK (close_page), shell);
  g_signal_connect (picker, "show-settings", G_CALLBACK (on_picker_settings), shell);
  clock_navigator_push (CLOCK_NAVIGATOR (shell->navigator), picker);
}

/* Escape is Back (a keyboard's; the shell's back gesture presses a page's "Back" button): it closes the page on
 * top. Over the tabs it does nothing, as before */
static gboolean
on_escape (GtkWidget *widget, GVariant *args, gpointer data)
{
  Shell *shell = data;

  return clock_navigator_pop (CLOCK_NAVIGATOR (shell->navigator));
}

static void
show_tab (Shell *shell, Tab tab)
{
  /* a notification's "show": the tabs, over whatever page was open */
  if (clock_navigator_get_depth (CLOCK_NAVIGATOR (shell->navigator)) > 1)
    clock_navigator_pop_to_root (CLOCK_NAVIGATOR (shell->navigator));
  vibeos_nav_bar_set_selected (VIBEOS_NAV_BAR (shell->nav), tab);
  select_tab (shell, tab);
}

/* --- layout */

/* The FAB is centred in a FAB_ROW-tall row at the bottom of the content, raised by the snackbar while it shows; the
 * side buttons are centred on the same line, each placed in the space beside the FAB by its bias. They are placed on
 * their own, not in a full-width box, so that touches between them still reach the list under them.
 * On a tablet in landscape they are in a FAB_COLUMN-wide column at the end instead, the pages beside it: the FAB in
 * its middle, the left button over it and the right one under it. */
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
  if (shell->column)
    {
      rect->x = width - FAB_COLUMN / 2 - size / 2;
      if (child == shell->buttons[CLOCK_FAB].widget)
        rect->y = (height - fab) / 2;
      else if (child == shell->buttons[CLOCK_LEFT].widget)
        rect->y = (height - fab) / 2 - COLUMN_GAP - size;
      else
        rect->y = (height + fab) / 2 + COLUMN_GAP;
      rect->width = rect->height = size;
      return TRUE;
    }
  centre_y = height - FAB_ROW / 2 - (int) vibeos_snackbar_get_lift (shell->snackbar);
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
on_lift (VibeosSnackbar *snackbar, GParamSpec *pspec, Shell *shell)
{
  gtk_widget_queue_allocate (shell->content);
}

/* a tablet: its own text sizes (clock.css); in landscape the pages end where the buttons' column starts */
static void
on_layout_changed (VibeosLayout *layout, Shell *shell)
{
  gboolean column = vibeos_layout_is_tablet (layout) && vibeos_layout_is_landscape (layout);

  if (vibeos_layout_is_tablet (layout))
    gtk_widget_add_css_class (shell->window, "tablet");
  else
    gtk_widget_remove_css_class (shell->window, "tablet");
  if (column == shell->column)
    return;
  shell->column = column;
  gtk_widget_set_margin_end (shell->stack, column ? FAB_COLUMN : 0);
  gtk_widget_queue_allocate (shell->content);
}

/* --- in front */

/* The service shows no timer or stopwatch notification while the app is in front, as the tabs show them. The window
 * being the active one is in front: the lock screen, the launcher or another app takes that from it. */
static void
on_active (GtkWindow *window, GParamSpec *pspec, Shell *shell)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());

  if (bus)
    g_dbus_connection_call (bus, "vibeos.Clock.Alarms", "/vibeos/Clock/App", "vibeos.Clock.App",
                            "SetForeground", g_variant_new ("(b)", gtk_window_is_active (window)), NULL,
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
shell_free (gpointer data)
{
  Shell *shell = data;

  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    vibeos_anim_free (shell->buttons[i].scale);
  g_signal_handlers_disconnect_by_data (shell->layout, shell);
  g_object_unref (shell->layout);
  g_object_unref (shell->settings);
  g_free (shell);
}

GtkWidget *
clock_window_new (GtkApplication *app, GSettings *settings)
{
  Shell *shell = g_new0 (Shell, 1);
  GtkWidget *window = vibeos_window_new (app);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *gear = gtk_image_new_from_icon_name ("clock-settings-symbolic");
  GtkWidget *settings_button;
  GtkEventController *key, *shortcuts;

  shell->window = window;
  shell->settings = g_object_ref (settings);
  g_object_set_data_full (G_OBJECT (window), "clock-shell", shell, shell_free);
  gtk_window_set_title (GTK_WINDOW (window), _("Clock"));

  /* the app bar: the tab's name, no lift, no shadow */
  gtk_widget_add_css_class (bar, "vibeos-app-bar");
  shell->title = gtk_label_new (NULL);
  gtk_widget_add_css_class (shell->title, "vibeos-app-bar-title");
  gtk_widget_set_hexpand (shell->title, TRUE);
  gtk_widget_set_valign (shell->title, GTK_ALIGN_CENTER);
  gtk_label_set_xalign (GTK_LABEL (shell->title), 0);
  vibeos_label_set_font_padding_sp (shell->title, 22);
  gtk_box_append (GTK_BOX (bar), shell->title);
  /* the Settings action, a white gear in a button flush with the end */
  gtk_image_set_pixel_size (GTK_IMAGE (gear), 24);
  settings_button = vibeos_pressable_new (gear);
  gtk_widget_set_size_request (settings_button, 48, 48);
  gtk_widget_set_valign (settings_button, GTK_ALIGN_CENTER);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (settings_button), VIBEOS_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (settings_button, "clock-settings-button");
  gtk_accessible_update_property (GTK_ACCESSIBLE (settings_button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Settings"),
                                  -1);
  g_signal_connect (settings_button, "clicked", G_CALLBACK (on_settings_button), shell);
  gtk_box_append (GTK_BOX (bar), settings_button);
  gtk_box_append (GTK_BOX (box), bar);

  /* the content: the pages, then the row of buttons and the snackbar over their bottom */
  shell->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (shell->stack), GTK_STACK_TRANSITION_TYPE_NONE);
  shell->snackbar = VIBEOS_SNACKBAR (vibeos_snackbar_new ());
  shell->pages[TAB_ALARM] = alarm_page_new (settings, shell->snackbar);
  shell->pages[TAB_CLOCK] = clock_page_new (settings);
  shell->pages[TAB_TIMER] = timer_page_new ();
  shell->pages[TAB_STOPWATCH] = stopwatch_page_new (shell->snackbar);
  g_signal_connect (shell->pages[TAB_CLOCK], "show-cities", G_CALLBACK (on_show_cities), shell);
  g_signal_connect (shell->pages[TAB_CLOCK], "show-screensaver", G_CALLBACK (on_show_screensaver), shell);
  g_signal_connect (shell->pages[TAB_ALARM], "pick-ringtone", G_CALLBACK (on_pick_ringtone), shell);
  for (int i = 0; i < N_TABS; i++)
    {
      gtk_stack_add_child (GTK_STACK (shell->stack), shell->pages[i]);
      g_signal_connect (shell->pages[i], "update-buttons", G_CALLBACK (on_update_buttons), shell);
    }

  shell->content = gtk_overlay_new ();
  gtk_widget_set_vexpand (shell->content, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (shell->content), shell->stack);

  /* the FAB and the smaller side buttons, in their own colours; hidden until a tab sets them */
  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    {
      Button *button = &shell->buttons[i];

      button->widget = vibeos_fab_new ("clock-add-symbolic", i == CLOCK_FAB);
      gtk_widget_set_visible (button->widget, FALSE);
      g_signal_connect (button->widget, "clicked", G_CALLBACK (on_button), shell);
      button->scale = vibeos_anim_new (button->widget, on_scale, shell);
      gtk_overlay_add_overlay (GTK_OVERLAY (shell->content), button->widget);
    }
  gtk_overlay_add_overlay (GTK_OVERLAY (shell->content), GTK_WIDGET (shell->snackbar));
  g_signal_connect (shell->content, "get-child-position", G_CALLBACK (on_child_position), shell);
  g_signal_connect (shell->snackbar, "notify::lift", G_CALLBACK (on_lift), shell);
  gtk_box_append (GTK_BOX (box), shell->content);

  shell->nav = vibeos_nav_bar_new ();
  for (int i = 0; i < N_TABS; i++)
    vibeos_nav_bar_append (VIBEOS_NAV_BAR (shell->nav), tabs[i].icon, _(tabs[i].name));
  g_signal_connect (shell->nav, "selected", G_CALLBACK (on_nav), shell);
  gtk_box_append (GTK_BOX (box), shell->nav);

  /* the tabs are the first page; the cities picker and Settings open over them */
  shell->navigator = clock_navigator_new ();
  clock_navigator_push (CLOCK_NAVIGATOR (shell->navigator), box);
  vibeos_window_set_content (GTK_WINDOW (window), shell->navigator);
  shortcuts = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                          gtk_callback_action_new (on_escape, shell, NULL)));
  gtk_widget_add_controller (window, shortcuts);
  g_signal_connect (window, "notify::is-active", G_CALLBACK (on_active), shell);
  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key), shell);
  gtk_widget_add_controller (window, key);
  shell->layout = g_object_ref (vibeos_layout_get_for_window (GTK_WINDOW (window)));
  g_signal_connect (shell->layout, "changed", G_CALLBACK (on_layout_changed), shell);
  on_layout_changed (shell->layout, shell);
  /* the tab it was left on */
  shell->tab = g_settings_get_enum (settings, "selected-tab");
  vibeos_nav_bar_set_selected (VIBEOS_NAV_BAR (shell->nav), shell->tab);
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
