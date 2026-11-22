/* System > Gestures: what a double press of the power key and the side button open. Laid out as Android 17's
 * Gestures and its "Double-press power button" page: a row per button saying what it opens ("On / Open Camera"),
 * and on the button's page a "Use double-press" switch over the list of what it can open, the one it opens marked.
 *
 * The choice is the shell's, in vibeos.Shell power-double-press-app and side-key-app (extension/keylaunch.js reads
 * them at each press): an app's desktop file id, 'recents' (the side button only), or '' for off. An app that is not
 * installed opens nothing (the double press, shown off) or recent apps (the side button). The side button's row is
 * there only on a device with such a key: an input device that reports KEY_APPSELECT (ns-caps.c). */
#include "ns-caps.h"
#include "ns-pages.h"

#include <gio/gdesktopappinfo.h>

#define SHELL_SCHEMA "vibeos.Shell"
#define RECENTS      "recents"

typedef struct {
  const char *key;        /* vibeos.Shell's */
  const char *title;      /* the Gestures row's and the page's */
  const char *use;        /* the page's switch */
  const char *open;       /* over the list */
  gboolean    recents;    /* recent apps is a choice (and what an app that is gone falls back to) */
  NsCap       needs;      /* what the device must have for the row */
  const char *note;       /* the footnote, %s the device's noun */
} Button;

static const Button buttons[] = {
  { "power-double-press-app", "Double-press power button", "Use double-press", "Double-press power button to open",
    FALSE, 0, "Press the power button twice quickly, with the screen on or off. On a locked %s, the app opens once "
              "you unlock it." },
  { "side-key-app", "Side button", "Use side button", "Press side button to open", TRUE, NS_CAP_SIDE_KEY,
    "Works with the screen on. On a locked %s, an app opens once you unlock it." },
};

static GDesktopAppInfo *
app_for (const char *id)
{
  return id && *id && !g_str_equal (id, RECENTS) ? g_desktop_app_info_new (id) : NULL;
}

/* what the button does now, as the shell sees it: a desktop id, RECENTS, or NULL for nothing */
static char *
in_effect (const Button *b, GSettings *s)
{
  g_autofree char *v = g_settings_get_string (s, b->key);
  if (!*v)
    return NULL;
  if (b->recents && g_str_equal (v, RECENTS))
    return g_strdup (RECENTS);
  g_autoptr (GDesktopAppInfo) app = app_for (v);
  if (app)
    return g_steal_pointer (&v);
  return b->recents ? g_strdup (RECENTS) : NULL;
}

static char *
summary (const Button *b, GSettings *s)
{
  g_autofree char *v = in_effect (b, s);
  if (!v)
    return g_strdup ("Off");
  if (g_str_equal (v, RECENTS))
    return g_strdup ("On / Recent apps");
  g_autoptr (GDesktopAppInfo) app = app_for (v);
  return g_strdup_printf ("On / Open %s", g_app_info_get_display_name (G_APP_INFO (app)));
}

/* ---- a button's page: the switch, and the list of what it opens ---- */

typedef struct {
  const Button *b;
  GSettings    *s;
  GtkWidget    *use;      /* the switch's row */
  GtkWidget    *list;     /* the card of choices */
  GPtrArray    *rows;     /* the choices' rows, each with its "ns-value" and "ns-mark" */
  char         *last;     /* the choice in effect last, which the switch turned on again brings back */
} Picker;

static void
picker_free (Picker *p)
{
  g_ptr_array_unref (p->rows);
  g_free (p->last);
  g_free (p);
}

static gboolean
is_choice (Picker *p, const char *v)
{
  for (guint i = 0; v && i < p->rows->len; i++)
    if (!g_strcmp0 (g_object_get_data (p->rows->pdata[i], "ns-value"), v))
      return TRUE;
  return FALSE;
}

/* the switch, the list's marks and its being enabled, as the key is now */
static void
picker_sync (Picker *p)
{
  g_autofree char *v = in_effect (p->b, p->s);
  if (v) {
    g_free (p->last);
    p->last = g_strdup (v);
  }
  ns_switch_quiet (ns_row_get_switch (p->use), v != NULL);
  gtk_widget_set_sensitive (p->list, v != NULL);
  for (guint i = 0; i < p->rows->len; i++) {
    GtkWidget *mark = g_object_get_data (p->rows->pdata[i], "ns-mark");
    gtk_widget_set_opacity (mark, v && !g_strcmp0 (g_object_get_data (p->rows->pdata[i], "ns-value"), v));
  }
}

static void
key_changed (NsPage *page)
{
  picker_sync (g_object_get_data (G_OBJECT (page), "ns-picker"));
}

/* the switch turned on: the choice it had last, else the key's default, else the camera, recent apps, the first */
static char *
first_choice (Picker *p)
{
  if (is_choice (p, p->last))
    return g_strdup (p->last);
  g_autoptr (GVariant) def = g_settings_get_default_value (p->s, p->b->key);
  const char *v = def ? g_variant_get_string (def, NULL) : NULL;
  if (is_choice (p, v))
    return g_strdup (v);
  if (is_choice (p, "vibeos.Camera.desktop"))
    return g_strdup ("vibeos.Camera.desktop");
  if (is_choice (p, RECENTS))
    return g_strdup (RECENTS);
  return g_strdup (p->rows->len ? g_object_get_data (p->rows->pdata[0], "ns-value") : "");
}

