/* An app's page: GET /apps/{id}, everything on it in one request.
 *
 * Its head (icon, name, developer) draws at once from the row that was tapped, while the rest loads. Then:
 *
 *   the action      Install; Open (and Uninstall) when installed; Update when the store has a newer version; while a
 *                   job runs, what App Installer is doing, its progress and Cancel (while it can still stop). All of
 *                   it through App Installer (sw-installer.h); Back leaves the page and the job goes on.
 *   the figures     rating, installs, download size, age rating
 *   screenshots     sideways
 *   about           the description, its first lines with Read more
 *   what's new      the latest release's notes
 *   permissions     what the app declares, as the store lists it. VibeOS enforces them: an app installed from here
 *                   runs sandboxed (vibeos-sandbox), and the user allows or denies each permission in Settings. Once
 *                   the app is installed, a row here opens its page in Settings. VibeOS's own apps are part of the
 *                   system and not sandboxed, and the section says so
 *   reviews         the average and how the stars spread, rating and writing (the store decides who may: the reason
 *                   it gives shows when the account may not), the most helpful reviews, all reviews
 *   similar apps
 *   information     version, sizes, licence, source, homepage, package */
#include "sw-pages.h"
#include "sw-account.h"
#include "sw-device.h"
#include "sw-image.h"
#include "sw-installer.h"
#include "sw-json.h"
#include "sw-menu.h"
#include "sw-updates.h"
#include "sw-viewer.h"
#include "sw-widgets.h"
#include "sw-window.h"

#include <gio/gdesktopappinfo.h>
#include <math.h>

/* the page's column on a tablet: the head, the Install button and the details in the middle, rather than a button
 * across the whole screen. A phone is narrower, so there the column keeps the screen's width */
#define PAGE_MAX 840

typedef struct {
  GtkWidget  *page, *status, *column, *icon, *ring, *name, *developer;
  GtkWidget  *action, *details, *review_box, *previews, *more_button;
  char       *id;
  JsonObject *summary, *app, *config, *my;
  GAppInfo   *launcher;
  gboolean    launcher_known;
  GtkWidget  *permission_link;  /* the card with the row to the app's page in Settings */
  guint       show_tick;     /* the details wait for the page's transition to end (and then fade in: the status) */
  guint       later_tick;    /* the sections below the first screen, built a frame apart */
  guint       built;         /* how many of those are in */
} Page;

static void load (Page *p);
static void update_action (Page *p);
static void update_permission_link (Page *p);
static void update_review_box (Page *p);
static void schedule_show (Page *p);

static const char *
package_of (Page *p)
{
  const char *package = sw_json_string (p->app, "package");

  return package ? package : sw_json_string (p->summary, "package");
}

static const char *
name_of (Page *p)
{
  const char *name = sw_json_string (p->app, "name");

  return name ? name : sw_json_string (p->summary, "name");
}

static void
page_free (gpointer data)
{
  Page *p = data;

  g_free (p->id);
  g_clear_pointer (&p->summary, json_object_unref);
  g_clear_pointer (&p->app, json_object_unref);
  g_clear_pointer (&p->config, json_object_unref);
  g_clear_pointer (&p->my, json_object_unref);
  g_clear_object (&p->launcher);
  g_free (p);
}

/* ---- the head and the action ----------------------------------------------------------------------------------- */

static void
head_update (Page *p, JsonObject *app)
{
  const char *developer = sw_json_string (app, "developer");

  gtk_label_set_text (GTK_LABEL (p->name), sw_json_string (app, "name"));
  sw_image_set_url (SW_IMAGE (p->icon), sw_app_icon_url (app, 192));
  gtk_label_set_text (GTK_LABEL (p->developer), developer ? developer : "");
  gtk_widget_set_visible (p->developer, developer != NULL);
}

static void
on_launcher (GAppInfo *info, gpointer data)
{
  Page *p = data;

  g_set_object (&p->launcher, info);
  p->launcher_known = TRUE;
  update_action (p);
  update_permission_link (p);
}

static void
find_launcher (Page *p)
{
  p->launcher_known = FALSE;
  sw_installer_find_app (sw_installer_get_default (), p->id, package_of (p), sw_cancellable_for (p->page), on_launcher,
                         p);
}

static void
on_install (GtkWidget *button, Page *p)
{
  sw_installer_install (sw_installer_get_default (), package_of (p), name_of (p), button);
}

static void
on_open (GtkWidget *button, Page *p)
{
  if (p->launcher && sw_installer_launch (p->launcher, button))
    return;
  sw_window_toast (button, "This app has no window of its own to open.");
}

static void
on_cancel (GtkWidget *button, Page *p)
{
  sw_installer_cancel (sw_installer_get_default (), package_of (p));
}

static void
on_uninstall_response (VibeosDialog *dialog, int response, Page *p)
{
  if (response == VIBEOS_RESPONSE_OK)
    sw_installer_remove (sw_installer_get_default (), package_of (p), name_of (p), p->page);
}

static void
on_uninstall (GtkWidget *button, Page *p)
{
  g_autofree char *title = g_strdup_printf ("Uninstall %s?", name_of (p));
  VibeosDialog *dialog = vibeos_dialog_new (title);

  vibeos_dialog_set_message (dialog, "The app goes. What it keeps in your home folder stays.");
  vibeos_dialog_add_button (dialog, "Cancel", VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, "Uninstall", VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_uninstall_response), p);
  vibeos_dialog_present (dialog, p->page);
}

