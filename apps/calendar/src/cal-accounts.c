/* Adding calendars and a calendar's page (cal-accounts.h).
 *
 * Calendars are evolution-data-server's ESources, written through its registry as GNOME Calendar and Evolution
 * write them:
 *  - an offline calendar is a "local" calendar under "local-stub",
 *  - a CalDAV account is a "webdav" collection (Evolution's collection account): EDS's webdav collection backend
 *    discovers its calendars, keeps the list in step with the server and syncs them; the password is kept by
 *    e_source_store_password (the Secret Service). The page asks as a CalDAV login does ("Login with
 *    URL and user name", then "Create account"),
 *  - a calendar's colour, name and visibility are ESourceSelectable's colour, the display name and :selected.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* dladdr */
#endif
#include "cal-accounts.h"
#include "cal-color-picker.h"
#include "cal-compat-switch.h"
#include "cal-dialogs.h"
#include "cal-page.h"
#include "cal-prefs.h"
#include "cal-store.h"
#include "cal-window.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>
#include <dlfcn.h>

/* --- a text field, outlined: the outline thicker and in the primary colour with focus; the hint resting where the
 * text goes and floating, smaller, into a gap in the top outline when the field has focus or text; an error in the
 * error colour under the box, with the error icon at the box's end. The text stays in the middle of the box
 * (outlined fields do not move it down). */

typedef struct {
  GtkWidget *box, *field, *hint, *text, *error, *error_icon;
} Field;

static void
field_update (Field *field, gboolean focus)
{
  const char *value = gtk_editable_get_text (GTK_EDITABLE (field->text));

  if (focus || (value && *value))
    gtk_widget_add_css_class (field->field, "floating");
  else
    gtk_widget_remove_css_class (field->field, "floating");
  if (focus)
    gtk_widget_add_css_class (field->field, "focused");
  else
    gtk_widget_remove_css_class (field->field, "focused");
}

static void
field_set_error (Field *field, const char *error)
{
  gtk_label_set_label (GTK_LABEL (field->error), error ? error : "");
  gtk_widget_set_visible (field->error, error != NULL);
  gtk_widget_set_visible (field->error_icon, error != NULL);
  if (error)
    gtk_widget_add_css_class (field->field, "error");
  else
    gtk_widget_remove_css_class (field->field, "error");
}

static void
on_field_changed (GtkEditable *editable, Field *field)
{
  /* typing clears the error */
  field_set_error (field, NULL);
  field_update (field, gtk_widget_has_focus (field->text));
}

static void
on_field_enter (GtkEventControllerFocus *focus, Field *field)
{
  field_update (field, TRUE);
}

static void
on_field_leave (GtkEventControllerFocus *focus, Field *field)
{
  field_update (field, FALSE);
}

/* the error icon: a disc with an exclamation mark cut out of it, in the CSS colour */
static void
draw_error_icon (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  GdkRGBA color;

  gtk_widget_get_color (GTK_WIDGET (area), &color);
  cairo_translate (cr, (width - 24) / 2.0, (height - 24) / 2.0);
  cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_arc (cr, 12, 12, 10, 0, 2 * G_PI);
  cairo_rectangle (cr, 11, 7, 2, 6);
  cairo_rectangle (cr, 11, 15, 2, 2);
  gdk_cairo_set_source_rgba (cr, &color);
  cairo_fill (cr);
}

static Field *
field_new (const char *hint, gboolean password, GtkInputPurpose purpose)
{
  Field *field = g_new0 (Field, 1);
  GtkWidget *overlay = gtk_overlay_new (), *outline = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkEventController *focus = gtk_event_controller_focus_new ();

  field->text = gtk_text_new ();
  gtk_text_set_visibility (GTK_TEXT (field->text), !password);
  gtk_text_set_input_purpose (GTK_TEXT (field->text), password ? GTK_INPUT_PURPOSE_PASSWORD : purpose);
  if (purpose == GTK_INPUT_PURPOSE_URL || purpose == GTK_INPUT_PURPOSE_EMAIL || password)
    gtk_text_set_input_hints (GTK_TEXT (field->text), GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_LOWERCASE);
  gtk_widget_add_css_class (field->text, "cal-field-text");
  gtk_widget_set_hexpand (field->text, TRUE);
  gtk_widget_set_valign (field->text, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (outline), field->text);
  field->error_icon = gtk_drawing_area_new ();
  gtk_widget_set_size_request (field->error_icon, 24, 24);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (field->error_icon), draw_error_icon, NULL, NULL);
  gtk_widget_set_valign (field->error_icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (field->error_icon, "cal-field-error-icon");
  gtk_widget_set_visible (field->error_icon, FALSE);
  gtk_box_append (GTK_BOX (outline), field->error_icon);
  gtk_widget_add_css_class (outline, "cal-field-outline");
  gtk_overlay_set_child (GTK_OVERLAY (overlay), outline);
  field->hint = gtk_label_new (hint);
  gtk_label_set_xalign (GTK_LABEL (field->hint), 0);
  gtk_label_set_ellipsize (GTK_LABEL (field->hint), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (field->hint, GTK_ALIGN_START);
  gtk_widget_set_valign (field->hint, GTK_ALIGN_START);
  gtk_widget_set_can_target (field->hint, FALSE);
  gtk_widget_add_css_class (field->hint, "cal-field-hint");
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), field->hint);
  gtk_widget_add_css_class (overlay, "cal-field");
  field->field = overlay;
  field->error = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (field->error), 0);
  gtk_label_set_wrap (GTK_LABEL (field->error), TRUE);
  gtk_widget_add_css_class (field->error, "cal-field-error");
  gtk_widget_set_visible (field->error, FALSE);
  field->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (field->box), overlay);
  gtk_box_append (GTK_BOX (field->box), field->error);
  gtk_accessible_update_property (GTK_ACCESSIBLE (field->text), GTK_ACCESSIBLE_PROPERTY_LABEL, hint, -1);
  g_signal_connect (field->text, "changed", G_CALLBACK (on_field_changed), field);
  g_signal_connect (focus, "enter", G_CALLBACK (on_field_enter), field);
  g_signal_connect (focus, "leave", G_CALLBACK (on_field_leave), field);
  gtk_widget_add_controller (field->text, focus);
  g_object_set_data_full (G_OBJECT (field->box), "cal-field", field, g_free);
  return field;
}

