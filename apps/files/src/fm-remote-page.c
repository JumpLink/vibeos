/* The Remote page and adding a remote (fm-remote-page.h). */
#include "fm-remote-page.h"
#include "fm-dialogs.h"
#include "fm-icon.h"
#include "fm-mount.h"
#include "fm-places.h"
#include "fm-scan.h"
#include "fm-ui.h"
#include "fm-util.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>
#include <stdlib.h>

#define SCAN_LINGER_MS 5000     /* how long the scan keeps its bar after the first listing: WS-Discovery and DNS-SD
                                 * answer later, through the monitor */

struct _FmRemotePage {
  FmShell *shell;
  GtkWidget *scroll, *box;
  gboolean editing;
};

/* ---- the server dialog ------------------------------------------------------------------------------------------- */

typedef struct {
  FmRemotePage *page;
  FmProto proto;
  char *id;                     /* the remote being edited, or NULL for a new one */
  GtkWidget *host, *port, *user, *password, *domain, *name;
  GtkWidget *anonymous, *secure, *plain;       /* the check, and the https / FTPS and http / FTP radios */
  GtkWidget *ok;
} Server;

static void
server_free (gpointer data)
{
  Server *server = data;

  g_free (server->id);
  g_free (server);
}

static void
server_check (Server *server)
{
  g_autofree char *host = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (server->host))));

  gtk_widget_set_sensitive (server->ok, *host != '\0');
}

static void
on_server_changed (GtkEditable *editable, Server *server)
{
  server_check (server);
}

static void
on_anonymous (GtkWidget *check, GParamSpec *pspec, Server *server)
{
  gboolean anonymous = fm_check_get_active (check);

  gtk_widget_set_sensitive (gtk_widget_get_parent (server->user), !anonymous);
  gtk_widget_set_sensitive (gtk_widget_get_parent (server->password), !anonymous);
  if (server->domain)
    gtk_widget_set_sensitive (gtk_widget_get_parent (server->domain), !anonymous);
}

/* the radios of a pair: one on clears the other */
static void
on_radio (GtkWidget *check, GParamSpec *pspec, GtkWidget *other)
{
  if (fm_check_get_active (check))
    fm_check_set_active (other, FALSE);
  else if (!fm_check_get_active (other))
    fm_check_set_active (check, TRUE);
}

typedef struct {
  FmShell *shell;
  char *uri;
  FmRemote *remote;
} Opening;

static void
on_server_mounted (gboolean ok, const GError *error, gpointer data)
{
  Opening *opening = data;

  if (ok)
    {
      g_autoptr(GFile) root = g_file_new_for_uri (opening->uri);

      fm_shell_open_location (opening->shell, root);
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
    {
      g_autoptr(GFile) root = g_file_new_for_uri (opening->uri);
      g_autofree char *message = fm_mount_error_message (root, opening->remote);

      fm_shell_toast (opening->shell, message);
    }
  fm_remote_free (opening->remote);
  g_free (opening->uri);
  g_free (opening);
}

/* a field's text into *@value, unless the field is empty and *@value is not (a URI typed in Host gave it) */
static void
take_field (char **value, GtkWidget *entry)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));

  if (*text || !*value || !**value)
    {
      g_free (*value);
      *value = g_steal_pointer (&text);
    }
}