static void
use_toggled (GtkWidget *sw, GParamSpec *ps, Picker *p)
{
  if (ns_switch_syncing (sw))
    return;
  g_autofree char *v = ns_switch_get_active (sw) ? first_choice (p) : g_strdup ("");
  g_settings_set_string (p->s, p->b->key, v);
  picker_sync (p);
}

static void
choice_tapped (GtkWidget *row, gpointer data)
{
  Picker *p = data;
  g_settings_set_string (p->s, p->b->key, g_object_get_data (G_OBJECT (row), "ns-value"));
  picker_sync (p);
}

static void
add_choice (Picker *p, GtkWidget *row, const char *value)
{
  g_object_set_data_full (G_OBJECT (row), "ns-value", g_strdup (value), g_free);
  GtkWidget *mark = ns_mark_new (FALSE);
  ns_row_suffix (row, mark);
  g_object_set_data (G_OBJECT (row), "ns-mark", mark);
  ns_row_on_activate (row, choice_tapped, p);
  g_ptr_array_add (p->rows, row);
}

static void
button_page (GtkWidget *row, gpointer data)
{
  const Button *b = data;
  GtkWidget *c;
  NsPage *page = ns_page_new (b->title, &c);
  GSettings *s = ns_page_settings (page, SHELL_SCHEMA);
  if (!s || !ns_has_key (s, b->key)) {
    ns_push (page);
    return;
  }
  Picker *p = g_new0 (Picker, 1);
  p->b = b;
  p->s = s;
  p->rows = g_ptr_array_new ();
  g_object_set_data_full (G_OBJECT (page), "ns-picker", p, (GDestroyNotify) picker_free);

  GtkWidget *g = ns_group (c, NULL);
  p->use = ns_row_toggle (g, NULL, b->use, NULL);
  g_signal_connect (ns_row_get_switch (p->use), "notify::active", G_CALLBACK (use_toggled), p);

  p->list = ns_group (c, b->open);
  if (b->recents) {
    GtkWidget *r = ns_row (p->list, NULL, "Recent apps", NULL);
    /* in an app icon's place, a circle as the main menu's: dark, the glyph white */
    GdkRGBA fill, glyph = { 1, 1, 1, 1 };
    gdk_rgba_parse (&fill, "#6c6c80");
    vibeos_list_row_set_icon_circle (VIBEOS_LIST_ROW (r), "view-dual-symbolic", &fill, &glyph);
    add_choice (p, r, RECENTS);
  }
  g_autoptr (GPtrArray) icons = g_ptr_array_new ();
  GList *apps = ns_apps_shown ();
  for (GList *l = apps; l; l = l->next) {
    GtkWidget *r = ns_row_app (p->list, l->data, NULL);
    g_ptr_array_add (icons, g_object_get_data (G_OBJECT (r), "ns-icon"));
    add_choice (p, r, g_app_info_get_id (l->data));
  }
  g_list_free_full (apps, g_object_unref);
  ns_apps_bake_icons (icons);
  g_autofree char *note = g_strdup_printf (b->note, ns_device_noun ());
  ns_footnote (c, note);

  picker_sync (p);
  g_autofree char *sig = g_strconcat ("changed::", b->key, NULL);
  g_signal_connect_object (s, sig, G_CALLBACK (key_changed), page, G_CONNECT_SWAPPED);
  ns_push (page);
}

/* ---- Gestures ---- */

static void
row_sync (GtkWidget *row)
{
  const Button *b = g_object_get_data (G_OBJECT (row), "ns-button");
  GSettings *s = g_object_get_data (G_OBJECT (row), "ns-settings");
  g_autofree char *text = summary (b, s);
  ns_row_set_subtitle (row, text);
}

/* the row says what the button opens again when its page closes: an app can be installed or removed meanwhile */
static void
gestures_back (NsPage *page)
{
  GPtrArray *rows = g_object_get_data (G_OBJECT (page), "ns-rows");
  for (guint i = 0; i < rows->len; i++)
    row_sync (rows->pdata[i]);
}

gboolean
ns_gestures_available (void)
{
  g_autoptr (GSettings) s = ns_settings (SHELL_SCHEMA);
  return s && ns_has_key (s, buttons[0].key);
}

void
ns_page_gestures (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Gestures", &c);
  GSettings *s = ns_page_settings (page, SHELL_SCHEMA);
  GPtrArray *rows = g_ptr_array_new ();
  g_object_set_data_full (G_OBJECT (page), "ns-rows", rows, (GDestroyNotify) g_ptr_array_unref);
  GtkWidget *g = ns_group (c, NULL);
  for (guint i = 0; s && i < G_N_ELEMENTS (buttons); i++) {
    const Button *b = &buttons[i];
    if (!ns_has_key (s, b->key) || (b->needs && !ns_caps_has (b->needs)))
      continue;
    GtkWidget *r = ns_row (g, NULL, b->title, NULL);
    g_object_set_data (G_OBJECT (r), "ns-button", (gpointer) b);
    g_object_set_data (G_OBJECT (r), "ns-settings", s);
    ns_row_on_activate (r, button_page, (gpointer) b);
    row_sync (r);
    g_autofree char *sig = g_strconcat ("changed::", b->key, NULL);
    g_signal_connect_object (s, sig, G_CALLBACK (row_sync), r, G_CONNECT_SWAPPED);
    g_ptr_array_add (rows, r);
  }
  ns_page_on_back (page, gestures_back);
  ns_push (page);
}
