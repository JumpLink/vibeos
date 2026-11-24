/* Running an app in its sandbox, and finding the ones running.
 *
 * vibeos-sandbox stays outside as the sandbox's supervisor: it starts the session bus filter (xdg-dbus-proxy, in a
 * bubblewrap of its own), then the app's bubblewrap, waits for it, and takes everything away again. Each sandbox is
 * an instance the way Flatpak's are, in $XDG_RUNTIME_DIR/.flatpak/<instance>: "info" (the app's /.flatpak-info),
 * "pid" (the supervisor's) and "bwrapinfo.json" (the sandbox's first process), which is how the portals find the
 * apps running in the background, and how `vibeos-sandbox set` stops an app whose permission was taken away. */
#pragma once

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

/* runs `argv` as the app: the exit status of the app */
int        vs_run            (const char *app_id, gboolean forward_files, char **argv);

typedef struct {
  char  *instance_id;
  char  *app_id;
  pid_t  pid;          /* the supervisor */
  pid_t  child_pid;    /* the sandbox's first process (bubblewrap's, in the host's process ids) */
} VsInstance;

void       vs_instance_free  (VsInstance *instance);
/* the instances running, of `app_id` or (NULL) all of them; the ones left by a supervisor that is gone are taken
 * away */
GPtrArray *vs_instances      (const char *app_id);
/* stops every instance of `app_id`: how many there were */
int        vs_kill_app       (const char *app_id);

G_END_DECLS
