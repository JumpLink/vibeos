/* The person using the phone (msg-account.h). */
#include "msg-account.h"

#define ACCOUNTS "org.freedesktop.Accounts"

struct _MsgAccount {
  GObject parent;
  char *name, *login;
  GBytes *picture;
  GDBusProxy *user;
};

G_DEFINE_FINAL_TYPE (MsgAccount, msg_account, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
read_user (MsgAccount *self)
{
  g_autoptr (GVariant) real_name = g_dbus_proxy_get_cached_property (self->user, "RealName");
  g_autoptr (GVariant) icon = g_dbus_proxy_get_cached_property (self->user, "IconFile");
  const char *name = real_name ? g_variant_get_string (real_name, NULL) : NULL;
  const char *path = icon ? g_variant_get_string (icon, NULL) : NULL;
  char *contents = NULL;
  gsize length;

  g_free (self->name);
  self->name = g_strdup (name && *name ? name : self->login);
  g_clear_pointer (&self->picture, g_bytes_unref);
  /* the file is there only when a picture was chosen */
  if (path && *path && g_file_get_contents (path, &contents, &length, NULL) && length > 0)
    self->picture = g_bytes_new_take (contents, length);
  else
    g_free (contents);
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
on_user_changed (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, MsgAccount *self)
{
  read_user (self);
}

static void
on_user (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (MsgAccount) self = data;

  self->user = g_dbus_proxy_new_for_bus_finish (result, NULL);
  if (!self->user)
    return;
  g_signal_connect (self->user, "g-properties-changed", G_CALLBACK (on_user_changed), self);
  read_user (self);
}

static void
on_found (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (MsgAccount) self = data;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  const char *path;

  if (!reply)
    return;
  g_variant_get (reply, "(&o)", &path);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, ACCOUNTS, path,
                            "org.freedesktop.Accounts.User", NULL, on_user, g_object_ref (self));
}

static void
on_bus (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (MsgAccount) self = data;
  g_autoptr (GDBusConnection) bus = g_bus_get_finish (result, NULL);

  if (!bus)
    return;
  g_dbus_connection_call (bus, ACCOUNTS, "/org/freedesktop/Accounts", ACCOUNTS, "FindUserByName",
                          g_variant_new ("(s)", self->login), G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, on_found, g_object_ref (self));
}

static void
msg_account_finalize (GObject *object)
{
  MsgAccount *self = MSG_ACCOUNT (object);

  g_clear_object (&self->user);
  g_clear_pointer (&self->picture, g_bytes_unref);
  g_free (self->name);
  g_free (self->login);
  G_OBJECT_CLASS (msg_account_parent_class)->finalize (object);
}

static void
msg_account_class_init (MsgAccountClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = msg_account_finalize;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
msg_account_init (MsgAccount *self)
{
  self->login = g_strdup (g_get_user_name ());
  self->name = g_strdup (g_get_real_name () && g_strcmp0 (g_get_real_name (), "Unknown") != 0 ? g_get_real_name ()
                                                                                              : self->login);
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, on_bus, g_object_ref (self));
}

MsgAccount *
msg_account_get_default (void)
{
  static MsgAccount *account;

  if (!account)
    account = g_object_new (MSG_TYPE_ACCOUNT, NULL);
  return account;
}

const char *
msg_account_get_name (MsgAccount *self)
{
  return self->name;
}

const char *
msg_account_get_login (MsgAccount *self)
{
  return self->login;
}

GBytes *
msg_account_get_picture (MsgAccount *self)
{
  return self->picture;
}
