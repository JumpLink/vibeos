/* ClockTab: see clock-tab.h. */
#include "clock-tab.h"

G_DEFINE_INTERFACE (ClockTab, clock_tab, GTK_TYPE_WIDGET)

enum { UPDATE_BUTTONS, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
clock_tab_default_init (ClockTabInterface *iface)
{
  signals[UPDATE_BUTTONS] = g_signal_new ("update-buttons", CLOCK_TYPE_TAB, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 1, G_TYPE_INT);
}

void
clock_tab_get_buttons (ClockTab *self, ClockButton *buttons)
{
  ClockTabInterface *iface = CLOCK_TAB_GET_IFACE (self);

  for (int i = 0; i < CLOCK_N_BUTTONS; i++)
    buttons[i] = (ClockButton) { NULL, NULL };
  if (iface->get_buttons)
    iface->get_buttons (self, buttons);
}

void
clock_tab_clicked (ClockTab *self, ClockButtonPosition button)
{
  ClockTabInterface *iface = CLOCK_TAB_GET_IFACE (self);

  if (iface->clicked)
    iface->clicked (self, button);
}

gboolean
clock_tab_has_fab (ClockTab *self)
{
  ClockTabInterface *iface = CLOCK_TAB_GET_IFACE (self);
  ClockButton buttons[CLOCK_N_BUTTONS];

  if (iface->has_fab)
    return iface->has_fab (self);
  clock_tab_get_buttons (self, buttons);
  return buttons[CLOCK_FAB].icon != NULL;
}

gboolean
clock_tab_key_pressed (ClockTab *self, guint keyval)
{
  ClockTabInterface *iface = CLOCK_TAB_GET_IFACE (self);

  return iface->key_pressed && iface->key_pressed (self, keyval);
}

void
clock_tab_update_buttons (ClockTab *self, ClockUpdate how)
{
  g_signal_emit (self, signals[UPDATE_BUTTONS], 0, how);
}
