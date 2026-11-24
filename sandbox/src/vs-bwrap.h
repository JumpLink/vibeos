/* The sandbox's command lines, from what the app is granted and what the device has: bubblewrap's for the app,
 * xdg-dbus-proxy's filter for its session bus, bubblewrap's for the proxy, and the /.flatpak-info that tells the
 * portals, PipeWire, mutter and GLib which app it is. Nothing here touches the system: the caller (vs-run.c) looks
 * at the device and fills a VsSandbox, so the tests can describe any device.
 *
 * Inside, the app sees:
 *   /usr, /etc, /opt     the system's, read-only; /etc/machine-id is the app's own random id
 *   its home             the same path as the user's, but the app's own directory
 *                        ($XDG_DATA_HOME/vibeos-sandbox/data/<package>); with the files permission the shared folders
 *                        (Documents, Download, Music, Pictures, Videos…) and removable media are the real ones
 *   /tmp, /var/tmp       its own, empty at each start
 *   /run/user/<uid>      its own, with the compositor's socket, PipeWire's and PulseAudio's, the filtered session
 *                        bus and the documents the user gave it (the document portal)
 *   /dev                 null, zero, random…, a terminal, and the GPU; the cameras only with the camera permission.
 *                        Never the sound cards, the input devices or anything else
 *   /proc                its own processes only; /sys the parts GPU drivers read
 *   the network          only with the network permission
 * and no system bus at all: ModemManager, GeoClue, NetworkManager, BlueZ, logind are out of reach. */
#pragma once

#include "vs-perms.h"

G_BEGIN_DECLS

typedef struct {
  const char *path;      /* /bin */
  const char *target;    /* usr/bin when it is a symlink on the host, NULL when it is a directory */
} VsRootEntry;

typedef struct {
  const char *app_id;
  const char *package;        /* the Debian package it is from (NULL: none known) */
  const char *instance_id;
  const char *home;           /* the user's home: the app's home is at the same path */
  const char *app_home;       /* the host directory that is the app's home */
  const char *runtime_dir;    /* the user's runtime directory, the same path inside */
  const char *user_name;

  /* the host's sockets and directories; NULL when there is none */
  const char *wayland;        /* the compositor's socket */
  const char *pipewire;       /* pipewire-0 (never pipewire-0-manager) */
  const char *pulse;          /* pulse/native */
  const char *bus_proxy;      /* the filtered session bus: the proxy's socket */
  const char *documents;      /* the document portal's view of this app (…/doc/by-app/<app id>) */
  const char *resolv_conf;    /* where /etc/resolv.conf points when that is outside /etc (NetworkManager's in /run) */
  const char *fontconfig_cache;  /* /var/cache/fontconfig */
  const char *games;          /* /var/games */
  const char *const *root;    /* NULL-terminated pairs: path, symlink target or "" for a directory (/bin, /lib…) */
  const char *const *user_dirs;    /* the shared folders that exist (Documents, Download…) */
  const char *const *media_dirs;   /* /media/<user>, /run/media/<user> */
  const char *const *gpu_devices;  /* /dev/dri */
  const char *const *camera_devices;  /* /dev/video*, /dev/media*, /dev/v4l-subdev* */
  const char *const *home_files;   /* the user's files read through to the app, read-only: GTK's settings and CSS,
                                    * fonts */
  gboolean has_opt;
  gboolean disable_userns;    /* bubblewrap can stop the app making user namespaces of its own */

  const char *const *own_names;   /* bus names the app may own besides its id */
  VsGrants grants;
  const char *const *env;     /* the environment the launcher was started with */

  int flatpak_info_fd;        /* -1 when not used */
  int machine_id_fd;
  int info_fd;                /* bubblewrap writes the sandbox's pid here */
  int block_fd;               /* bubblewrap waits for a byte here before it starts the app */

  const char *const *command;
} VsSandbox;

/* bubblewrap's arguments for the app, the program first ("bwrap") */
GPtrArray *vs_bwrap_args       (const VsSandbox *sb);
/* the environment inside: what passes from the launcher's, and what the sandbox sets ("NAME=value" each) */
GPtrArray *vs_sandbox_env      (const VsSandbox *sb);
/* xdg-dbus-proxy's arguments after the bus address and the socket: --filter and its rules */
GPtrArray *vs_proxy_rules      (const VsSandbox *sb);
/* bubblewrap's arguments for the proxy (it runs with the app's /.flatpak-info: the portals identify the app by the
 * process at the other end of its bus connection, which is the proxy), then the proxy's own, before its rules */
GPtrArray *vs_proxy_bwrap_args (const char *proxy_dir, int flatpak_info_fd, const char *bus_address,
                                const char *socket, int sync_fd, const char *const *root);
/* the socket of a D-Bus address's first unix:path= transport (unix:path=/run/user/1000/bus,guid=… as dbus-daemon
 * gives the services it starts), or NULL */
char      *vs_bus_socket_path  (const char *address);
/* /.flatpak-info */
char      *vs_flatpak_info     (const VsSandbox *sb);

/* a valid app id for the portals: two or more elements of letters, digits and _ (- in the last one only) */
gboolean   vs_is_valid_app_id  (const char *id);
/* a valid D-Bus well-known name (what --own takes) */
gboolean   vs_is_valid_bus_name (const char *name);

G_END_DECLS
