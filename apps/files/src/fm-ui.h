/* Small pieces every Files screen uses (spec §1, §8): colours, Roboto labels in Android's line boxes, the toolbar's
 * icon buttons, AppCompat's dialog buttons, check box and radio button, the thin progress bar, and the popup menu. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the colours of the spec's table (night values, as measured) */
#define FM_C_WINDOW          0xff121212
#define FM_C_BAR             0xff1f1f1f
#define FM_C_DIVIDER         0xff272727
#define FM_C_NAME            0xffdbdbdb
#define FM_C_SECONDARY       0xffa7a7a7
#define FM_C_ACCENT          0xff80cbc4
#define FM_C_CARD            0xff424242
#define FM_C_SELECTED_ROW    0xff00796b
#define FM_C_CAB             0xff002e3e
#define FM_C_CAB_LINE        0xff33b5e5
#define FM_C_RIPPLE          0x50ffffff
#define FM_C_RIPPLE_CARD     0x33ffffff
#define FM_C_CHECK_OFF       0xb3ffffff
#define FM_C_STORAGE_USED    0xff3c87ee
#define FM_C_STORAGE_REST    0xff474f58

/* an ARGB constant (0xAARRGGBB) as a GdkRGBA */
GdkRGBA    fm_rgba (guint32 argb);

/* a label in one of the app's CSS text styles, Android's font padding added; single line, ellipsized at the end
 * unless @wrap (word, then character) */
GtkWidget *fm_label (const char *text, const char *css_class, double size, gboolean wrap);

/* a toolbar icon button: @width x @height, the icon theme's glyph centred at 20 dp (a 16-unit symbolic icon drawn
 * to its edges), a borderless ripple; @name for assistive technology ("Back" is what the shell's back gesture
 * presses) */
GtkWidget *fm_icon_button (const char *icon_name, const char *name, int width, int height);
/* the same with the glyph @glyph_size dp (the path bar's up arrow) */
GtkWidget *fm_icon_button_sized (const char *icon_name, const char *name, int width, int height, int glyph_size);

/* AppCompat's borderless dialog button: 14 sp Medium caps in the accent, 48 dp tall */
GtkWidget *fm_text_button (const char *label);

/* AppCompat's check box and radio button (abc_btn_check_material, abc_btn_radio_material): a 32 dp box, white 70 %
 * when off, the accent when on */
GtkWidget *fm_check_new        (gboolean radio);
void       fm_check_set_active (GtkWidget *check, gboolean active);
gboolean   fm_check_get_active (GtkWidget *check);
/* a row of a check box (or a radio button) and its label that a tap anywhere toggles; "toggled" on the row */
GtkWidget *fm_check_row        (const char *label, gboolean radio, gboolean active);
GtkWidget *fm_check_row_get_check (GtkWidget *row);

/* a thin progress bar: a fraction 0..1, or a fraction < 0 for the moving segments of an indeterminate one
 * (RefreshProgressBar); @height dp tall */
GtkWidget *fm_progress_new          (double height);
void       fm_progress_set_fraction (GtkWidget *progress, double fraction);

/* the popup menu over the window (the overflow ⋮, a row's ⋮, a long press): #424242, 2 dp corners, 48 dp rows of
 * 16 sp white text 16 dp in, at least 196 dp wide. A tap outside, Back or Escape closes it; a tap on a row closes it
 * and then runs the choice (-1 is never given). */
typedef void (*FmPopupChosen) (int index, gpointer data);
typedef enum {
  FM_POPUP_OVER_END,            /* an overflow menu: its top on the anchor's, its end on the anchor's end */
  FM_POPUP_AT,                  /* a context menu: its top-left at (x, y) in the anchor */
} FmPopupPlace;
void       fm_popup_show    (GtkWidget *anchor, const char *const *labels, int n_labels, FmPopupPlace place,
                             double x, double y, FmPopupChosen chosen, gpointer data);
/* closes the menu of @widget's window if one shows; TRUE when it did */
gboolean   fm_popup_dismiss (GtkWidget *widget);

/* the plain upper-cased form of a label (allCaps) */
char      *fm_caps (const char *label);

G_END_DECLS