static GtkWidget *
action_button (const char *label, SwButtonStyle style, GCallback callback, Page *p)
{
  GtkWidget *button = sw_button_new (label, style);

  gtk_widget_set_hexpand (button, TRUE);
  gtk_widget_add_css_class (button, "large");
  g_signal_connect (button, "clicked", callback, p);
  return button;
}

static void
update_action (Page *p)
{
  const char *package = package_of (p);
  SwAppState state = sw_app_state (package);
  GtkWidget *child, *row;
  gboolean system = sw_json_bool (p->app, "system_app", FALSE);

  while ((child = gtk_widget_get_first_child (p->action)))
    gtk_box_remove (GTK_BOX (p->action), child);
  gtk_widget_set_visible (p->ring, state == SW_APP_BUSY);
  if (!package)
    return;

  if (state == SW_APP_BUSY)
    {
      const SwJob *job = sw_installer_get_job (sw_installer_get_default (), package);
      g_autofree char *text = sw_job_describe (job);
      GtkWidget *line = sw_label_new (text, "sw-job-line"), *bar = sw_progress_bar_new (4), *cancel;
      int percent = job->state == SW_JOB_RUNNING ? job->percent : -1;

      sw_progress_set_percent (SW_PROGRESS (p->ring), percent);
      sw_progress_set_percent (SW_PROGRESS (bar), percent);
      gtk_box_append (GTK_BOX (p->action), line);
      row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
      gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (row), bar);
      cancel = sw_button_new ("Cancel", SW_BUTTON_TONAL);
      gtk_widget_set_sensitive (cancel, sw_job_can_cancel (job));
      g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), p);
      gtk_box_append (GTK_BOX (row), cancel);
      gtk_box_append (GTK_BOX (p->action), row);
      return;
    }

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append (GTK_BOX (p->action), row);
  if (state == SW_APP_NOT_INSTALLED)
    {
      gtk_box_append (GTK_BOX (row), action_button ("Install", SW_BUTTON_FILLED, G_CALLBACK (on_install), p));
      return;
    }
  /* installed: Uninstall (not for VibeOS's own apps), and Update or Open */
  if (!system)
    gtk_box_append (GTK_BOX (row), action_button ("Uninstall", SW_BUTTON_TONAL, G_CALLBACK (on_uninstall), p));
  if (state == SW_APP_UPDATABLE)
    gtk_box_append (GTK_BOX (row), action_button ("Update", SW_BUTTON_FILLED, G_CALLBACK (on_install), p));
  else if (!p->launcher_known || p->launcher)
    gtk_box_append (GTK_BOX (row), action_button ("Open", SW_BUTTON_FILLED, G_CALLBACK (on_open), p));
  else if (system)
    gtk_box_append (GTK_BOX (row), sw_label_new ("Installed", "sw-installed-note"));
  if (state == SW_APP_UPDATABLE)
    {
      JsonObject *update = sw_updates_get (sw_updates_get_default (), package);
      g_autofree char *size = sw_format_size (sw_json_int (update, "download_size", 0));
      g_autofree char *text = g_strdup_printf ("Version %s is out%s%s", sw_json_string (update, "version"),
                                               *size ? " · " : "", size);

      gtk_box_append (GTK_BOX (p->action), sw_label_new (text, "sw-update-note"));
    }
}

static void
on_installer_changed (SwInstaller *installer, const char *package, GtkWidget *page)
{
  Page *p = g_object_get_data (G_OBJECT (page), "sw-app-page");

  if (package && g_strcmp0 (package, package_of (p)))
    return;
  /* the installed list changed: the app may have a launcher now */
  if (!package && sw_installer_get_installed (installer, package_of (p)) && !p->launcher)
    find_launcher (p);
  update_action (p);
  update_permission_link (p);
}

static void
on_updates_changed (SwUpdates *updates, GtkWidget *page)
{
  update_action (g_object_get_data (G_OBJECT (page), "sw-app-page"));
}

/* ---- the details ----------------------------------------------------------------------------------------------- */

static GtkWidget *
section (GtkWidget *column, const char *title)
{
  GtkWidget *header = sw_section_header_new (title, NULL, NULL, NULL, NULL);

  gtk_box_append (GTK_BOX (column), header);
  return header;
}

static GtkWidget *
stat (const char *value, const char *icon, const char *caption)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2), *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *label = gtk_label_new (value), *under = gtk_label_new (caption);

  gtk_widget_add_css_class (box, "sw-stat");
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_halign (top, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (label, "sw-stat-value");
  gtk_box_append (GTK_BOX (top), label);
  if (icon)
    {
      GtkWidget *image = gtk_image_new_from_icon_name (icon);

      gtk_image_set_pixel_size (GTK_IMAGE (image), 14);
      gtk_box_append (GTK_BOX (top), image);
    }
  gtk_box_append (GTK_BOX (box), top);
  gtk_label_set_ellipsize (GTK_LABEL (under), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (under, "sw-stat-caption");
  gtk_box_append (GTK_BOX (box), under);
  return box;
}

static GtkWidget *
stats (Page *p)
{
  JsonObject *rating = sw_json_object (p->app, "rating"), *installs = sw_json_object (p->app, "installs");
  JsonObject *package = sw_app_package_for_arch (p->app, sw_device_arch ());
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  double average = sw_json_double (rating, "average");
  gint64 count = sw_json_int (rating, "count", 0);
  g_autofree char *value = NULL, *caption = NULL, *size = NULL;
  const char *age = sw_json_string (p->app, "age_rating");

  gtk_widget_add_css_class (row, "sw-stats");
  value = isnan (average) ? g_strdup ("–") : sw_format_rating (average);
  caption = sw_format_ratings_count (count);
  gtk_box_append (GTK_BOX (row), stat (value, isnan (average) ? NULL : "sw-star-symbolic", caption));
  gtk_box_append (GTK_BOX (row), stat (sw_json_string (installs, "label") ? sw_json_string (installs, "label") : "0",
                                       NULL, "installs"));
  size = sw_format_size (sw_json_int (package, "download_size", 0));
  if (*size)
    gtk_box_append (GTK_BOX (row), stat (size, NULL, "download"));
  if (age)
    gtk_box_append (GTK_BOX (row), stat (age, NULL, "age rating"));
  return row;
}

/* a screenshot tapped: all of them full screen, from that one */
static void
on_screenshot_clicked (GtkWidget *button, GtkWidget *strip)
{
  JsonArray *list = g_object_get_data (G_OBJECT (strip), "sw-shots");
  GPtrArray *thumbs = g_object_get_data (G_OBJECT (strip), "sw-thumbs");
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "sw-index"));

  if (list && thumbs && index < thumbs->len)
    sw_viewer_present (g_ptr_array_index (thumbs, index), list, index, thumbs);
}

