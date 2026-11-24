/* Accounts: the online accounts of GNOME Online Accounts (goa-daemon), and Settings' own pages to add one and to look
 * after one.
 *
 * An account is added the way GOA 3.58's own provider dialogs add it, with Settings' fields and buttons in place of
 * those dialogs (libadwaita dialogs, which open as a bare window of their own over an app that is not a libadwaita
 * one): the same checks of the server first (ns-accounts-net.c), then goa-daemon's AddAccount with the same details
 * and credentials, then the account's EnsureCredentials. A check that fails shows the server's answer on the page and
 * adds nothing; an account whose EnsureCredentials fails is removed again. Checking before adding, as GOA's dialogs do,
 * keeps a wrong password away from goa-daemon, which would mark the account as needing attention and post a "Failed
 * to sign in" notification.
 *
 * Google and Microsoft 365 sign in in the browser with GOA's own OAuth2 code, which holds the client IDs: their pages
 * start GOA's flow with its dialog kept hidden (oauth_*). Kerberos signs in through goa-identity-service's D-Bus
 * interface (org.gnome.Identity), as GOA's Kerberos provider does. */
#include "ns-accounts-net.h"
#include "ns-caps.h"
#include "ns-pages.h"
#include "ns-ui.h"

#define GOA_API_IS_SUBJECT_TO_CHANGE
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE
#define GCR_API_SUBJECT_TO_CHANGE
#include <adwaita.h>
#include <gcr/gcr.h>
#include <goabackend/goabackend.h>
#include <libsoup/soup.h>
#include <math.h>
#ifdef HAVE_SPONGY
#include <spongy-gtk.h>
#endif
#include <string.h>

#define GOA_BUS "org.gnome.OnlineAccounts"
#define GOA_MANAGER_PATH "/org/gnome/OnlineAccounts/Manager"
#define GOA_ACCOUNT_IFACE "org.gnome.OnlineAccounts.Account"
#define IDENTITY_BUS "org.gnome.Identity"
#define IDENTITY_MANAGER_PATH "/org/gnome/Identity/Manager"
#define IDENTITY_MANAGER_IFACE "org.gnome.Identity.Manager"
#define FEDORA_REALM "FEDORAPROJECT.ORG"
/* a tap: a touch that ends this close to where it began (Contacts' CT_TOUCH_SLOP_DP) */
#define TOUCH_SLOP 8.0
/* how often Nextcloud is asked whether the user has signed in in the browser (GOA's LOGINFLOW2_POLL_INTERVAL) */
#define NEXTCLOUD_POLL_SECONDS 5
/* GoaDialogState: GOA's provider dialog's "state" property */
enum { GOA_DIALOG_BUSY = 2, GOA_DIALOG_DONE = 4 };

static GDBusConnection *
session_bus (void)
{
  return g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
}

/* the account list's page, while it is open: an account added closes the pages over it */
static NsPage *accounts_list;

/* GOA's client, made once when first needed: the providers' own code takes it, and it knows the accounts */
static GoaClient *goa;

typedef void (*WithGoa) (gpointer data);

typedef struct {
  WithGoa  then;
  gpointer data;
  GObject *alive;  /* the widget the call is for: nothing happens once it is gone */
} GoaWait;

static void
goa_ready (GObject *src, GAsyncResult *res, gpointer user)
{
  GoaWait *w = user;
  g_autoptr (GError) err = NULL;
  GoaClient *c = goa_client_new_finish (res, &err);
  if (c && !goa)
    goa = c;
  else
    g_clear_object (&c);
  if (goa && w->alive)
    w->then (w->data);
  else if (!goa)
    g_warning ("GOA: %s", err->message);
  if (w->alive)
    g_object_remove_weak_pointer (w->alive, (gpointer *) &w->alive);
  g_free (w);
}

static void
with_goa (GtkWidget *alive, WithGoa then, gpointer data)
{
  if (goa) {
    then (data);
    return;
  }
  GoaWait *w = g_new0 (GoaWait, 1);
  w->then = then;
  w->data = data;
  w->alive = G_OBJECT (alive);
  g_object_add_weak_pointer (w->alive, (gpointer *) &w->alive);
  goa_client_new (NULL, goa_ready, w);
}

/* the text of an error to show: without D-Bus's name for it, and without the "Invalid password with username “…”
 * (goa-error-quark, 4): " goa-daemon puts before its provider's own words */
static char *
error_text (const GError *error)
{
  g_autoptr (GError) e = g_error_copy (error);
  if (g_dbus_error_is_remote_error (e))
    g_dbus_error_strip_remote_error (e);
  const char *m = e->message;
  const char *quark = strstr (m, "-quark, ");
  const char *end = quark ? strstr (quark, "): ") : NULL;
  if (end && end[3])
    m = end + 3;
  return g_strdup (m);
}

/* the account's group of goa-daemon's key file, which holds its details: what an account page needs that the D-Bus
 * interfaces leave out (an interface is there only while its feature is on). GOA's providers read it the same way. */
static GHashTable *
account_details (const char *id)
{
  GHashTable *h = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *path = g_build_filename (g_get_user_config_dir (), "goa-1.0", "accounts.conf", NULL);
  g_autofree char *group = g_strdup_printf ("Account %s", id);
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return h;
  g_auto (GStrv) keys = g_key_file_get_keys (kf, group, NULL, NULL);
  for (guint i = 0; keys && keys[i]; i++) {
    char *v = g_key_file_get_string (kf, group, keys[i], NULL);
    if (v)
      g_hash_table_insert (h, g_strdup (keys[i]), v);
  }
  return h;
}

static const char *
detail (GHashTable *h, const char *key)
{
  const char *v = h ? g_hash_table_lookup (h, key) : NULL;
  return v ? v : "";
}

static gboolean
detail_true (GHashTable *h, const char *key)
{
  return g_strcmp0 (detail (h, key), "true") == 0;
}

/* a provider is GOA's OAuth2 one or a subclass of it (Google, Microsoft 365) */
static gboolean
is_oauth2 (GoaProvider *p)
{
  GType t = g_type_from_name ("GoaOAuth2Provider");
  return t && g_type_is_a (G_OBJECT_TYPE (p), t);
}

/* ---- a field: the Contacts editor's touch handling (apps/contacts/src/ct-editor.c) ----
 *
 * A finger on a field is the page's scroll until it lifts near where it landed: GtkText claims a touch as it lands
 * (its click gesture's "pressed") and as it moves (its drag's "drag-update"), and a claim cancels the page's drag, so
 * a page of fields could not be scrolled by them. GtkEntry around it claims every press on itself too (its own click
 * gesture's "pressed"). Those handlers are blocked while a finger is down (a mouse keeps them); a touch that ends near where it began is a tap, handled here: an unfocused field takes the focus with
 * the cursor at the end, the focused one its cursor at the tapped spot. GtkText's own "released" still runs after
 * that and asks the shell for the keyboard; mutter takes that request in with the next commit, which a tap handled
 * here does not cause, so one input hint the compositor has no use for is flipped and back (field_released). */

static void
field_released (GtkGestureClick *gesture, int n_press, double x, double y, GtkWidget *text)
{
  GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (gesture));
  if (!event || gdk_device_get_source (gdk_event_get_device (event)) != GDK_SOURCE_TOUCHSCREEN ||
      !gtk_widget_has_focus (text))
    return;
  GtkInputHints hints = gtk_text_get_input_hints (GTK_TEXT (text));
  gtk_text_set_input_hints (GTK_TEXT (text), hints ^ GTK_INPUT_HINT_VERTICAL_WRITING);
  gtk_text_set_input_hints (GTK_TEXT (text), hints);
}

/* the gestures of `text` and of `entry` around it, found once (a list of a widget's controllers costs while it is held) */
static void
field_find_gestures (GtkWidget *text, GtkWidget *entry)
{
  GListModel *controllers = gtk_widget_observe_controllers (text);
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++) {
    g_autoptr (GObject) c = g_list_model_get_item (controllers, i);
    if (GTK_IS_GESTURE_CLICK (c)) {
      g_object_set_data (G_OBJECT (text), "ns-click", c);
      g_signal_connect_after (c, "released", G_CALLBACK (field_released), text);
    } else if (GTK_IS_GESTURE_DRAG (c)) {
      g_object_set_data (G_OBJECT (text), "ns-drag", c);
    }
  }
  g_object_unref (controllers);
  controllers = gtk_widget_observe_controllers (entry);
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++) {
    g_autoptr (GObject) c = g_list_model_get_item (controllers, i);
    if (GTK_IS_GESTURE_CLICK (c))
      g_object_set_data (G_OBJECT (text), "ns-entry-click", c);
  }
  g_object_unref (controllers);
}

static void
field_hold (GtkWidget *text, gboolean hold)
{
  /* the gesture (a key on `text`), its signal, and whether it is GtkEntry's */
  static const struct {
    const char *key, *signal;
    gboolean    entry;
  } claims[] = { { "ns-click", "pressed", FALSE }, { "ns-drag", "drag-update", FALSE },
                 { "ns-entry-click", "pressed", TRUE } };
  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (text), "ns-held")) == hold)
    return;
  g_object_set_data (G_OBJECT (text), "ns-held", GINT_TO_POINTER (hold));
  for (guint i = 0; i < G_N_ELEMENTS (claims); i++) {
    GObject *g = g_object_get_data (G_OBJECT (text), claims[i].key);
    guint id = g ? g_signal_lookup (claims[i].signal, G_OBJECT_TYPE (g)) : 0;
    /* GtkText's handlers are the ones whose data is `text`; every handler on GtkEntry's own gesture is GtkEntry's */
    GSignalMatchType match = claims[i].entry ? G_SIGNAL_MATCH_ID : G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DATA;
    if (!id)
      continue;
    if (hold)
      g_signal_handlers_block_matched (g, match, id, 0, NULL, NULL, text);
    else
      g_signal_handlers_unblock_matched (g, match, id, 0, NULL, NULL, text);
  }
}

/* the focused field's cursor to where `event` is: GtkText lays its text out from its left edge, on one line */
static void
field_place_cursor (GtkWidget *text, GdkEvent *event)
{
  GtkNative *native = gtk_widget_get_native (text);
  double x, y, nx, ny;
  graphene_point_t p;
  if (!native)
    return;
  gdk_event_get_position (event, &x, &y);
  gtk_native_get_surface_transform (native, &nx, &ny);
  if (!gtk_widget_compute_point (GTK_WIDGET (native), text, &GRAPHENE_POINT_INIT ((float) (x - nx), (float) (y - ny)),
                                 &p))
    return;
  const char *s = gtk_editable_get_text (GTK_EDITABLE (text));
  g_autoptr (PangoLayout) layout = gtk_widget_create_pango_layout (text, s);
  int index = 0, trailing = 0;
  pango_layout_xy_to_index (layout, (int) (p.x * PANGO_SCALE), 0, &index, &trailing);
  gtk_editable_set_position (GTK_EDITABLE (text), (int) g_utf8_pointer_to_offset (s, s + index) + trailing);
}

static gboolean
field_touch (GtkEventControllerLegacy *controller, GdkEvent *event, GtkWidget *text)
{
  static double start_x, start_y;
  GdkEventType type = gdk_event_get_event_type (event);
  double x, y;
  if (type != GDK_TOUCH_BEGIN && type != GDK_TOUCH_UPDATE && type != GDK_TOUCH_END && type != GDK_TOUCH_CANCEL)
    return GDK_EVENT_PROPAGATE;
  gdk_event_get_position (event, &x, &y);
  if (type == GDK_TOUCH_BEGIN) {
    start_x = x;
    start_y = y;
    field_hold (text, TRUE);
  } else if (type == GDK_TOUCH_END && hypot (x - start_x, y - start_y) < TOUCH_SLOP) {
    if (gtk_widget_has_focus (text)) {
      field_place_cursor (text, event);
    } else {
      gtk_widget_grab_focus (text);
      gtk_editable_set_position (GTK_EDITABLE (text), -1);
    }
  }
  if (type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL)
    field_hold (text, FALSE);
  return GDK_EVENT_PROPAGATE;
}

/* ---- a sign-in page: fields, a line saying what is happening, Cancel and Sign in ---- */

enum {
  F_SERVER, F_USER, F_PASSWORD, F_FILES, F_CALDAV, F_CARDDAV,
  F_NAME, F_EMAIL, F_IMAP_HOST, F_IMAP_USER, F_IMAP_PASSWORD, F_SMTP_HOST, F_SMTP_USER, F_SMTP_PASSWORD,
  F_DOMAIN, F_PRINCIPAL, F_CLIENT_ID, F_TENANT,
  N_FIELDS
};

