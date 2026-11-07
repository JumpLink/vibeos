/* ClockTab: what a tab of the Clock's window gives the row of buttons at the bottom (DeskClock's FabController and
 * FabContainer).
 *
 * The row belongs to the window and is the same on every tab: the FAB in the middle and a smaller button either
 * side of it. Each tab says what they show and is told when one is pressed. When that changes, the tab asks the
 * window to show it, saying how: at once, or with the FAB or the side buttons shrinking to nothing and growing back
 * with their new icons (the animations are the window's, so every tab moves the same way). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  CLOCK_FAB,
  CLOCK_LEFT,
  CLOCK_RIGHT,
  CLOCK_N_BUTTONS,
} ClockButtonPosition;

/* one button of the row: its icon and what it does (read out by a screen reader); a NULL icon hides it */
typedef struct {
  const char *icon;
  const char *description;
} ClockButton;

/* how the row changes (UpdateFabFlag): the FAB and the side buttons each at once or shrinking and growing back,
 * or all three shrinking away (a page change starts) and growing back (it ended) */
typedef enum {
  CLOCK_FAB_IMMEDIATE = 1 << 0,
  CLOCK_FAB_SHRINK_AND_EXPAND = 1 << 1,
  CLOCK_BUTTONS_IMMEDIATE = 1 << 2,
  CLOCK_BUTTONS_SHRINK_AND_EXPAND = 1 << 3,
  CLOCK_ALL_SHRINK = 1 << 4,
  CLOCK_ALL_EXPAND = 1 << 5,
} ClockUpdate;

#define CLOCK_ALL_IMMEDIATE (CLOCK_FAB_IMMEDIATE | CLOCK_BUTTONS_IMMEDIATE)

#define CLOCK_TYPE_TAB (clock_tab_get_type ())
G_DECLARE_INTERFACE (ClockTab, clock_tab, CLOCK, TAB, GtkWidget)

struct _ClockTabInterface
{
  GTypeInterface parent;

  /* the row as the tab wants it now: buttons[CLOCK_FAB], [CLOCK_LEFT], [CLOCK_RIGHT] (all hidden unless set) */
  void (*get_buttons) (ClockTab *self, ClockButton *buttons);
  void (*clicked)     (ClockTab *self, ClockButtonPosition button);
  /* whether the tab counts as having a FAB when the tabs change (getFabTargetVisibility); without this, whether
   * get_buttons gives it one */
  gboolean (*has_fab) (ClockTab *self);
  /* a key the window got, offered to the tab on screen first (onKeyDown); TRUE when the tab used it */
  gboolean (*key_pressed) (ClockTab *self, guint keyval);
};

void     clock_tab_get_buttons (ClockTab *self, ClockButton *buttons);
void     clock_tab_clicked (ClockTab *self, ClockButtonPosition button);
gboolean clock_tab_has_fab (ClockTab *self);
gboolean clock_tab_key_pressed (ClockTab *self, guint keyval);

/* the tab's buttons changed: emits "update-buttons" (a ClockUpdate) for the window, which shows them if the tab is
 * the one on screen */
void clock_tab_update_buttons (ClockTab *self, ClockUpdate how);

G_END_DECLS
