/* Lock screen and Keyboard: what the shell itself follows.
 *
 * The lock screen is VibeOS's keyguard (extension/keyguard.js). It reads vibeos.Shell lock-type at every lock
 * to pick its bouncer: none (swipe), the pattern or the PIN pad. Pattern and PIN are both the user's own password,
 * checked by vibeos-auth through PAM; ns-pattern.c sets it. The keyguard's notification icons follow GNOME's
 * show-in-lock-screen, globally and per app (Notifications).
 *
 * The on-screen keyboard is one of two, vibeos.Shell keyboard: VibeOS's own (extension/keyboard) or GNOME Shell's;
 * the shell swaps them as the key changes. On a touchscreen either opens by itself (keyboard.js: touch mode and the
 * last device a touchscreen); screen-keyboard-enabled keeps it on with a hardware keyboard or a mouse in use too.
 * Its layouts, like a hardware keyboard's, are org.gnome.desktop.input-sources, named from the XKB rules file. */
#include "ns-pages.h"
#include "ns-pattern.h"
#include "ns-ui.h"

/* ---- the lock type ---- */

static const char *const lock_labels[] = { "Swipe", "Pattern", "PIN", NULL };
static const char *const lock_types[] = { "swipe", "pattern", "pin" };

typedef struct {
  GSettings *vibeos;
  GtkWidget *type, *change;
} Lock;

/* the keyguard's own reading: "swipe" and "pattern" by name, anything else is the PIN pad */
static guint
lock_index (Lock *l)
{
  g_autofree char *t = g_settings_get_string (l->vibeos, "lock-type");
  for (guint i = 0; i < G_N_ELEMENTS (lock_types); i++)
    if (!g_strcmp0 (t, lock_types[i]))
      return i;
  return 2;
}

static void
lock_show (Lock *l)
{
  guint i = lock_index (l);
  ns_row_pick_select (l->type, i);
  gtk_widget_set_visible (l->change, i != 0);
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (l->change), "ns-title-label")),
                      i == 1 ? "Change pattern" : "Change PIN");
}

typedef struct {
  Lock      *l;
  guint      i;
  GtkWidget *row;
} LockSet;

/* the pattern (or PIN) dialog is done: the type changes only once the new secret is the password */
static void
secret_done (gboolean ok, gpointer data)
{
  LockSet *ls = data;
  if (ok)
    g_settings_set_string (ls->l->vibeos, "lock-type", lock_types[ls->i]);
  lock_show (ls->l);
  g_object_unref (ls->row);
  g_free (ls);
}

static void
secret_dialog (Lock *l, GtkWidget *row, guint i)
{
  LockSet *ls = g_new0 (LockSet, 1);
  ls->l = l;
  ls->i = i;
  ls->row = g_object_ref (row);
  ns_pattern_dialog_present (row, i == 2, secret_done, ls);
}

static void
lock_picked (GtkWidget *row, guint i, gpointer data)
{
  Lock *l = data;
  if (i == lock_index (l))
    return;
  /* a pattern or a PIN counts once it is entered, confirmed and set as the password; swipe needs none. A PIN does
   * not keep the password there is: the lock screen's PIN pad types digits only, and a password with a letter in it
   * could not be typed there to unlock. */
  if (i != 0) {
    adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
    secret_dialog (l, row, i);
    lock_show (l);
    return;
  }
  g_settings_set_string (l->vibeos, "lock-type", lock_types[i]);
}

static void
change_tapped (GtkWidget *row, gpointer data)
{
  Lock *l = data;
  secret_dialog (l, row, lock_index (l));
}

static void
lock_free (Lock *l)
{
  g_signal_handlers_disconnect_by_data (l->vibeos, l);
  g_object_unref (l->vibeos);
  g_free (l);
}

/* The always-on display is for OLED panels: black costs them nothing, and VibeOS's AOD (extension/aod.js) puts
 * them in their low power mode, where the panel itself keeps the picture at a few nits and ticks the clock. That
 * mode is the `lpm` attribute of the panel's DSI device, which only an OLED panel's driver has (the S10e's); an LCD
 * (the POCO's) would light its whole backlight for a black screen, so it gets no switch (owner, 2026-11-12). */