typedef struct _Form Form;
typedef void (*FormStep) (Form *f);
typedef gboolean (*FormReady) (Form *f);

struct _Form {
  int           refs;
  NsPage       *page;     /* NULL once the page is closed */
  GtkWidget    *content;
  GtkWidget    *field[N_FIELDS];
  GPtrArray    *order;    /* the fields in the order shown, for Enter */
  GPtrArray    *inputs;   /* everything the user can change: insensitive while a check runs */
  GtkWidget    *status, *spinner, *message, *go, *cancel;
  char         *go_label;
  GoaProvider  *provider;
  char         *type, *name;
  FormStep      submit;
  FormReady     ready;
  GCancellable *cancellable;
  gboolean      busy, quiet;
  int           focus_slot;  /* the field typed in when a check started: typed in again when it ends */
  /* a certificate refused by the last check ([0] the server's or IMAP's, [1] SMTP's), and taken from then on once
   * the user chose to ignore it, as GOA's dialogs do */
  gboolean      had_ssl[2], accept_ssl[2];
  /* an account added and not yet confirmed: removed when its check fails or the page closes first */
  char         *adding;
  /* signing in again: the account's path, id, identities and key file details */
  char         *path, *id, *identity, *presentation;
  GHashTable   *details;
  /* IMAP and SMTP */
  guint         imap_tls, smtp_tls;  /* positions in tls_labels */
  GtkWidget    *imap_pick, *smtp_pick, *detect;
  guint         detect_id;
  GCancellable *detecting;
  NsSmtpAuth    smtp_auth;
  /* Nextcloud */
  char         *poll, *token;
  guint         poll_id;
  /* Kerberos */
  GtkWidget    *remember;
  gboolean      fedora;
  char         *principal, *password;
  /* OAuth2: GOA's dialog, hidden; Microsoft 365's organization switch and its fields */
  GtkWidget    *dialog;
  GtkWidget    *org, *org_group;
};

static Form *
form_ref (Form *f)
{
  f->refs++;
  return f;
}

static void
form_unref (Form *f)
{
  if (--f->refs > 0)
    return;
  g_ptr_array_unref (f->order);
  g_ptr_array_unref (f->inputs);
  g_free (f->go_label);
  g_clear_object (&f->provider);
  g_free (f->type);
  g_free (f->name);
  g_clear_object (&f->cancellable);
  g_free (f->adding);
  g_free (f->path);
  g_free (f->id);
  g_free (f->identity);
  g_free (f->presentation);
  g_clear_pointer (&f->details, g_hash_table_unref);
  g_clear_object (&f->detecting);
  g_free (f->poll);
  g_free (f->token);
  g_free (f->principal);
  if (f->password)
    memset (f->password, 0, strlen (f->password));
  g_free (f->password);
  g_free (f);
}

/* a callback's form whose page is still open; the reference the call held is dropped either way */
static gboolean
form_alive (Form *f)
{
  if (f->page)
    return TRUE;
  form_unref (f);
  return FALSE;
}

static const char *
form_text (Form *f, int slot)
{
  return f->field[slot] ? gtk_editable_get_text (GTK_EDITABLE (f->field[slot])) : "";
}

static void
form_set_text (Form *f, int slot, const char *text)
{
  if (!f->field[slot])
    return;
  f->quiet = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (f->field[slot]), text ? text : "");
  f->quiet = FALSE;
}

static void remove_account (const char *path);

static void
form_update_go (Form *f)
{
  gtk_widget_set_sensitive (f->go, !f->busy && (!f->ready || f->ready (f)));
}

/* the page's scroller moved as little as it takes for the top of `from` and the bottom of `to` to be on screen (the
 * top first, when both do not fit), with room for their CSS margins (which their bounds leave out) */
static void
scroll_into_view (GtkWidget *from, GtkWidget *to)
{
  GtkAdjustment *adj = NULL;
  GtkWidget *child = NULL;
#ifdef HAVE_SPONGY
  GtkWidget *sc = gtk_widget_get_ancestor (from, SPONGY_TYPE_SCROLL_VIEW);
  if (sc) {
    adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (sc));
    child = spongy_scroll_view_get_child (SPONGY_SCROLL_VIEW (sc));
  }
#else
  GtkWidget *sc = gtk_widget_get_ancestor (from, GTK_TYPE_SCROLLED_WINDOW);
  if (sc) {
    adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sc));
    child = gtk_scrolled_window_get_child (GTK_SCROLLED_WINDOW (sc));
    if (GTK_IS_VIEWPORT (child))
      child = gtk_viewport_get_child (GTK_VIEWPORT (child));
  }
#endif
  graphene_rect_t top, end;
  if (!adj || !child || !gtk_widget_get_mapped (from) || !gtk_widget_compute_bounds (from, child, &top) ||
      !gtk_widget_compute_bounds (to, child, &end))
    return;
  const double margin = 24;
  double value = gtk_adjustment_get_value (adj), page = gtk_adjustment_get_page_size (adj);
  double bottom = end.origin.y + end.size.height + margin, head = top.origin.y - margin;
  if (bottom > value + page)
    value = bottom - page;
  if (head < value)
    value = head;
  gtk_adjustment_set_value (adj, value);
}

/* the status line and the buttons under it on screen (a check started with Enter from a field at the top shows its
 * progress and its answer there), once the line has its new size */
static gboolean
form_reveal_now (gpointer data)
{
  GtkWidget *status = data;
  GtkWidget *bar = gtk_widget_get_next_sibling (status);
  if (bar)
    scroll_into_view (gtk_widget_get_visible (status) ? status : bar, bar);
  return G_SOURCE_REMOVE;
}

static void
form_reveal (Form *f)
{
  g_timeout_add_full (G_PRIORITY_DEFAULT, 60, form_reveal_now, g_object_ref (f->status), g_object_unref);
}

/* the field that has the keyboard focus, as its slot; -1 for none */
static int
form_focused (Form *f)
{
  GtkRoot *root = gtk_widget_get_root (f->status);
  GtkWidget *focus = root ? gtk_root_get_focus (root) : NULL;
  for (int i = 0; focus && i < N_FIELDS; i++)
    if (f->field[i] && (focus == f->field[i] || gtk_widget_is_ancestor (focus, f->field[i])))
      return i;
  return -1;
}

/* nothing happening: the line hidden, the fields open */
static void
form_idle (Form *f)
{
  f->busy = FALSE;
  gtk_widget_set_visible (f->status, FALSE);
  for (guint i = 0; i < f->inputs->len; i++)
    gtk_widget_set_sensitive (g_ptr_array_index (f->inputs, i), TRUE);
  form_update_go (f);
}

/* a check running: the line says what it is, the fields wait */
static void
form_busy (Form *f, const char *what)
{
  if (!f->busy)
    f->focus_slot = form_focused (f);
  f->busy = TRUE;
  gtk_widget_remove_css_class (f->status, "ns-form-error");
  gtk_widget_set_visible (f->spinner, TRUE);
  gtk_label_set_text (GTK_LABEL (f->message), what);
  gtk_widget_set_visible (f->status, TRUE);
  for (guint i = 0; i < f->inputs->len; i++)
    gtk_widget_set_sensitive (g_ptr_array_index (f->inputs, i), FALSE);
  form_update_go (f);
  form_reveal (f);
}

/* the server's answer, on the page; a certificate error makes the button "Ignore", as in GOA's dialog */
static void
form_error (Form *f, const GError *error, int stage)
{
  g_autofree char *text = error_text (error);
  form_idle (f);
  gtk_widget_add_css_class (f->status, "ns-form-error");
  gtk_widget_set_visible (f->spinner, FALSE);
  gtk_label_set_text (GTK_LABEL (f->message), text);
  gtk_widget_set_visible (f->status, TRUE);
  gboolean ssl = g_error_matches (error, GOA_ERROR, GOA_ERROR_SSL);
  if (stage >= 0 && stage < 2)
    f->had_ssl[stage] = ssl;
  gtk_button_set_label (GTK_BUTTON (f->go), ssl ? _("Ignore") : f->go_label);
  /* the field lost the focus while it was insensitive: typing goes on where it was */
  if (f->focus_slot >= 0 && f->field[f->focus_slot])
    gtk_widget_grab_focus (f->field[f->focus_slot]);
  f->focus_slot = -1;
  form_reveal (f);
}

static void
form_error_text (Form *f, const char *text)
{
  g_autoptr (GError) e = g_error_new_literal (GOA_ERROR, GOA_ERROR_FAILED, text);
  form_error (f, e, -1);
}

/* the account is there: back to the list of accounts, or (signing in again) to the account's page */
static void
form_done (Form *f)
{
  g_clear_pointer (&f->adding, g_free);
  form_idle (f);
  if (!f->path && accounts_list)
    ns_pop_to (accounts_list);
  else
    ns_pop (f->page);
}

static void
form_changed (GtkEditable *e, Form *f)
{
  if (f->quiet || f->busy)
    return;
  /* a changed field is a new try: a certificate refused before is refused again until ignored again */
  memset (f->had_ssl, 0, sizeof f->had_ssl);
  memset (f->accept_ssl, 0, sizeof f->accept_ssl);
  gtk_button_set_label (GTK_BUTTON (f->go), f->go_label);
  gtk_widget_set_visible (f->status, FALSE);
  form_update_go (f);
}

static void
form_go (GtkWidget *button, Form *f)
{
  if (f->busy || (f->ready && !f->ready (f)))
    return;
  for (guint i = 0; i < 2; i++) {
    if (f->had_ssl[i])
      f->accept_ssl[i] = TRUE;
    f->had_ssl[i] = FALSE;
  }
  gtk_button_set_label (GTK_BUTTON (f->go), f->go_label);
  f->submit (f);
}

/* Enter: the next field, or Sign in from the last */
static void
form_next (GtkWidget *entry, Form *f)
{
  guint i;
  if (!g_ptr_array_find (f->order, entry, &i))
    return;
  for (guint j = i + 1; j < f->order->len; j++) {
    GtkWidget *next = g_ptr_array_index (f->order, j);
    if (gtk_widget_is_visible (next) && gtk_widget_is_sensitive (next)) {
      gtk_widget_grab_focus (next);
      /* the field's row on screen, with its title: the scroller does not follow the focus by itself */
      GtkWidget *row = gtk_widget_get_parent (next);
      scroll_into_view (row ? row : next, row ? row : next);
      return;
    }
  }
  form_go (f->go, f);
}

static void
form_cancel (GtkWidget *button, Form *f)
{
  ns_pop (f->page);
}

/* GOA's hidden dialog (an OAuth2 sign-in waiting for the browser) closed as its own Close button would: GOA stops
 * its flow, lets go of its D-Bus name and ends its call with "dismissed". adw_dialog_close, as a dialog in a window
 * of its own says "closed" only from there (force_close closes the window and nothing ends GOA's flow) */
static gboolean
unref_later (gpointer object)
{
  g_object_unref (object);
  return G_SOURCE_REMOVE;
}

static void
oauth_dismiss_dialog (Form *f)
{
  if (!f->dialog)
    return;
  GtkWidget *d = f->dialog;
  g_object_remove_weak_pointer (G_OBJECT (d), (gpointer *) &f->dialog);
  f->dialog = NULL;
  /* GOA's browser step ends from an idle after this and still asks the dialog for its state then (without a
   * reference: "goa_provider_dialog_get_state: assertion failed" once the closed window had taken the dialog with
   * it): the dialog is kept until after that idle */
  g_object_ref (d);
  adw_dialog_close (ADW_DIALOG (d));
  g_idle_add_full (G_PRIORITY_LOW, unref_later, d, NULL);
}

/* the page closed: what runs stops, and an account added and not yet confirmed goes. GOA's dialog is closed before
 * the cancellable is cancelled: cancelled first, GOA closes its dialog from inside the cancellation and the app hung
 * there (GLib waits for that same cancellation to end when the dialog lets go of the cancellable) */
static void
form_closed (GtkWidget *page, Form *f)
{
  f->page = NULL;
  oauth_dismiss_dialog (f);
  g_cancellable_cancel (f->cancellable);
  if (f->detecting)
    g_cancellable_cancel (f->detecting);
  g_clear_handle_id (&f->detect_id, g_source_remove);
  g_clear_handle_id (&f->poll_id, g_source_remove);
  if (f->adding) {
    remove_account (f->adding);
    g_clear_pointer (&f->adding, g_free);
  }
}