static char *
field_get (Field *field)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (field->text))));
}

/* the dialog's own buttons (the negative one, then the positive one, at the end), for a dialog that stays open when
 * its positive button finds something wrong */
static GtkWidget *
dialog_buttons (GtkWidget **negative, const char *negative_label, GtkWidget **positive, const char *positive_label)
{
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_widget_add_css_class (buttons, "vibeos-dialog-buttons");
  *negative = vibeos_text_button_new (negative_label);
  *positive = vibeos_text_button_new (positive_label);
  gtk_box_append (GTK_BOX (buttons), *negative);
  gtk_box_append (GTK_BOX (buttons), *positive);
  return buttons;
}

/* "rgb(r,g,b)", as GNOME Calendar writes ESourceSelectable's colour (gdk_rgba_to_string) */
static char *
color_string (guint32 rgb)
{
  return g_strdup_printf ("rgb(%u,%u,%u)", (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

static ESourceRegistry *
registry_of (GtkWidget *window)
{
  return cal_store_get_registry (cal_window_get_store (window));
}

/* --- Add offline calendar */

typedef struct {
  GtkWidget *window;
  VibeosDialog *dialog;
  Field *name;
} Offline;

static void
on_offline_committed (GObject *registry, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  g_autoptr(GError) error = NULL;

  if (!e_source_registry_commit_source_finish (E_SOURCE_REGISTRY (registry), result, &error))
    {
      g_warning ("add offline calendar: %s", error->message);
      cal_window_toast (window, _("The calendar could not be added."));
    }
  g_object_unref (window);
}

static void
on_offline_add (VibeosPressable *button, Offline *offline)
{
  ESourceRegistry *registry = registry_of (offline->window);
  /* the name as typed: only an empty one is refused */
  g_autofree char *name = g_strdup (gtk_editable_get_text (GTK_EDITABLE (offline->name->text)));
  g_autofree char *color = NULL;
  g_autoptr(ESource) source = NULL;
  ESourceBackend *calendar;

  if (!*name)
    {
      field_set_error (offline->name, _("Enter a calendar name!"));
      return;
    }
  if (!registry || !(source = e_source_new (NULL, NULL, NULL)))
    {
      cal_window_toast (offline->window, _("The calendar could not be added."));
      vibeos_dialog_respond (offline->dialog, VIBEOS_RESPONSE_CANCEL);
      return;
    }
  /* visible, the offline default colour; as GNOME Calendar's local calendar: backend "local" under "local-stub" */
  e_source_set_display_name (source, name);
  e_source_set_parent (source, "local-stub");
  calendar = e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR);
  e_source_backend_set_backend_name (calendar, "local");
  color = color_string (CAL_COLOR_PICKER_OFFLINE_DEFAULT);
  e_source_selectable_set_color (E_SOURCE_SELECTABLE (calendar), color);
  e_source_selectable_set_selected (E_SOURCE_SELECTABLE (calendar), TRUE);
  e_source_registry_commit_source (registry, source, NULL, on_offline_committed, g_object_ref (offline->window));
  vibeos_dialog_respond (offline->dialog, VIBEOS_RESPONSE_OK);
}

static void
on_offline_cancel (VibeosPressable *button, Offline *offline)
{
  vibeos_dialog_respond (offline->dialog, VIBEOS_RESPONSE_CANCEL);
}

static void
on_offline_activate (GtkText *text, Offline *offline)
{
  on_offline_add (NULL, offline);
}

void
cal_accounts_add_offline (GtkWidget *window)
{
  Offline *offline = g_new0 (Offline, 1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *cancel, *add;

  offline->window = window;
  offline->dialog = vibeos_dialog_new (_("Add offline calendar"));
  vibeos_dialog_set_message (offline->dialog,
                          _("Offline calendars are NOT synchronized to a cloud service and only available on your phone."));
  /* the field in a padded frame */
  offline->name = field_new (_("Name"), FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
  gtk_text_set_input_hints (GTK_TEXT (offline->name->text), GTK_INPUT_HINT_UPPERCASE_SENTENCES);
  g_signal_connect (offline->name->text, "activate", G_CALLBACK (on_offline_activate), offline);
  gtk_widget_add_css_class (frame, "cal-offline-frame");
  gtk_box_append (GTK_BOX (frame), offline->name->box);
  gtk_box_append (GTK_BOX (content), frame);
  gtk_box_append (GTK_BOX (content), dialog_buttons (&cancel, _("Cancel"), &add, _("Add calendar")));
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_offline_cancel), offline);
  g_signal_connect (add, "clicked", G_CALLBACK (on_offline_add), offline);
  vibeos_dialog_set_content (offline->dialog, content);
  g_object_set_data_full (G_OBJECT (offline->dialog), "cal-offline", offline, g_free);
  gtk_widget_add_css_class (GTK_WIDGET (offline->dialog), "cal-offline-dialog");
  vibeos_dialog_present (offline->dialog, window);
  gtk_widget_grab_focus (offline->name->text);
}

/* --- Add CalDAV calendar */

/* EDS's calendar backends and registry modules live in its private library directory (libedataserver-1.2's
 * privlibdir: /usr/lib/evolution-data-server on Debian, $libdir/evolution-data-server elsewhere). The CalDAV row
 * needs both the caldav calendar backend and the webdav collection module. */
static gboolean
eds_has (const char *relative)
{
  g_autoptr(GPtrArray) dirs = g_ptr_array_new_with_free_func (g_free);
  Dl_info info;

#ifdef CAL_EDS_PRIVLIBDIR
  g_ptr_array_add (dirs, g_strdup (CAL_EDS_PRIVLIBDIR));
#endif
  /* beside the libedataserver this app runs with */
  if (dladdr ((void *) e_source_new, &info) && info.dli_fname)
    {
      g_autofree char *libdir = g_path_get_dirname (info.dli_fname);

      g_ptr_array_add (dirs, g_build_filename (libdir, "evolution-data-server", NULL));
    }
  g_ptr_array_add (dirs, g_strdup ("/usr/lib/evolution-data-server"));
  g_ptr_array_add (dirs, g_strdup ("/usr/lib64/evolution-data-server"));
  g_ptr_array_add (dirs, g_strdup ("/usr/local/lib/evolution-data-server"));
  for (guint i = 0; i < dirs->len; i++)
    {
      g_autofree char *path = g_build_filename (dirs->pdata[i], relative, NULL);

      if (g_file_test (path, G_FILE_TEST_EXISTS))
        return TRUE;
    }
  return FALSE;
}

gboolean
cal_accounts_can_caldav (void)
{
  static int can = -1;

  if (can < 0)
    can = eds_has ("calendar-backends/libecalbackendcaldav.so")
          && eds_has ("registry-modules/module-webdav-backend.so");
  return can;
}

typedef struct {
  GtkWidget *window, *page, *list;
  Field *url, *user, *password, *account;
  GtkWidget *login, *create, *status, *found_title, *found;
  GCancellable *cancellable;
  ESource *probe;               /* the discovery's source, not in the registry */
  char *uri;                    /* the URL discovered from */
  GPtrArray *calendars;         /* EWebDAVDiscoveredSource's display names, for the list */
  gboolean busy;
} Caldav;

static void
caldav_free (gpointer data)
{
  Caldav *caldav = data;

  g_cancellable_cancel (caldav->cancellable);
  g_object_unref (caldav->cancellable);
  g_clear_object (&caldav->probe);
  g_free (caldav->uri);
  g_clear_pointer (&caldav->calendars, g_ptr_array_unref);
  g_free (caldav);
}

static void
caldav_status (Caldav *caldav, const char *text)
{
  gtk_label_set_label (GTK_LABEL (caldav->status), text ? text : "");
  gtk_widget_set_visible (caldav->status, text != NULL);
}

static void
caldav_busy (Caldav *caldav, gboolean busy)
{
  caldav->busy = busy;
  gtk_widget_set_sensitive (caldav->login, !busy);
  gtk_widget_set_sensitive (caldav->create, !busy);
  gtk_widget_set_sensitive (caldav->url->text, !busy);
  gtk_widget_set_sensitive (caldav->user->text, !busy);
  gtk_widget_set_sensitive (caldav->password->text, !busy);
}

/* the URL as typed, with https:// when it has no scheme (DAVx⁵ wants one; a bare host is meant as https) */
static GUri *
parse_url (const char *text)
{
  g_autofree char *full = strstr (text, "://") ? g_strdup (text) : g_strconcat ("https://", text, NULL);
  GUri *uri = g_uri_parse (full, SOUP_HTTP_URI_FLAGS, NULL);

  if (uri && (!g_uri_get_host (uri) || !*g_uri_get_host (uri)
              || (g_ascii_strcasecmp (g_uri_get_scheme (uri), "https") != 0
                  && g_ascii_strcasecmp (g_uri_get_scheme (uri), "http") != 0)))
    g_clear_pointer (&uri, g_uri_unref);
  return uri;
}

static void
set_auth (ESource *source, GUri *uri, const char *user)
{
  ESourceAuthentication *auth = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
  ESourceSecurity *security = e_source_get_extension (source, E_SOURCE_EXTENSION_SECURITY);
  gboolean secure = g_ascii_strcasecmp (g_uri_get_scheme (uri), "https") == 0;

  e_source_authentication_set_host (auth, g_uri_get_host (uri));
  e_source_authentication_set_port (auth, g_uri_get_port (uri) > 0 ? g_uri_get_port (uri) : secure ? 443 : 80);
  e_source_authentication_set_user (auth, user);
  e_source_authentication_set_method (auth, "plain/password");
  e_source_authentication_set_remember_password (auth, TRUE);
  e_source_security_set_secure (security, secure);
  e_source_security_set_method (security, secure ? "tls" : "none");
}

static void
show_found (Caldav *caldav, GSList *found)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (caldav->found)))
    gtk_box_remove (GTK_BOX (caldav->found), child);
  for (GSList *l = found; l; l = l->next)
    {
      EWebDAVDiscoveredSource *source = l->data;
      guint32 color = 0;
      GdkRGBA rgba;
      CalPref pref;

      if (source->color && gdk_rgba_parse (&rgba, source->color))
        color = 0xff000000 | ((guint32) (rgba.red * 255) << 16) | ((guint32) (rgba.green * 255) << 8)
                | (guint32) (rgba.blue * 255);
      pref = cal_pref_row ("cal-circle-symbolic", color ? color : 0xff000000 | CAL_COLOR_PICKER_OFFLINE_DEFAULT,
                           source->display_name && *source->display_name ? source->display_name
                                                                         : _("Unnamed calendar"),
                           NULL, NULL);
      gtk_widget_set_can_target (pref.row, FALSE);
      gtk_box_append (GTK_BOX (caldav->found), pref.row);
    }
  gtk_widget_set_visible (caldav->found_title, TRUE);
  gtk_widget_set_visible (caldav->found, TRUE);
  gtk_widget_set_visible (caldav->account->box, TRUE);
  gtk_widget_set_visible (caldav->create, TRUE);
  gtk_widget_set_visible (caldav->login, FALSE);
}