static GtkWidget *
screenshots (JsonArray *list)
{
  GtkWidget *box, *strip = sw_strip_new (&box);
  GPtrArray *thumbs = g_ptr_array_new ();
  guint n = json_array_get_length (list);

  gtk_widget_add_css_class (box, "sw-screenshots");
  for (guint i = 0; i < n; i++)
    {
      JsonObject *shot = sw_json_array_object (list, i);
      double w = sw_json_double (shot, "width"), h = sw_json_double (shot, "height");
      /* 220 tall, as wide as the picture's shape makes it (phone-shaped pictures narrow, desktop ones wide) */
      int height = 220, width = (int) (w > 0 && h > 0 ? height * w / h : height * 0.5);
      GtkWidget *image = sw_image_new (MIN (width, 320), height, 12), *button;
      const char *caption = sw_json_string (shot, "caption");
      g_autofree char *label = caption ? g_strdup (caption) : g_strdup_printf ("Screenshot %u of %u", i + 1, n);

      sw_image_set_url (SW_IMAGE (image), sw_json_string (shot, "url"));
      button = vibeos_pressable_new (image);
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 12);
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
      g_object_set_data (G_OBJECT (button), "sw-index", GUINT_TO_POINTER (i));
      g_signal_connect (button, "clicked", G_CALLBACK (on_screenshot_clicked), strip);
      gtk_box_append (GTK_BOX (box), button);
      /* the strip holds the thumbnails in its box: they live as long as it does */
      g_ptr_array_add (thumbs, image);
    }
  g_object_set_data_full (G_OBJECT (strip), "sw-shots", json_array_ref (list), (GDestroyNotify) json_array_unref);
  g_object_set_data_full (G_OBJECT (strip), "sw-thumbs", thumbs, (GDestroyNotify) g_ptr_array_unref);
  return strip;
}

/* the API's text blocks: paragraphs, bullets and numbered lists */
static void
add_blocks (GtkWidget *box, JsonArray *blocks)
{
  for (guint i = 0; blocks && i < json_array_get_length (blocks); i++)
    {
      JsonObject *block = sw_json_array_object (blocks, i);
      const char *type = sw_json_string (block, "type");
      JsonArray *items = sw_json_array (block, "items");

      if (!g_strcmp0 (type, "paragraph") && sw_json_string (block, "text"))
        gtk_box_append (GTK_BOX (box), sw_label_new (sw_json_string (block, "text"), "sw-body-text"));
      else if (items)
        for (guint j = 0; j < json_array_get_length (items); j++)
          {
            JsonNode *node = json_array_get_element (items, j);
            const char *item = JSON_NODE_HOLDS_VALUE (node) ? json_node_get_string (node) : NULL;
            g_autofree char *line = NULL;

            if (!item)
              continue;
            line = !g_strcmp0 (type, "numbered") ? g_strdup_printf ("%u.  %s", j + 1, item)
                                                 : g_strdup_printf ("•  %s", item);
            gtk_box_append (GTK_BOX (box), sw_label_new (line, "sw-body-item"));
          }
    }
}

static void
on_read_more (GtkWidget *button, GtkWidget *revealer)
{
  gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), TRUE);
  gtk_widget_set_visible (button, FALSE);
}

