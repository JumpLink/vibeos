/* navi-inhibit: see navi-inhibit.h */
#include "navi-inhibit.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#define LOGIN1      "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MGR  "org.freedesktop.login1.Manager"

struct _NaviSleepBlock {
  gint refs;       /* the owner's, and one per call to logind under way */
  gboolean wanted; /* held, as far as the app is concerned */
  gboolean asking; /* a call to logind under way */
  gboolean freed;  /* the owner is done with it */
  int fd;          /* the inhibitor, or -1 */
};

static void
unref (NaviSleepBlock *self)
{
  if (--self->refs == 0)
    g_free (self);
}

static void
let_go (NaviSleepBlock *self)
{
  if (self->fd >= 0) {
    close (self->fd);
    self->fd = -1;
    g_debug ("sleep: the inhibitor is let go");
  }
}

static void
answered (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviSleepBlock *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GUnixFDList) fds = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (src), &fds, res,
                                                                             &error);
  self->asking = FALSE;
  int fd = -1;
  if (v && fds) {
    gint32 index = -1;
    g_variant_get (v, "(h)", &index);
    fd = g_unix_fd_list_get (fds, index, &error);
  }
  if (fd < 0) {
    g_message ("sleep: no inhibitor from logind: %s", error ? error->message : "no file descriptor");
  } else if (self->wanted && !self->freed && self->fd < 0) {
    self->fd = fd;
    g_debug ("sleep: the inhibitor is held");
  } else {
    close (fd); /* let go while logind answered */
  }
  unref (self);
}

static void
got_bus (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviSleepBlock *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) bus = g_bus_get_finish (res, &error);
  if (!bus || !self->wanted || self->freed) {
    if (!bus)
      g_message ("sleep: no system bus: %s", error->message);
    self->asking = FALSE;
    unref (self);
    return;
  }
  /* the reference goes on to the call */
  g_dbus_connection_call_with_unix_fd_list (bus, LOGIN1, LOGIN1_PATH, LOGIN1_MGR, "Inhibit",
                                            g_variant_new ("(ssss)", "sleep", "VibeOS Navi",
                                                           "Guiding to a destination", "block"),
                                            G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, answered,
                                            self);
}

static void
ask (NaviSleepBlock *self)
{
  if (self->asking || self->fd >= 0)
    return;
  self->asking = TRUE;
  self->refs++;
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, got_bus, self);
}

NaviSleepBlock *
navi_sleep_block_new (void)
{
  NaviSleepBlock *self = g_new0 (NaviSleepBlock, 1);
  self->refs = 1;
  self->fd = -1;
  return self;
}

void
navi_sleep_block_set (NaviSleepBlock *self, gboolean held)
{
  self->wanted = held;
  if (held)
    ask (self);
  else
    let_go (self);
}

gboolean
navi_sleep_block_is_held (NaviSleepBlock *self)
{
  return self->fd >= 0;
}

void
navi_sleep_block_free (NaviSleepBlock *self)
{
  if (!self)
    return;
  self->wanted = FALSE;
  self->freed = TRUE;
  let_go (self);
  unref (self);
}