static void
on_discovered (GObject *object, GAsyncResult *result, gpointer data)
{
  Caldav *caldav = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *pem = NULL;
  GTlsCertificateFlags certificate_errors = 0;
  GSList *found = NULL, *addresses = NULL;
  gboolean ok = e_webdav_discover_sources_finish (E_SOURCE (object), result, &pem, &certificate_errors, &found,
                                                   &addresses, &error);

  g_slist_free_full (addresses, g_free);
  if (!ok && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  caldav_busy (caldav, FALSE);
  if (!ok)
    {
      if (g_error_matches (error, E_SOUP_SESSION_ERROR, SOUP_STATUS_UNAUTHORIZED)
          || g_error_matches (error, E_SOUP_SESSION_ERROR, SOUP_STATUS_FORBIDDEN))
        field_set_error (caldav->password, _("The server did not accept this user name and password."));
      else if (pem || certificate_errors)
        caldav_status (caldav, _("The server's certificate is not trusted, so nothing was sent to it."));
      else
        {
          g_autofree char *text = g_strdup_printf (_("Could not reach the server: %s"),
                                                   error ? error->message : _("unknown error"));

          caldav_status (caldav, text);
        }
      return;
    }
  if (!found)
    {
      caldav_status (caldav, _("The server has no calendars for this user."));
      return;
    }
  caldav_status (caldav, NULL);
  show_found (caldav, found);
  e_webdav_discover_free_discovered_sources (found);
}

static void
on_login (VibeosPressable *button, Caldav *caldav)
{
  g_autofree char *text = field_get (caldav->url);
  g_autofree char *user = field_get (caldav->user);
  g_autofree char *password = g_strdup (gtk_editable_get_text (GTK_EDITABLE (caldav->password->text)));
  g_autoptr(GUri) uri = NULL;
  ENamedParameters *credentials;
  ESourceWebdav *webdav;
  gboolean bad = FALSE;

  if (caldav->busy)
    return;
  caldav_status (caldav, NULL);
  if (!*text || !(uri = parse_url (text)))
    {
      field_set_error (caldav->url, _("Enter a valid URL"));
      bad = TRUE;
    }
  if (!*user)
    {
      field_set_error (caldav->user, _("Enter a user name"));
      bad = TRUE;
    }
  if (!*password)
    {
      field_set_error (caldav->password, _("Enter a password"));
      bad = TRUE;
    }
  if (bad)
    return;
  /* the discovery runs on a source of its own that is never saved: nothing is written before "Create account" */
  g_clear_object (&caldav->probe);
  caldav->probe = e_source_new (NULL, NULL, NULL);
  webdav = e_source_get_extension (caldav->probe, E_SOURCE_EXTENSION_WEBDAV_BACKEND);
  e_source_webdav_set_uri (webdav, uri);
  set_auth (caldav->probe, uri, user);
  g_free (caldav->uri);
  caldav->uri = g_uri_to_string (uri);
  credentials = e_named_parameters_new ();
  e_named_parameters_set (credentials, E_SOURCE_CREDENTIAL_USERNAME, user);
  e_named_parameters_set (credentials, E_SOURCE_CREDENTIAL_PASSWORD, password);
  caldav_busy (caldav, TRUE);
  caldav_status (caldav, _("Looking for calendars…"));
  e_webdav_discover_sources (caldav->probe, NULL, E_WEBDAV_DISCOVER_SUPPORTS_EVENTS, credentials,
                             caldav->cancellable, on_discovered, caldav);
  e_named_parameters_free (credentials);
  /* the account's name, as DAVx⁵ suggests it: the user name, with the host when it is not an address */
  if (!*gtk_editable_get_text (GTK_EDITABLE (caldav->account->text)))
    {
      g_autofree char *name = strchr (user, '@') ? g_strdup (user)
                                                 : g_strdup_printf ("%s@%s", user, g_uri_get_host (uri));

      gtk_editable_set_text (GTK_EDITABLE (caldav->account->text), name);
    }
}

typedef struct {
  Caldav *caldav;               /* gone once the page's cancellable is cancelled */
  GCancellable *cancellable;
  GtkWidget *window;
  ESource *collection;
} Create;

static void
create_free (Create *create)
{
  g_object_unref (create->cancellable);
  g_object_unref (create->window);
  g_object_unref (create->collection);
  g_free (create);
}

static void
on_collection_committed (GObject *registry, GAsyncResult *result, gpointer data)
{
  Create *create = data;
  g_autoptr(GError) error = NULL;

  if (!e_source_registry_commit_source_finish (E_SOURCE_REGISTRY (registry), result, &error))
    {
      g_warning ("add CalDAV account: %s", error->message);
      /* the password went in first: take it out again */
      e_source_delete_password (create->collection, NULL, NULL, NULL);
      if (!g_cancellable_is_cancelled (create->cancellable))
        {
          caldav_busy (create->caldav, FALSE);
          caldav_status (create->caldav, _("The account could not be added."));
        }
      create_free (create);
      return;
    }
  if (!g_cancellable_is_cancelled (create->cancellable))
    cal_window_pop (create->window);
  cal_window_toast (create->window, _("Account added. Its calendars appear once they are synchronized."));
  create_free (create);
}

static void
on_password_stored (GObject *source, GAsyncResult *result, gpointer data)
{
  Create *create = data;
  g_autoptr(GError) error = NULL;
  ESourceRegistry *registry = registry_of (create->window);

  if (!e_source_store_password_finish (E_SOURCE (source), result, &error) || !registry)
    {
      g_warning ("add CalDAV account: password: %s", error ? error->message : "no registry");
      if (!g_cancellable_is_cancelled (create->cancellable))
        {
          caldav_busy (create->caldav, FALSE);
          caldav_status (create->caldav, _("The password could not be kept (no keyring), so the account was not added."));
        }
      create_free (create);
      return;
    }
  e_source_registry_commit_source (registry, create->collection, NULL, on_collection_committed, create);
}

static void
on_create (VibeosPressable *button, Caldav *caldav)
{
  g_autofree char *name = field_get (caldav->account);
  g_autofree char *user = field_get (caldav->user);
  g_autoptr(GUri) uri = g_uri_parse (caldav->uri, SOUP_HTTP_URI_FLAGS, NULL);
  Create *create;
  ESourceCollection *collection;

  if (caldav->busy || !uri)
    return;
  if (!*name)
    {
      field_set_error (caldav->account, _("Enter an account name"));
      return;
    }
  create = g_new0 (Create, 1);
  create->caldav = caldav;
  create->cancellable = g_object_ref (caldav->cancellable);
  create->window = g_object_ref (caldav->window);
  create->collection = e_source_new (NULL, NULL, NULL);
  /* Evolution's collection account for CalDAV: the "webdav" collection backend with the calendar URL, calendars
   * only (no address books, no mail) */
  e_source_set_display_name (create->collection, name);
  collection = e_source_get_extension (create->collection, E_SOURCE_EXTENSION_COLLECTION);
  e_source_backend_set_backend_name (E_SOURCE_BACKEND (collection), "webdav");
  e_source_collection_set_identity (collection, user);
  e_source_collection_set_calendar_url (collection, caldav->uri);
  e_source_collection_set_calendar_enabled (collection, TRUE);
  e_source_collection_set_contacts_enabled (collection, FALSE);
  e_source_collection_set_mail_enabled (collection, FALSE);
  set_auth (create->collection, uri, user);
  caldav_busy (caldav, TRUE);
  caldav_status (caldav, NULL);
  /* the password first, under the new account's uid, so the registry finds it when it first signs in */
  e_source_store_password (create->collection, gtk_editable_get_text (GTK_EDITABLE (caldav->password->text)), TRUE,
                           NULL, on_password_stored, create);
}

static void
on_caldav_url_changed (GtkEditable *editable, Caldav *caldav)
{
  /* a changed login is a new one: back to "Log in" */
  caldav_status (caldav, NULL);
  gtk_widget_set_visible (caldav->found_title, FALSE);
  gtk_widget_set_visible (caldav->found, FALSE);
  gtk_widget_set_visible (caldav->account->box, FALSE);
  gtk_widget_set_visible (caldav->create, FALSE);
  gtk_widget_set_visible (caldav->login, TRUE);
}

static GtkWidget *
filled_button (const char *label)
{
  GtkWidget *button = vibeos_pressable_new (gtk_label_new (label));

  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
  gtk_widget_add_css_class (button, "cal-filled-button");
  gtk_widget_set_halign (button, GTK_ALIGN_END);
  return button;
}

void
cal_accounts_add_caldav (GtkWidget *window)
{
  Caldav *caldav = g_new0 (Caldav, 1);
  GtkWidget *page = cal_page_new (window), *scroll = spongy_scroll_view_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *intro;

  caldav->window = window;
  caldav->page = page;
  caldav->cancellable = g_cancellable_new ();
  gtk_box_append (GTK_BOX (page), cal_page_toolbar (CAL_PAGE (page), _("Add CalDAV calendar"), NULL));
  /* DAVx⁵'s "Login with URL and user name" */
  intro = gtk_label_new (_("Login with URL and user name"));
  gtk_label_set_xalign (GTK_LABEL (intro), 0);
  gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
  gtk_widget_add_css_class (intro, "cal-caldav-intro");
  gtk_box_append (GTK_BOX (box), intro);
  caldav->url = field_new (_("Base URL"), FALSE, GTK_INPUT_PURPOSE_URL);
  caldav->user = field_new (_("User name"), FALSE, GTK_INPUT_PURPOSE_EMAIL);
  caldav->password = field_new (_("Password"), TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  caldav->account = field_new (_("Account name"), FALSE, GTK_INPUT_PURPOSE_EMAIL);
  for (Field **f = (Field *[]) { caldav->url, caldav->user, caldav->password, NULL }; *f; f++)
    {
      gtk_widget_add_css_class ((*f)->box, "cal-caldav-field");
      g_signal_connect ((*f)->text, "changed", G_CALLBACK (on_caldav_url_changed), caldav);
      gtk_box_append (GTK_BOX (box), (*f)->box);
    }
  caldav->status = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (caldav->status), 0);
  gtk_label_set_wrap (GTK_LABEL (caldav->status), TRUE);
  gtk_widget_add_css_class (caldav->status, "cal-caldav-status");
  gtk_widget_set_visible (caldav->status, FALSE);
  gtk_box_append (GTK_BOX (box), caldav->status);
  caldav->login = filled_button (_("Log in"));
  g_signal_connect (caldav->login, "clicked", G_CALLBACK (on_login), caldav);
  gtk_box_append (GTK_BOX (box), caldav->login);

  /* then what was found, the account's name and "Create account" */
  caldav->found_title = cal_pref_category (NULL, _("Calendars"));
  gtk_widget_set_visible (caldav->found_title, FALSE);
  gtk_box_append (GTK_BOX (box), caldav->found_title);
  caldav->found = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_visible (caldav->found, FALSE);
  gtk_box_append (GTK_BOX (box), caldav->found);
  gtk_widget_add_css_class (caldav->account->box, "cal-caldav-field");
  gtk_widget_set_visible (caldav->account->box, FALSE);
  gtk_box_append (GTK_BOX (box), caldav->account->box);
  caldav->create = filled_button (_("Create account"));
  g_signal_connect (caldav->create, "clicked", G_CALLBACK (on_create), caldav);
  gtk_widget_set_visible (caldav->create, FALSE);
  gtk_box_append (GTK_BOX (box), caldav->create);

  gtk_widget_add_css_class (box, "cal-caldav");
  /* on a tablet the form is a column in the middle */
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), vibeos_clamp_new (box, VIBEOS_CLAMP_SETTINGS));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  g_object_set_data_full (G_OBJECT (page), "cal-caldav", caldav, caldav_free);
  cal_window_push (window, page);
  gtk_widget_grab_focus (caldav->url->text);
}