static Form *
form_new (GoaProvider *p, const char *title, const char *intro, const char *go_label)
{
  Form *f = g_new0 (Form, 1);
  f->refs = 1;
  f->focus_slot = -1;
  f->provider = g_object_ref (p);
  f->type = g_strdup (goa_provider_get_provider_type (p));
  f->name = goa_provider_get_provider_name (p, NULL);
  f->order = g_ptr_array_new ();
  f->inputs = g_ptr_array_new ();
  f->cancellable = g_cancellable_new ();
  f->go_label = g_strdup (go_label);
  f->page = ns_page_new (title ? title : f->name, &f->content);
  /* the form lives as long as the page's widgets (they hold it in their handlers); it is freed with the page */
  g_object_set_data_full (G_OBJECT (f->page), "ns-form", f, (GDestroyNotify) form_unref);
  g_signal_connect (f->page, "destroy", G_CALLBACK (form_closed), f);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (head, "ns-account-head");
  g_autoptr (GIcon) icon = goa_provider_get_provider_icon (p, NULL);
  GtkWidget *img = icon ? gtk_image_new_from_gicon (icon) : gtk_image_new_from_icon_name ("goa-panel");
  gtk_image_set_pixel_size (GTK_IMAGE (img), 48);
  gtk_box_append (GTK_BOX (head), img);
  if (intro) {
    GtkWidget *l = gtk_label_new (intro);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (l, "ns-sub");
    gtk_box_append (GTK_BOX (head), l);
  }
  gtk_box_append (GTK_BOX (f->content), head);
  return f;
}

/* a field in `group` under its title */
static GtkWidget *
form_field (Form *f, GtkWidget *group, int slot, const char *title, const char *text, gboolean secret,
            GtkInputPurpose purpose)
{
  GtkWidget *e = secret ? gtk_password_entry_new () : gtk_entry_new ();
  gtk_widget_add_css_class (e, "vibeos-entry");
  gtk_widget_set_hexpand (e, TRUE);
  if (secret) {
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (e), TRUE);
  } else {
    gtk_entry_set_input_purpose (GTK_ENTRY (e), purpose);
    gtk_entry_set_input_hints (GTK_ENTRY (e), GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI);
  }
  gtk_editable_set_text (GTK_EDITABLE (e), text ? text : "");
  gtk_accessible_update_property (GTK_ACCESSIBLE (e), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  ns_row_widget (group, title, e);

  GtkWidget *t = GTK_WIDGET (gtk_editable_get_delegate (GTK_EDITABLE (e)));
  if (GTK_IS_TEXT (t)) {
    GtkEventController *touch = gtk_event_controller_legacy_new ();
    gtk_event_controller_set_propagation_phase (touch, GTK_PHASE_CAPTURE);
    g_signal_connect (touch, "event", G_CALLBACK (field_touch), t);
    gtk_widget_add_controller (e, touch);
    field_find_gestures (t, e);
  }
  g_signal_connect (e, "changed", G_CALLBACK (form_changed), f);
  g_signal_connect (e, "activate", G_CALLBACK (form_next), f);
  g_ptr_array_add (f->order, e);
  g_ptr_array_add (f->inputs, e);
  f->field[slot] = e;
  return e;
}

/* the line and the buttons, under the fields */
static void
form_finish (Form *f, FormStep submit, FormReady ready)
{
  f->submit = submit;
  f->ready = ready;
  f->status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class (f->status, "ns-form-status");
  f->spinner = ns_spinner_new ();
  gtk_widget_set_valign (f->spinner, GTK_ALIGN_CENTER);
  f->message = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (f->message), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (f->message), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (f->message), 0);
  gtk_label_set_selectable (GTK_LABEL (f->message), FALSE);
  gtk_widget_set_hexpand (f->message, TRUE);
  gtk_box_append (GTK_BOX (f->status), f->spinner);
  gtk_box_append (GTK_BOX (f->status), f->message);
  gtk_widget_set_visible (f->status, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (f->status), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Status"), -1);
  gtk_box_append (GTK_BOX (f->content), f->status);

  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (bar, "ns-form-buttons");
  gtk_widget_set_halign (bar, GTK_ALIGN_END);
  f->cancel = ns_button_new (_("Cancel"), FALSE);
  f->go = ns_button_new (f->go_label, TRUE);
  g_signal_connect (f->cancel, "clicked", G_CALLBACK (form_cancel), f);
  g_signal_connect (f->go, "clicked", G_CALLBACK (form_go), f);
  /* a tap on Sign in leaves the focus in the field it was in, which takes it back after an error (form_error) */
  gtk_widget_set_focus_on_click (f->go, FALSE);
  gtk_box_append (GTK_BOX (bar), f->cancel);
  gtk_box_append (GTK_BOX (bar), f->go);
  gtk_box_append (GTK_BOX (f->content), bar);
  form_update_go (f);
  ns_push (f->page);
  if (f->order->len)
    gtk_widget_grab_focus (g_ptr_array_index (f->order, 0));
}

/* ---- goa-daemon: adding, confirming, removing ---- */

static void
remove_done (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!v)
    g_warning ("GOA: removing an account: %s", err->message);
}

static void
remove_account (const char *path)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, GOA_BUS, path, GOA_ACCOUNT_IFACE, "Remove", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, remove_done, NULL);
}

/* goa_utils_check_duplicate: an account of `type` with both identities exists already */
static gboolean
form_duplicate (Form *f, const char *type, const char *identity, const char *presentation, GError **error)
{
  if (!goa)
    return FALSE;
  g_autolist (GoaObject) all = goa_client_get_accounts (goa);
  for (GList *l = all; l; l = l->next) {
    GoaAccount *a = goa_object_peek_account (l->data);
    if (!a || g_strcmp0 (goa_account_get_provider_type (a), type) != 0 ||
        g_strcmp0 (goa_account_get_identity (a), identity) != 0 ||
        g_strcmp0 (goa_account_get_presentation_identity (a), presentation) != 0)
      continue;
    g_set_error (error, GOA_ERROR, GOA_ERROR_ACCOUNT_EXISTS, "A %s account already exists for %s",
                 goa_account_get_provider_name (a), presentation);
    return TRUE;
  }
  return FALSE;
}

static gboolean
form_check_duplicate (Form *f, const char *type, const char *identity, const char *presentation)
{
  g_autoptr (GError) e = NULL;
  if (f->path || !form_duplicate (f, type, identity, presentation, &e))
    return FALSE;
  form_error (f, e, -1);
  return TRUE;
}

static void
ensured (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!form_alive (f))
    return;
  if (!v) {
    /* an account that cannot sign in is not kept; one signed in again stays, waiting for another try */
    if (f->adding) {
      remove_account (f->adding);
      g_clear_pointer (&f->adding, g_free);
    }
    /* goa-daemon refusing a certificate that the check here took: once a certificate was ignored, later TLS
     * connections of this process to that server pass without asking again (resumed TLS sessions), goa-daemon's
     * do not. "Ignore" then applies to the account's server, or for mail to both, as goa-daemon does not say
     * which */
    form_error (f, err, 0);
    if (g_str_equal (f->type, "imap_smtp") && f->had_ssl[0])
      f->had_ssl[1] = TRUE;
  } else {
    form_done (f);
  }
  form_unref (f);
}

static void
form_ensure (Form *f, const char *path)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  form_busy (f, _("Signing in…"));
  g_dbus_connection_call (bus, GOA_BUS, path, GOA_ACCOUNT_IFACE, "EnsureCredentials", NULL, G_VARIANT_TYPE ("(i)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, f->cancellable, ensured, form_ref (f));
}

typedef void (*FormAdded) (Form *f, const char *path);

typedef struct {
  Form     *f;
  FormAdded then;
} Adding;

static void
added (GObject *src, GAsyncResult *res, gpointer data)
{
  Adding *a = data;
  Form *f = a->f;
  FormAdded then = a->then;
  g_free (a);
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  const char *path = NULL;
  if (v)
    g_variant_get (v, "(&o)", &path);
  /* the page closed while goa-daemon added it: a new account goes again */
  if (!f->page) {
    if (path && !f->path)
      remove_account (path);
    form_unref (f);
    return;
  }
  if (!v) {
    form_error (f, err, -1);
  } else {
    if (!f->path)
      g_set_str (&f->adding, path);
    if (then)
      then (f, path);
    else
      form_ensure (f, path);
  }
  form_unref (f);
}

/* goa-daemon's AddAccount: a new account, or (with "Id" in the details) new credentials and details for one that is
 * there. Then `then`, or EnsureCredentials when `then` is NULL. No cancellable: an account goa-daemon adds after the
 * page has closed would be lost, so the answer is always waited for (added). */
static void
form_add (Form *f, const char *identity, const char *presentation, GVariant *credentials, GVariant *details,
          FormAdded then)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  Adding *a = g_new0 (Adding, 1);
  a->f = form_ref (f);
  a->then = then;
  form_busy (f, f->path ? _("Saving…") : _("Adding the account…"));
  g_dbus_connection_call (bus, GOA_BUS, GOA_MANAGER_PATH, "org.gnome.OnlineAccounts.Manager", "AddAccount",
                          g_variant_new ("(sss@a{sv}@a{ss})", f->type, identity, presentation, credentials, details),
                          G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, added, a);
}

/* the details of signing in again: the account's own id, so goa-daemon keeps everything else */
static GVariant *
refresh_details (Form *f)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&b, "{ss}", "Id", f->id);
  return g_variant_builder_end (&b);
}

static GVariant *
password_credentials (const char *key, const char *password, const char *key2, const char *password2)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE_VARDICT);
  if (key)
    g_variant_builder_add (&b, "{sv}", key, g_variant_new_string (password));
  if (key2)
    g_variant_builder_add (&b, "{sv}", key2, g_variant_new_string (password2));
  return g_variant_builder_end (&b);
}

static gboolean
filled (Form *f, int slot)
{
  return *form_text (f, slot) != '\0';
}

/* ---- WebDAV (goawebdavprovider.c) ---- */

static gboolean
webdav_ready (Form *f)
{
  if (f->path)
    return filled (f, F_PASSWORD);
  g_autofree char *uri = ns_goa_normalize_url (form_text (f, F_SERVER), NULL, NULL);
  return uri && filled (f, F_USER) && filled (f, F_PASSWORD);
}

static void
webdav_checked (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (NsDavFound) found = ns_dav_add_check_finish (res, &err);
  if (!form_alive (f))
    return;
  if (!found) {
    form_error (f, err, 0);
    form_unref (f);
    return;
  }
  const char *user = form_text (f, F_USER);
  GVariantBuilder d;
  g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&d, "{ss}", "Uri", found->webdav ? found->webdav : "");
  g_variant_builder_add (&d, "{ss}", "CalendarEnabled", found->caldav ? "true" : "false");
  g_variant_builder_add (&d, "{ss}", "CalDavUri", found->caldav ? found->caldav : "");
  g_variant_builder_add (&d, "{ss}", "ContactsEnabled", found->carddav ? "true" : "false");
  g_variant_builder_add (&d, "{ss}", "CardDavUri", found->carddav ? found->carddav : "");
  g_variant_builder_add (&d, "{ss}", "FilesEnabled", found->webdav ? "true" : "false");
  g_variant_builder_add (&d, "{ss}", "AcceptSslErrors", f->accept_ssl[0] ? "true" : "false");
  form_add (f, user, f->presentation, password_credentials ("password", form_text (f, F_PASSWORD), NULL, NULL),
            g_variant_builder_end (&d), NULL);
  form_unref (f);
}

/* signing in again: the account's own address checked with the new password */
static void
dav_rechecked (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  gboolean ok = ns_dav_check_finish (res, &err);
  if (!form_alive (f))
    return;
  if (!ok)
    form_error (f, err, 0);
  else
    form_add (f, f->identity, f->presentation, password_credentials ("password", form_text (f, F_PASSWORD), NULL, NULL),
              refresh_details (f), NULL);
  form_unref (f);
}

