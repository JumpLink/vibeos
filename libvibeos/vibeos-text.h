/* vibeos-text: text that follows the system's Font size and Bold text settings.
 *
 * Sizes come in two units:
 *
 *   sp  text that follows Font size. Below a font size of 1.05 (and above 2) its px are plain multiplication; in
 *       between, a curve under which small text grows more than large text, and text of 100 sp and more does not
 *       grow at all.
 *   dp  sizes that never follow Font size (paddings, icons, and text that has to fit a fixed place). One dp is one
 *       GTK px: Display size changes the screen's scale, and with it every dp.
 *
 * Font size is org.gnome.desktop.interface text-scaling-factor, the key GNOME's own apps follow too (linearly).
 * Bold text is vibeos.Interface bold-text: on, weights are 300 heavier (400 is drawn as 700), up to 1000.
 *
 * An app follows both once it asks to, with vibeos_init_full (VIBEOS_INIT_SCALED_TEXT). Until then nothing here
 * changes what it draws: sp is px at a font size of 1 and Bold text is on, as the apps were drawn before they had
 * these settings. So an app can move its sizes to sp and dp first (and look the same), then ask.
 *
 * Its stylesheets go through vibeos_css_load_resource (or _string): "14sp" becomes the px of 14 sp, "16dp" becomes
 * "16px", and each font-weight gets Bold text's 300 when it is on. When a setting changes, every such stylesheet
 * (libvibeos's own too) is converted again, VibeosText::changed is emitted and every window is laid out again in its
 * next frame, with no transition: a widget that sizes text itself (Pango sizes in C) reads vibeos_sp () in the
 * signal, or as it measures.
 *
 * For tests and development, the environment overrides the settings: VIBEOS_FONT_SCALE (0.5 to 3) and
 * VIBEOS_BOLD_TEXT (1 or 0). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_TEXT (vibeos_text_get_type ())
G_DECLARE_FINAL_TYPE (VibeosText, vibeos_text, VIBEOS, TEXT, GObject)

/* The process's text settings. Signal: "changed" (), after the stylesheets were converted again. */
VibeosText *vibeos_text_get_default (void);

/* the font size in effect: the setting in an app that follows it, else 1 */
double   vibeos_font_scale  (void);
/* Bold text in effect: the setting in an app that follows it, else on */
gboolean vibeos_bold_text   (void);

/* the px of `sp` at the font size in effect: for Pango sizes and line heights set in C */
double   vibeos_sp          (double sp);
/* `weight` (100 to 1000) as Bold text in effect draws it */
int      vibeos_font_weight (int weight);

/* the px of `sp` at font size `scale`, whatever the settings say */
double   vibeos_sp_at_scale (double sp, double scale);

/* `css` with every "Nsp" turned into the px of N sp at `scale`, every "Ndp" into N px, and, with `bold`, every
 * font-weight 300 heavier (normal and bold too, as 400 and 700). Nothing else changes, comments and strings
 * included. */
char    *vibeos_css_convert (const char *css, double scale, gboolean bold);

/* Loads a stylesheet with sp and dp sizes for the whole display at `priority`
 * (GTK_STYLE_PROVIDER_PRIORITY_APPLICATION and up), converted for the settings in effect and again whenever they
 * change. The provider belongs to libvibeos and stays for the life of the process. Call after vibeos_init. */
GtkCssProvider *vibeos_css_load_resource (const char *resource_path, guint priority);
GtkCssProvider *vibeos_css_load_string   (const char *css, guint priority);

/* vibeos_label_set_font_padding for text of `sp`, kept right as the font size changes */
void     vibeos_label_set_font_padding_sp (GtkWidget *label, double sp);

G_END_DECLS