static void
description (Page *p)
{
  JsonArray *blocks = sw_json_array (p->app, "description");
  GtkWidget *first = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8), *rest = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *revealer = gtk_revealer_new ();
  g_autoptr (JsonArray) head = json_array_new ();
  g_autoptr (JsonArray) tail = json_array_new ();
  const char *summary = sw_json_string (p->app, "summary");
  int length = 0;

  section (p->details, "About this app");
  if (summary)
    gtk_box_append (GTK_BOX (p->details), sw_label_new (summary, "sw-summary"));
  /* the first blocks up to about 300 characters show, the rest behind Read more */
  for (guint i = 0; blocks && i < json_array_get_length (blocks); i++)
    {
      JsonNode *node = json_array_get_element (blocks, i);
      JsonObject *block = sw_json_node_object (node);

      if (length < 300 || !i)
        json_array_add_element (head, json_node_copy (node));
      else
        json_array_add_element (tail, json_node_copy (node));
      length += sw_text_length (sw_json_string (block, "text"));
      /* a list item counts as a short line */
      if (sw_json_array (block, "items"))
        length += 40 * json_array_get_length (sw_json_array (block, "items"));
    }
  gtk_widget_add_css_class (first, "sw-description");
  add_blocks (first, head);
  gtk_box_append (GTK_BOX (p->details), first);
  if (json_array_get_length (tail))
    {
      gtk_widget_add_css_class (rest, "sw-description");
      add_blocks (rest, tail);
      gtk_revealer_set_child (GTK_REVEALER (revealer), rest);
      gtk_revealer_set_transition_type (GTK_REVEALER (revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
      gtk_box_append (GTK_BOX (p->details), revealer);
      p->more_button = sw_button_new ("Read more", SW_BUTTON_TEXT);
      gtk_widget_set_halign (p->more_button, GTK_ALIGN_START);
      gtk_widget_add_css_class (p->more_button, "sw-read-more");
      g_signal_connect (p->more_button, "clicked", G_CALLBACK (on_read_more), revealer);
      gtk_box_append (GTK_BOX (p->details), p->more_button);
    }
}

static void
whats_new (Page *p)
{
  JsonObject *release = sw_json_object (p->app, "release");
  JsonArray *notes = sw_json_array (release, "notes");
  const char *version = sw_json_string (release, "version");
  g_autofree char *date = sw_format_date (sw_json_string (release, "date"));
  g_autofree char *line = NULL;
  GtkWidget *box;

  if (!release || (!version && (!notes || !json_array_get_length (notes))))
    return;
  section (p->details, "What's new");
  line = version && date ? g_strdup_printf ("Version %s · %s", version, date)
         : version ? g_strdup_printf ("Version %s", version) : g_strdup (date);
  if (line)
    gtk_box_append (GTK_BOX (p->details), sw_label_new (line, "sw-caption"));
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (box, "sw-description");
  add_blocks (box, notes);
  if (!notes || !json_array_get_length (notes))
    gtk_box_append (GTK_BOX (box), sw_label_new ("The developer did not say what changed.", "sw-body-text"));
  gtk_box_append (GTK_BOX (p->details), box);
}

static JsonObject *
permission_info (Page *p, const char *id)
{
  JsonArray *list = sw_json_array (p->config, "permissions");

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *perm = sw_json_array_object (list, i);

      if (!g_strcmp0 (sw_json_string (perm, "id"), id))
        return perm;
    }
  return NULL;
}

/* installed and sandboxed: its permissions are changed on its page in Settings */
static void
update_permission_link (Page *p)
{
  gboolean sandboxed = p->launcher && G_IS_DESKTOP_APP_INFO (p->launcher)
                       && g_desktop_app_info_has_key (G_DESKTOP_APP_INFO (p->launcher), "X-VibeOS-Sandbox")
                       && sw_installer_get_installed (sw_installer_get_default (), package_of (p));

  if (p->permission_link)
    gtk_widget_set_visible (p->permission_link, sandboxed);
}

/* the platform data of an activation of Settings: the token that lets its window come to the front (xdg-activation;
 * without it a Settings already running behind this app only posts "Settings is ready") */
static GVariant *
settings_platform_data (GtkWidget *from)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new ("vibeos.Settings.desktop");
  g_autoptr (GdkAppLaunchContext) context = gdk_display_get_app_launch_context (gtk_widget_get_display (from));
  g_autofree char *token = info ? g_app_launch_context_get_startup_notify_id (G_APP_LAUNCH_CONTEXT (context),
                                                                             G_APP_INFO (info), NULL) : NULL;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (token)
    {
      g_variant_builder_add (&builder, "{sv}", "activation-token", g_variant_new_string (token));
      g_variant_builder_add (&builder, "{sv}", "desktop-startup-id", g_variant_new_string (token));
    }
  return g_variant_builder_end (&builder);
}

/* the app's page in Settings: GNOME Settings' "launch-panel" action with the "applications" panel and the app's
 * desktop id, which VibeOS Settings answers */
static void
on_permission_settings (VibeosListRow *row, Page *p)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());
  GVariantBuilder args, panel;

  if (!bus || !p->launcher)
    return;
  g_variant_builder_init (&panel, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&panel, "v", g_variant_new_string (g_app_info_get_id (p->launcher)));
  g_variant_builder_init (&args, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&args, "v", g_variant_new ("(sav)", "applications", &panel));
  g_dbus_connection_call (bus, "vibeos.Settings", "/vibeos/Settings", "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sav@a{sv})", "launch-panel", &args,
                                         settings_platform_data (GTK_WIDGET (row))),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
permissions (Page *p)
{
  JsonArray *list = sw_json_array (p->app, "permissions");
  GtkWidget *cards, *row;
  gboolean system = sw_json_bool (p->app, "system_app", FALSE);

  section (p->details, "Permissions");
  gtk_box_append (GTK_BOX (p->details),
                  sw_label_new (system ? "What the app says it uses. VibeOS's own apps are part of the system: they "
                                         "do not run in a sandbox."
                                       : "What the app asks for. VibeOS enforces these permissions: the app runs in "
                                         "a sandbox, and you allow or deny each one in Settings. Taking one away "
                                         "stops the app.", "sw-caption"));
  if (!list || !json_array_get_length (list))
    gtk_box_append (GTK_BOX (p->details), sw_label_new ("The app declares no permissions.", "sw-body-text"));
  else
    {
      cards = vibeos_card_list_new ();
      for (guint i = 0; i < json_array_get_length (list); i++)
        {
          JsonNode *node = json_array_get_element (list, i);
          const char *id = JSON_NODE_HOLDS_VALUE (node) ? json_node_get_string (node) : NULL;
          JsonObject *info = permission_info (p, id);

          if (!id)
            continue;
          row = vibeos_list_row_new (info ? sw_json_string (info, "name") : id, sw_json_string (info, "description"));
          vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-shield-symbolic");
          gtk_widget_set_can_target (row, FALSE);
          vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
        }
      gtk_box_append (GTK_BOX (p->details), cards);
    }
  /* installed: a row to the app's page in Settings, where they are changed */
  p->permission_link = vibeos_card_list_new ();
  row = vibeos_list_row_new ("Permissions in Settings", "Allow or deny each one for this app");
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "sw-settings-symbolic");
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  g_signal_connect (row, "clicked", G_CALLBACK (on_permission_settings), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (p->permission_link), row);
  gtk_box_append (GTK_BOX (p->details), p->permission_link);
  update_permission_link (p);
}