/* --- a calendar's page */

typedef struct {
  GtkWidget *window, *page, *list, *title;
  char *uid;
  gboolean local;
  GtkWidget *toggle, *color_row, *count;
  gulong handler;
  CalStore *store;
  GCancellable *cancellable;
  ECalClient *client;           /* a connection of its own when the store has none (a hidden calendar) */
} CalendarPage;

static void
calendar_page_free (gpointer data)
{
  CalendarPage *page = data;

  g_signal_handler_disconnect (page->store, page->handler);
  g_object_unref (page->store);
  g_cancellable_cancel (page->cancellable);
  g_object_unref (page->cancellable);
  g_clear_object (&page->client);
  g_free (page->uid);
  g_free (page);
}

static ESource *
page_source (CalendarPage *page)
{
  ESourceRegistry *registry = registry_of (page->window);

  return registry ? e_source_registry_ref_source (registry, page->uid) : NULL;
}

static void
on_visible (GObject *toggle, GParamSpec *pspec, CalendarPage *page)
{
  cal_store_set_visible (page->store, page->uid, cal_compat_switch_get_active (CAL_COMPAT_SWITCH (toggle)));
}

static void
on_source_written (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!e_source_write_finish (E_SOURCE (source), result, &error))
    g_warning ("calendar %s: %s", e_source_get_uid (E_SOURCE (source)), error->message);
}