static gboolean
panel_is_oled (void)
{
  g_autoptr (GDir) d = g_dir_open ("/sys/bus/mipi-dsi/devices", 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    g_autofree char *lpm = g_build_filename ("/sys/bus/mipi-dsi/devices", n, "lpm", NULL);
    if (g_file_test (lpm, G_FILE_TEST_EXISTS))
      return TRUE;
  }
  return FALSE;
}

/* Not on this page, and why:
 * - "Lock automatically" / "Lock after the screen turns off" (org.gnome.desktop.screensaver lock-enabled and
 *   lock-delay): the keyguard locks the moment the screen goes off (power.js goToSleep calls keyguard.lock()). A
 *   delay would need power.js to note when the screen went off and to lock on the next wake only past the delay.
 * - A lock screen wallpaper of its own (the screensaver's picture-uri): the keyguard shows the home screen's
 *   wallpaper, as Shell 51's own lock screen does.
 * - Shuffling the PIN keys (Phosh's shuffle-keypad): the keyguard's keypad is fixed.
 * - Lock screen widgets (Phosh's plugins): the keyguard has none. */
void
ns_page_lock_screen (GtkWidget *row, gpointer data)
{
  GSettings *vibeos = ns_settings ("vibeos.Shell");
  if (!vibeos || !ns_has_key (vibeos, "lock-type")) {
    g_clear_object (&vibeos);
    return;
  }
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Lock screen", &c);
  Lock *l = g_new0 (Lock, 1);
  l->vibeos = vibeos;
  g_object_set_data_full (G_OBJECT (page), "ns-lock", l, (GDestroyNotify) lock_free);

  GtkWidget *g = ns_group (c, NULL);
  l->type = ns_row_pick (g, "Screen lock type", lock_labels, lock_index (l), lock_picked, l);
  l->change = ns_row (g, NULL, NULL, "Change PIN", "It is also your password");
  ns_row_on_activate (l->change, change_tapped, l);
  g_signal_connect_swapped (vibeos, "changed::lock-type", G_CALLBACK (lock_show), l);
  lock_show (l);
  ns_note (c, "Swipe unlocks without a password. Pattern and PIN unlock with your password: a pattern or PIN set "
              "here becomes your password (also for sudo, SSH and the console).");

  g = ns_group (c, "On the lock screen");
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  ns_row_switch (g, "Notifications", "Their apps' icons below the clock", n, "show-in-lock-screen");
  if (ns_has_key (vibeos, "aod") && panel_is_oled ())
    ns_row_switch (g, "Always On Display", "The time, the date and notifications while the screen is off", vibeos,
                   "aod");
  ns_push (page);
}

/* ---- keyboard layouts: org.gnome.desktop.input-sources, named from the XKB rules ---- */

/* the XKB registry every keyboard layout comes from (xkb-data); GNOME's own layout list reads it too */
#define XKB_RULES "/usr/share/X11/xkb/rules/evdev.xml"
/* where each layout's keys are, a file named as the layout */
#define XKB_SYMBOLS "/usr/share/X11/xkb/symbols"

typedef struct {
  char *id, *name;             /* "us" and "English (US)" */
} Layout;

typedef struct {
  GSettings *src;
  GPtrArray *all;              /* every Layout of the rules file, by name */
  GtkWidget *group;
} Layouts;

static void
layout_free (Layout *l)
{
  g_free (l->id);
  g_free (l->name);
  g_free (l);
}

static void
layouts_free (Layouts *l)
{
  g_ptr_array_unref (l->all);
  g_free (l);
}

/* Reading the rules file: <layoutList><layout><configItem><name>us</name><description>English (US)</description>.
 * A layout's variants sit in its <variantList>, with configItems of their own: those are skipped, so the list
 * has the layouts only (about a hundred), not their six hundred variants. */
typedef struct {
  GPtrArray *out;
  int        variants;         /* > 0 while inside a <variantList> */
  GString   *text;             /* the text of the <name> or <description> being read, or NULL */
  char      *id, *name;        /* the current layout's */
} XkbRead;

