/* CalcTextToolbar: the floating text toolbar (ActionMode.TYPE_FLOATING) that a long press on the formula (MR,
 * Paste) or on a result (MS, M+, M−, Copy) opens, inside the app's own window.
 *
 * Android's FloatingToolbar: a 48 dp bar of text buttons above the text it is about (8 dp clear of it, or below it
 * when there is no room above), centred on it and kept 16 dp inside the window. Only the items that are both
 * visible and enabled are on it (getVisibleAndEnabledMenuItems). Any touch elsewhere, Back or Escape closes it, as
 * Calculator.dispatchTouchEvent and onBackPressed stop the action mode.
 *
 * NOT MEASURED: no still has it open. The sizes are the framework's dimens (floating_toolbar_height 48 dp,
 * floating_toolbar_menu_button_side_padding 11 dp... rounded up to a 16 dp button padding here, 14 sp text), the
 * colours Material 3's floating surface; check against a still when there is one.
 *
 * A libneo candidate: any app with selectable text would want it.
 *
 * Signals: "closed" (), once, when it starts to go. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_TEXT_TOOLBAR (calc_text_toolbar_get_type ())
G_DECLARE_FINAL_TYPE (CalcTextToolbar, calc_text_toolbar, CALC, TEXT_TOOLBAR, GtkWidget)

/* returns FALSE to keep the toolbar (onActionItemClicked: Copy refuses while the digits are being computed) */
typedef gboolean (*CalcToolbarAction) (gpointer data);

CalcTextToolbar *calc_text_toolbar_new     (void);
void             calc_text_toolbar_append  (CalcTextToolbar *self, const char *label, CalcToolbarAction action,
                                            gpointer data);
/* shows it over the window `widget` is in, about `content` (in `widget`'s coordinates) */
void             calc_text_toolbar_present (CalcTextToolbar *self, GtkWidget *widget, const graphene_rect_t *content);
void             calc_text_toolbar_dismiss (CalcTextToolbar *self);

G_END_DECLS
