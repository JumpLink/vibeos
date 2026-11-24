/* vibeos-unifiedpush: what its parts share. See vup-main.c for the whole picture. */
#pragma once

#include <gio/gio.h>

#define VUP_BUS_NAME          "org.unifiedpush.Distributor.vibeos"
#define VUP_DISTRIBUTOR_PATH  "/org/unifiedpush/Distributor"
#define VUP_DISTRIBUTOR_IFACE "org.unifiedpush.Distributor2"
#define VUP_CONNECTOR_PATH    "/org/unifiedpush/Connector"
#define VUP_CONNECTOR_IFACE   "org.unifiedpush.Connector2"
#define VUP_MANAGER_PATH      "/vibeos/UnifiedPush"
#define VUP_MANAGER_IFACE     "vibeos.UnifiedPush1"
#define VUP_SCHEMA            "vibeos.UnifiedPush"

/* the spec's limits, in bytes */
#define VUP_TOKEN_MAX        100
#define VUP_DESCRIPTION_MAX  100
#define VUP_ENDPOINT_MAX     1000
#define VUP_MESSAGE_MAX      4096
#define VUP_VAPID_LENGTH     87        /* base64url of an uncompressed P-256 point (65 bytes), no padding */

/* ours: a runaway app cannot fill the table, nor make the stream's URL too long for the server */
#define VUP_MAX_REGISTRATIONS     256
#define VUP_MAX_PER_SERVICE       16

/* a topic is "up" and 12 base64url characters: 9 random bytes, 72 bits */
#define VUP_TOPIC_RANDOM_BYTES    9
#define VUP_TOPIC_LENGTH          14

/* one app's registration */
typedef struct {
  char *token;          /* the app's secret for it (never logged in full) */
  char *service;        /* the app's D-Bus name: where its messages go */
  char *topic;          /* the push server topic (a secret too: anyone who knows it can send) */
  char *description;    /* NULL, or what the app said it is for (shown in Settings) */
  char *vapid;          /* NULL, or the app server's VAPID public key */
  char *id;             /* a handle for Settings: the first 16 hex digits of SHA-256 (token) */
  gint64 created;       /* Unix seconds */
  gint64 last_message;  /* Unix seconds of the last message delivered, 0 for none */
  guint64 messages;     /* messages delivered */
  gboolean active;      /* the push server has seen a subscriber for the topic (it refuses messages before) */
  gboolean activatable; /* the app had a D-Bus service file when it registered: it can be started for a message */
  guint endpoints_owed; /* Registers made while it was not active: each gets its NewEndpoint once it is (not kept) */
} VupRegistration;

/* --- vup-util.c */
char       *vup_new_topic          (void);
char       *vup_registration_id    (const char *token);
const char *vup_redact             (const char *secret);
gboolean    vup_vapid_is_valid     (const char *vapid);
gboolean    vup_utf8_within        (const char *text, gsize max_bytes);
char       *vup_normalize_server   (const char *url);
GBytes     *vup_decode_message     (const char *text, const char *encoding);

/* --- vup-store.c: the registrations and what the stream has seen, in $XDG_STATE_HOME/vibeos-unifiedpush */
void             vup_store_load            (void);
void             vup_store_save            (void);
void             vup_store_save_soon       (void);
void             vup_store_flush           (void);
GList           *vup_store_list            (void);   /* VupRegistration*, by creation; free with g_list_free */
VupRegistration *vup_store_by_token        (const char *token);
VupRegistration *vup_store_by_topic        (const char *topic);
VupRegistration *vup_store_by_id           (const char *id);
guint            vup_store_count           (const char *service);
VupRegistration *vup_store_add             (const char *token, const char *service, const char *description,
                                            const char *vapid);
void             vup_store_remove          (VupRegistration *reg);
char            *vup_store_endpoint        (VupRegistration *reg);
char            *vup_store_topics          (void);   /* the active stream's topics, comma separated, or NULL */
const char      *vup_store_get_server      (void);
gboolean         vup_store_set_server      (const char *server);
const char      *vup_store_get_since       (void);
void             vup_store_set_since       (const char *id);
GStrv            vup_store_get_done        (void);
void             vup_store_set_done        (GPtrArray *ids);

/* --- vup-stream.c: the one stream from the push server */
typedef enum {
  VUP_STATE_IDLE,        /* nothing registered: no stream */
  VUP_STATE_OFFLINE,     /* no network */
  VUP_STATE_CONNECTING,
  VUP_STATE_CONNECTED,
  VUP_STATE_WAITING,     /* the stream failed: it tries again after a pause */
} VupState;

typedef void (*VupCollected) (GError *error, gpointer data);

void        vup_stream_init        (GSettings *settings);
void        vup_stream_start       (void);
void        vup_stream_shutdown    (void);
void        vup_stream_topics_changed (void);
void        vup_stream_reconnect   (const char *why);
void        vup_stream_collect     (VupCollected done, gpointer data);
VupState    vup_stream_get_state   (void);
const char *vup_stream_state_name  (VupState state);
const char *vup_stream_get_error   (void);
void        vup_stream_get_addresses (const char **remote, guint16 *remote_port, const char **local,
                                      guint16 *local_port);

/* --- vup-deliver.c: calls to the apps' Connector2, in order, per app */
void        vup_deliver_init       (GDBusConnection *bus);
void        vup_deliver_shutdown   (void);
void        vup_deliver_received   (const char *id, const char *topic, GBytes *message);
void        vup_deliver_new_endpoint (VupRegistration *reg, gboolean answering);
/* who hears that a registration went */
typedef enum {
  VUP_TELL_NOBODY,      /* its app is gone */
  VUP_TELL_CONFIRM,     /* the app's own Unregister, which Unregistered confirms */
  VUP_TELL_REMOVED,     /* removed in Settings: the app is told */
} VupTell;
void        vup_deliver_unregistered (const char *service, const char *token, VupTell tell);
void        vup_deliver_forget     (VupRegistration *reg);
VupTell     vup_deliver_unregistered_waiting (const char *service, const char *token);
guint       vup_deliver_pending    (const char *service);
gboolean    vup_deliver_is_idle    (void);
void        vup_deliver_when_idle  (GSourceFunc func, gpointer data);
gboolean    vup_deliver_seen       (const char *id);
void        vup_deliver_check_installed (void);
void        vup_deliver_check_activatable (VupRegistration *reg);
guint       vup_deliver_count      (void);

/* --- vup-main.c */
void        vup_notify_state       (void);
void        vup_notify_registrations (void);
void        vup_registration_removed (VupRegistration *reg, VupTell tell);