static void
webdav_submit (Form *f)
{
  const char *password = form_text (f, F_PASSWORD);
  if (f->path) {
    /* goa_webdav_provider's ensure_credentials: the first of its addresses there is */
    static const char *const keys[][2] = { { "Uri", "webdav" }, { "CalDavUri", "caldav" }, { "CardDavUri", "carddav" } };
    gboolean accept = detail_true (f->details, "AcceptSslErrors") || f->accept_ssl[0];
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
      const char *uri = detail (f->details, keys[i][0]);
      if (!*uri)
        continue;
      form_busy (f, _("Checking the server…"));
      ns_dav_check (keys[i][1], uri, f->identity, password, accept, f->cancellable, dav_rechecked, form_ref (f));
      return;
    }
    form_add (f, f->identity, f->presentation, password_credentials ("password", password, NULL, NULL),
              refresh_details (f), NULL);
    return;
  }

  const char *user = form_text (f, F_USER);
  g_autofree char *server = NULL;
  g_autofree char *uri = ns_goa_normalize_url (form_text (f, F_SERVER), NULL, &server);
  if (!uri) {
    g_autofree char *text = g_strdup_printf (_("Invalid URI: %s"), form_text (f, F_SERVER));
    form_error_text (f, text);
    return;
  }
  g_free (f->presentation);
  f->presentation = strchr (user, '@') ? g_strdup (user) : g_strconcat (user, "@", server, NULL);
  if (form_check_duplicate (f, f->type, user, f->presentation))
    return;
  /* the address GOA uses, shown as it does */
  form_set_text (f, F_SERVER, uri);
  form_busy (f, _("Looking for the server's calendars, contacts and files…"));
  ns_dav_add_check (uri, user, password, form_text (f, F_FILES), form_text (f, F_CALDAV), form_text (f, F_CARDDAV),
                    f->accept_ssl[0], f->cancellable, webdav_checked, form_ref (f));
}

/* the page that asks for a password again: what the account is, then the field */
static void
refresh_head (Form *f, GtkWidget *g)
{
  ns_row_info (g, _("Account"), f->presentation);
}

