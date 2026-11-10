/* Mounting a server's share (fm-mount.h). */
#include "fm-mount.h"
#include "fm-config.h"
#include "fm-dialogs.h"
#include "fm-ui.h"

#include <glib/gi18n.h>
#include <string.h>

#if HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

typedef struct {
  GtkWidget *parent;
  GFile *location;
  FmRemote *remote;
  char *password;
  gboolean used;                /* the server dialog's password was given once */
  FmMountDone done;
  gpointer data;
  NeoDialog *dialog;            /* the question showing */
  GMountOperation *op;
} Mount;

static void
mount_free (Mount *mount)
{
  g_clear_object (&mount->parent);
  g_clear_object (&mount->location);
  g_clear_pointer (&mount->remote, fm_remote_free);
  if (mount->password)
    {
      memset (mount->password, 0, strlen (mount->password));
      g_free (mount->password);
    }
  g_clear_object (&mount->op);
  g_free (mount);
}

static char *
server_name (GFile *location)
{
  g_autofree char *uri = g_file_get_uri (location);
  g_autoptr(GUri) parsed = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);

  return g_strdup (parsed && g_uri_get_host (parsed) ? g_uri_get_host (parsed) : uri);
}

char *
fm_mount_error_message (GFile *location, const FmRemote *remote)
{
  g_autofree char *host = remote ? g_strdup (remote->host) : server_name (location);

  if (remote && *remote->user && !remote->anonymous)
    return g_strdup_printf (_("Could not connect to %1$s with user %2$s."), host, remote->user);
  return g_strdup_printf (_("Could not connect to %s."), host);
}

/* ---- the password dialog (dialog_password_input) ----------------------------------------------------------------- */

typedef struct {
  Mount *mount;
  GtkWidget *user, *domain, *password, *anonymous, *remember;
} Ask;

static void
on_anonymous_toggled (GtkWidget *check, GParamSpec *pspec, Ask *ask)
{
  gboolean anonymous = fm_check_get_active (check);

  for (GtkWidget **field = &ask->user; field <= &ask->password; field++)
    if (*field)
      gtk_widget_set_sensitive (gtk_widget_get_parent (*field), !anonymous);
}

static void
on_ask_response (NeoDialog *dialog, int response, Ask *ask)
{
  GMountOperation *op = ask->mount->op;

  ask->mount->dialog = NULL;
  if (response != NEO_RESPONSE_OK)
    {
      g_mount_operation_reply (op, G_MOUNT_OPERATION_ABORTED);
      return;
    }
  if (ask->anonymous && fm_check_get_active (fm_check_row_get_check (ask->anonymous)))
    g_mount_operation_set_anonymous (op, TRUE);
  else
    {
      if (ask->user)
        g_mount_operation_set_username (op, gtk_editable_get_text (GTK_EDITABLE (ask->user)));
      if (ask->domain)
        g_mount_operation_set_domain (op, gtk_editable_get_text (GTK_EDITABLE (ask->domain)));
      if (ask->password)
        g_mount_operation_set_password (op, gtk_editable_get_text (GTK_EDITABLE (ask->password)));
      g_mount_operation_set_password_save (op, ask->remember
                                                   && fm_check_get_active (fm_check_row_get_check (ask->remember))
                                               ? G_PASSWORD_SAVE_PERMANENTLY
                                               : G_PASSWORD_SAVE_NEVER);
    }
  g_mount_operation_reply (op, G_MOUNT_OPERATION_HANDLED);
}

static void
on_ask_activate (GtkEntry *entry, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, NEO_RESPONSE_OK);
}