static void
on_color_chosen (guint32 rgb, gpointer data)
{
  CalendarPage *page = data;
  g_autoptr(ESource) source = page_source (page);
  g_autofree char *color = color_string (rgb);

  if (!source || !e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
    return;
  e_source_selectable_set_color (e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR), color);
  e_source_write (source, NULL, on_source_written, NULL);
}

static void
show_color_picker (CalendarPage *page)
{
  const CalCalendar *calendar = cal_store_find_calendar (page->store, page->uid);
  int n;
  const guint32 *colors = cal_color_picker_default_colors (&n);

  if (calendar)
    cal_color_picker_show (page->window, _("Calendar Color"), colors, n, calendar->color, on_color_chosen, page);
}

/* a calendar an account keeps in step with its server: its colour may come back from there */
static gboolean
synchronized (CalendarPage *page)
{
  g_autoptr(ESource) source = page_source (page);
  ESourceRegistry *registry = registry_of (page->window);
  g_autoptr(ESource) collection = source && registry ? e_source_registry_find_extension (registry, source,
                                                                                          E_SOURCE_EXTENSION_COLLECTION)
                                                     : NULL;

  return collection != NULL;
}

static void
on_color (VibeosPressable *row, CalendarPage *page)
{
  /* an account without colours of its own gets the default ones, after a warning that a sync may undo the
   * change */
  if (!page->local && synchronized (page))
    {
      VibeosDialog *dialog = vibeos_dialog_new (_("Warning"));

      vibeos_dialog_set_message (dialog, _("Changing the color may be reverted when the calendar is synchronized again."));
      vibeos_dialog_add_button (dialog, _("I understand"), VIBEOS_RESPONSE_OK);
      /* both at once, the warning over the palette */
      show_color_picker (page);
      vibeos_dialog_present (dialog, page->window);
      return;
    }
  show_color_picker (page);
}