static void
webdav_page (GoaProvider *p, Form *refresh)
{
  Form *f = refresh;
  if (!f) {
    f = form_new (p, NULL, _("Calendars, contacts and files from a WebDAV server"), _("Sign in"));
    GtkWidget *g = ns_group (f->content, NULL);
    form_field (f, g, F_SERVER, _("Server address"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
    ns_note (f->content, _("For example example.com or 192.0.2.82"));
    g = ns_group (f->content, NULL);
    form_field (f, g, F_USER, _("User name"), NULL, FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
    form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
    /* GOA offers its own addresses only for WebDAV itself, not for the providers built on it */
    if (g_str_equal (f->type, "webdav")) {
      g = ns_group (f->content, _("Server addresses (optional)"));
      form_field (f, g, F_FILES, _("Files"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
      form_field (f, g, F_CALDAV, _("Calendar (CalDAV)"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
      form_field (f, g, F_CARDDAV, _("Contacts (CardDAV)"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
    }
  } else {
    GtkWidget *g = ns_group (f->content, NULL);
    refresh_head (f, g);
    form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  }
  form_finish (f, webdav_submit, webdav_ready);
}

/* ---- Nextcloud (goaowncloudprovider.c): Login Flow v2, in the browser ---- */

static void nextcloud_poll (gpointer data);

static void
nextcloud_got (Form *f, const char *server, const char *login, const char *app_password)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GUri) uri = g_uri_parse (server, SOUP_HTTP_URI_FLAGS, &err);
  if (!uri) {
    form_error (f, err, -1);
    return;
  }
  if (f->path) {
    /* the same account signed in again, or nothing */
    if (g_strcmp0 (f->identity, login) != 0) {
      /* TRANSLATORS: the user name asked for, then the one the server signed in */
      g_autofree char *text = g_strdup_printf (_("Was asked to log in as %s, but logged in as %s"), f->identity, login);
      form_error_text (f, text);
      return;
    }
    form_add (f, f->identity, f->presentation, password_credentials ("password", app_password, NULL, NULL),
              refresh_details (f), NULL);
    return;
  }
  g_autofree char *presentation = strchr (login, '@') ? g_strdup (login)
                                                      : g_strconcat (login, "@", g_uri_get_host (uri), NULL);
  g_autofree char *base = g_uri_join (G_URI_FLAGS_PARSE_RELAXED, g_uri_get_scheme (uri), NULL, g_uri_get_host (uri),
                                      g_uri_get_port (uri), g_uri_get_path (uri), g_uri_get_query (uri),
                                      g_uri_get_fragment (uri));
  g_autofree char *webdav = ns_goa_normalize_url (base, "remote.php/webdav", NULL);
  g_autofree char *dav = ns_goa_normalize_url (base, "remote.php/dav", NULL);
  if (!webdav || !dav) {
    g_autofree char *text = g_strdup_printf (_("Invalid URI: %s"), server);
    form_error_text (f, text);
    return;
  }
  GVariantBuilder d;
  g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&d, "{ss}", "Uri", webdav);
  g_variant_builder_add (&d, "{ss}", "FilesEnabled", "true");
  g_variant_builder_add (&d, "{ss}", "CalendarEnabled", "true");
  g_variant_builder_add (&d, "{ss}", "CalDavUri", dav);
  g_variant_builder_add (&d, "{ss}", "ContactsEnabled", "true");
  g_variant_builder_add (&d, "{ss}", "CardDavUri", dav);
  /* GOA writes "false" here whatever was chosen; the choice is written instead, so that an account whose
   * certificate the user chose to ignore can sign in afterwards too */
  g_variant_builder_add (&d, "{ss}", "AcceptSslErrors", f->accept_ssl[0] ? "true" : "false");
  form_add (f, login, presentation, password_credentials ("password", app_password, NULL, NULL),
            g_variant_builder_end (&d), NULL);
}

static void
nextcloud_polled (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autofree char *server = NULL, *login = NULL, *app_password = NULL;
  gboolean ok = ns_nextcloud_poll_finish (res, &server, &login, &app_password, &err);
  if (!form_alive (f))
    return;
  if (!ok)
    form_error (f, err, 0);
  else if (!server)
    f->poll_id = g_timeout_add_seconds_once (NEXTCLOUD_POLL_SECONDS, nextcloud_poll, f);
  else
    nextcloud_got (f, server, login, app_password);
  if (app_password)
    memset (app_password, 0, strlen (app_password));
  form_unref (f);
}

static void
nextcloud_poll (gpointer data)
{
  Form *f = data;
  f->poll_id = 0;
  ns_nextcloud_poll (f->poll, f->token, f->accept_ssl[0], f->cancellable, nextcloud_polled, form_ref (f));
}

static void
nextcloud_launched (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  gboolean ok = g_app_info_launch_default_for_uri_finish (res, &err);
  if (!form_alive (f))
    return;
  if (!ok) {
    form_error (f, err, -1);
  } else {
    form_busy (f, _("Sign in on the page that opened in the browser, then come back here."));
    f->poll_id = g_timeout_add_seconds_once (NEXTCLOUD_POLL_SECONDS, nextcloud_poll, f);
  }
  form_unref (f);
}

static void
nextcloud_started (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autofree char *login = NULL;
  g_clear_pointer (&f->poll, g_free);
  g_clear_pointer (&f->token, g_free);
  gboolean ok = ns_nextcloud_start_finish (res, &login, &f->poll, &f->token, &err);
  if (!form_alive (f))
    return;
  if (!ok) {
    form_error (f, err, 0);
  } else {
    form_busy (f, _("Opening the sign-in page in the browser…"));
    g_app_info_launch_default_for_uri_async (login, NULL, f->cancellable, nextcloud_launched, form_ref (f));
  }
  form_unref (f);
}

static void
nextcloud_submit (Form *f)
{
  const char *server = f->path ? detail (f->details, "Uri") : form_text (f, F_SERVER);
  g_autofree char *uri = NULL;
  if (f->path) {
    /* the server the account's files are on, without remote.php/webdav/ */
    g_autofree char *base = g_strdup (server);
    char *at = g_strrstr (base, "remote.php/webdav");
    if (at)
      *at = '\0';
    uri = ns_goa_normalize_url (base, NULL, NULL);
  } else {
    uri = ns_goa_normalize_url (server, NULL, NULL);
  }
  if (!uri) {
    g_autofree char *text = g_strdup_printf (_("Invalid URI: %s"), server);
    form_error_text (f, text);
    return;
  }
  if (!f->path)
    form_set_text (f, F_SERVER, uri);
  form_busy (f, _("Connecting to the server…"));
  ns_nextcloud_start (uri, f->accept_ssl[0] || detail_true (f->details, "AcceptSslErrors"), f->cancellable,
                      nextcloud_started, form_ref (f));
}

static gboolean
nextcloud_ready (Form *f)
{
  if (f->path)
    return TRUE;
  g_autofree char *uri = ns_goa_normalize_url (form_text (f, F_SERVER), NULL, NULL);
  return uri != NULL;
}

static void
nextcloud_page (GoaProvider *p, Form *refresh)
{
  Form *f = refresh;
  if (!f) {
    f = form_new (p, NULL, _("Your Nextcloud server's files, calendars and contacts. You sign in in the browser."),
                  _("Sign in"));
    GtkWidget *g = ns_group (f->content, NULL);
    form_field (f, g, F_SERVER, _("Server address"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
    ns_note (f->content, _("For example example.com or 192.0.2.82"));
  } else {
    GtkWidget *g = ns_group (f->content, NULL);
    refresh_head (f, g);
    ns_note (f->content, _("The server's sign-in page opens in the browser."));
  }
  form_finish (f, nextcloud_submit, nextcloud_ready);
}

/* ---- IMAP and SMTP (goaimapsmtpprovider.c) ---- */

/* GoaTlsType - 1, as GOA's dialog lists them */
static const char *const tls_labels[] = { N_("STARTTLS after connecting"), N_("SSL on a dedicated port"), NULL };

static NsTls
tls_of (guint pos)
{
  return pos == 0 ? NS_TLS_STARTTLS : NS_TLS_SSL;
}

static void
imap_picked (GtkWidget *row, guint i, gpointer data)
{
  Form *f = data;
  if (row == f->imap_pick)
    f->imap_tls = i;
  else
    f->smtp_tls = i;
  form_changed (NULL, f);
}

static void
imap_found (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (NsMailFound) found = ns_mail_autoconfig_finish (res, &err);
  if (!form_alive (f))
    return;
  if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    form_unref (f);
    return;
  }
  g_clear_object (&f->detecting);
  /* GOA's update_account_details_ui: what was found, or empty fields; SSL where nothing (or "plain") was said */
  form_set_text (f, F_IMAP_HOST, found ? found->imap_host : NULL);
  form_set_text (f, F_IMAP_USER, found ? found->imap_user : NULL);
  f->imap_tls = found && found->imap_host && found->imap_tls == NS_TLS_STARTTLS ? 0 : 1;
  ns_row_pick_select (f->imap_pick, f->imap_tls);
  form_set_text (f, F_SMTP_HOST, found ? found->smtp_host : NULL);
  form_set_text (f, F_SMTP_USER, found ? found->smtp_user : NULL);
  f->smtp_tls = found && found->smtp_host && found->smtp_tls == NS_TLS_STARTTLS ? 0 : 1;
  ns_row_pick_select (f->smtp_pick, f->smtp_tls);
  if (found && (found->imap_host || found->smtp_host))
    gtk_label_set_text (GTK_LABEL (f->detect), _("The servers below were found for this address."));
  else
    gtk_label_set_text (GTK_LABEL (f->detect), _("The servers for this address could not be found: enter them below."));
  form_update_go (f);
  form_unref (f);
}

static void
imap_detect (gpointer data)
{
  Form *f = data;
  f->detect_id = 0;
  if (!ns_goa_parse_email (form_text (f, F_EMAIL), NULL, NULL))
    return;
  f->detecting = g_cancellable_new ();
  gtk_label_set_text (GTK_LABEL (f->detect), _("Looking up the servers for this address…"));
  ns_mail_autoconfig (form_text (f, F_EMAIL), f->detecting, imap_found, form_ref (f));
}

/* the address typed: its servers looked up, as GOA's dialog does on every change (here once typing pauses) */
static void
imap_email_changed (GtkEditable *e, Form *f)
{
  if (f->quiet || f->busy)
    return;
  if (f->detecting) {
    g_cancellable_cancel (f->detecting);
    g_clear_object (&f->detecting);
  }
  g_clear_handle_id (&f->detect_id, g_source_remove);
  gtk_label_set_text (GTK_LABEL (f->detect),
                      _("The servers are looked up from your provider when possible."));
  if (ns_goa_parse_email (form_text (f, F_EMAIL), NULL, NULL))
    f->detect_id = g_timeout_add_once (400, imap_detect, f);
}

/* GOA's on_password_changed: the address's password is the servers' too */
static void
imap_password_changed (GtkEditable *e, Form *f)
{
  if (f->quiet || f->busy || !filled (f, F_PASSWORD) || !ns_goa_parse_email (form_text (f, F_EMAIL), NULL, NULL))
    return;
  form_set_text (f, F_IMAP_PASSWORD, form_text (f, F_PASSWORD));
  form_set_text (f, F_SMTP_PASSWORD, form_text (f, F_PASSWORD));
}

static gboolean
imap_ready (Form *f)
{
  if (f->path)
    return filled (f, F_IMAP_PASSWORD);
  return ns_goa_parse_email (form_text (f, F_EMAIL), NULL, NULL) && filled (f, F_IMAP_HOST) && filled (f, F_IMAP_USER) &&
         filled (f, F_IMAP_PASSWORD);
}

static void
imap_add (Form *f)
{
  const char *email = form_text (f, F_EMAIL);
  const char *smtp = form_text (f, F_SMTP_HOST);
  gboolean auth = *smtp && f->smtp_auth.use_auth;
  GVariant *creds = password_credentials ("imap-password", form_text (f, F_IMAP_PASSWORD),
                                          auth ? "smtp-password" : NULL, form_text (f, F_SMTP_PASSWORD));
  if (f->path) {
    form_add (f, f->identity, f->presentation, creds, refresh_details (f), NULL);
    return;
  }
  NsTls imap_tls = tls_of (f->imap_tls), smtp_tls = tls_of (f->smtp_tls);
  GVariantBuilder d;
  g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&d, "{ss}", "Enabled", "true");
  g_variant_builder_add (&d, "{ss}", "EmailAddress", email);
  g_variant_builder_add (&d, "{ss}", "Name", form_text (f, F_NAME));
  g_variant_builder_add (&d, "{ss}", "ImapHost", form_text (f, F_IMAP_HOST));
  g_variant_builder_add (&d, "{ss}", "ImapUserName", form_text (f, F_IMAP_USER));
  g_variant_builder_add (&d, "{ss}", "ImapUseSsl", imap_tls == NS_TLS_SSL ? "true" : "false");
  g_variant_builder_add (&d, "{ss}", "ImapUseTls", imap_tls == NS_TLS_STARTTLS ? "true" : "false");
  g_variant_builder_add (&d, "{ss}", "ImapAcceptSslErrors", f->accept_ssl[0] ? "true" : "false");
  if (*smtp) {
    g_variant_builder_add (&d, "{ss}", "SmtpHost", smtp);
    g_variant_builder_add (&d, "{ss}", "SmtpUseAuth", auth ? "true" : "false");
    if (auth) {
      g_variant_builder_add (&d, "{ss}", "SmtpUserName", form_text (f, F_SMTP_USER));
      g_variant_builder_add (&d, "{ss}", "SmtpAuthLogin", f->smtp_auth.auth_login ? "true" : "false");
      g_variant_builder_add (&d, "{ss}", "SmtpAuthPlain", f->smtp_auth.auth_plain ? "true" : "false");
    }
    g_variant_builder_add (&d, "{ss}", "SmtpUseSsl", smtp_tls == NS_TLS_SSL ? "true" : "false");
    g_variant_builder_add (&d, "{ss}", "SmtpUseTls", smtp_tls == NS_TLS_STARTTLS ? "true" : "false");
    g_variant_builder_add (&d, "{ss}", "SmtpAcceptSslErrors", f->accept_ssl[1] ? "true" : "false");
  }
  form_add (f, email, email, creds, g_variant_builder_end (&d), NULL);
}

static void
mail_failed (Form *f, GError *err, int stage)
{
  /* GOA's own words, after which server it was */
  g_prefix_error_literal (&err, stage == 0 ? "IMAP: " : "SMTP: ");
  form_error (f, err, stage);
}

static void
smtp_checked (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  NsSmtpAuth auth = { 0 };
  gboolean ok = ns_mail_check_finish (res, &auth, &err);
  if (!form_alive (f))
    return;
  if (!ok) {
    mail_failed (f, err, 1);
  } else {
    f->smtp_auth = auth;
    imap_add (f);
  }
  form_unref (f);
}

/* the account's mail address's domain, for SMTP's EHLO */
static char *
imap_domain (Form *f)
{
  char *domain = NULL;
  ns_goa_parse_email (f->path ? detail (f->details, "EmailAddress") : form_text (f, F_EMAIL), NULL, &domain);
  return domain;
}

static void
imap_checked (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  gboolean ok = ns_mail_check_finish (res, NULL, &err);
  if (!form_alive (f))
    return;
  if (!ok) {
    mail_failed (f, err, 0);
    form_unref (f);
    return;
  }
  const char *smtp = f->path ? detail (f->details, "SmtpHost") : form_text (f, F_SMTP_HOST);
  /* no SMTP server: an account for reading mail only; signing in again checks SMTP only where it signs in */
  if (!*smtp || (f->path && !detail_true (f->details, "SmtpUseAuth"))) {
    f->smtp_auth.use_auth = f->path ? FALSE : f->smtp_auth.use_auth;
    imap_add (f);
    form_unref (f);
    return;
  }
  g_autofree char *domain = imap_domain (f);
  NsTls tls = f->path ? (detail_true (f->details, "SmtpUseTls") ? NS_TLS_STARTTLS : NS_TLS_SSL) : tls_of (f->smtp_tls);
  const char *user = f->path ? detail (f->details, "SmtpUserName") : form_text (f, F_SMTP_USER);
  gboolean accept = f->accept_ssl[1] || (f->path && detail_true (f->details, "SmtpAcceptSslErrors"));
  form_busy (f, _("Checking the outgoing mail server…"));
  ns_mail_check (TRUE, smtp, tls, accept, user, form_text (f, F_SMTP_PASSWORD), domain, f->cancellable, smtp_checked,
                 form_ref (f));
  form_unref (f);
}

static void
imap_submit (Form *f)
{
  if (!f->path) {
    const char *email = form_text (f, F_EMAIL);
    if (form_check_duplicate (f, f->type, email, email))
      return;
  }
  const char *host = f->path ? detail (f->details, "ImapHost") : form_text (f, F_IMAP_HOST);
  const char *user = f->path ? detail (f->details, "ImapUserName") : form_text (f, F_IMAP_USER);
  NsTls tls = f->path ? (detail_true (f->details, "ImapUseTls") ? NS_TLS_STARTTLS : NS_TLS_SSL) : tls_of (f->imap_tls);
  gboolean accept = f->accept_ssl[0] || (f->path && detail_true (f->details, "ImapAcceptSslErrors"));
  if (f->path)
    f->smtp_auth.use_auth = detail_true (f->details, "SmtpUseAuth");
  form_busy (f, _("Checking the incoming mail server…"));
  ns_mail_check (FALSE, host, tls, accept, user, form_text (f, F_IMAP_PASSWORD), NULL, f->cancellable, imap_checked,
                 form_ref (f));
}

static void
imap_page (GoaProvider *p, Form *refresh)
{
  Form *f = refresh;
  if (refresh) {
    GtkWidget *g = ns_group (f->content, NULL);
    refresh_head (f, g);
    g = ns_group (f->content, _("Incoming mail (IMAP)"));
    ns_row_info (g, _("Server"), detail (f->details, "ImapHost"));
    form_field (f, g, F_IMAP_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
    if (*detail (f->details, "SmtpHost") && detail_true (f->details, "SmtpUseAuth")) {
      g = ns_group (f->content, _("Outgoing mail (SMTP)"));
      ns_row_info (g, _("Server"), detail (f->details, "SmtpHost"));
      form_field (f, g, F_SMTP_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
    }
    form_finish (f, imap_submit, imap_ready);
    return;
  }

  f = form_new (p, NULL, _("Mail from any provider, by IMAP and SMTP"), _("Sign in"));
  const char *real = g_get_real_name ();
  GtkWidget *g = ns_group (f->content, NULL);
  form_field (f, g, F_NAME, _("Name"), g_strcmp0 (real, "Unknown") ? real : NULL, FALSE, GTK_INPUT_PURPOSE_NAME);
  form_field (f, g, F_EMAIL, _("Email address"), NULL, FALSE, GTK_INPUT_PURPOSE_EMAIL);
  form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  f->detect = ns_note (f->content, _("The servers are looked up from your provider when possible."));
  g_signal_connect (f->field[F_EMAIL], "changed", G_CALLBACK (imap_email_changed), f);
  g_signal_connect (f->field[F_PASSWORD], "changed", G_CALLBACK (imap_password_changed), f);

  f->imap_tls = f->smtp_tls = 1;
  g = ns_group (f->content, _("Incoming mail (IMAP)"));
  form_field (f, g, F_IMAP_HOST, _("Server"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
  f->imap_pick = ns_row_pick (g, _("Encryption"), tls_labels, f->imap_tls, imap_picked, f);
  g_ptr_array_add (f->inputs, f->imap_pick);
  form_field (f, g, F_IMAP_USER, _("User name"), NULL, FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
  form_field (f, g, F_IMAP_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  ns_note (f->content, _("For example imap.example.com"));

  g = ns_group (f->content, _("Outgoing mail (SMTP)"));
  form_field (f, g, F_SMTP_HOST, _("Server"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
  f->smtp_pick = ns_row_pick (g, _("Encryption"), tls_labels, f->smtp_tls, imap_picked, f);
  g_ptr_array_add (f->inputs, f->smtp_pick);
  form_field (f, g, F_SMTP_USER, _("User name"), NULL, FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
  form_field (f, g, F_SMTP_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  ns_note (f->content, _("For example smtp.example.com. Leave it empty for an account that only reads mail."));
  form_finish (f, imap_submit, imap_ready);
}

/* ---- Microsoft Exchange (goaexchangeprovider.c) ---- */

static void
exchange_checked (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  gboolean ok = ns_ews_autodiscover_finish (res, &err);
  if (!form_alive (f))
    return;
  if (!ok) {
    form_error (f, err, 0);
    form_unref (f);
    return;
  }
  GVariant *creds = password_credentials ("password", form_text (f, F_PASSWORD), NULL, NULL);
  if (f->path) {
    form_add (f, f->identity, f->presentation, creds, refresh_details (f), NULL);
    form_unref (f);
    return;
  }
  GVariantBuilder d;
  g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&d, "{ss}", "MailEnabled", "true");
  g_variant_builder_add (&d, "{ss}", "CalendarEnabled", "true");
  g_variant_builder_add (&d, "{ss}", "ContactsEnabled", "true");
  g_variant_builder_add (&d, "{ss}", "Host", form_text (f, F_DOMAIN));
  g_variant_builder_add (&d, "{ss}", "AcceptSslErrors", f->accept_ssl[0] ? "true" : "false");
  form_add (f, form_text (f, F_USER), form_text (f, F_EMAIL), creds, g_variant_builder_end (&d), NULL);
  form_unref (f);
}

static void
exchange_submit (Form *f)
{
  const char *email, *user, *server;
  gboolean accept = f->accept_ssl[0];
  if (f->path) {
    email = f->presentation;
    user = f->identity;
    server = detail (f->details, "Host");
    accept = accept || detail_true (f->details, "AcceptSslErrors");
  } else {
    email = form_text (f, F_EMAIL);
    user = form_text (f, F_USER);
    server = form_text (f, F_DOMAIN);
    if (form_check_duplicate (f, f->type, user, email))
      return;
  }
  form_busy (f, _("Looking for the Exchange server…"));
  ns_ews_autodiscover (email, form_text (f, F_PASSWORD), user, server, accept, f->cancellable, exchange_checked,
                       form_ref (f));
}

/* GOA's on_email_or_password_changed: the address gives the user name and the domain */
static void
exchange_email_changed (GtkEditable *e, Form *f)
{
  g_autofree char *local = NULL, *domain = NULL;
  if (f->quiet || f->busy || !ns_goa_parse_email (form_text (f, F_EMAIL), &local, &domain))
    return;
  form_set_text (f, F_USER, local);
  form_set_text (f, F_DOMAIN, domain);
}

static gboolean
exchange_ready (Form *f)
{
  if (f->path)
    return filled (f, F_PASSWORD);
  return ns_goa_parse_email (form_text (f, F_EMAIL), NULL, NULL) && filled (f, F_PASSWORD) && filled (f, F_USER) &&
         filled (f, F_DOMAIN);
}

static void
exchange_page (GoaProvider *p, Form *refresh)
{
  Form *f = refresh;
  if (refresh) {
    GtkWidget *g = ns_group (f->content, NULL);
    refresh_head (f, g);
    form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  } else {
    f = form_new (p, NULL, _("Calendars, contacts and mail from a Microsoft Exchange server"), _("Sign in"));
    GtkWidget *g = ns_group (f->content, NULL);
    form_field (f, g, F_EMAIL, _("Email address"), NULL, FALSE, GTK_INPUT_PURPOSE_EMAIL);
    form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
    ns_note (f->content, _("The account's details are found from the email address when possible."));
    g = ns_group (f->content, _("Account details"));
    form_field (f, g, F_USER, _("User name"), NULL, FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
    form_field (f, g, F_DOMAIN, _("Domain"), NULL, FALSE, GTK_INPUT_PURPOSE_URL);
    ns_note (f->content, _("For example example.com"));
    g_signal_connect (f->field[F_EMAIL], "changed", G_CALLBACK (exchange_email_changed), f);
  }
  form_finish (f, exchange_submit, exchange_ready);
}

/* ---- Kerberos (goakerberosprovider.c), signed in by goa-identity-service ---- */

/* normalize_principal: user@REALM, the realm in capitals */
static char *
kerberos_principal (Form *f, char **realm)
{
  g_autofree char *local = NULL, *domain = NULL;
  const char *text = form_text (f, F_PRINCIPAL);
  g_autofree char *with_realm = NULL;
  /* Fedora's realm is its own: the user name alone is enough */
  if (f->fedora && !strchr (text, '@'))
    text = with_realm = g_strconcat (text, "@" FEDORA_REALM, NULL);
  if (!ns_goa_parse_email (text, &local, &domain))
    return NULL;
  char *up = g_utf8_strup (domain, -1);
  char *principal = g_strconcat (local, "@", up, NULL);
  if (realm)
    *realm = up;
  else
    g_free (up);
  return principal;
}

/* goa_kerberos_provider_set_error: the identity service's errors in GOA's words */
static void
kerberos_error (Form *f, GError *err)
{
  g_autofree char *remote = g_dbus_error_get_remote_error (err);
  if (g_strcmp0 (remote, "org.gnome.Identity.Manager.Error.CreatingIdentity") == 0 ||
      g_strcmp0 (remote, "org.gnome.Identity.Error.AuthenticationFailed") == 0) {
    g_autoptr (GError) e = g_error_new_literal (GOA_ERROR, GOA_ERROR_NOT_AUTHORIZED, _("Authentication failed"));
    form_error (f, e, -1);
    return;
  }
  form_error (f, err, -1);
}

static void
kerberos_failed (Form *f, GError *err)
{
  /* a temporary account that did not sign in goes, as in GOA's add_account_ticket_cb */
  if (f->adding) {
    remove_account (f->adding);
    g_clear_pointer (&f->adding, g_free);
  }
  kerberos_error (f, err);
}

/* signed in: the account kept (IsTemporary false), with the password when it is to be remembered */
static void
kerberos_signed_in (Form *f)
{
  gboolean remember = f->remember && ns_switch_get_active (ns_row_get_switch (f->remember));
  GVariant *creds = password_credentials (remember ? "password" : NULL, f->password, NULL, NULL);
  GVariantBuilder d;
  g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&d, "{ss}", "Id", f->id);
  if (!f->path)
    g_variant_builder_add (&d, "{ss}", "IsTemporary", "false");
  form_add (f, f->principal, f->principal, creds, g_variant_builder_end (&d), NULL);
}

static void
kerberos_signin_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!form_alive (f))
    return;
  if (!v)
    kerberos_failed (f, err);
  else
    kerberos_signed_in (f);
  form_unref (f);
}

typedef struct {
  Form              *f;
  GcrSecretExchange *exchange;
} KerberosKeys;

static void
kerberos_keys_exchanged (GObject *src, GAsyncResult *res, gpointer data)
{
  KerberosKeys *k = data;
  Form *f = k->f;
  g_autoptr (GcrSecretExchange) exchange = k->exchange;
  g_free (k);
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!form_alive (f))
    return;
  const char *key = NULL;
  if (v)
    g_variant_get (v, "(&s)", &key);
  if (!v) {
    kerberos_failed (f, err);
  } else if (!gcr_secret_exchange_receive (exchange, key)) {
    g_autoptr (GError) e = g_error_new_literal (GOA_ERROR, GOA_ERROR_FAILED,
                                                _("Identity service returned invalid key"));
    kerberos_failed (f, e);
  } else {
    /* the password goes to the identity service encrypted with the key it agreed */
    g_autofree char *concealed = gcr_secret_exchange_send (exchange, f->password, -1);
    GVariantBuilder d;
    g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
    g_variant_builder_add (&d, "{ss}", "initial-password", concealed);
    const char *preauth = detail (f->details, "PreauthenticationSource");
    if (*preauth)
      g_variant_builder_add (&d, "{ss}", "preauthentication-source", preauth);
    g_dbus_connection_call (G_DBUS_CONNECTION (src), IDENTITY_BUS, IDENTITY_MANAGER_PATH, IDENTITY_MANAGER_IFACE,
                            "SignIn", g_variant_new ("(s@a{ss})", f->principal, g_variant_builder_end (&d)),
                            G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, f->cancellable, kerberos_signin_done,
                            form_ref (f));
  }
  form_unref (f);
}

/* goa_kerberos_provider_sign_in_sync: the secret keys exchanged with the identity service (gcr's secret exchange),
 * then its SignIn with the password */
static void
kerberos_sign_in (Form *f)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  KerberosKeys *k = g_new0 (KerberosKeys, 1);
  k->f = form_ref (f);
  k->exchange = gcr_secret_exchange_new (NULL);
  g_autofree char *mine = gcr_secret_exchange_begin (k->exchange);
  form_busy (f, _("Signing in…"));
  g_dbus_connection_call (bus, IDENTITY_BUS, IDENTITY_MANAGER_PATH, IDENTITY_MANAGER_IFACE, "ExchangeSecretKeys",
                          g_variant_new ("(ss)", f->principal, mine), G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE,
                          -1, f->cancellable, kerberos_keys_exchanged, k);
}

/* the temporary account is there: its id, then the ticket */
static void
kerberos_temporary (Form *f, const char *path)
{
  const char *slash = strrchr (path, '/');
  g_set_str (&f->id, slash ? slash + 1 : path);
  kerberos_sign_in (f);
}

static void
kerberos_submit (Form *f)
{
  g_clear_pointer (&f->password, g_free);
  f->password = g_strdup (form_text (f, F_PASSWORD));
  if (f->path) {
    kerberos_sign_in (f);
    return;
  }
  g_autofree char *realm = NULL;
  g_free (f->principal);
  f->principal = kerberos_principal (f, &realm);
  if (!f->principal) {
    form_error_text (f, _("The principal is a user name and a realm: user@EXAMPLE.COM"));
    return;
  }
  form_set_text (f, F_PRINCIPAL, f->principal);
  g_autoptr (GError) e = NULL;
  if (form_duplicate (f, "kerberos", f->principal, f->principal, &e) ||
      form_duplicate (f, "fedora", f->principal, f->principal, &e)) {
    form_error (f, e, -1);
    return;
  }
  /* GOA's add_account_action_cb: a temporary account first, signed in, then kept */
  GVariantBuilder d;
  g_variant_builder_init (&d, G_VARIANT_TYPE ("a{ss}"));
  g_variant_builder_add (&d, "{ss}", "Realm", realm);
  g_variant_builder_add (&d, "{ss}", "IsTemporary", "true");
  g_variant_builder_add (&d, "{ss}", "TicketingEnabled", "true");
  form_add (f, f->principal, f->principal, password_credentials (NULL, NULL, NULL, NULL), g_variant_builder_end (&d),
            kerberos_temporary);
}

static gboolean
kerberos_ready (Form *f)
{
  if (f->path)
    return filled (f, F_PASSWORD);
  g_autofree char *principal = kerberos_principal (f, NULL);
  return principal && filled (f, F_PASSWORD);
}

static void
kerberos_page (GoaProvider *p, Form *refresh)
{
  Form *f = refresh;
  if (refresh) {
    g_set_str (&f->principal, f->identity);
    GtkWidget *g = ns_group (f->content, NULL);
    refresh_head (f, g);
    form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
  } else {
    f = form_new (p, NULL, _("Web and network resources of your organization, by Kerberos"), _("Sign in"));
    f->fedora = g_str_equal (f->type, "fedora");
    GtkWidget *g = ns_group (f->content, NULL);
    form_field (f, g, F_PRINCIPAL, f->fedora ? _("User name") : _("Principal"), NULL, FALSE, GTK_INPUT_PURPOSE_EMAIL);
    form_field (f, g, F_PASSWORD, _("Password"), NULL, TRUE, GTK_INPUT_PURPOSE_PASSWORD);
    ns_note (f->content, f->fedora ? _("Your Fedora account's user name") : _("For example user@EXAMPLE.COM"));
  }
  GtkWidget *g = ns_group (f->content, NULL);
  f->remember = ns_row_switch (g, _("Remember the password"), _("Signs in again on its own when the ticket runs out"),
                               NULL, NULL);
  g_ptr_array_add (f->inputs, f->remember);
  form_finish (f, kerberos_submit, kerberos_ready);
}

/* ---- Google, Microsoft 365: GOA's OAuth2, in the browser ----
 *
 * GOA's OAuth2 providers hold their client IDs in goa-backend and run their flow from their dialog: when the dialog's
 * "state" becomes busy they open the provider's sign-in page in the browser and wait for its answer on D-Bus
 * (org.gnome.OnlineAccounts.OAuth2, which goa-oauth2-handler calls with the redirect). goa_provider_add_account and
 * goa_provider_refresh_account show that dialog as a window of its own; the page here hides it as it opens, fills in
 * what the page asked (Microsoft 365's organization details) and sets its state, so the flow and its client IDs stay
 * GOA's. The account comes back from GOA's call as from its dialog. */

/* the GOA dialog under a window GOA just opened */
static GtkWidget *
find_goa_dialog (GtkWidget *w)
{
  if (g_str_equal (G_OBJECT_TYPE_NAME (w), "GoaProviderDialog"))
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c; c = gtk_widget_get_next_sibling (c)) {
    GtkWidget *found = find_goa_dialog (c);
    if (found)
      return found;
  }
  return NULL;
}

/* rows of the dialog by type, in their order */
static void
find_rows (GtkWidget *w, GType type, GPtrArray *out)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, type))
    g_ptr_array_add (out, w);
  for (GtkWidget *c = gtk_widget_get_first_child (w); c; c = gtk_widget_get_next_sibling (c))
    find_rows (c, type, out);
}

/* the windows that are there now */
static GList *
toplevels (void)
{
  GList *l = gtk_window_list_toplevels ();
  for (GList *i = l; i; i = i->next)
    g_object_ref (i->data);
  return l;
}

static void
oauth_close_dialog (Form *f)
{
  if (!f->dialog)
    return;
  GtkWidget *d = f->dialog;
  g_object_remove_weak_pointer (G_OBJECT (d), (gpointer *) &f->dialog);
  f->dialog = NULL;
  /* done: GOA's dialog closes itself on that, without completing its call a second time */
  int state = 0;
  g_object_get (d, "state", &state, NULL);
  if (state != GOA_DIALOG_DONE)
    g_object_set (d, "state", GOA_DIALOG_DONE, NULL);
}

static void
oauth_finished (Form *f, gboolean ok, GError *err)
{
  oauth_close_dialog (f);
  if (ok)
    form_done (f);
  else if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
           !g_error_matches (err, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED))
    form_error (f, err, -1);
  else if (f->busy)
    form_idle (f);
}

static void
oauth_added (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GoaObject) obj = goa_provider_add_account_finish (GOA_PROVIDER (src), res, &err);
  if (!form_alive (f))
    return;
  oauth_finished (f, obj != NULL, err);
  form_unref (f);
}

static void
oauth_refreshed (GObject *src, GAsyncResult *res, gpointer data)
{
  Form *f = data;
  g_autoptr (GError) err = NULL;
  gboolean ok = goa_provider_refresh_account_finish (GOA_PROVIDER (src), res, &err);
  if (!form_alive (f))
    return;
  oauth_finished (f, ok, err);
  form_unref (f);
}

static GoaObject *
lookup_account (const char *path)
{
  g_autolist (GoaObject) all = goa ? goa_client_get_accounts (goa) : NULL;
  for (GList *l = all; l; l = l->next)
    if (g_strcmp0 (g_dbus_object_get_object_path (l->data), path) == 0)
      return g_object_ref (l->data);
  return NULL;
}

static void
oauth_start (gpointer data)
{
  Form *f = data;
  if (!f->page)
    return;
  g_autolist (GtkWindow) before = toplevels ();
  if (f->path) {
    g_autoptr (GoaObject) obj = lookup_account (f->path);
    if (!obj) {
      form_error_text (f, _("The account is gone."));
      return;
    }
    goa_provider_refresh_account (f->provider, goa, obj, f->go, f->cancellable, oauth_refreshed, form_ref (f));
  } else {
    goa_provider_add_account (f->provider, goa, f->go, f->cancellable, oauth_added, form_ref (f));
  }

  /* the window GOA opened for its dialog: hidden before it is drawn */
  GtkWidget *dialog = NULL;
  g_autolist (GtkWindow) after = toplevels ();
  for (GList *l = after; l && !dialog; l = l->next) {
    if (g_list_find (before, l->data))
      continue;
    dialog = find_goa_dialog (l->data);
    if (dialog)
      gtk_widget_set_visible (l->data, FALSE);
  }
  if (!dialog) {
    /* not GOA's dialog as this page knows it: its own window stays, and the sign-in is done there */
    form_busy (f, _("Continue in the window that opened."));
    return;
  }
  f->dialog = dialog;
  g_object_add_weak_pointer (G_OBJECT (dialog), (gpointer *) &f->dialog);

  if (f->org && ns_switch_get_active (ns_row_get_switch (f->org))) {
    g_autoptr (GPtrArray) switches = g_ptr_array_new ();
    g_autoptr (GPtrArray) entries = g_ptr_array_new ();
    find_rows (dialog, ADW_TYPE_SWITCH_ROW, switches);
    find_rows (dialog, ADW_TYPE_ENTRY_ROW, entries);
    if (switches->len < 1 || entries->len < 2) {
      oauth_dismiss_dialog (f);
      form_error_text (f, _("This version of GNOME Online Accounts asks for an organization's details differently."));
      return;
    }
    adw_switch_row_set_active (g_ptr_array_index (switches, 0), TRUE);
    gtk_editable_set_text (g_ptr_array_index (entries, 0), form_text (f, F_CLIENT_ID));
    gtk_editable_set_text (g_ptr_array_index (entries, 1), form_text (f, F_TENANT));
  }
  form_busy (f, _("Sign in on the page that opened in the browser, then come back here."));
  g_object_set (dialog, "state", GOA_DIALOG_BUSY, NULL);
}

static void
oauth_submit (Form *f)
{
  form_busy (f, _("Opening the sign-in page in the browser…"));
  with_goa (f->page, oauth_start, f);
}

static gboolean
oauth_ready (Form *f)
{
  return !f->org || !ns_switch_get_active (ns_row_get_switch (f->org)) || filled (f, F_TENANT);
}

static void
oauth_org_toggled (GtkWidget *sw, GParamSpec *ps, Form *f)
{
  ns_group_set_visible (f->org_group, ns_switch_get_active (sw));
  form_changed (NULL, f);
}

static void
oauth_page (GoaProvider *p, Form *refresh)
{
  Form *f = refresh;
  g_autofree char *intro = NULL;
  if (refresh) {
    GtkWidget *g = ns_group (f->content, NULL);
    refresh_head (f, g);
    ns_note (f->content, _("The sign-in page opens in the browser."));
  } else {
    g_autofree char *name = goa_provider_get_provider_name (p, NULL);
    /* TRANSLATORS: %s is the provider ("Google") */
    intro = g_strdup_printf (_("You sign in to %s in the browser, on its own page."), name);
    f = form_new (p, NULL, intro, _("Sign in"));
    if (g_str_equal (f->type, "ms_graph")) {
      GtkWidget *g = ns_group (f->content, NULL);
      f->org = ns_row_switch (g, _("Use an organization account"),
                              _("Sign in with details from an organization or developer account"), NULL, NULL);
      g_ptr_array_add (f->inputs, f->org);
      f->org_group = ns_group (f->content, _("Details from your organization"));
      form_field (f, f->org_group, F_CLIENT_ID, _("Client ID (optional)"), NULL, FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
      form_field (f, f->org_group, F_TENANT, _("Tenant ID"), NULL, FALSE, GTK_INPUT_PURPOSE_FREE_FORM);
      ns_group_set_visible (f->org_group, FALSE);
      g_signal_connect (ns_row_get_switch (f->org), "notify::active", G_CALLBACK (oauth_org_toggled), f);
    }
  }
  form_finish (f, oauth_submit, oauth_ready);
}

/* ---- the provider's page ---- */

typedef void (*ProviderPage) (GoaProvider *p, Form *refresh);

static ProviderPage
page_for (GoaProvider *p)
{
  const char *type = goa_provider_get_provider_type (p);
  if (g_str_equal (type, "webdav"))
    return webdav_page;
  if (g_str_equal (type, "owncloud"))
    return nextcloud_page;
  if (g_str_equal (type, "imap_smtp"))
    return imap_page;
  if (g_str_equal (type, "exchange"))
    return exchange_page;
  if (g_str_equal (type, "kerberos") || g_str_equal (type, "fedora"))
    return kerberos_page;
  if (is_oauth2 (p))
    return oauth_page;
  return NULL;
}

/* a provider no page here knows: GOA's own dialog */
static void
goa_added (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GoaObject) obj = goa_provider_add_account_finish (GOA_PROVIDER (src), res, &err);
  if (obj) {
    if (accounts_list)
      ns_pop_to (accounts_list);
  } else if (!g_error_matches (err, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED) && ns_page_of (row)) {
    g_autofree char *text = error_text (err);
    ns_row_set_subtitle (row, text);
  }
  g_object_unref (row);
}

static void
provider_go (gpointer row)
{
  GoaProvider *p = g_object_get_data (G_OBJECT (row), "ns-provider");
  ProviderPage page = page_for (p);
  if (page)
    page (p, NULL);
  else
    goa_provider_add_account (p, goa, row, NULL, goa_added, g_object_ref (row));
}

static void
provider_tapped (GtkWidget *row, gpointer data)
{
  with_goa (row, provider_go, row);
}

static GtkWidget *
provider_icon (GoaProvider *p, int size)
{
  g_autoptr (GIcon) icon = p ? goa_provider_get_provider_icon (p, NULL) : NULL;
  GtkWidget *img = icon ? gtk_image_new_from_gicon (icon) : gtk_image_new_from_icon_name ("avatar-default-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (img), size);
  return img;
}

static void
providers_got (GObject *src, GAsyncResult *res, gpointer group)
{
  GList *all = NULL;
  g_autoptr (GError) err = NULL;
  if (!goa_provider_get_all_finish (&all, res, &err))
    ns_row (group, NULL, _("No providers"), err->message);
  for (GList *l = all; l; l = l->next) {
    GoaProvider *p = l->data;
    g_autofree char *name = goa_provider_get_provider_name (p, NULL);
    GtkWidget *r = ns_row (group, NULL, name, NULL);
    vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (r), provider_icon (p, 32));
    g_object_set_data_full (G_OBJECT (r), "ns-provider", g_object_ref (p), g_object_unref);
    ns_row_on_activate (r, provider_tapped, NULL);
  }
  g_list_free_full (all, g_object_unref);
  g_object_unref (group);
}

static void
add_account (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Add account"), &c);
  GtkWidget *g = ns_group (c, NULL);
  goa_provider_get_all (providers_got, g_object_ref (g));
  ns_push (page);
}

/* ---- an account's page: what it is used for, its servers, signing in again, removing it ---- */

/* What an account is used for, of what GOA offers, in the order its account dialog lists them: only what a VibeOS
 * app takes from an account. Calendar and Contacts (and the Phone and Messages, through Contacts) read them from
 * evolution-data-server's GOA module, Files from gvfs's; nothing here reads mail, chat, documents, music, photos,
 * network resources, read-later lists, printers, maps or to-do lists from an account, so those are not shown. */
static const struct {
  GoaProviderFeatures feature;
  const char         *prop, *label;
} features[] = {
  { GOA_PROVIDER_FEATURE_CALENDAR, "CalendarDisabled", N_("Calendar") },
  { GOA_PROVIDER_FEATURE_CONTACTS, "ContactsDisabled", N_("Contacts") },
  { GOA_PROVIDER_FEATURE_FILES, "FilesDisabled", N_("Files") },
};

typedef struct {
  char      *path;
  NsPage    *page;
  GtkWidget *body;      /* rebuilt when the account changes */
  char      *shape;     /* what the body shows, but the switches' values: unchanged, only the switches are set */
  GPtrArray *switches;
  GVariant  *ifaces;    /* the account's interfaces and properties */
  guint      sub;
} Acct;

static void
acct_free (Acct *a)
{
  g_free (a->path);
  g_free (a->shape);
  g_ptr_array_unref (a->switches);
  g_clear_pointer (&a->ifaces, g_variant_unref);
  g_free (a);
}

static const char *
prop_str (GVariant *ifaces, const char *iface, const char *prop)
{
  g_autoptr (GVariant) props = ifaces ? g_variant_lookup_value (ifaces, iface, G_VARIANT_TYPE_VARDICT) : NULL;
  const char *s = NULL;
  if (props && g_variant_lookup (props, prop, "&s", &s))
    return s;
  return NULL;
}

static gboolean
prop_bool (GVariant *ifaces, const char *iface, const char *prop, gboolean *out)
{
  g_autoptr (GVariant) props = ifaces ? g_variant_lookup_value (ifaces, iface, G_VARIANT_TYPE_VARDICT) : NULL;
  return props && g_variant_lookup (props, prop, "b", out);
}

/* a feature switched: goa-daemon writes it to the account's key file and says so; the page reads it back from there */
static void
feature_set (GtkWidget *sw, GParamSpec *ps, gpointer data)
{
  if (ns_switch_syncing (sw))
    return;
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, GOA_BUS, g_object_get_data (G_OBJECT (sw), "ns-path"),
                            "org.freedesktop.DBus.Properties", "Set",
                            g_variant_new ("(ssv)", GOA_ACCOUNT_IFACE, g_object_get_data (G_OBJECT (sw), "ns-prop"),
                                           g_variant_new_boolean (!ns_switch_get_active (sw))),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
acct_removed (gboolean yes, gpointer row)
{
  if (!yes)
    return;
  remove_account (g_object_get_data (G_OBJECT (row), "ns-path"));
  ns_pop (row);
}

static void
acct_remove (GtkWidget *row, gpointer data)
{
  const char *leave = ns_device_text (_("The phone stops using it and forgets its sign-in. The account itself stays "
                                        "with its provider."),
                                      _("The tablet stops using it and forgets its sign-in. The account itself stays "
                                        "with its provider."));
  ns_confirm (row, _("Remove this account?"), leave, _("Remove"), TRUE, acct_removed, row);
}

/* signing in again with GOA's own dialog: a provider no page here knows */
static void
goa_refreshed (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GError) err = NULL;
  if (!goa_provider_refresh_account_finish (GOA_PROVIDER (src), res, &err) &&
      !g_error_matches (err, GOA_ERROR, GOA_ERROR_DIALOG_DISMISSED) && ns_page_of (row)) {
    g_autofree char *text = error_text (err);
    ns_row_set_subtitle (row, text);
  }
  g_object_unref (row);
}

static void
sign_in_again_go (gpointer row)
{
  const char *path = g_object_get_data (G_OBJECT (row), "ns-path");
  g_autoptr (GoaObject) obj = lookup_account (path);
  GoaAccount *acc = obj ? goa_object_peek_account (obj) : NULL;
  if (!acc)
    return;
  g_autoptr (GoaProvider) p = goa_provider_get_for_provider_type (goa_account_get_provider_type (acc));
  if (!p)
    return;
  ProviderPage page = page_for (p);
  if (!page) {
    goa_provider_refresh_account (p, goa, obj, row, NULL, goa_refreshed, g_object_ref (row));
    return;
  }
  Form *f = form_new (p, _("Sign in again"), NULL, _("Sign in"));
  f->path = g_strdup (path);
  f->id = g_strdup (goa_account_get_id (acc));
  f->identity = g_strdup (goa_account_get_identity (acc));
  f->presentation = g_strdup (goa_account_get_presentation_identity (acc));
  f->details = account_details (f->id);
  page (p, f);
}

static void
sign_in_again (GtkWidget *row, gpointer data)
{
  with_goa (row, sign_in_again_go, row);
}

static void
info (GtkWidget **group, GtkWidget *body, GString *shape, const char *title, const char *value)
{
  if (!value || !*value)
    return;
  if (!*group)
    *group = ns_group (body, _("Servers"));
  ns_row_info (*group, title, value);
  g_string_append_printf (shape, "%s=%s;", title, value);
}

static void
acct_fill (Acct *a)
{
  GVariant *ifaces = a->ifaces;
  const char *type = prop_str (ifaces, GOA_ACCOUNT_IFACE, "ProviderType");
  g_autoptr (GoaProvider) p = type ? goa_provider_get_for_provider_type (type) : NULL;
  GoaProviderFeatures have = p ? goa_provider_get_provider_features (p) : 0;
  gboolean attention = FALSE;
  prop_bool (ifaces, GOA_ACCOUNT_IFACE, "AttentionNeeded", &attention);

  /* what the page shows, but the switches' values */
  g_autoptr (GString) shape = g_string_new (NULL);
  g_string_append_printf (shape, "%s|%d|", prop_str (ifaces, GOA_ACCOUNT_IFACE, "PresentationIdentity"), attention);
  for (guint i = 0; i < G_N_ELEMENTS (features); i++) {
    gboolean off;
    if ((have & features[i].feature) && prop_bool (ifaces, GOA_ACCOUNT_IFACE, features[i].prop, &off))
      g_string_append_printf (shape, "%s;", features[i].prop);
  }
  /* the servers, from the features' interfaces: there only while the feature is on */
  static const char *const servers[][3] = {
    { "org.gnome.OnlineAccounts.Mail", "EmailAddress", N_("Email address") },
    { "org.gnome.OnlineAccounts.Mail", "ImapHost", N_("Incoming mail (IMAP)") },
    { "org.gnome.OnlineAccounts.Mail", "SmtpHost", N_("Outgoing mail (SMTP)") },
    { "org.gnome.OnlineAccounts.Exchange", "Host", N_("Exchange server") },
    { "org.gnome.OnlineAccounts.Calendar", "Uri", N_("Calendars") },
    { "org.gnome.OnlineAccounts.Contacts", "Uri", N_("Contacts") },
    { "org.gnome.OnlineAccounts.Files", "Uri", N_("Files") },
  };
  for (guint i = 0; i < G_N_ELEMENTS (servers); i++)
    g_string_append_printf (shape, "%s;", prop_str (ifaces, servers[i][0], servers[i][1]));

  if (g_strcmp0 (shape->str, a->shape) == 0) {
    /* the same page: only the switches read again */
    for (guint i = 0; i < a->switches->len; i++) {
      GtkWidget *sw = g_ptr_array_index (a->switches, i);
      gboolean off = FALSE;
      prop_bool (ifaces, GOA_ACCOUNT_IFACE, g_object_get_data (G_OBJECT (sw), "ns-prop"), &off);
      ns_switch_quiet (sw, !off);
    }
    return;
  }
  g_free (a->shape);
  a->shape = g_strdup (shape->str);
  g_string_truncate (shape, 0);
  g_ptr_array_set_size (a->switches, 0);
  for (GtkWidget *c; (c = gtk_widget_get_first_child (a->body));)
    gtk_box_remove (GTK_BOX (a->body), c);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (head, "ns-account-head");
  gtk_box_append (GTK_BOX (head), provider_icon (p, 48));
  GtkWidget *who = gtk_label_new (prop_str (ifaces, GOA_ACCOUNT_IFACE, "PresentationIdentity"));
  gtk_label_set_wrap (GTK_LABEL (who), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (who), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_justify (GTK_LABEL (who), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (who, "ns-account-who");
  gtk_box_append (GTK_BOX (head), who);
  gtk_box_append (GTK_BOX (a->body), head);

  if (attention) {
    GtkWidget *g = ns_group (a->body, NULL);
    GtkWidget *r = ns_row (g, "dialog-warning-symbolic", _("Sign in again"),
                           _("The account could not sign in. Its calendars, contacts and files wait until it does."));
    g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (a->path), g_free);
    ns_row_on_activate (r, sign_in_again, NULL);
  }

  GtkWidget *g = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (features); i++) {
    gboolean off;
    if (!(have & features[i].feature) || !prop_bool (ifaces, GOA_ACCOUNT_IFACE, features[i].prop, &off))
      continue;
    if (!g)
      g = ns_group (a->body, _("Use for"));
    GtkWidget *r = ns_row_switch (g, _(features[i].label), NULL, NULL, NULL);
    GtkWidget *sw = ns_row_get_switch (r);
    ns_switch_quiet (sw, !off);
    g_object_set_data_full (G_OBJECT (sw), "ns-path", g_strdup (a->path), g_free);
    g_object_set_data (G_OBJECT (sw), "ns-prop", (gpointer) features[i].prop);
    g_signal_connect (sw, "notify::active", G_CALLBACK (feature_set), NULL);
    g_ptr_array_add (a->switches, sw);
  }

  g = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (servers); i++)
    info (&g, a->body, shape, _(servers[i][2]), prop_str (ifaces, servers[i][0], servers[i][1]));

  g = ns_group (a->body, NULL);
  if (!attention) {
    GtkWidget *r = ns_row_button (g, _("Sign in again"), FALSE, sign_in_again, NULL);
    g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (a->path), g_free);
  }
  GtkWidget *r = ns_row_button (g, _("Remove account"), TRUE, acct_remove, NULL);
  g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (a->path), g_free);
}

static void
acct_got (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *body = data;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  Acct *a = g_object_get_data (G_OBJECT (body), "ns-acct");
  if (a && v) {
    g_autoptr (GVariant) objs = g_variant_get_child_value (v, 0);
    g_autoptr (GVariant) ifaces = g_variant_lookup_value (objs, a->path, G_VARIANT_TYPE ("a{sa{sv}}"));
    if (!ifaces) {
      /* removed (here or elsewhere): the page goes */
      ns_pop (a->page);
    } else {
      g_clear_pointer (&a->ifaces, g_variant_unref);
      a->ifaces = g_variant_ref (ifaces);
      acct_fill (a);
    }
  }
  g_object_unref (body);
}

static void
acct_refresh (NsPage *page)
{
  Acct *a = g_object_get_data (G_OBJECT (page), "ns-acct-page");
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus && a)
    g_dbus_connection_call (bus, GOA_BUS, "/org/gnome/OnlineAccounts", "org.freedesktop.DBus.ObjectManager",
                            "GetManagedObjects", NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
                            5000, NULL, acct_got, g_object_ref (a->body));
}

static void
goa_changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
             GVariant *params, gpointer page)
{
  ns_later (page);
}