/* ---- reviews --------------------------------------------------------------------------------------------------- */

static GtkWidget *
histogram (JsonObject *rating)
{
  JsonObject *bars = sw_json_object (rating, "histogram");
  gint64 count = sw_json_int (rating, "count", 0);
  double average = sw_json_double (rating, "average");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24), *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4), *big;
  g_autofree char *value = isnan (average) ? g_strdup ("–") : sw_format_rating (average);
  g_autofree char *total = sw_format_ratings_count (count);

  gtk_widget_add_css_class (box, "sw-histogram");
  big = gtk_label_new (value);
  gtk_widget_add_css_class (big, "sw-big-rating");
  gtk_box_append (GTK_BOX (left), big);
  gtk_box_append (GTK_BOX (left), sw_stars_new (isnan (average) ? 0 : average, 14, 2, FALSE));
  gtk_box_append (GTK_BOX (left), sw_label_new (total, "sw-caption"));
  gtk_widget_set_valign (left, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), left);
  gtk_widget_set_hexpand (right, TRUE);
  for (int star = 5; star >= 1; star--)
    {
      char key[2] = { '0' + star, 0 };
      gint64 n = sw_json_int (bars, key, 0);
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8), *bar = gtk_progress_bar_new ();

      gtk_box_append (GTK_BOX (line), sw_label_new (key, "sw-histogram-key"));
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), count > 0 ? (double) n / count : 0);
      gtk_widget_set_hexpand (bar, TRUE);
      gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (bar, "sw-histogram-bar");
      gtk_box_append (GTK_BOX (line), bar);
      gtk_box_append (GTK_BOX (right), line);
    }
  gtk_box_append (GTK_BOX (box), right);
  return box;
}

static void
on_all_reviews (GtkWidget *button, Page *p)
{
  sw_window_push (button, sw_reviews_page_new (p->id, name_of (p)));
}

static void
on_sign_in (GtkWidget *button, Page *p)
{
  sw_window_push (button, sw_sign_in_page_new ());
}

static void
on_confirm_email (GtkWidget *button, Page *p)
{
  sw_window_push (button, sw_verify_page_new (FALSE));
}

static void
on_rate (SwStars *stars, Page *p)
{
  int rating = (int) sw_stars_get_rating (stars);

  sw_window_push (GTK_WIDGET (stars), sw_review_editor_new (p->id, name_of (p), rating,
                                                            sw_json_object (p->my, "review")));
  /* the stars wait for the review to be sent */
  sw_stars_set_rating (stars, 0);
}

static void
on_write (GtkWidget *button, Page *p)
{
  JsonObject *review = sw_json_object (p->my, "review");

  int rating = review ? (int) sw_json_int (review, "rating", 0) : 0;

  sw_window_push (button, sw_review_editor_new (p->id, name_of (p), rating, review));
}

/* the store's reasons, in a sentence each */
static const char *
reason_text (const char *reason)
{
  if (!g_strcmp0 (reason, "email_not_verified"))
    return "Confirm your email address to rate and review apps.";
  if (!g_strcmp0 (reason, "not_installed"))
    return "Install the app to rate it: the store takes reviews from people who have had it on one of their devices.";
  return "The store does not take a review of this app from your account.";
}