static void
on_name_done (const char *text, gpointer data)
{
  CalendarPage *page = data;
  g_autoptr(ESource) source = NULL;

  if (!text || !(source = page_source (page)))
    return;
  /* the name as typed, and the page's title with it */
  e_source_set_display_name (source, text);
  e_source_write (source, NULL, on_source_written, NULL);
  if (page->title)
    gtk_label_set_label (GTK_LABEL (page->title), *text ? text : _("Unnamed calendar"));
}

static void
on_name (VibeosPressable *row, CalendarPage *page)
{
  const CalCalendar *calendar = cal_store_find_calendar (page->store, page->uid);

  cal_text_dialog (page->window, _("Change name"), calendar && calendar->name ? calendar->name : "", on_name_done,
                   page);
}

static void
on_removed (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  g_autoptr(GError) error = NULL;

  if (!e_source_remove_finish (E_SOURCE (source), result, &error))
    {
      g_warning ("delete calendar: %s", error->message);
      cal_window_toast (window, _("The calendar could not be deleted."));
    }
  g_object_unref (window);
}

static void
on_delete_confirmed (int index, gpointer data)
{
  CalendarPage *page = data;
  g_autoptr(ESource) source = NULL;
  GtkWidget *window = page->window;

  if (index < 0 || !(source = page_source (page)) || !e_source_get_removable (source))
    return;
  e_source_remove (source, NULL, on_removed, g_object_ref (window));
  /* back to the list */
  cal_window_pop (window);
}

