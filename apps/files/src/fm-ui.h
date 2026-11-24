/* Small pieces every Files screen uses: colours, Roboto labels with font padding, the toolbar's icon buttons, the
 * dialog buttons, check box and radio button, the thin progress bar, and the popup menu. */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

/* the colours the app draws itself: roles of the theme (the rest is in files.css) */
#define FM_C_ACCENT          VIBEOS_PRIMARY             /* a check box or radio on, the progress bar */
#define FM_C_CHECK_OFF       VIBEOS_ON_SURFACE_VARIANT
#define FM_C_CHECK_MARK      VIBEOS_ON_PRIMARY          /* the tick in a check box that is on */

/* a role's colour as the theme has it now: read it as you draw */
GdkRGBA    fm_rgba (VibeosColor color);

/* a label in one of the app's CSS text styles, font padding added; single line, ellipsized at the end
 * unless @wrap (word, then character) */
GtkWidget *fm_label (const char *text, const char *css_class, double size, gboolean wrap);

/* a toolbar icon button: @width x @height, the icon theme's glyph centred (smaller than the original's box, as a
 * symbolic icon is drawn to its edges), a borderless ripple; @name for assistive technology ("Back" is what the
 * shell's back gesture presses) */
GtkWidget *fm_icon_button (const char *icon_name, const char *name, int width, int height);
/* the same with the glyph @glyph_size px (the path bar's up arrow) */
GtkWidget *fm_icon_button_sized (const char *icon_name, const char *name, int width, int height, int glyph_size);

/* a borderless dialog button: caps in the accent */
GtkWidget *fm_text_button (const char *label);

/* a check box or a radio button: dim white when off, the accent when on */
GtkWidget *fm_check_new        (gboolean radio);
void       fm_check_set_active (GtkWidget *check, gboolean active);
gboolean   fm_check_get_active (GtkWidget *check);
/* a row of a check box (or a radio button) and its label that a tap anywhere toggles; "toggled" on the row */
GtkWidget *fm_check_row        (const char *label, gboolean radio, gboolean active);
GtkWidget *fm_check_row_get_check (GtkWidget *row);

/* a thin progress bar: a fraction 0..1, or a fraction < 0 for the moving segments of an indeterminate one
 * @height px tall */
GtkWidget *fm_progress_new          (double height);
void       fm_progress_set_fraction (GtkWidget *progress, double fraction);

/* the popup menu over the window (the overflow ⋮, a row's ⋮, a long press): a card of one-line rows, as wide as
 * the widest. A tap outside, Back or Escape closes it; a tap on a row closes it and then runs the choice (-1 is never
 * given). */
typedef void (*FmPopupChosen) (int index, gpointer data);
typedef enum {
  FM_POPUP_OVER_END,            /* an overflow menu: its top on the anchor's, its end on the anchor's end */
  FM_POPUP_AT,                  /* a context menu: its top-left at (x, y) in the anchor */
} FmPopupPlace;
void       fm_popup_show    (GtkWidget *anchor, const char *const *labels, int n_labels, FmPopupPlace place,
                             double x, double y, FmPopupChosen chosen, gpointer data);
/* the same with an icon before each label (@icons: n_labels icon names), as File Manager+'s selection menu has */
void       fm_popup_show_icons (GtkWidget *anchor, const char *const *labels, const char *const *icons, int n_labels,
                                FmPopupPlace place, double x, double y, FmPopupChosen chosen, gpointer data);
/* closes the menu of @widget's window if one shows; TRUE when it did */
gboolean   fm_popup_dismiss (GtkWidget *widget);

/* the plain upper-cased form of a label */
char      *fm_caps (const char *label);

G_END_DECLS