static void
update_review_box (Page *p)
{
  SwAccount *account = sw_account_get_default ();
  GtkWidget *child, *button;
  JsonObject *mine = sw_json_object (p->my, "review");

  /* not built yet (or being rebuilt): the reviews section asks for the account's review itself */
  if (!p->review_box)
    return;
  while ((child = gtk_widget_get_first_child (p->review_box)))
    gtk_box_remove (GTK_BOX (p->review_box), child);
  /* the preview's reviews do not say which is the account's (the API leaves "mine" out of them): the account's own
   * shows once, as "Your review" */
  for (child = p->previews ? gtk_widget_get_first_child (p->previews) : NULL; child;
       child = gtk_widget_get_next_sibling (child))
    gtk_widget_set_visible (child, !mine || GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (child), "sw-review-id"))
                                             != (gsize) sw_json_int (mine, "id", -1));
  if (!sw_account_is_signed_in (account))
    {
      gtk_box_append (GTK_BOX (p->review_box), sw_label_new ("Sign in to rate and review apps.", "sw-body-text"));
      button = sw_button_new ("Sign in", SW_BUTTON_TONAL);
      gtk_widget_set_halign (button, GTK_ALIGN_START);
      g_signal_connect (button, "clicked", G_CALLBACK (on_sign_in), p);
      gtk_box_append (GTK_BOX (p->review_box), button);
      return;
    }
  if (!p->my)
    return;
  if (mine)
    {
      gtk_box_append (GTK_BOX (p->review_box), sw_label_new ("Your review", "sw-subtitle"));
      if (sw_json_bool (p->my, "hidden", FALSE))
        gtk_box_append (GTK_BOX (p->review_box),
                        sw_label_new ("A moderator hid your review: only you see it.", "sw-caption"));
      gtk_box_append (GTK_BOX (p->review_box), sw_review_card_new (mine, p->id, name_of (p)));
      return;
    }
  if (!sw_json_bool (p->my, "can_review", FALSE))
    {
      const char *reason = sw_json_string (p->my, "reason");

      gtk_box_append (GTK_BOX (p->review_box), sw_label_new (reason_text (reason), "sw-body-text"));
      if (!g_strcmp0 (reason, "email_not_verified"))
        {
          button = sw_button_new ("Confirm the address", SW_BUTTON_TONAL);
          gtk_widget_set_halign (button, GTK_ALIGN_START);
          g_signal_connect (button, "clicked", G_CALLBACK (on_confirm_email), p);
          gtk_box_append (GTK_BOX (p->review_box), button);
        }
      return;
    }
  {
    GtkWidget *stars = sw_stars_new (0, 36, 16, TRUE);

    gtk_box_append (GTK_BOX (p->review_box), sw_label_new ("Rate the app", "sw-subtitle"));
    gtk_widget_set_halign (stars, GTK_ALIGN_START);
    g_signal_connect (stars, "changed", G_CALLBACK (on_rate), p);
    gtk_box_append (GTK_BOX (p->review_box), stars);
    button = sw_button_new ("Write a review", SW_BUTTON_TONAL);
    gtk_widget_set_halign (button, GTK_ALIGN_START);
    g_signal_connect (button, "clicked", G_CALLBACK (on_write), p);
    gtk_box_append (GTK_BOX (p->review_box), button);
  }
}

static void show (Page *p);

static void load_my_review (Page *p);

static void
on_loaded_again (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Page *p = data;

  if (!result || !sw_json_node_object (result->data))
    {
      load_my_review (p);
      return;
    }
  g_clear_pointer (&p->app, json_object_unref);
  p->app = json_object_ref (sw_json_node_object (result->data));
  schedule_show (p);
}

static void
on_my_review (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Page *p = data;

  g_clear_pointer (&p->my, json_object_unref);
  if (result && sw_json_node_object (result->data))
    p->my = json_object_ref (sw_json_node_object (result->data));
  update_review_box (p);
}

static void
load_my_review (Page *p)
{
  g_autofree char *id = sw_api_escape (p->id);
  g_autofree char *path = g_strdup_printf ("/apps/%s/my-review", id);

  if (!sw_account_is_signed_in (sw_account_get_default ()))
    {
      update_review_box (p);
      return;
    }
  sw_api_get (sw_api_get_default (), path, SW_API_NONE, sw_cancellable_for (p->page), on_my_review, p);
}

static void
on_account_changed (SwAccount *account, GtkWidget *page)
{
  Page *p = g_object_get_data (G_OBJECT (page), "sw-app-page");

  if (p->app)
    load_my_review (p);
}

/* back on the page after writing a review: the figures and the account's review changed */
static void
on_page_map (GtkWidget *page, Page *p)
{
  g_autofree char *id = NULL, *path = NULL;

  if (!p->app || !sw_reviews_take_changed (p->id))
    return;
  id = sw_api_escape (p->id);
  path = g_strdup_printf ("/apps/%s", id);
  sw_api_get (sw_api_get_default (), path, SW_API_CACHED | SW_API_REVALIDATE, sw_cancellable_for (p->page),
              on_loaded_again, p);
}

static void
reviews (Page *p)
{
  JsonObject *rating = sw_json_object (p->app, "rating");
  JsonArray *preview = sw_json_array (p->app, "reviews_preview");

  section (p->details, "Ratings and reviews");
  gtk_box_append (GTK_BOX (p->details), histogram (rating));
  p->review_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (p->review_box, "sw-review-box");
  gtk_box_append (GTK_BOX (p->details), p->review_box);
  load_my_review (p);
  p->previews = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (p->details), p->previews);
  for (guint i = 0; preview && i < json_array_get_length (preview); i++)
    {
      JsonObject *review = sw_json_array_object (preview, i);

      if (review)
        gtk_box_append (GTK_BOX (p->previews), sw_review_card_new (review, p->id, name_of (p)));
    }
  if (sw_json_int (rating, "count", 0) > 0)
    {
      GtkWidget *all = sw_button_new ("All reviews", SW_BUTTON_OUTLINED);

      gtk_widget_add_css_class (all, "sw-all-reviews");
      g_signal_connect (all, "clicked", G_CALLBACK (on_all_reviews), p);
      gtk_box_append (GTK_BOX (p->details), all);
    }
}

static void
similar (Page *p)
{
  JsonArray *list = sw_json_array (p->app, "similar");
  GtkWidget *box, *strip;

  if (!list || !json_array_get_length (list))
    return;
  section (p->details, "Similar apps");
  strip = sw_strip_new (&box);
  for (guint i = 0; i < json_array_get_length (list); i++)
    if (sw_json_array_object (list, i))
      gtk_box_append (GTK_BOX (box), sw_app_tile_new (sw_json_array_object (list, i), 76));
  gtk_widget_add_css_class (strip, "sw-bleed");
  gtk_box_append (GTK_BOX (p->details), strip);
}