static void
on_delete (VibeosPressable *row, CalendarPage *page)
{
  cal_confirm_dialog (page->window, _("Do you really want to delete this calendar?"), _("Delete calendar"),
                      on_delete_confirmed, page);
}

static void
on_configure (VibeosPressable *row, GDesktopAppInfo *info)
{
  g_app_info_launch (G_APP_INFO (info), NULL, NULL, NULL);
}

static void
on_calendars_changed (CalStore *store, CalendarPage *page)
{
  const CalCalendar *calendar = cal_store_find_calendar (store, page->uid);
  GtkWidget *icon;

  if (!calendar)
    return;
  g_signal_handlers_block_matched (page->toggle, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_visible, NULL);
  cal_compat_switch_set_active (CAL_COMPAT_SWITCH (page->toggle), calendar->visible);
  g_signal_handlers_unblock_matched (page->toggle, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_visible, NULL);
  /* the colour row's circle in the calendar's colour */
  icon = cal_pref_icon ("cal-circle-symbolic", 0xff000000 | calendar->color);
  {
    GtkWidget *frame = g_object_get_data (G_OBJECT (page->color_row), "cal-icon-frame");
    GtkWidget *old = gtk_widget_get_first_child (frame);

    if (old)
      gtk_box_remove (GTK_BOX (frame), old);
    gtk_box_append (GTK_BOX (frame), icon);
  }
  if (page->title)
    gtk_label_set_label (GTK_LABEL (page->title), calendar->name && *calendar->name ? calendar->name
                                                                                   : _("Unnamed calendar"));
}

static void
show_count (CalendarPage *page, int count)
{
  g_autofree char *text = g_strdup_printf (_("%d events"), count);

  gtk_label_set_label (GTK_LABEL (page->count), text);
}

static void
on_objects (GObject *client, GAsyncResult *result, gpointer data)
{
  CalendarPage *page = data;
  g_autoptr(GError) error = NULL;
  GSList *objects = NULL;

  /* an empty calendar answers FALSE without an error (no list to hand over): that is a count of 0 */
  if (!e_cal_client_get_object_list_finish (E_CAL_CLIENT (client), result, &objects, &error) && error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("calendar %s: no count: %s", page->uid, error->message);
      return;
    }
  /* Events._COUNT for the calendar: every VEVENT, a moved occurrence of a series being one of its own */
  show_count (page, (int) g_slist_length (objects));
  g_slist_free_full (objects, g_object_unref);
}

static void
count_events (CalendarPage *page, ECalClient *client)
{
  e_cal_client_get_object_list (client, "#t", page->cancellable, on_objects, page);
}

static void
on_connected (GObject *source, GAsyncResult *result, gpointer data)
{
  CalendarPage *page = data;
  g_autoptr(GError) error = NULL;
  EClient *client = e_cal_client_connect_finish (result, &error);

  if (!client)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("calendar %s: no count: %s", page->uid, error->message);
      return;
    }
  page->client = E_CAL_CLIENT (client);
  count_events (page, page->client);
}

/* the account's label for "%s account": what kind of account EDS keeps it in */
static char *
account_label (CalendarPage *page, const CalCalendar *calendar)
{
  g_autoptr(ESource) source = page_source (page);
  ESourceRegistry *registry = registry_of (page->window);
  g_autoptr(ESource) collection = source ? e_source_registry_find_extension (registry, source,
                                                                              E_SOURCE_EXTENSION_COLLECTION)
                                         : NULL;
  static const struct { const char *backend, *label; } labels[] = {
    { "google", N_("Google") }, { "webdav", N_("CalDAV") }, { "caldav", N_("CalDAV") },
    { "owncloud", N_("Nextcloud") }, { "outlook", N_("Outlook") }, { "microsoft365", N_("Microsoft 365") },
    { "ews", N_("Exchange") }, { "yahoo", N_("Yahoo") }, { "webcal", N_("Web calendar") },
    { "contacts", N_("Contacts") }, { "weather", N_("Weather") },
  };
  const char *backend = NULL;

  if (collection)
    backend = e_source_backend_get_backend_name (e_source_get_extension (collection, E_SOURCE_EXTENSION_COLLECTION));
  else if (source && e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR))
    backend = e_source_backend_get_backend_name (e_source_get_extension (source, E_SOURCE_EXTENSION_CALENDAR));
  for (guint i = 0; backend && i < G_N_ELEMENTS (labels); i++)
    if (g_str_equal (backend, labels[i].backend))
      return g_strdup (_(labels[i].label));
  return g_strdup (calendar->account);
}

/* "Configure calendar in %s": an account from GNOME Online Accounts opens its settings panel, when the system has
 * one; none for any other */