static void
on_server_response (NeoDialog *dialog, int response, Server *server)
{
  FmShell *shell = server->page->shell;
  FmRemote *remote, *old = server->id ? fm_shell_find_remote (shell, server->id) : NULL;
  const char *port = gtk_editable_get_text (GTK_EDITABLE (server->port));
  Opening *opening;
  g_autofree char *password = NULL;

  if (response != NEO_RESPONSE_OK)
    return;
  remote = fm_remote_new (server->proto);
  if (server->secure && fm_check_get_active (server->secure))
    remote->proto = server->proto == FM_PROTO_FTP ? FM_PROTO_FTPS : FM_PROTO_DAVS;
  else if (server->plain && fm_check_get_active (server->plain))
    remote->proto = server->proto == FM_PROTO_FTP ? FM_PROTO_FTP : FM_PROTO_DAV;
  fm_remote_set_address (remote, gtk_editable_get_text (GTK_EDITABLE (server->host)));
  if (*port)
    remote->port = atoi (port);
  if (remote->port == fm_proto_default_port (remote->proto))
    remote->port = 0;
  /* the fields win over what a whole URI typed in Host says, when they say anything */
  take_field (&remote->user, server->user);
  if (server->domain)
    take_field (&remote->domain, server->domain);
  remote->anonymous = server->anonymous && fm_check_get_active (fm_check_row_get_check (server->anonymous));
  g_free (remote->name);
  remote->name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (server->name))));
  g_free (remote->id);
  remote->id = old ? g_strdup (old->id) : fm_remotes_new_id (shell->remotes);
  /* the password goes to GVfs with the mount, and from there to the keyring: never into the remote (a URI's own,
   * "smb://user:secret@host/share", when the field is empty) */
  password = g_strdup (gtk_editable_get_text (GTK_EDITABLE (server->password)));
  if (!*password)
    {
      g_autoptr(GUri) uri = g_uri_parse (gtk_editable_get_text (GTK_EDITABLE (server->host)), G_URI_FLAGS_HAS_PASSWORD,
                                         NULL);

      if (uri && g_uri_get_password (uri))
        {
          g_free (password);
          password = g_uri_unescape_string (g_uri_get_password (uri), NULL);
        }
    }
  if (old)
    {
      for (guint i = 0; i < shell->remotes->len; i++)
        if (shell->remotes->pdata[i] == old)
          {
            g_ptr_array_index (shell->remotes, i) = remote;
            fm_remote_free (old);
            break;
          }
    }
  else
    g_ptr_array_add (shell->remotes, remote);
  fm_shell_remotes_changed (shell);

  opening = g_new0 (Opening, 1);
  opening->shell = shell;
  opening->uri = fm_remote_uri (remote);
  opening->remote = fm_remote_copy (remote);
  {
    g_autoptr(GFile) root = g_file_new_for_uri (opening->uri);

    fm_mount (shell->window, root, remote, remote->anonymous ? NULL : password, on_server_mounted, opening);
  }
  gtk_editable_set_text (GTK_EDITABLE (server->password), "");
}

static GtkWidget *
radio_pair (Server *server, const char *label, const char *first, const char *second, gboolean second_on)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *a = fm_check_row (first, TRUE, !second_on);
  GtkWidget *b = fm_check_row (second, TRUE, second_on);

  gtk_widget_set_hexpand (a, TRUE);
  gtk_widget_set_hexpand (b, TRUE);
  gtk_box_append (GTK_BOX (box), a);
  gtk_box_append (GTK_BOX (box), b);
  server->plain = fm_check_row_get_check (a);
  server->secure = fm_check_row_get_check (b);
  g_signal_connect (server->plain, "notify::active", G_CALLBACK (on_radio), server->secure);
  g_signal_connect (server->secure, "notify::active", G_CALLBACK (on_radio), server->plain);
  return fm_form_row (label, box);
}

static GtkWidget *
field (const char *text, const char *hint)
{
  GtkWidget *entry = fm_entry_new (text);

  if (hint)
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry), hint);
  return entry;
}