static void
unsubscribe (GtkWidget *page, gpointer sub)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_signal_unsubscribe (bus, GPOINTER_TO_UINT (sub));
}

/* every signal of GOA's on `page`: accounts come and go on its object manager, and change on their own paths */
static void
follow_goa (NsPage *page)
{
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (!bus)
    return;
  guint sub = g_dbus_connection_signal_subscribe (bus, GOA_BUS, NULL, NULL, NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                  goa_changed, page, NULL);
  g_signal_connect_data (page, "destroy", G_CALLBACK (unsubscribe), GUINT_TO_POINTER (sub), NULL, 0);
}

static void
account_page (GtkWidget *row, gpointer data)
{
  const char *path = g_object_get_data (G_OBJECT (row), "ns-path");
  GVariant *ifaces = g_object_get_data (G_OBJECT (row), "ns-ifaces");
  const char *provider = prop_str (ifaces, GOA_ACCOUNT_IFACE, "ProviderName");
  Acct *a = g_new0 (Acct, 1);
  a->path = g_strdup (path);
  a->switches = g_ptr_array_new ();
  a->ifaces = ifaces ? g_variant_ref (ifaces) : NULL;
  GtkWidget *c;
  a->page = ns_page_new (provider ? provider : _("Account"), &c);
  a->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (c), a->body);
  g_object_set_data (G_OBJECT (a->body), "ns-acct", a);
  g_object_set_data_full (G_OBJECT (a->page), "ns-acct-page", a, (GDestroyNotify) acct_free);
  acct_fill (a);
  ns_later_init (a->page, acct_refresh);
  follow_goa (a->page);
  ns_push (a->page);
}