static GDesktopAppInfo *
configure_app (CalendarPage *page)
{
  g_autoptr(ESource) source = page_source (page);
  ESourceRegistry *registry = registry_of (page->window);
  g_autoptr(ESource) goa = source ? e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_GOA)
                                  : NULL;

  return goa ? g_desktop_app_info_new ("gnome-online-accounts-panel.desktop") : NULL;
}

static GtkWidget *
info_row (const char *title, GtkWidget **label)
{
  CalPref pref = cal_pref_row (NULL, 0, title, NULL, NULL);

  /* not selectable: no ripple, no tap */
  gtk_widget_set_can_target (pref.row, FALSE);
  if (label)
    *label = pref.title;
  return pref.row;
}

GtkWidget *
cal_calendar_page_new (GtkWidget *window, const CalCalendar *calendar)
{
  CalendarPage *data = g_new0 (CalendarPage, 1);
  g_autoptr(ESource) source = NULL;
  g_autoptr(GDesktopAppInfo) configure = NULL;
  GtkWidget *list, *page, *frame;
  CalPref pref;

  page = cal_pref_page (window, calendar->name && *calendar->name ? calendar->name : _("Unnamed calendar"), &list);
  data->window = window;
  data->page = page;
  data->list = list;
  data->uid = g_strdup (calendar->uid);
  data->store = g_object_ref (cal_window_get_store (window));
  data->cancellable = g_cancellable_new ();
  /* the toolbar's title, which "Change name" updates */
  for (GtkWidget *child = gtk_widget_get_first_child (gtk_widget_get_first_child (page)); child;
       child = gtk_widget_get_next_sibling (child))
    if (gtk_widget_has_css_class (child, "cal-toolbar-title"))
      data->title = child;
  source = page_source (data);
  data->local = source && e_source_has_extension (source, E_SOURCE_EXTENSION_CALENDAR)
                && g_strcmp0 (e_source_backend_get_backend_name (e_source_get_extension (source,
                                                                                         E_SOURCE_EXTENSION_CALENDAR)),
                              "local") == 0;
  g_object_set_data_full (G_OBJECT (page), "cal-calendar", data, calendar_page_free);

  /* Display events */
  data->toggle = cal_compat_switch_new ();
  cal_compat_switch_set_active (CAL_COMPAT_SWITCH (data->toggle), calendar->visible);
  gtk_accessible_update_property (GTK_ACCESSIBLE (data->toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Display events"),
                                  -1);
  g_signal_connect (data->toggle, "notify::active", G_CALLBACK (on_visible), data);
  pref = cal_pref_row (NULL, 0, _("Display events"), NULL, data->toggle);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (cal_pref_toggle_row), data->toggle);
  gtk_box_append (GTK_BOX (list), pref.row);

  /* no "Manage notifications": reminders are one notification source for the whole app, with no per-calendar
   * settings to open */

  /* Color: a circle in the calendar's colour */
  pref = cal_pref_row ("cal-circle-symbolic", 0xff000000 | calendar->color, _("Color"), NULL, NULL);
  frame = gtk_widget_get_first_child (gtk_widget_get_parent (gtk_widget_get_parent (pref.title)));
  g_object_set_data (G_OBJECT (pref.row), "cal-icon-frame", frame);
  data->color_row = pref.row;
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_color), data);
  gtk_box_append (GTK_BOX (list), pref.row);

  /* Change name and Delete calendar: an offline calendar's only */
  if (data->local)
    {
      pref = cal_pref_row (NULL, 0, _("Change name"), NULL, NULL);
      g_signal_connect (pref.row, "clicked", G_CALLBACK (on_name), data);
      gtk_box_append (GTK_BOX (list), pref.row);
      if (source && e_source_get_removable (source))
        {
          pref = cal_pref_row (NULL, 0, _("Delete calendar"), NULL, NULL);
          g_signal_connect (pref.row, "clicked", G_CALLBACK (on_delete), data);
          gtk_box_append (GTK_BOX (list), pref.row);
        }
    }
  else if ((configure = configure_app (data)))
    {
      g_autofree char *title = g_strdup_printf (_("Configure calendar in %s"),
                                                g_app_info_get_display_name (G_APP_INFO (configure)));

      pref = cal_pref_row (NULL, 0, title, NULL, NULL);
      g_signal_connect_data (pref.row, "clicked", G_CALLBACK (on_configure), g_object_ref (configure),
                             (GClosureNotify) (void (*) (void)) g_object_unref, 0);
      gtk_box_append (GTK_BOX (list), pref.row);
    }

  /* Calendar information: the number of events, the account */
  gtk_box_append (GTK_BOX (list), cal_pref_category (NULL, _("Calendar information")));
  gtk_box_append (GTK_BOX (list), info_row ("", &data->count));
  if (data->local)
    {
      gtk_box_append (GTK_BOX (list), info_row (_("Offline calendar"), NULL));
      gtk_box_append (GTK_BOX (list),
                      info_row (_("Offline calendars are NOT synchronized to a cloud service and only available on your phone."),
                                NULL));
    }
  else
    {
      g_autofree char *label = account_label (data, calendar);
      g_autofree char *title = g_strdup_printf (_("%s account"), label);

      gtk_box_append (GTK_BOX (list), info_row (title, NULL));
    }
  /* the count: from the store's connection, or one of the page's own for a calendar that is not shown */
  if (cal_store_get_client (data->store, data->uid))
    count_events (data, cal_store_get_client (data->store, data->uid));
  else if (source)
    e_cal_client_connect (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, data->cancellable, on_connected, data);

  data->handler = g_signal_connect (data->store, "calendars-changed", G_CALLBACK (on_calendars_changed), data);
  return page;
}