static void
server_dialog (FmRemotePage *page, FmProto proto, const FmRemote *editing, const char *host)
{
  GtkWidget *content, *form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *scroll = spongy_scroll_view_new ();
  Server *server = g_new0 (Server, 1);
  NeoDialog *dialog;
  g_autofree char *address = editing ? fm_remote_get_address (editing) : g_strdup (host);
  g_autofree char *port = editing && editing->port ? g_strdup_printf ("%d", editing->port) : NULL;
  g_autofree char *default_port = NULL;
  const char *hint;
  FmProto base = proto == FM_PROTO_FTPS ? FM_PROTO_FTP : proto == FM_PROTO_DAVS ? FM_PROTO_DAV : proto;

  server->page = page;
  server->proto = base;
  server->id = editing ? g_strdup (editing->id) : NULL;
  dialog = fm_dialog_new (fm_proto_label (base), &content);
  gtk_widget_add_css_class (content, "fm-server-dialog");
  switch (base)
    {
    case FM_PROTO_SMB:
    case FM_PROTO_SFTP:
      hint = _("e.g. 192.168.1.101 or 192.168.1.101/folder");
      break;
    case FM_PROTO_DAV:
      hint = _("e.g. example.com/webdav");
      break;
    default:
      hint = _("e.g. 192.168.1.101");
      break;
    }
  server->host = field (address, hint);
  gtk_entry_set_input_purpose (GTK_ENTRY (server->host), GTK_INPUT_PURPOSE_URL);
  gtk_box_append (GTK_BOX (form), fm_form_row (_("Host"), server->host));
  /* WebDAV's http / https, https first */
  if (base == FM_PROTO_DAV)
    gtk_box_append (GTK_BOX (form), radio_pair (server, "", "http", "https", proto != FM_PROTO_DAV || !editing));
  default_port = g_strdup_printf ("%d", fm_proto_default_port (proto == FM_PROTO_DAV && !editing ? FM_PROTO_DAVS
                                                                                                  : proto));
  server->port = field (port, default_port);
  gtk_entry_set_input_purpose (GTK_ENTRY (server->port), GTK_INPUT_PURPOSE_DIGITS);
  gtk_box_append (GTK_BOX (form), fm_form_row (_("Port"), server->port));
  if (base == FM_PROTO_FTP)
    gtk_box_append (GTK_BOX (form), radio_pair (server, _("Security mode"), "FTP", "FTPS", proto == FM_PROTO_FTPS));
  server->user = field (editing ? editing->user : NULL, NULL);
  gtk_box_append (GTK_BOX (form), fm_form_row (_("Username"), server->user));
  server->password = field (NULL, editing ? _("(saved)") : NULL);
  gtk_entry_set_visibility (GTK_ENTRY (server->password), FALSE);
  gtk_entry_set_input_purpose (GTK_ENTRY (server->password), GTK_INPUT_PURPOSE_PASSWORD);
  gtk_box_append (GTK_BOX (form), fm_form_row (_("Password"), server->password));
  if (base == FM_PROTO_SMB || base == FM_PROTO_FTP)
    {
      server->anonymous = fm_check_row (_("Anonymous"), FALSE, editing && editing->anonymous);
      gtk_widget_add_css_class (server->anonymous, "fm-form-check");
      gtk_box_append (GTK_BOX (form), server->anonymous);
    }
  if (base == FM_PROTO_SMB)
    {
      server->domain = field (editing ? editing->domain : NULL, _("Optional domain name"));
      gtk_box_append (GTK_BOX (form), fm_form_row (_("Domain"), server->domain));
    }
  server->name = field (editing ? editing->name : NULL, _("Optional"));
  gtk_box_append (GTK_BOX (form), fm_form_row (_("Display name"), server->name));
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), form);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (content), scroll);
  if (server->anonymous)
    {
      g_signal_connect (fm_check_row_get_check (server->anonymous), "notify::active", G_CALLBACK (on_anonymous),
                        server);
      on_anonymous (fm_check_row_get_check (server->anonymous), NULL, server);
    }
  fm_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  server->ok = fm_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_signal_connect (server->host, "changed", G_CALLBACK (on_server_changed), server);
  server_check (server);
  g_object_set_data_full (G_OBJECT (dialog), "fm-server", server, server_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_server_response), server);
  neo_dialog_present (dialog, page->shell->window);
}

/* ---- the Local Network scan --------------------------------------------------------------------------------------- *
 * Two sources, one list: the subnet scan (fm-scan.h), which finds the file servers whether they announce themselves
 * or not, and network:/// (the workgroup's browse list, DNS-SD and WS-Discovery, when the system has them). A host
 * both of them find shows once: network:///'s names are resolved and the rows are keyed by address. A tap browses
 * the host's shares (smb://host/); a share mounts with the password dialog, anonymously if the server lets it. */