static void
on_ask_password (GMountOperation *op, const char *message, const char *default_user, const char *default_domain,
                 GAskPasswordFlags flags, Mount *mount)
{
  g_autofree char *title = NULL;
  GtkWidget *content;
  NeoDialog *dialog;
  Ask *ask;

  /* the server dialog's answer first, once: GVfs asks again only when it was wrong */
  if (!mount->used && mount->remote && (mount->password || mount->remote->anonymous))
    {
      mount->used = TRUE;
      if (mount->remote->anonymous && (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED))
        g_mount_operation_set_anonymous (op, TRUE);
      else
        {
          g_mount_operation_set_username (op, *mount->remote->user ? mount->remote->user : default_user);
          if (*mount->remote->domain)
            g_mount_operation_set_domain (op, mount->remote->domain);
          else if (default_domain)
            g_mount_operation_set_domain (op, default_domain);
          g_mount_operation_set_password (op, mount->password ? mount->password : "");
          /* the server dialog's password is always remembered (spec §12) */
          g_mount_operation_set_password_save (op, G_PASSWORD_SAVE_PERMANENTLY);
        }
      g_mount_operation_reply (op, G_MOUNT_OPERATION_HANDLED);
      return;
    }

  /* the dialog answers later: GMountOperation's own handler, after this one, would answer "unhandled" at once (in
   * an idle), and GVfs would give up on the password before it is typed */
  g_signal_stop_emission_by_name (op, "ask-password");
  title = server_name (mount->location);
  dialog = fm_dialog_new (title, &content);
  ask = g_new0 (Ask, 1);
  ask->mount = mount;
  if (message && *message)
    gtk_box_append (GTK_BOX (content), fm_label (message, "fm-dialog-message", 14, TRUE));
  if (flags & G_ASK_PASSWORD_NEED_USERNAME)
    {
      ask->user = fm_entry_new (mount->remote && *mount->remote->user ? mount->remote->user : default_user);
      gtk_box_append (GTK_BOX (content), fm_form_row (_("Username"), ask->user));
    }
  if (flags & G_ASK_PASSWORD_NEED_DOMAIN)
    {
      ask->domain = fm_entry_new (mount->remote && *mount->remote->domain ? mount->remote->domain : default_domain);
      gtk_box_append (GTK_BOX (content), fm_form_row (_("Domain"), ask->domain));
    }
  if (flags & G_ASK_PASSWORD_NEED_PASSWORD)
    {
      ask->password = fm_entry_new (NULL);
      gtk_entry_set_visibility (GTK_ENTRY (ask->password), FALSE);
      gtk_entry_set_input_purpose (GTK_ENTRY (ask->password), GTK_INPUT_PURPOSE_PASSWORD);
      gtk_box_append (GTK_BOX (content), fm_form_row (_("Password"), ask->password));
      g_signal_connect (ask->password, "activate", G_CALLBACK (on_ask_activate), dialog);
    }
  if (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED)
    {
      ask->anonymous = fm_check_row (_("Anonymous"), FALSE, FALSE);
      g_signal_connect (fm_check_row_get_check (ask->anonymous), "notify::active", G_CALLBACK (on_anonymous_toggled),
                        ask);
      gtk_box_append (GTK_BOX (content), ask->anonymous);
    }
  if (flags & G_ASK_PASSWORD_SAVING_SUPPORTED)
    {
      ask->remember = fm_check_row (_("Remember password"), FALSE, TRUE);
      gtk_box_append (GTK_BOX (content), ask->remember);
    }
  fm_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  fm_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "fm-ask", ask, g_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_ask_response), ask);
  mount->dialog = dialog;
  neo_dialog_present (dialog, mount->parent);
  /* the password field takes the keyboard when the user is already filled in */
  if (ask->password && ask->user && *gtk_editable_get_text (GTK_EDITABLE (ask->user)))
    gtk_widget_grab_focus (ask->password);
}

/* ---- a question (an unknown host key ...) ------------------------------------------------------------------------ */

static void
on_question_response (NeoDialog *dialog, int response, Mount *mount)
{
  mount->dialog = NULL;
  if (response < 0)
    {
      g_mount_operation_reply (mount->op, G_MOUNT_OPERATION_ABORTED);
      return;
    }
  g_mount_operation_set_choice (mount->op, response);
  g_mount_operation_reply (mount->op, G_MOUNT_OPERATION_HANDLED);
}