static void
info_row (GtkWidget *cards, const char *title, const char *value)
{
  GtkWidget *row;

  if (!value || !*value)
    return;
  row = vibeos_list_row_new (title, value);
  gtk_widget_set_can_target (row, FALSE);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
}

static void
on_homepage (VibeosListRow *row, Page *p)
{
  g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (sw_json_string (p->app, "homepage"));

  gtk_uri_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (row))), NULL, NULL, NULL);
}

static void
information (Page *p)
{
  GtkWidget *cards = vibeos_card_list_new ();
  JsonObject *package = sw_app_package_for_arch (p->app, sw_device_arch ());
  const char *installed = sw_installer_get_installed (sw_installer_get_default (), package_of (p));
  g_autofree char *download = sw_format_size (sw_json_int (package, "download_size", 0));
  g_autofree char *size = sw_format_size (sw_json_int (package, "installed_size", 0));
  g_autofree char *updated = sw_format_date (sw_json_string (sw_json_object (p->app, "release"), "date"));
  JsonArray *categories = sw_json_array (p->app, "categories");
  g_autoptr (GString) names = g_string_new (NULL);
  const char *source = sw_json_string (p->app, "source");

  for (guint i = 0; categories && i < json_array_get_length (categories); i++)
    {
      if (names->len)
        g_string_append (names, ", ");
      g_string_append (names, sw_json_string (sw_json_array_object (categories, i), "name") ?: "");
    }
  section (p->details, "Information");
  info_row (cards, "Developer", sw_json_string (p->app, "developer"));
  info_row (cards, "Version", sw_json_string (package, "version"));
  info_row (cards, "Installed version", installed);
  info_row (cards, "Released", updated);
  info_row (cards, "Download size", download);
  info_row (cards, "Size when installed", size);
  info_row (cards, "Category", names->str);
  info_row (cards, "Licence", sw_json_string (p->app, "license"));
  info_row (cards, "Comes from", !g_strcmp0 (source, "vibeos") ? "The VibeOS software sources"
                                 : !g_strcmp0 (source, "debian") ? "Debian's software sources" : NULL);
  info_row (cards, "Fits a phone", sw_json_bool (p->app, "phone_friendly", FALSE) ? "Yes" : "Not known to");
  info_row (cards, "Package", package_of (p));
  if (sw_json_string (p->app, "homepage"))
    {
      GtkWidget *row = vibeos_list_row_new ("Homepage", sw_json_string (p->app, "homepage"));

      vibeos_list_row_set_end_icon (VIBEOS_LIST_ROW (row), "sw-public-symbolic");
      g_signal_connect (row, "clicked", G_CALLBACK (on_homepage), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
    }
  gtk_box_append (GTK_BOX (p->details), cards);
}

static gboolean page_moving (Page *p);

/* The sections below the first screen. Built all at once, the details take several frames' time; built one a frame
 * after the first screen has faded in, no frame waits long for them (and nobody sees them arrive: they are below). */
static void (*const later[]) (Page *p) = { whats_new, reviews, permissions, similar, information };

static gboolean
build_later (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  Page *p = data;

  if (sw_status_is_fading (SW_STATUS (p->status)) || page_moving (p))
    return G_SOURCE_CONTINUE;
  later[p->built++] (p);
  if (p->built < G_N_ELEMENTS (later))
    return G_SOURCE_CONTINUE;
  p->later_tick = 0;
  return G_SOURCE_REMOVE;
}

static void
build_rest (Page *p)
{
  if (!gtk_widget_get_mapped (p->page))
    {
      while (p->built < G_N_ELEMENTS (later))
        later[p->built++] (p);
      return;
    }
  if (!p->later_tick)
    p->later_tick = gtk_widget_add_tick_callback (p->page, build_later, p, NULL);
}

static void
show (Page *p)
{
  GtkWidget *child;
  JsonArray *shots = sw_json_array (p->app, "screenshots");

  if (p->later_tick)
    gtk_widget_remove_tick_callback (p->page, p->later_tick);
  p->later_tick = 0;
  p->built = 0;
  while ((child = gtk_widget_get_first_child (p->details)))
    gtk_box_remove (GTK_BOX (p->details), child);
  p->more_button = p->previews = p->review_box = p->permission_link = NULL;
  head_update (p, p->app);
  gtk_box_append (GTK_BOX (p->details), stats (p));
  if (shots && json_array_get_length (shots))
    {
      GtkWidget *strip = screenshots (shots);

      gtk_widget_add_css_class (strip, "sw-bleed");
      gtk_box_append (GTK_BOX (p->details), strip);
    }
  description (p);
  update_action (p);
  if (sw_installer_get_installed (sw_installer_get_default (), package_of (p)))
    find_launcher (p);
  /* the first time, where the page showed it loading, the details fade in rather than appear */
  sw_status_show (SW_STATUS (p->status));
  build_rest (p);
}

static gboolean
page_moving (Page *p)
{
  GtkWidget *stack = gtk_widget_get_ancestor (p->page, VIBEOS_TYPE_PAGE_STACK);

  return stack && vibeos_page_stack_is_animating (VIBEOS_PAGE_STACK (stack));
}

static gboolean
show_when_still (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  Page *p = data;

  if (page_moving (p))
    return G_SOURCE_CONTINUE;
  p->show_tick = 0;
  show (p);
  return G_SOURCE_REMOVE;
}

/* The details, built now or, while the page slides in or out, right after: building a page of them takes longer than
 * a frame, and the slide would stop for it. Two calls before then build them once. */
static void
schedule_show (Page *p)
{
  if (p->show_tick)
    return;
  if (!gtk_widget_get_mapped (p->page) || !page_moving (p))
    {
      show (p);
      return;
    }
  p->show_tick = gtk_widget_add_tick_callback (p->page, show_when_still, p, NULL);
}

static void
on_config (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Page *p = data;

  if (!result || !sw_json_node_object (result->data) || result->not_modified)
    return;
  g_clear_pointer (&p->config, json_object_unref);
  p->config = json_object_ref (sw_json_node_object (result->data));
  if (p->app)
    schedule_show (p);
}

static void
on_loaded (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Page *p = data;

  if (error)
    {
      if (error->status == 404)
        sw_status_empty (SW_STATUS (p->status), "sw-apps-symbolic",
                         "This app is not in the store any more.", NULL, NULL, NULL);
      else if (!p->app)
        sw_status_error (SW_STATUS (p->status), error, (SwRetryFunc) load, p);
      return;
    }
  if (result->not_modified && p->app)
    return;
  g_clear_pointer (&p->app, json_object_unref);
  p->app = json_object_ref (sw_json_node_object (result->data));
  schedule_show (p);
}

static void
load (Page *p)
{
  g_autofree char *id = sw_api_escape (p->id);
  g_autofree char *path = g_strdup_printf ("/apps/%s", id);

  sw_status_loading (SW_STATUS (p->status));
  sw_api_get (sw_api_get_default (), path, SW_API_CACHED | SW_API_STALE_FIRST, sw_cancellable_for (p->page),
              on_loaded, p);
}

/* ---- the menu -------------------------------------------------------------------------------------------------- */

static void
menu_uninstall (gpointer data)
{
  on_uninstall (((Page *) data)->page, data);
}

static void
menu_reviews (gpointer data)
{
  Page *p = data;

  sw_window_push (p->page, sw_reviews_page_new (p->id, name_of (p)));
}

static void
on_more (GtkWidget *button, Page *p)
{
  SwMenu *menu = sw_menu_new ();
  SwAppState state = sw_app_state (package_of (p));

  sw_menu_append (menu, "All reviews", menu_reviews, p);
  if ((state == SW_APP_INSTALLED || state == SW_APP_UPDATABLE) && !sw_json_bool (p->app, "system_app", FALSE))
    sw_menu_append (menu, "Uninstall", menu_uninstall, p);
  sw_menu_present (menu, button);
}

GtkWidget *
sw_app_page_new (const char *app_id, JsonObject *summary)
{
  Page *p = g_new0 (Page, 1);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16), *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *icon_box = gtk_overlay_new (), *top, *more;

  p->id = g_strdup (app_id);
  p->summary = summary ? json_object_ref (summary) : NULL;
  p->page = sw_page_new (NULL);
  g_object_set_data_full (G_OBJECT (p->page), "sw-app-page", p, page_free);
  more = vibeos_top_bar_add_action (sw_page_get_bar (p->page), "sw-more-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, "More", -1);
  g_signal_connect (more, "clicked", G_CALLBACK (on_more), p);

  p->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->column, "sw-app-page");

  /* the head: the icon (with the job's ring around it), the name and developer */
  gtk_widget_add_css_class (head, "sw-app-head");
  p->icon = sw_image_new (88, 88, 20);
  sw_image_set_fallback (SW_IMAGE (p->icon), "sw-package-symbolic");
  gtk_widget_set_halign (p->icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->icon, GTK_ALIGN_CENTER);
  p->ring = sw_progress_new (104, 3);
  gtk_widget_set_visible (p->ring, FALSE);
  gtk_overlay_set_child (GTK_OVERLAY (icon_box), p->ring);
  gtk_overlay_add_overlay (GTK_OVERLAY (icon_box), p->icon);
  gtk_widget_set_size_request (icon_box, 104, 104);
  gtk_widget_set_valign (icon_box, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (head), icon_box);
  p->name = sw_label_new (NULL, "sw-app-title");
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->name), GTK_ACCESSIBLE_PROPERTY_LEVEL, 1, -1);
  p->developer = sw_label_new (NULL, "sw-app-developer");
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (text), p->name);
  gtk_box_append (GTK_BOX (text), p->developer);
  gtk_box_append (GTK_BOX (head), text);
  gtk_box_append (GTK_BOX (p->column), head);

  p->action = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (p->action, "sw-app-action");
  gtk_box_append (GTK_BOX (p->column), p->action);

  p->details = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->details, "sw-app-details");
  p->status = sw_status_new (p->details);
  gtk_widget_set_vexpand (p->status, FALSE);
  gtk_box_append (GTK_BOX (p->column), p->status);
  top = sw_scroll_new (vibeos_clamp_new (p->column, PAGE_MAX));
  sw_page_set_body (p->page, top);

  if (summary)
    head_update (p, summary);
  else
    gtk_widget_set_visible (p->developer, FALSE);
  g_signal_connect_object (sw_installer_get_default (), "changed", G_CALLBACK (on_installer_changed), p->page, 0);
  g_signal_connect_object (sw_updates_get_default (), "changed", G_CALLBACK (on_updates_changed), p->page, 0);
  g_signal_connect_object (sw_account_get_default (), "changed", G_CALLBACK (on_account_changed), p->page, 0);
  g_signal_connect (p->page, "map", G_CALLBACK (on_page_map), p);
  update_action (p);
  sw_api_get (sw_api_get_default (), "/config", SW_API_CACHED | SW_API_STALE_FIRST, sw_cancellable_for (p->page),
              on_config, p);
  load (p);
  return p->page;
}