typedef struct {
  FmRemotePage *page;
  NeoDialog *dialog;
  GtkWidget *list, *progress, *empty;
  GFileMonitor *monitor;
  GCancellable *cancellable;
  GHashTable *rows;             /* an address (or an unresolved host name) -> its row, not held */
  guint linger;
  gboolean network_busy, subnet_busy;
} Scan;

static void
scan_free (gpointer data)
{
  Scan *scan = data;

  g_cancellable_cancel (scan->cancellable);
  g_object_unref (scan->cancellable);
  if (scan->monitor)
    {
      g_signal_handlers_disconnect_by_data (scan->monitor, scan);
      g_file_monitor_cancel (scan->monitor);
      g_object_unref (scan->monitor);
    }
  g_clear_handle_id (&scan->linger, g_source_remove);
  g_hash_table_unref (scan->rows);
  g_free (scan);
}

static void
scan_update (Scan *scan)
{
  gboolean scanning = scan->network_busy || scan->subnet_busy;
  gboolean any = gtk_widget_get_first_child (scan->list) != NULL;

  fm_progress_set_fraction (scan->progress, scanning ? -1 : 0);
  gtk_widget_set_visible (scan->progress, scanning);
  gtk_widget_set_visible (scan->empty, !scanning && !any);
}

static void
on_scan_row (NeoPressable *row, Scan *scan)
{
  const char *host = g_object_get_data (G_OBJECT (row), "fm-host");
  /* the host's shares: GVfs lists them (smb-browse), and a tap on one mounts it */
  g_autofree char *uri = g_uri_join (G_URI_FLAGS_NONE, "smb", NULL, host, -1, "/", NULL, NULL);
  g_autoptr(GFile) server = g_file_new_for_uri (uri);
  FmRemotePage *page = scan->page;

  neo_dialog_respond (scan->dialog, NEO_RESPONSE_OK);
  fm_shell_open_location (page->shell, server);
}

/* a host's row: the server icon and its name; @host is what smb:// is given (an address, or a name) */
static GtkWidget *
scan_row_add (Scan *scan, const char *name, const char *host)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *row, *label;
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE_CARD);

  gtk_box_append (GTK_BOX (line), fm_icon_new (FM_GLYPH_SERVER, 40));
  label = fm_label (name, "fm-scan-name", 18, FALSE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (line), label);
  gtk_widget_add_css_class (line, "fm-scan-line");
  row = neo_pressable_new (line);
  gtk_widget_add_css_class (row, "fm-scan-row");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
  g_object_set_data_full (G_OBJECT (row), "fm-host", g_strdup (host), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_scan_row), scan);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  gtk_box_append (GTK_BOX (scan->list), row);
  scan_update (scan);
  return row;
}

/* the callbacks hold the scan's cancellable, not the scan: a closed dialog has freed the scan and cancelled it */
static Scan *
scan_of (GCancellable *cancellable)
{
  return g_cancellable_is_cancelled (cancellable) ? NULL : g_object_get_data (G_OBJECT (cancellable), "fm-scan");
}

/* the subnet scan's hosts (fm_scan_start calls nothing once the cancellable is cancelled, so the scan is there) */
static void
on_subnet_found (const char *address, const char *name, gpointer data)
{
  Scan *scan = data;

  if (!g_hash_table_contains (scan->rows, address))
    g_hash_table_insert (scan->rows, g_strdup (address), scan_row_add (scan, name, address));
}

static void
on_subnet_done (gpointer data)
{
  Scan *scan = data;

  scan->subnet_busy = FALSE;
  scan_update (scan);
}

/* a host of network:///, resolved: one row for its addresses, unless the subnet scan has one for them */
typedef struct {
  GCancellable *cancellable;
  char *name, *host;
} Announced;

static void
announced_free (Announced *announced)
{
  g_object_unref (announced->cancellable);
  g_free (announced->name);
  g_free (announced->host);
  g_free (announced);
}