static void
xkb_start (GMarkupParseContext *ctx, const char *element, const char **names, const char **values, gpointer data,
           GError **error)
{
  XkbRead *r = data;
  if (!g_strcmp0 (element, "variantList"))
    r->variants++;
  else if (!r->variants && (!g_strcmp0 (element, "name") || !g_strcmp0 (element, "description")))
    r->text = g_string_new (NULL);
}

static void
xkb_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  XkbRead *r = data;
  if (r->text)
    g_string_append_len (r->text, text, len);
}

static void
xkb_end (GMarkupParseContext *ctx, const char *element, gpointer data, GError **error)
{
  XkbRead *r = data;
  if (!g_strcmp0 (element, "variantList")) {
    r->variants--;
  } else if (r->text && !g_strcmp0 (element, "name")) {
    g_free (r->id);
    r->id = g_string_free (g_steal_pointer (&r->text), FALSE);
  } else if (r->text && !g_strcmp0 (element, "description")) {
    g_free (r->name);
    r->name = g_string_free (g_steal_pointer (&r->text), FALSE);
  } else if (!g_strcmp0 (element, "layout")) {
    /* The layout's own configItem came first, its variants after: what is held is the layout's. A layout with no
     * symbols file could not be used, and the registry lists one: "custom", for a file of the user's own. */
    g_autofree char *symbols = r->id ? g_build_filename (XKB_SYMBOLS, r->id, NULL) : NULL;
    if (r->id && r->name && g_file_test (symbols, G_FILE_TEST_EXISTS)) {
      Layout *l = g_new0 (Layout, 1);
      l->id = g_steal_pointer (&r->id);
      l->name = g_steal_pointer (&r->name);
      g_ptr_array_add (r->out, l);
    }
    g_clear_pointer (&r->id, g_free);
    g_clear_pointer (&r->name, g_free);
  }
}

static gint
layout_by_name (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(Layout **) a)->name, (*(Layout **) b)->name);
}

static GPtrArray *
xkb_layouts (void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) layout_free);
  g_autofree char *xml = NULL;
  gsize len;
  if (!g_file_get_contents (XKB_RULES, &xml, &len, NULL))
    return out;
  static const GMarkupParser parser = { xkb_start, xkb_end, xkb_text, NULL, NULL };
  XkbRead r = { .out = out };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &r, NULL);
  if (!g_markup_parse_context_parse (ctx, xml, len, NULL))
    g_warning ("%s: not read whole", XKB_RULES);
  g_markup_parse_context_free (ctx);
  if (r.text)
    g_string_free (r.text, TRUE);
  g_free (r.id);
  g_free (r.name);
  g_ptr_array_sort (out, layout_by_name);
  return out;
}

/* a source's name: the layout's description, or its id (a variant, "de+nodeadkeys", or an input method) */
static const char *
layout_name (Layouts *l, const char *type, const char *id)
{
  if (!g_strcmp0 (type, "xkb"))
    for (guint i = 0; i < l->all->len; i++)
      if (!g_strcmp0 (((Layout *) l->all->pdata[i])->id, id))
        return ((Layout *) l->all->pdata[i])->name;
  return id;
}

static void layouts_fill (AdwNavigationPage *page);

static void
layout_remove (GtkButton *btn, AdwNavigationPage *page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  guint drop = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (btn), "ns-index"));
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < g_variant_n_children (v); i++)
    if (i != drop)
      g_variant_builder_add_value (&b, g_variant_get_child_value (v, i));
  g_settings_set_value (l->src, "sources", g_variant_builder_end (&b));
}

/* a layout picked on the "Add a layout" page: appended, so the first one stays the one used first */
static void
layout_added (GtkWidget *row, gpointer data)
{
  AdwNavigationPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  Layout *add = l->all->pdata[GPOINTER_TO_UINT (data)];
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < g_variant_n_children (v); i++)
    g_variant_builder_add_value (&b, g_variant_get_child_value (v, i));
  g_variant_builder_add (&b, "(ss)", "xkb", add->id);
  g_settings_set_value (l->src, "sources", g_variant_builder_end (&b));
  adw_navigation_view_pop (ADW_NAVIGATION_VIEW (gtk_widget_get_ancestor (row, ADW_TYPE_NAVIGATION_VIEW)));
}