/* ---- the list of accounts ---- */

static void
accounts_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  guint n = 0;
  ns_group_clear (group);
  if (v) {
    g_autoptr (GVariant) objs = g_variant_get_child_value (v, 0);
    GVariantIter it;
    const char *path;
    GVariant *ifaces;
    g_variant_iter_init (&it, objs);
    while (g_variant_iter_next (&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
      const char *provider = prop_str (ifaces, GOA_ACCOUNT_IFACE, "ProviderName");
      const char *type = prop_str (ifaces, GOA_ACCOUNT_IFACE, "ProviderType");
      const char *who = prop_str (ifaces, GOA_ACCOUNT_IFACE, "PresentationIdentity");
      gboolean attention = FALSE;
      prop_bool (ifaces, GOA_ACCOUNT_IFACE, "AttentionNeeded", &attention);
      if (!provider) {
        g_variant_unref (ifaces);
        continue;
      }
      GtkWidget *r = ns_row (group, NULL, provider, attention ? _("Needs you to sign in again") : who);
      g_autoptr (GoaProvider) p = type ? goa_provider_get_for_provider_type (type) : NULL;
      vibeos_list_row_set_start_widget (VIBEOS_LIST_ROW (r), provider_icon (p, 32));
      g_object_set_data_full (G_OBJECT (r), "ns-path", g_strdup (path), g_free);
      g_object_set_data_full (G_OBJECT (r), "ns-ifaces", ifaces, (GDestroyNotify) g_variant_unref);
      ns_row_on_activate (r, account_page, NULL);
      n++;
      const char *id = prop_str (ifaces, GOA_ACCOUNT_IFACE, "Id");
      NsPage *page = ns_page_of (group);
      const char *wanted = page ? g_object_get_data (G_OBJECT (page), "ns-show") : NULL;
      if (wanted && !g_strcmp0 (id, wanted) && page == ns_top_page ()) {
        g_object_set_data (G_OBJECT (page), "ns-show", NULL);
        account_page (r, NULL);
      }
    }
  }
  /* asked for once: an account that is not there leaves the list open */
  NsPage *page = ns_page_of (group);
  if (page)
    g_object_set_data (G_OBJECT (page), "ns-show", NULL);
  ns_group_set_visible (group, n > 0);
  gtk_widget_set_visible (g_object_get_data (group, "ns-none"), n == 0);
  g_object_unref (group);
}

