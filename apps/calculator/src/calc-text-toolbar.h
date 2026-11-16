/* CalcTextToolbar: the floating text toolbar that a long press on the formula (MR, Paste) or on a result (MS, M+,
 * M−, Copy) opens, inside the app's own window.
 *
 * A bar of text buttons just above the text it is about (or below it when there is no room above), centred on it
 * and kept inside the window. Only the items that are both visible and enabled are on it. Any touch elsewhere,
 * Back or Escape closes it.
 *
 * A libvibeos candidate: any app with selectable text would want it.
 *
 * Signals: "closed" (), once, when it starts to go. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_TEXT_TOOLBAR (calc_text_toolbar_get_type ())
G_DECLARE_FINAL_TYPE (CalcTextToolbar, calc_text_toolbar, CALC, TEXT_TOOLBAR, GtkWidget)

/* returns FALSE to keep the toolbar (Copy refuses while the digits are being computed) */
typedef gboolean (*CalcToolbarAction) (gpointer data);

CalcTextToolbar *calc_text_toolbar_new     (void);
void             calc_text_toolbar_append  (CalcTextToolbar *self, const char *label, CalcToolbarAction action,
                                            gpointer data);
/* shows it over the window `widget` is in, about `content` (in `widget`'s coordinates) */
void             calc_text_toolbar_present (CalcTextToolbar *self, GtkWidget *widget, const graphene_rect_t *content);
void             calc_text_toolbar_dismiss (CalcTextToolbar *self);

G_END_DECLS