static gboolean
has_source (GVariant *sources, const char *id)
{
  for (guint k = 0; k < g_variant_n_children (sources); k++) {
    const char *t, *have;
    g_variant_get_child (sources, k, "(&s&s)", &t, &have);
    if (!g_strcmp0 (t, "xkb") && !g_strcmp0 (have, id))
      return TRUE;
  }
  return FALSE;
}

static void
layout_add (GtkWidget *row, gpointer page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  GtkWidget *c;
  AdwNavigationPage *add = ns_page_new ("Add a layout", &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  for (guint i = 0; i < l->all->len; i++) {
    Layout *one = l->all->pdata[i];
    if (has_source (v, one->id))
      continue;
    GtkWidget *r = ns_row (g, NULL, NULL, one->name, NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, layout_added, GUINT_TO_POINTER (i));
  }
  ns_push (add);
}

static void
layouts_fill (AdwNavigationPage *page)
{
  Layouts *l = g_object_get_data (G_OBJECT (page), "ns-layouts");
  ns_group_clear (l->group);
  g_autoptr (GVariant) v = g_settings_get_value (l->src, "sources");
  guint n = g_variant_n_children (v);
  for (guint i = 0; i < n; i++) {
    const char *t, *id;
    g_variant_get_child (v, i, "(&s&s)", &t, &id);
    GtkWidget *r = ns_row (l->group, NULL, NULL, layout_name (l, t, id), i == 0 ? "Used first" : NULL);
    if (n > 1) {
      GtkWidget *x = gtk_button_new_from_icon_name ("user-trash-symbolic");
      gtk_widget_add_css_class (x, "flat");
      gtk_widget_add_css_class (x, "circular");
      gtk_widget_set_tooltip_text (x, "Remove");
      g_object_set_data (G_OBJECT (x), "ns-index", GUINT_TO_POINTER (i));
      g_signal_connect (x, "clicked", G_CALLBACK (layout_remove), page);
      ns_row_suffix (r, x);
    }
  }
  if (l->all->len)
    ns_row_button (l->group, "Add a layout", FALSE, layout_add, page);
}

static void
layouts_rows (AdwNavigationPage *page, GtkWidget *c)
{
  GSettings *src = ns_page_settings (page, "org.gnome.desktop.input-sources");
  if (!src)
    return;
  Layouts *l = g_new0 (Layouts, 1);
  l->src = src;
  l->all = xkb_layouts ();
  g_object_set_data_full (G_OBJECT (page), "ns-layouts", l, (GDestroyNotify) layouts_free);
  l->group = ns_group (c, "Layouts");
  ns_later_init (page, layouts_fill);
  g_signal_connect_object (src, "changed::sources", G_CALLBACK (ns_later), page, G_CONNECT_SWAPPED);
  layouts_fill (page);
}

/* Not on this page, and why: Phosh's keyboards (Stevia or Squeekboard), their word suggestions and sizes, and how
 * long a press on the home bar opens one (Phosh's osk-unfold-delay). The shell's keyboards replaced them all, and
 * have no settings of their own besides which one, the switch and the layouts. */
void
ns_page_keyboard (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  AdwNavigationPage *page = ns_page_new ("Keyboard", &c);
  GSettings *shell = ns_page_settings (page, "vibeos.Shell");
  if (ns_has_key (shell, "keyboard")) {
    static const char *const keyboards[] = { "vibeos", "gnome", NULL };
    static const char *const keyboard_labels[] = { "VibeOS keyboard", "GNOME keyboard", NULL };
    GtkWidget *g = ns_group (c, NULL);
    ns_row_choice (g, "On-screen keyboard", shell, "keyboard", keyboards, keyboard_labels);
  }
  GSettings *a11y = ns_page_settings (page, "org.gnome.desktop.a11y.applications");
  if (ns_has_key (a11y, "screen-keyboard-enabled")) {
    GtkWidget *g = ns_group (c, NULL);
    ns_row_switch (g, "Show on-screen keyboard", "Also while a hardware keyboard or a mouse is used", a11y,
                   "screen-keyboard-enabled");
    ns_note (c, "Typing on the touchscreen opens the on-screen keyboard either way.");
  }
  layouts_rows (page, c);
  ns_push (page);
}
