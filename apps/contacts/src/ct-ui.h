/* Small pieces every Contacts screen uses: the theme's colours, Roboto text in line boxes with font padding (the
 * Dialer's text.c), the icon buttons, the checkbox, and handing a tel:, sms:, mailto: or web address to the app that
 * takes it. */
#pragma once

#include "ct-constants.h"
#include "ct-store.h"
#include "text.h"

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

/* the cards the lists, the pages and the sheets are made of */
#define CT_CARD VIBEOS_SURFACE_BRIGHT

/* Roboto at an effective weight (Bold text adds 300: regular 700, medium 800, bold 900) */
#define CT_TEXT(size, weight) ((TextStyle) { TEXT_ROBOTO, (size), (weight) })

/* An icon button: @width x @height, the icon centred, an unbounded ripple with a radius of half the button's
 * diagonal, and @name for assistive technology. "Back" is the name the shell's back gesture presses. */
GtkWidget *ct_icon_button (const char *icon_name, const char *name, double width, double height);

/* An icon in one of the theme's colours, read as it draws (so it follows a light/dark change), for the places a
 * style sheet does not reach. @size px square. */
GtkWidget *ct_tinted_icon_new (const char *icon_name, double size, VibeosColor role);
void       ct_tinted_icon_set (GtkWidget *icon, const char *icon_name, VibeosColor role);

/* a GtkLabel in one of the app's CSS text styles, the font padding added (vibeos_label_set_font_padding) */
GtkWidget *ct_label (const char *text, const char *css_class, double size);
/* a label without the font padding: its box is the line's, as the list screen's text is placed */
GtkWidget *ct_text (const char *text, const char *css_class);

/* A borderless text button: a dialog's or the empty view's, with a bounded ripple. The label is upper-cased. */
GtkWidget *ct_text_button (const char *label, const char *css_class);

/* A checkbox 32 px square at (x, y), its top-left: a rounded square outlined in the secondary text colour, or
 * filled with the primary colour and the check mark drawn over it */
void       ct_snapshot_checkbox (GtkSnapshot *snapshot, double x, double y, gboolean checked);

/* the same check box as a widget */
GtkWidget *ct_check_new        (void);
void       ct_check_set_active (GtkWidget *check, gboolean active);
gboolean   ct_check_get_active (GtkWidget *check);

/* whether an app takes @scheme ("tel", "sms", "mailto", "geo", "sip"); an action nothing could take is not shown */
gboolean   ct_can_open_scheme (const char *scheme);
/* hands @uri to its app; FALSE (and a toast from the caller) when none takes it */
gboolean   ct_open_uri (GtkWidget *widget, const char *uri);

/* text into the clipboard (QuickContact's long press: "Copy to clipboard") */
void       ct_copy_text (GtkWidget *widget, const char *text);

/* Settings' choices: "Name format" is the order names show in, "Sort by" the order the list is in */
CtNameOrder ct_display_order (GSettings *settings);
CtNameOrder ct_sort_order    (GSettings *settings);
/* "Contacts to display": whether the contacts of @book_uid are in the list (all, or the books Customize kept) */
gboolean    ct_book_shown    (GSettings *settings, const char *book_uid);
/* the contacts the list is for: those "Contacts to display" keeps (CtEntry, owned) */
GPtrArray  *ct_shown_entries (CtStore *store, GSettings *settings);
/* ICU's {count, plural, =1 {# contact} other {# contacts}} for the two forms the app has */
char       *ct_contacts_count (guint count, const char *account);

G_END_DECLS