static void
on_announced_resolved (GObject *source, GAsyncResult *result, gpointer data)
{
  Announced *announced = data;
  GList *addresses = g_resolver_lookup_by_name_finish (G_RESOLVER (source), result, NULL);
  Scan *scan = scan_of (announced->cancellable);
  g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func (g_free);

  for (GList *l = addresses; l; l = l->next)
    g_ptr_array_add (keys, g_inet_address_to_string (l->data));
  g_resolver_free_addresses (addresses);
  if (!keys->len)
    g_ptr_array_add (keys, g_strdup (announced->host));
  for (guint i = 0; scan && i < keys->len; i++)
    if (g_hash_table_contains (scan->rows, keys->pdata[i]))
      scan = NULL;
  if (scan)
    {
      GtkWidget *row = scan_row_add (scan, announced->name, announced->host);

      for (guint i = 0; i < keys->len; i++)
        g_hash_table_insert (scan->rows, g_strdup (keys->pdata[i]), row);
    }
  announced_free (announced);
}

/* a host from network:///: its name, and the host of its smb:// target */
static void
scan_add (Scan *scan, GFileInfo *info)
{
  const char *target = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
  const char *name = g_file_info_get_display_name (info);
  g_autoptr(GUri) uri = target ? g_uri_parse (target, G_URI_FLAGS_NONE, NULL) : NULL;
  Announced *announced;
  const char *host;

  /* SMB servers only, as File Manager+ lists (the workgroup's, WS-Discovery's and DNS-SD's _smb._tcp) */
  if (!uri || g_strcmp0 (g_uri_get_scheme (uri), "smb") || !(host = g_uri_get_host (uri)) || !*host
      || g_hash_table_contains (scan->rows, host))
    return;
  announced = g_new0 (Announced, 1);
  announced->cancellable = g_object_ref (scan->cancellable);
  announced->name = g_strdup (name ? name : host);
  announced->host = g_strdup (host);
  g_resolver_lookup_by_name_async (g_resolver_get_default (), host, scan->cancellable, on_announced_resolved,
                                   announced);
}

static gboolean
on_linger_done (gpointer data)
{
  Scan *scan = data;

  scan->linger = 0;
  scan->network_busy = FALSE;
  scan_update (scan);
  return G_SOURCE_REMOVE;
}

static void
on_network_info (GObject *source, GAsyncResult *result, gpointer data)
{
  GCancellable *cancellable = data;
  g_autoptr(GFileInfo) info = g_file_query_info_finish (G_FILE (source), result, NULL);
  Scan *scan = scan_of (cancellable);

  if (info && scan)
    scan_add (scan, info);
  g_object_unref (cancellable);
}

static void
on_network_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, Scan *scan)
{
  if (event == G_FILE_MONITOR_EVENT_CREATED)
    g_file_query_info_async (file, "standard::display-name,standard::target-uri", G_FILE_QUERY_INFO_NONE,
                             G_PRIORITY_DEFAULT, scan->cancellable, on_network_info,
                             g_object_ref (scan->cancellable));
}

static void
on_network_next (GObject *source, GAsyncResult *result, gpointer data)
{
  GCancellable *cancellable = data;
  GFileEnumerator *enumerator = G_FILE_ENUMERATOR (source);
  GList *infos = g_file_enumerator_next_files_finish (enumerator, result, NULL);
  Scan *scan = scan_of (cancellable);

  if (!scan)
    {
      g_list_free_full (infos, g_object_unref);
      g_object_unref (enumerator);
      g_object_unref (cancellable);
      return;
    }
  for (GList *l = infos; l; l = l->next)
    scan_add (scan, l->data);
  if (infos)
    {
      g_list_free_full (infos, g_object_unref);
      g_file_enumerator_next_files_async (enumerator, 50, G_PRIORITY_DEFAULT, cancellable, on_network_next,
                                          cancellable);
      return;
    }
  g_object_unref (enumerator);
  g_object_unref (cancellable);
  scan->linger = g_timeout_add (SCAN_LINGER_MS, on_linger_done, scan);
}