void
ns_accounts_show (const char *id)
{
  if (accounts_list && accounts_list == ns_top_page ())
    g_object_set_data_full (G_OBJECT (accounts_list), "ns-show", g_strdup (id), g_free);
}

static void
accounts_refresh (NsPage *page)
{
  GtkWidget *g = g_object_get_data (G_OBJECT (page), "ns-accounts");
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, GOA_BUS, "/org/gnome/OnlineAccounts", "org.freedesktop.DBus.ObjectManager",
                            "GetManagedObjects", NULL, G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
                            5000, NULL, accounts_got, g_object_ref (g));
}

void
ns_page_accounts (GtkWidget *row, gpointer data)
{
  /* GOA's error codes for goa-daemon's D-Bus errors */
  goa_error_quark ();
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Accounts"), &c);
  GtkWidget *g = ns_group (c, _("Online accounts"));
  ns_group_set_visible (g, FALSE);
  g_object_set_data (G_OBJECT (page), "ns-accounts", g);
  GtkWidget *none = ns_note (c, _("No online accounts yet. Add one to get its calendars, contacts and files here."));
  gtk_widget_set_visible (none, FALSE);
  g_object_set_data (G_OBJECT (g), "ns-none", none);
  GtkWidget *a = ns_group (c, NULL);
  ns_row_button (a, _("Add account"), FALSE, add_account, NULL);
  ns_later_init (page, accounts_refresh);
  follow_goa (page);
  if (accounts_list)
    g_object_remove_weak_pointer (G_OBJECT (accounts_list), (gpointer *) &accounts_list);
  accounts_list = page;
  g_object_add_weak_pointer (G_OBJECT (accounts_list), (gpointer *) &accounts_list);
  accounts_refresh (page);
  ns_push (page);
}