static void
on_ask_question (GMountOperation *op, const char *message, const char **choices, Mount *mount)
{
  g_auto(GStrv) lines = g_strsplit (message, "\n", 2);
  GtkWidget *content;
  NeoDialog *dialog;
  int n = choices ? (int) g_strv_length ((char **) choices) : 0;

  /* answered later, as the password is */
  g_signal_stop_emission_by_name (op, "ask-question");
  /* GVfs's first line is the title, the rest the body */
  dialog = fm_dialog_new (lines[0] && lines[1] ? lines[0] : NULL, &content);
  gtk_box_append (GTK_BOX (content), fm_label (lines[0] && lines[1] ? lines[1] : message, "fm-dialog-message", 14,
                                               TRUE));
  /* the first choice is the positive one: it goes at the end */
  for (int i = n - 1; i >= 0; i--)
    fm_dialog_add_button (dialog, choices[i], i);
  g_signal_connect (dialog, "response", G_CALLBACK (on_question_response), mount);
  mount->dialog = dialog;
  neo_dialog_present (dialog, mount->parent);
}

static void
on_show_processes (GMountOperation *op, const char *message, GArray *processes, const char **choices, Mount *mount)
{
  int n = choices ? (int) g_strv_length ((char **) choices) : 0;

  /* only an eject comes here, while files are open: the last choice is to leave it mounted */
  g_mount_operation_set_choice (op, MAX (n - 1, 0));
  g_mount_operation_reply (op, G_MOUNT_OPERATION_HANDLED);
}

static void
on_aborted (GMountOperation *op, Mount *mount)
{
  if (mount->dialog)
    neo_dialog_respond (mount->dialog, NEO_RESPONSE_CANCEL);
}

static void
on_mounted (GObject *source, GAsyncResult *result, gpointer data)
{
  Mount *mount = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = g_file_mount_enclosing_volume_finish (G_FILE (source), result, &error);

  if (!ok && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED))
    ok = TRUE;
  if (!ok && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
    {
      g_autofree char *uri = g_file_get_uri (mount->location);

      g_warning ("mounting %s: %s", uri, error->message);
    }
  if (mount->dialog)
    neo_dialog_respond (mount->dialog, NEO_RESPONSE_CANCEL);
  mount->done (ok, error, mount->data);
  mount_free (mount);
}

void
fm_mount (GtkWidget *parent, GFile *location, const FmRemote *remote, const char *password, FmMountDone done,
          gpointer data)
{
  Mount *mount = g_new0 (Mount, 1);

  mount->parent = g_object_ref (parent);
  mount->location = g_object_ref (location);
  mount->remote = remote ? fm_remote_copy (remote) : NULL;
  mount->password = password && *password ? g_strdup (password) : NULL;
  mount->done = done;
  mount->data = data;
  mount->op = g_mount_operation_new ();
  g_signal_connect (mount->op, "ask-password", G_CALLBACK (on_ask_password), mount);
  g_signal_connect (mount->op, "ask-question", G_CALLBACK (on_ask_question), mount);
  g_signal_connect (mount->op, "show-processes", G_CALLBACK (on_show_processes), mount);
  g_signal_connect (mount->op, "aborted", G_CALLBACK (on_aborted), mount);
  g_file_mount_enclosing_volume (location, G_MOUNT_MOUNT_NONE, mount->op, NULL, on_mounted, mount);
}

/* ---- forgetting a password --------------------------------------------------------------------------------------- */

#if HAVE_LIBSECRET
static const SecretSchema network_password = {
  "org.gnome.keyring.NetworkPassword", SECRET_SCHEMA_DONT_MATCH_NAME,
  {
    { "user", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "domain", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "server", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "object", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "protocol", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "authtype", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "port", SECRET_SCHEMA_ATTRIBUTE_INTEGER },
    { NULL, 0 },
  },
};

static void
on_cleared (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  if (!secret_password_clear_finish (result, &error) && error)
    g_warning ("forgetting the password: %s", error->message);
}
#endif

void
fm_mount_forget_password (const FmRemote *remote)
{
#if HAVE_LIBSECRET
  g_autoptr(GHashTable) attributes = NULL;

  /* an anonymous login has no password; a remote without a user, none GVfs could tell apart from another's */
  if (remote->anonymous || !*remote->user)
    return;
  attributes = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (attributes, "server", remote->host);
  g_hash_table_insert (attributes, "user", remote->user);
  g_hash_table_insert (attributes, "protocol", (gpointer) fm_proto_scheme (remote->proto));
  secret_password_clearv (&network_password, attributes, NULL, on_cleared, NULL);
#endif
}