static void
on_network_listed (GObject *source, GAsyncResult *result, gpointer data)
{
  GCancellable *cancellable = data;
  g_autoptr(GError) error = NULL;
  GFileEnumerator *enumerator = g_file_enumerate_children_finish (G_FILE (source), result, &error);
  Scan *scan = scan_of (cancellable);

  if (!scan)
    {
      g_clear_object (&enumerator);
      g_object_unref (cancellable);
      return;
    }
  if (!enumerator)
    {
      /* no network:/// (no gvfs-backends): the subnet scan alone */
      g_debug ("network:///: %s", error->message);
      scan->network_busy = FALSE;
      scan_update (scan);
      g_object_unref (cancellable);
      return;
    }
  g_file_enumerator_next_files_async (enumerator, 50, G_PRIORITY_DEFAULT, cancellable, on_network_next, cancellable);
}

static void
on_scan_response (NeoDialog *dialog, int response, Scan *scan)
{
  g_cancellable_cancel (scan->cancellable);
  /* MANUAL INPUT: the SMB server dialog, empty */
  if (response == NEO_RESPONSE_OK + 1)
    server_dialog (scan->page, FM_PROTO_SMB, NULL, NULL);
}

static void
local_network_dialog (FmRemotePage *page)
{
  GtkWidget *content, *area = gtk_overlay_new (), *scroll = spongy_scroll_view_new ();
  g_autoptr(GFile) network = g_file_new_for_uri ("network:///");
  Scan *scan = g_new0 (Scan, 1);

  scan->page = page;
  scan->cancellable = g_cancellable_new ();
  g_object_set_data (G_OBJECT (scan->cancellable), "fm-scan", scan);
  scan->rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  scan->network_busy = scan->subnet_busy = TRUE;
  scan->dialog = fm_dialog_new (_("Local Network"), &content);
  gtk_widget_add_css_class (content, "fm-scan-dialog");
  scan->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (scan->list, "fm-scan-list");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), scan->list);
  gtk_overlay_set_child (GTK_OVERLAY (area), scroll);
  scan->progress = fm_progress_new (2.67);
  gtk_widget_add_css_class (scan->progress, "fm-scan-progress");
  gtk_widget_set_valign (scan->progress, GTK_ALIGN_START);
  gtk_overlay_add_overlay (GTK_OVERLAY (area), scan->progress);
  scan->empty = fm_label (_("Empty"), "fm-scan-empty", 16, FALSE);
  gtk_widget_set_halign (scan->empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (scan->empty, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay (GTK_OVERLAY (area), scan->empty);
  gtk_widget_add_css_class (area, "fm-scan-area");
  gtk_widget_set_vexpand (area, TRUE);
  gtk_box_append (GTK_BOX (content), area);
  fm_dialog_add_button (scan->dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  fm_dialog_add_button (scan->dialog, _("Manual input"), NEO_RESPONSE_OK + 1);
  g_object_set_data_full (G_OBJECT (scan->dialog), "fm-scan", scan, scan_free);
  g_signal_connect (scan->dialog, "response", G_CALLBACK (on_scan_response), scan);
  scan_update (scan);
  /* every address of the networks the device is on, as File Manager+ does */
  fm_scan_start (scan->cancellable, on_subnet_found, on_subnet_done, scan);
  /* network:/// merges the workgroup's browse list, DNS-SD and WS-Discovery; the monitor brings the late ones */
  scan->monitor = g_file_monitor_directory (network, G_FILE_MONITOR_NONE, NULL, NULL);
  if (scan->monitor)
    g_signal_connect (scan->monitor, "changed", G_CALLBACK (on_network_changed), scan);
  g_file_enumerate_children_async (network, "standard::display-name,standard::target-uri", G_FILE_QUERY_INFO_NONE,
                                   G_PRIORITY_DEFAULT, scan->cancellable, on_network_listed,
                                   g_object_ref (scan->cancellable));
  neo_dialog_present (scan->dialog, page->shell->window);
}

/* ---- Add Remote ---------------------------------------------------------------------------------------------------- */

static void
on_add_chosen (int index, gpointer data)
{
  static const FmProto protos[] = { FM_PROTO_SMB, FM_PROTO_FTP, FM_PROTO_SFTP, FM_PROTO_DAV };
  FmRemotePage *page = data;

  if (index == 0)
    local_network_dialog (page);
  else if (index > 0)
    server_dialog (page, protos[index - 1], NULL, NULL);
}

void
fm_remote_page_add (FmRemotePage *page)
{
  const FmListRow rows[] = {
    { FM_GLYPH_NETWORK, NULL, _("Local Network") },
    { FM_GLYPH_REMOTE_SMB, NULL, "SMB" },
    { FM_GLYPH_REMOTE_FTP, NULL, "FTP" },
    { FM_GLYPH_REMOTE_SFTP, NULL, "SFTP" },
    { FM_GLYPH_REMOTE_DAV, NULL, "WebDAV" },
  };

  fm_list_dialog (page->shell->window, _("Add Remote"), rows, G_N_ELEMENTS (rows), on_add_chosen, page);
}

/* ---- the list --------------------------------------------------------------------------------------------------- */

typedef struct {
  FmRemotePage *page;
  char *id;
} RemoteRef;

static void
remote_ref_free (gpointer data)
{
  RemoteRef *ref = data;

  g_free (ref->id);
  g_free (ref);
}

static void
on_renamed (const char *text, gpointer data)
{
  RemoteRef *ref = data;
  FmRemote *remote = fm_shell_find_remote (ref->page->shell, ref->id);

  if (text && remote)
    {
      g_free (remote->name);
      remote->name = g_strstrip (g_strdup (text));
      fm_shell_remotes_changed (ref->page->shell);
      fm_shell_toast (ref->page->shell, _("Renamed successfully"));
    }
  remote_ref_free (ref);
}

static void
on_remote_menu (int index, gpointer data)
{
  RemoteRef *ref = data;
  FmShell *shell = ref->page->shell;
  FmRemote *remote = fm_shell_find_remote (shell, ref->id);

  if (!remote)
    return;
  switch (index)
    {
    case 0:
      server_dialog (ref->page, remote->proto, remote, NULL);
      break;
    case 1:
      {
        RemoteRef *rename = g_new0 (RemoteRef, 1);

        rename->page = ref->page;
        rename->id = g_strdup (ref->id);
        fm_text_dialog (shell->window, _("Rename"), fm_remote_label (remote), -1, NULL, on_renamed, rename);
        break;
      }
    case 2:
      /* its saved password goes too: the only keyring call the app makes */
      fm_mount_forget_password (remote);
      g_ptr_array_remove (shell->remotes, remote);
      fm_shell_remotes_changed (shell);
      break;
    default:
      break;
    }
}

static void
remote_menu (FmRemotePage *page, GtkWidget *row)
{
  const char *labels[] = { _("Edit"), _("Rename"), _("Remove") };

  fm_popup_show (row, labels, 3, FM_POPUP_AT, 60, 40, on_remote_menu, g_object_get_data (G_OBJECT (row), "fm-ref"));
}

static void
on_remote_clicked (NeoPressable *row, FmRemotePage *page)
{
  RemoteRef *ref = g_object_get_data (G_OBJECT (row), "fm-ref");
  FmRemote *remote = fm_shell_find_remote (page->shell, ref->id);
  g_autofree char *uri = NULL;
  g_autoptr(GFile) root = NULL;

  if (!remote)
    return;
  if (page->editing)
    {
      remote_menu (page, GTK_WIDGET (row));
      return;
    }
  /* the browser mounts it if it has to (the password from the keyring, else the password dialog) */
  uri = fm_remote_uri (remote);
  root = g_file_new_for_uri (uri);
  fm_shell_open_location (page->shell, root);
}

static void
on_remote_long_pressed (NeoPressable *row, FmRemotePage *page)
{
  remote_menu (page, GTK_WIDGET (row));
}

static void
on_footer (NeoPressable *footer, FmRemotePage *page)
{
  fm_remote_page_add (page);
}

void
fm_remote_page_refresh (FmRemotePage *page)
{
  GPtrArray *remotes = page->shell->remotes;
  GtkWidget *child, *footer, *line;
  GdkRGBA ripple = fm_rgba (FM_C_RIPPLE);

  while ((child = gtk_widget_get_first_child (page->box)))
    gtk_box_remove (GTK_BOX (page->box), child);
  if (!remotes->len)
    page->editing = FALSE;
  for (guint i = 0; i < remotes->len; i++)
    {
      FmRemote *remote = remotes->pdata[i];
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *icon = fm_icon_new (fm_glyph_for_proto (remote->proto), 44);
      g_autofree char *uri = fm_remote_uri (remote);
      g_autoptr(GFile) file = g_file_new_for_uri (uri);
      g_autofree char *shown = fm_display_path (file);
      GtkWidget *row;
      RemoteRef *ref = g_new0 (RemoteRef, 1);

      gtk_widget_add_css_class (icon, "fm-row-icon");
      gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (text), fm_label (fm_remote_label (remote), "fm-row-name", 18, TRUE));
      gtk_box_append (GTK_BOX (text), fm_label (shown, "fm-row-sub", 14, FALSE));
      gtk_widget_add_css_class (text, "fm-row-text");
      gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (text, TRUE);
      gtk_box_append (GTK_BOX (box), icon);
      gtk_box_append (GTK_BOX (box), text);
      if (page->editing)
        {
          GtkWidget *more = gtk_image_new_from_icon_name ("view-more-symbolic");

          gtk_image_set_pixel_size (GTK_IMAGE (more), 16);
          gtk_widget_add_css_class (more, "fm-remote-more");
          gtk_box_append (GTK_BOX (box), more);
        }
      row = neo_pressable_new (box);
      gtk_widget_add_css_class (row, "fm-row");
      gtk_widget_add_css_class (row, "fm-remote-row");
      neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
      neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &ripple);
      ref->page = page;
      ref->id = g_strdup (remote->id);
      g_object_set_data_full (G_OBJECT (row), "fm-ref", ref, remote_ref_free);
      g_signal_connect (row, "clicked", G_CALLBACK (on_remote_clicked), page);
      g_signal_connect (row, "long-pressed", G_CALLBACK (on_remote_long_pressed), page);
      gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, fm_remote_label (remote),
                                      -1);
      gtk_box_append (GTK_BOX (page->box), row);
    }
  /* the footer over a line: "+ Add a remote location", centred */
  line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  {
    GtkWidget *plus = gtk_image_new_from_icon_name ("list-add-symbolic");
    GtkWidget *label = fm_label (_("Add a remote location"), "fm-footer-text", 18, FALSE);

    gtk_image_set_pixel_size (GTK_IMAGE (plus), 16);
    gtk_widget_add_css_class (plus, "fm-footer-plus");
    gtk_widget_set_valign (plus, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (line), plus);
    gtk_box_append (GTK_BOX (line), label);
  }
  gtk_widget_set_halign (line, GTK_ALIGN_CENTER);
  footer = neo_pressable_new (line);
  gtk_widget_add_css_class (footer, "fm-remote-footer");
  neo_pressable_set_ripple (NEO_PRESSABLE (footer), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (footer), &ripple);
  g_signal_connect (footer, "clicked", G_CALLBACK (on_footer), page);
  gtk_box_append (GTK_BOX (page->box), footer);
}

void
fm_remote_page_toggle_edit (FmRemotePage *page)
{
  page->editing = !page->editing && page->shell->remotes->len > 0;
  fm_remote_page_refresh (page);
  fm_shell_update_bars (page->shell);
}

gboolean
fm_remote_page_is_editing (FmRemotePage *page)
{
  return page->editing;
}

FmRemotePage *
fm_remote_page_new (FmShell *shell)
{
  FmRemotePage *page = g_new0 (FmRemotePage, 1);

  page->shell = shell;
  page->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (page->box, "fm-remote-list");
  page->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (page->scroll), page->box);
  gtk_widget_add_css_class (page->scroll, "fm-remote-page");
  return page;
}

GtkWidget *
fm_remote_page_get_widget (FmRemotePage *page)
{
  return page->scroll;
}

void
fm_remote_page_free (FmRemotePage *page)
{
  g_free (page);
}
