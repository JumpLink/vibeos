/* sw-widgets: see sw-widgets.h */
#include "sw-widgets.h"
#include "sw-account.h"
#include "sw-image.h"
#include "sw-installer.h"
#include "sw-json.h"
#include "sw-updates.h"
#include "sw-window.h"

#include <math.h>
#include <spongy-gtk.h>
#include <string.h>

/* ---- pages ----------------------------------------------------------------------------------------------------- */

static void
on_page_back (VibeosTopBar *bar, GtkWidget *page)
{
  sw_window_pop (page);
}

GtkWidget *
sw_page_new (const char *title)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_top_bar_new (title);

  gtk_widget_add_css_class (page, "sw-page");
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (on_page_back), page);
  gtk_box_append (GTK_BOX (page), bar);
  g_object_set_data (G_OBJECT (page), "sw-bar", bar);
  return page;
}

VibeosTopBar *
sw_page_get_bar (GtkWidget *page)
{
  return g_object_get_data (G_OBJECT (page), "sw-bar");
}

void
sw_page_set_body (GtkWidget *page, GtkWidget *body)
{
  GtkWidget *old = g_object_get_data (G_OBJECT (page), "sw-body");

  if (old)
    gtk_box_remove (GTK_BOX (page), old);
  gtk_widget_set_vexpand (body, TRUE);
  gtk_box_append (GTK_BOX (page), body);
  g_object_set_data (G_OBJECT (page), "sw-body", body);
}

GtkWidget *
sw_scroll_new (GtkWidget *child)
{
  GtkWidget *scroll = spongy_scroll_view_new ();

  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), child);
  gtk_widget_set_vexpand (scroll, TRUE);
  return scroll;
}

void
sw_scroll_to_top (GtkWidget *scroll)
{
  if (SPONGY_IS_SCROLL_VIEW (scroll))
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (scroll), 0);
}

static void
on_destroy_cancel (GtkWidget *widget, GCancellable *cancellable)
{
  g_cancellable_cancel (cancellable);
}

GCancellable *
sw_cancellable_for (GtkWidget *widget)
{
  GCancellable *cancellable = g_object_get_data (G_OBJECT (widget), "sw-cancellable");

  if (!cancellable)
    {
      cancellable = g_cancellable_new ();
      g_object_set_data_full (G_OBJECT (widget), "sw-cancellable", cancellable, g_object_unref);
      g_signal_connect (widget, "destroy", G_CALLBACK (on_destroy_cancel), cancellable);
    }
  return cancellable;
}

GtkWidget *
sw_label_new (const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  if (css_class)
    gtk_widget_add_css_class (label, css_class);
  return label;
}

GtkWidget *
sw_column_new (int spacing)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, spacing);

  gtk_widget_add_css_class (box, "sw-column");
  return box;
}

/* ---- status ---------------------------------------------------------------------------------------------------- */

struct _SwStatus {
  GtkWidget  parent_instance;
  GtkWidget *stack, *content, *message;
};

G_DEFINE_FINAL_TYPE (SwStatus, sw_status, GTK_TYPE_WIDGET)

static void
sw_status_dispose (GObject *object)
{
  SwStatus *self = SW_STATUS (object);

  g_clear_pointer (&self->stack, gtk_widget_unparent);
  G_OBJECT_CLASS (sw_status_parent_class)->dispose (object);
}

static void
sw_status_class_init (SwStatusClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = sw_status_dispose;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "sw-status");
}

static void
sw_status_init (SwStatus *self)
{
  GtkWidget *loading = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *ring = sw_progress_new (40, 4);

  self->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (self->stack), 150);
  gtk_stack_set_hhomogeneous (GTK_STACK (self->stack), FALSE);
  gtk_stack_set_vhomogeneous (GTK_STACK (self->stack), FALSE);
  gtk_widget_set_parent (self->stack, GTK_WIDGET (self));
  gtk_widget_set_valign (ring, GTK_ALIGN_START);
  gtk_widget_set_halign (ring, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (ring, 96);
  gtk_box_append (GTK_BOX (loading), ring);
  gtk_stack_add_named (GTK_STACK (self->stack), loading, "loading");
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

GtkWidget *
sw_status_new (GtkWidget *content)
{
  SwStatus *self = g_object_new (SW_TYPE_STATUS, NULL);

  if (content)
    sw_status_set_content (self, content);
  sw_status_loading (self);
  return GTK_WIDGET (self);
}

void
sw_status_set_content (SwStatus *self, GtkWidget *content)
{
  if (self->content)
    gtk_stack_remove (GTK_STACK (self->stack), self->content);
  self->content = content;
  gtk_stack_add_named (GTK_STACK (self->stack), content, "content");
}

void
sw_status_loading (SwStatus *self)
{
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "loading");
}

void
sw_status_show (SwStatus *self)
{
  if (self->content)
    gtk_stack_set_visible_child (GTK_STACK (self->stack), self->content);
}

gboolean
sw_status_is_showing (SwStatus *self)
{
  return self->content && gtk_stack_get_visible_child (GTK_STACK (self->stack)) == self->content;
}

typedef struct {
  SwRetryFunc func;
  gpointer    data;
} Retry;

static void
on_retry (GtkWidget *button, Retry *retry)
{
  retry->func (retry->data);
}

static void
show_message (SwStatus *self, const char *icon_name, const char *title, const char *text, const char *action,
              SwRetryFunc func, gpointer data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *icon, *label;

  gtk_widget_add_css_class (box, "sw-message");
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  if (icon_name)
    {
      icon = gtk_image_new_from_icon_name (icon_name);
      gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
      gtk_widget_add_css_class (icon, "sw-message-icon");
      gtk_box_append (GTK_BOX (box), icon);
    }
  if (title)
    {
      label = gtk_label_new (title);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
      gtk_widget_add_css_class (label, "sw-message-title");
      gtk_box_append (GTK_BOX (box), label);
    }
  label = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (label, "sw-message-text");
  gtk_box_append (GTK_BOX (box), label);
  if (action && func)
    {
      GtkWidget *button = sw_button_new (action, SW_BUTTON_TONAL);
      Retry *retry = g_new0 (Retry, 1);

      retry->func = func;
      retry->data = data;
      gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (button, "sw-message-button");
      g_signal_connect_data (button, "clicked", G_CALLBACK (on_retry), retry, (GClosureNotify) (void (*) (void)) g_free,
                             0);
      gtk_box_append (GTK_BOX (box), button);
    }
  if (self->message)
    gtk_stack_remove (GTK_STACK (self->stack), self->message);
  self->message = box;
  gtk_stack_add_named (GTK_STACK (self->stack), box, NULL);
  gtk_stack_set_visible_child (GTK_STACK (self->stack), box);
}

void
sw_status_error (SwStatus *self, const SwApiError *error, SwRetryFunc retry, gpointer data)
{
  g_autofree char *text = sw_api_error_describe (error);
  gboolean offline = error && error->kind == SW_API_ERROR_NETWORK;

  show_message (self, offline ? "sw-cloud-off-symbolic" : "sw-warning-symbolic",
                offline ? "No connection" : "Something went wrong", text, "Retry", retry, data);
}

void
sw_status_empty (SwStatus *self, const char *icon_name, const char *text, const char *action, SwRetryFunc retry,
                 gpointer data)
{
  show_message (self, icon_name, NULL, text, action, retry, data);
}

/* ---- buttons --------------------------------------------------------------------------------------------------- */

static const char *const button_classes[] = { "filled", "tonal", "outlined", "text", "danger" };

GtkWidget *
sw_button_new (const char *label, SwButtonStyle style)
{
  GtkWidget *text = gtk_label_new (label), *button = vibeos_pressable_new (text);

  gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (button, "sw-button");
  gtk_widget_add_css_class (button, button_classes[style]);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  return button;
}

void
sw_button_set_label (GtkWidget *button, const char *label)
{
  GtkWidget *text = vibeos_pressable_get_child (VIBEOS_PRESSABLE (button));

  gtk_label_set_text (GTK_LABEL (text), label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

void
sw_button_set_style (GtkWidget *button, SwButtonStyle style)
{
  for (guint i = 0; i < G_N_ELEMENTS (button_classes); i++)
    gtk_widget_remove_css_class (button, button_classes[i]);
  gtk_widget_add_css_class (button, button_classes[style]);
}

GtkWidget *
sw_icon_button_new (const char *icon_name, const char *tooltip)
{
  GtkWidget *button = vibeos_icon_button_new (icon_name);

  if (tooltip)
    gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
  return button;
}

/* ---- the install button ---------------------------------------------------------------------------------------- */

SwAppState
sw_app_state (const char *package)
{
  SwInstaller *installer = sw_installer_get_default ();

  if (!package)
    return SW_APP_NOT_INSTALLED;
  if (sw_installer_get_job (installer, package))
    return SW_APP_BUSY;
  if (!sw_installer_get_installed (installer, package))
    return SW_APP_NOT_INSTALLED;
  return sw_updates_get (sw_updates_get_default (), package) ? SW_APP_UPDATABLE : SW_APP_INSTALLED;
}

const char *
sw_app_state_action (SwAppState state)
{
  switch (state)
    {
    case SW_APP_INSTALLED:
      return "Open";
    case SW_APP_UPDATABLE:
      return "Update";
    default:
      return "Install";
    }
}

typedef struct {
  GtkWidget *from;
  char      *name;
} Launch;

static void
on_launch_found (GAppInfo *info, gpointer data)
{
  Launch *launch = data;

  if (!info || !sw_installer_launch (info, launch->from))
    {
      g_autofree char *text = g_strdup_printf ("%s has no window of its own to open.", launch->name);

      sw_window_toast (launch->from, text);
    }
  g_object_unref (launch->from);
  g_free (launch->name);
  g_free (launch);
}

void
sw_app_activate (GtkWidget *from, JsonObject *app)
{
  const char *package = sw_json_string (app, "package"), *name = sw_json_string (app, "name");
  SwAppState state = sw_app_state (package);

  if (state == SW_APP_INSTALLED)
    {
      Launch *launch = g_new0 (Launch, 1);

      launch->from = g_object_ref (from);
      launch->name = g_strdup (name ? name : package);
      sw_installer_find_app (sw_installer_get_default (), sw_json_string (app, "id"), package, NULL,
                             on_launch_found, launch);
    }
  else if (state != SW_APP_BUSY && package)
    sw_installer_install (sw_installer_get_default (), package, name, from);
}

typedef struct {
  GtkWidget  *stack, *button, *ring, *ring_box;
  JsonObject *app;
} InstallButton;

static void
install_button_free (gpointer data)
{
  InstallButton *ib = data;

  json_object_unref (ib->app);
  g_free (ib);
}

static void
install_button_update (InstallButton *ib)
{
  const char *package = sw_json_string (ib->app, "package");
  SwAppState state = sw_app_state (package);

  if (state == SW_APP_BUSY)
    {
      const SwJob *job = sw_installer_get_job (sw_installer_get_default (), package);
      g_autofree char *text = sw_job_describe (job);

      sw_progress_set_percent (SW_PROGRESS (ib->ring), job->state == SW_JOB_RUNNING ? job->percent : -1);
      gtk_widget_set_sensitive (ib->ring_box, sw_job_can_cancel (job));
      gtk_accessible_update_property (GTK_ACCESSIBLE (ib->ring_box), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
      gtk_stack_set_visible_child (GTK_STACK (ib->stack), ib->ring_box);
      return;
    }
  sw_button_set_label (ib->button, sw_app_state_action (state));
  gtk_stack_set_visible_child (GTK_STACK (ib->stack), ib->button);
}

static void
on_install_changed (SwInstaller *installer, const char *package, GtkWidget *stack)
{
  InstallButton *ib = g_object_get_data (G_OBJECT (stack), "sw-install");

  if (!package || !g_strcmp0 (package, sw_json_string (ib->app, "package")))
    install_button_update (ib);
}

static void
on_updates_changed (SwUpdates *updates, GtkWidget *stack)
{
  install_button_update (g_object_get_data (G_OBJECT (stack), "sw-install"));
}

static void
on_install_clicked (GtkWidget *button, InstallButton *ib)
{
  sw_app_activate (button, ib->app);
}

static void
on_ring_clicked (GtkWidget *ring, InstallButton *ib)
{
  sw_installer_cancel (sw_installer_get_default (), sw_json_string (ib->app, "package"));
}

GtkWidget *
sw_install_button_new (JsonObject *app)
{
  InstallButton *ib = g_new0 (InstallButton, 1);
  GtkWidget *overlay = gtk_overlay_new (), *stop = gtk_image_new_from_icon_name ("sw-close-symbolic");

  ib->app = json_object_ref (app);
  ib->stack = gtk_stack_new ();
  gtk_stack_set_hhomogeneous (GTK_STACK (ib->stack), FALSE);
  gtk_stack_set_interpolate_size (GTK_STACK (ib->stack), FALSE);
  gtk_widget_set_valign (ib->stack, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (ib->stack, GTK_ALIGN_END);
  gtk_widget_add_css_class (ib->stack, "sw-install");
  ib->button = sw_button_new ("Install", SW_BUTTON_TONAL);
  gtk_widget_add_css_class (ib->button, "small");
  g_signal_connect (ib->button, "clicked", G_CALLBACK (on_install_clicked), ib);
  gtk_stack_add_child (GTK_STACK (ib->stack), ib->button);

  /* while a job runs: its ring, and a cross to stop it */
  ib->ring = sw_progress_new (36, 3);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), ib->ring);
  gtk_image_set_pixel_size (GTK_IMAGE (stop), 16);
  gtk_widget_set_halign (stop, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (stop, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (stop, "sw-ring-stop");
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), stop);
  ib->ring_box = vibeos_pressable_new (overlay);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (ib->ring_box), VIBEOS_RIPPLE_CIRCLE, 24);
  gtk_widget_add_css_class (ib->ring_box, "sw-ring-button");
  g_signal_connect (ib->ring_box, "clicked", G_CALLBACK (on_ring_clicked), ib);
  gtk_stack_add_child (GTK_STACK (ib->stack), ib->ring_box);

  g_object_set_data_full (G_OBJECT (ib->stack), "sw-install", ib, install_button_free);
  g_signal_connect_object (sw_installer_get_default (), "changed", G_CALLBACK (on_install_changed), ib->stack, 0);
  g_signal_connect_object (sw_updates_get_default (), "changed", G_CALLBACK (on_updates_changed), ib->stack, 0);
  install_button_update (ib);
  return ib->stack;
}

/* ---- app rows and tiles ---------------------------------------------------------------------------------------- */

static GHashTable *app_cache;

JsonObject *
sw_app_cache_get (const char *package)
{
  return app_cache && package ? g_hash_table_lookup (app_cache, package) : NULL;
}

void
sw_app_cache_put (JsonObject *app)
{
  const char *package = sw_json_string (app, "package");

  if (!package || !sw_json_string (app, "id"))
    return;
  if (!app_cache)
    app_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) json_object_unref);
  g_hash_table_replace (app_cache, g_strdup (package), json_object_ref (app));
}

static void
on_app_clicked (GtkWidget *pressable, gpointer data)
{
  JsonObject *app = g_object_get_data (G_OBJECT (pressable), "sw-app");

  sw_window_open_app (pressable, sw_json_string (app, "id"), app);
}

static GtkWidget *
app_icon (JsonObject *app, int size)
{
  GtkWidget *icon = sw_image_new (size, size, size * 0.22);

  sw_image_set_fallback (SW_IMAGE (icon), "sw-package-symbolic");
  sw_image_set_url (SW_IMAGE (icon), sw_app_icon_url (app, size));
  gtk_widget_set_valign (icon, GTK_ALIGN_START);
  return icon;
}

GtkWidget *
sw_rating_label_new (JsonObject *app)
{
  double rating = sw_json_double (app, "rating");
  JsonObject *detail = sw_json_object (app, "rating");
  GtkWidget *box, *star, *label;
  g_autofree char *text = NULL;

  if (detail)
    rating = sw_json_double (detail, "average");
  if (isnan (rating))
    return NULL;
  text = sw_format_rating (rating);
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (box, "sw-rating");
  star = gtk_image_new_from_icon_name ("sw-star-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (star), 14);
  label = gtk_label_new (text);
  gtk_box_append (GTK_BOX (box), star);
  gtk_box_append (GTK_BOX (box), label);
  return box;
}

GtkWidget *
sw_app_row_new (JsonObject *app, const char *subtitle)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16), *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *name = gtk_label_new (sw_json_string (app, "name")), *summary, *rating, *row;
  const char *line = subtitle ? subtitle : sw_json_string (app, "summary");

  sw_app_cache_put (app);
  gtk_widget_add_css_class (box, "sw-app-row-box");
  gtk_box_append (GTK_BOX (box), app_icon (app, 56));

  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (name), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (GTK_LABEL (name), 2);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_width_chars (GTK_LABEL (name), 1);
  gtk_widget_add_css_class (name, "sw-app-name");
  gtk_box_append (GTK_BOX (text), name);
  if (line && *line)
    {
      summary = gtk_label_new (line);
      gtk_label_set_xalign (GTK_LABEL (summary), 0);
      gtk_label_set_ellipsize (GTK_LABEL (summary), PANGO_ELLIPSIZE_END);
      gtk_label_set_width_chars (GTK_LABEL (summary), 1);
      gtk_widget_add_css_class (summary, "sw-app-summary");
      gtk_box_append (GTK_BOX (text), summary);
    }
  if ((rating = sw_rating_label_new (app)))
    gtk_box_append (GTK_BOX (text), rating);
  gtk_box_append (GTK_BOX (box), text);
  if (sw_json_string (app, "package"))
    gtk_box_append (GTK_BOX (box), sw_install_button_new (app));

  row = vibeos_pressable_new (box);
  gtk_widget_add_css_class (row, "sw-app-row");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  g_object_set_data_full (G_OBJECT (row), "sw-app", json_object_ref (app), (GDestroyNotify) json_object_unref);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, sw_json_string (app, "name"),
                                  -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_app_clicked), NULL);
  return row;
}

GtkWidget *
sw_app_tile_new (JsonObject *app, int width)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6), *name = gtk_label_new (sw_json_string (app, "name"));
  GtkWidget *rating, *tile;

  sw_app_cache_put (app);
  gtk_widget_add_css_class (box, "sw-app-tile-box");
  gtk_box_append (GTK_BOX (box), app_icon (app, width));
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (name), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (GTK_LABEL (name), 2);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 1);
  gtk_widget_set_size_request (name, width, -1);
  gtk_widget_add_css_class (name, "sw-tile-name");
  gtk_box_append (GTK_BOX (box), name);
  if ((rating = sw_rating_label_new (app)))
    gtk_box_append (GTK_BOX (box), rating);

  tile = vibeos_pressable_new (box);
  gtk_widget_add_css_class (tile, "sw-app-tile");
  gtk_widget_set_valign (tile, GTK_ALIGN_START);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (tile), VIBEOS_RIPPLE_BOUNDED, 16);
  g_object_set_data_full (G_OBJECT (tile), "sw-app", json_object_ref (app), (GDestroyNotify) json_object_unref);
  gtk_accessible_update_property (GTK_ACCESSIBLE (tile), GTK_ACCESSIBLE_PROPERTY_LABEL, sw_json_string (app, "name"),
                                  -1);
  g_signal_connect (tile, "clicked", G_CALLBACK (on_app_clicked), NULL);
  return tile;
}

GtkWidget *
sw_strip_new (GtkWidget **box)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();

  *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (*box, "sw-strip");
  /* sideways only, and no scrollbar: the next tile peeking in says there is more */
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), *box);
  return scroll;
}

GtkWidget *
sw_section_header_new (const char *title, const char *count, const char *action, GCallback callback, gpointer data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8), *label = gtk_label_new (title);

  gtk_widget_add_css_class (box, "sw-section-header");
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (label, "sw-section-title");
  gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LEVEL, 2, -1);
  gtk_box_append (GTK_BOX (box), label);
  if (count)
    {
      GtkWidget *pill = gtk_label_new (count);

      gtk_widget_add_css_class (pill, "sw-section-count");
      gtk_widget_set_valign (pill, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), pill);
    }
  if (action)
    {
      GtkWidget *button = sw_button_new (action, SW_BUTTON_TEXT);

      gtk_widget_set_hexpand (button, TRUE);
      gtk_widget_set_halign (button, GTK_ALIGN_END);
      g_signal_connect (button, "clicked", callback, data);
      gtk_box_append (GTK_BOX (box), button);
    }
  return box;
}

/* ---- chips ----------------------------------------------------------------------------------------------------- */

GtkWidget *
sw_category_icon_new (JsonObject *category, int size)
{
  const char *icon = sw_json_string (category, "icon"), *id = sw_json_string (category, "id");
  g_autofree char *name = g_strdup_printf ("sw-fa-%s-symbolic", icon ? icon : "shapes");
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  GtkWidget *image;
  char hue[16];

  if (!gtk_icon_theme_has_icon (theme, name))
    {
      g_free (name);
      name = g_strdup ("sw-fa-shapes-symbolic");
    }
  image = gtk_image_new_from_icon_name (name);
  gtk_image_set_pixel_size (GTK_IMAGE (image), size);
  /* one of four of the theme's colours, the same for a category every time */
  g_snprintf (hue, sizeof hue, "hue-%u", (id ? g_str_hash (id) : 0) % 4);
  gtk_widget_add_css_class (image, "sw-category-icon");
  gtk_widget_add_css_class (image, hue);
  return image;
}

static void
on_category_clicked (GtkWidget *chip, gpointer data)
{
  JsonObject *category = g_object_get_data (G_OBJECT (chip), "sw-category");
  g_autofree char *id = sw_api_escape (sw_json_string (category, "id"));
  g_autofree char *path = g_strdup_printf ("/apps?category=%s", id);

  sw_window_open_list (chip, sw_json_string (category, "name"), path);
}

GtkWidget *
sw_category_chip_new (JsonObject *category)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8), *label, *chip;

  gtk_box_append (GTK_BOX (box), sw_category_icon_new (category, 16));
  label = gtk_label_new (sw_json_string (category, "name"));
  gtk_box_append (GTK_BOX (box), label);
  chip = vibeos_pressable_new (box);
  gtk_widget_add_css_class (chip, "sw-chip");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (chip), VIBEOS_RIPPLE_BOUNDED, 18);
  g_object_set_data_full (G_OBJECT (chip), "sw-category", json_object_ref (category),
                          (GDestroyNotify) json_object_unref);
  gtk_accessible_update_property (GTK_ACCESSIBLE (chip), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  sw_json_string (category, "name"), -1);
  g_signal_connect (chip, "clicked", G_CALLBACK (on_category_clicked), NULL);
  return chip;
}

GtkWidget *
sw_filter_chip_new (const char *label)
{
  GtkWidget *chip = vibeos_pressable_new (gtk_label_new (label));

  gtk_widget_add_css_class (chip, "sw-chip");
  gtk_widget_add_css_class (chip, "filter");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (chip), VIBEOS_RIPPLE_BOUNDED, 8);
  return chip;
}

void
sw_filter_chip_set_selected (GtkWidget *chip, gboolean selected)
{
  if (selected)
    gtk_widget_add_css_class (chip, "selected");
  else
    gtk_widget_remove_css_class (chip, "selected");
  gtk_accessible_update_state (GTK_ACCESSIBLE (chip), GTK_ACCESSIBLE_STATE_SELECTED, selected, -1);
}

/* ---- stars ----------------------------------------------------------------------------------------------------- */

struct _SwStars {
  GtkWidget parent_instance;
  double    rating;
  int       size, gap;
  gboolean  tappable;
};

G_DEFINE_FINAL_TYPE (SwStars, sw_stars, GTK_TYPE_WIDGET)

static guint stars_changed;

static void
star_path (cairo_t *cr, double cx, double cy, double r)
{
  /* a five-pointed star, its inner points at 0.45 of its outer radius (Material's star) */
  for (int i = 0; i < 10; i++)
    {
      double a = -G_PI / 2 + i * G_PI / 5, rr = i % 2 ? r * 0.45 : r;

      if (i == 0)
        cairo_move_to (cr, cx + rr * cos (a), cy + rr * sin (a));
      else
        cairo_line_to (cr, cx + rr * cos (a), cy + rr * sin (a));
    }
  cairo_close_path (cr);
}

static void
sw_stars_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SwStars *self = SW_STARS (widget);
  int w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &GRAPHENE_RECT_INIT (0, 0, w, h));
  GdkRGBA on = vibeos_color (VIBEOS_PRIMARY), off = vibeos_color (VIBEOS_OUTLINE_VARIANT);
  double r = self->size / 2.0;

  for (int i = 0; i < 5; i++)
    {
      double cx = i * (self->size + self->gap) + r, cy = h / 2.0 + r * 0.06;
      double fill = CLAMP (self->rating - i, 0, 1);

      star_path (cr, cx, cy, r);
      gdk_cairo_set_source_rgba (cr, &off);
      cairo_fill (cr);
      if (fill > 0)
        {
          /* a half star for x.5 and up */
          double part = fill >= 0.75 ? 1 : fill >= 0.25 ? 0.5 : 0;

          if (part > 0)
            {
              cairo_save (cr);
              cairo_rectangle (cr, cx - r, 0, 2 * r * part, h);
              cairo_clip (cr);
              star_path (cr, cx, cy, r);
              gdk_cairo_set_source_rgba (cr, &on);
              cairo_fill (cr);
              cairo_restore (cr);
            }
        }
    }
  cairo_destroy (cr);
}

static void
sw_stars_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *mb, int *nb)
{
  SwStars *self = SW_STARS (widget);

  *min = *nat = orientation == GTK_ORIENTATION_HORIZONTAL ? 5 * self->size + 4 * self->gap : self->size;
}

static void
on_stars_pressed (GtkGestureClick *click, int n, double x, double y, SwStars *self)
{
  int star = CLAMP ((int) (x / (self->size + self->gap)) + 1, 1, 5);

  sw_stars_set_rating (self, star);
  g_signal_emit (self, stars_changed, 0);
}

static void
sw_stars_class_init (SwStarsClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = sw_stars_snapshot;
  GTK_WIDGET_CLASS (klass)->measure = sw_stars_measure;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "sw-stars");
  stars_changed = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                G_TYPE_NONE, 0);
}

static void
sw_stars_init (SwStars *self)
{
}

GtkWidget *
sw_stars_new (double rating, int size, int gap, gboolean tappable)
{
  SwStars *self = g_object_new (SW_TYPE_STARS, NULL);

  self->rating = rating;
  self->size = size;
  self->gap = gap;
  self->tappable = tappable;
  if (tappable)
    {
      GtkGesture *click = gtk_gesture_click_new ();

      g_signal_connect (click, "pressed", G_CALLBACK (on_stars_pressed), self);
      gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
      gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, "Your rating", -1);
      /* a press here is the stars', not a row's around them */
      g_object_set_data (G_OBJECT (self), "vibeos-tappable", GINT_TO_POINTER (1));
    }
  return GTK_WIDGET (self);
}

void
sw_stars_set_rating (SwStars *self, double rating)
{
  self->rating = rating;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
sw_stars_get_rating (SwStars *self)
{
  return self->rating;
}

/* ---- progress -------------------------------------------------------------------------------------------------- */

struct _SwProgress {
  GtkWidget parent_instance;
  int       size, percent;
  double    width;
  gboolean  bar;
  guint     tick;
  gint64    start;
};

G_DEFINE_FINAL_TYPE (SwProgress, sw_progress, GTK_TYPE_WIDGET)

static gboolean
on_progress_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  gtk_widget_queue_draw (widget);
  return G_SOURCE_CONTINUE;
}

static void
progress_update_tick (SwProgress *self)
{
  gboolean spin = self->percent < 0 && gtk_widget_get_mapped (GTK_WIDGET (self));

  if (spin && !self->tick)
    {
      self->start = g_get_monotonic_time ();
      self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_progress_tick, NULL, NULL);
    }
  else if (!spin && self->tick)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
      self->tick = 0;
    }
}

static void
sw_progress_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (sw_progress_parent_class)->map (widget);
  progress_update_tick (SW_PROGRESS (widget));
}

static void
sw_progress_unmap (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (sw_progress_parent_class)->unmap (widget);
  progress_update_tick (SW_PROGRESS (widget));
}

static void
sw_progress_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SwProgress *self = SW_PROGRESS (widget);
  int w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &GRAPHENE_RECT_INIT (0, 0, w, h));
  GdkRGBA track = vibeos_color (VIBEOS_SECONDARY_CONTAINER), fill = vibeos_color (VIBEOS_PRIMARY);
  double t = (g_get_monotonic_time () - self->start) / (double) G_USEC_PER_SEC;

  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  if (self->bar)
    {
      double y = h / 2.0, from, to;

      cairo_set_line_width (cr, h);
      gdk_cairo_set_source_rgba (cr, &track);
      cairo_move_to (cr, h / 2.0, y);
      cairo_line_to (cr, w - h / 2.0, y);
      cairo_stroke (cr);
      if (self->percent >= 0)
        from = 0, to = self->percent / 100.0;
      else
        {
          /* a segment sweeping across, once a second and a half */
          double phase = fmod (t / 1.5, 1.0);

          from = MAX (0, phase * 1.4 - 0.4);
          to = MIN (1, phase * 1.4);
        }
      if (to > from)
        {
          gdk_cairo_set_source_rgba (cr, &fill);
          cairo_move_to (cr, h / 2.0 + from * (w - h), y);
          cairo_line_to (cr, h / 2.0 + to * (w - h), y);
          cairo_stroke (cr);
        }
    }
  else
    {
      double cx = w / 2.0, cy = h / 2.0, r = (MIN (w, h) - self->width) / 2.0;

      cairo_set_line_width (cr, self->width);
      if (self->percent >= 0)
        {
          gdk_cairo_set_source_rgba (cr, &track);
          cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
          cairo_stroke (cr);
          gdk_cairo_set_source_rgba (cr, &fill);
          if (self->percent > 0)
            {
              cairo_arc (cr, cx, cy, r, -G_PI / 2, -G_PI / 2 + 2 * G_PI * self->percent / 100.0);
              cairo_stroke (cr);
            }
        }
      else
        {
          /* an arc turning once a second, growing and shrinking as Material's indeterminate ring does */
          double start = t * 2 * G_PI, sweep = G_PI * (0.25 + 0.6 * (1 + sin (t * G_PI)) / 2);

          gdk_cairo_set_source_rgba (cr, &fill);
          cairo_arc (cr, cx, cy, r, start, start + sweep);
          cairo_stroke (cr);
        }
    }
  cairo_destroy (cr);
}

static void
sw_progress_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *mb,
                     int *nb)
{
  SwProgress *self = SW_PROGRESS (widget);

  if (self->bar)
    *min = *nat = orientation == GTK_ORIENTATION_VERTICAL ? self->size : 0;
  else
    *min = *nat = self->size;
}

static void
sw_progress_class_init (SwProgressClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->map = sw_progress_map;
  widget_class->unmap = sw_progress_unmap;
  widget_class->snapshot = sw_progress_snapshot;
  widget_class->measure = sw_progress_measure;
  gtk_widget_class_set_css_name (widget_class, "sw-progress");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_PROGRESS_BAR);
}

static void
sw_progress_init (SwProgress *self)
{
  self->percent = -1;
}

GtkWidget *
sw_progress_new (int size, double width)
{
  SwProgress *self = g_object_new (SW_TYPE_PROGRESS, NULL);

  self->size = size;
  self->width = width;
  return GTK_WIDGET (self);
}

GtkWidget *
sw_progress_bar_new (int height)
{
  SwProgress *self = g_object_new (SW_TYPE_PROGRESS, NULL);

  self->size = height;
  self->bar = TRUE;
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  return GTK_WIDGET (self);
}

void
sw_progress_set_percent (SwProgress *self, int percent)
{
  self->percent = CLAMP (percent, -1, 100);
  if (percent >= 0)
    gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, (double) percent,
                                    GTK_ACCESSIBLE_PROPERTY_VALUE_MAX, 100.0, -1);
  progress_update_tick (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- tabs ------------------------------------------------------------------------------------------------------ */

struct _SwTabs {
  GtkWidget  parent_instance;
  GtkWidget *box;
  GPtrArray *tabs;
  int        selected;
};

G_DEFINE_FINAL_TYPE (SwTabs, sw_tabs, GTK_TYPE_WIDGET)

static guint tabs_changed;

static void
sw_tabs_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SwTabs *self = SW_TABS (widget);
  int w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  GdkRGBA line = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST), on = vibeos_color (VIBEOS_ON_SURFACE);
  GtkWidget *tab = self->selected >= 0 && self->selected < (int) self->tabs->len
                   ? g_ptr_array_index (self->tabs, self->selected) : NULL;

  GTK_WIDGET_CLASS (sw_tabs_parent_class)->snapshot (widget, snapshot);
  gtk_snapshot_append_color (snapshot, &line, &GRAPHENE_RECT_INIT (0, h - 1, w, 1));
  if (tab)
    {
      GtkWidget *label = vibeos_pressable_get_child (VIBEOS_PRESSABLE (tab));
      graphene_rect_t b;
      GskRoundedRect r;

      /* under the label's text, as wide as it plus a little */
      if (gtk_widget_compute_bounds (label, widget, &b))
        {
          gsk_rounded_rect_init (&r, &GRAPHENE_RECT_INIT (b.origin.x - 8, h - 3, b.size.width + 16, 3),
                                 &GRAPHENE_SIZE_INIT (3, 3), &GRAPHENE_SIZE_INIT (3, 3), &GRAPHENE_SIZE_INIT (0, 0),
                                 &GRAPHENE_SIZE_INIT (0, 0));
          gtk_snapshot_push_rounded_clip (snapshot, &r);
          gtk_snapshot_append_color (snapshot, &on, &r.bounds);
          gtk_snapshot_pop (snapshot);
        }
    }
}

static void
sw_tabs_dispose (GObject *object)
{
  SwTabs *self = SW_TABS (object);

  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->tabs, g_ptr_array_unref);
  G_OBJECT_CLASS (sw_tabs_parent_class)->dispose (object);
}

static void
sw_tabs_class_init (SwTabsClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = sw_tabs_dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = sw_tabs_snapshot;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "sw-tabs");
  gtk_widget_class_set_accessible_role (GTK_WIDGET_CLASS (klass), GTK_ACCESSIBLE_ROLE_TAB_LIST);
  tabs_changed = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                               G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
sw_tabs_init (SwTabs *self)
{
  self->tabs = g_ptr_array_new ();
  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (self->box), TRUE);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
}

static void
on_tab_clicked (GtkWidget *tab, SwTabs *self)
{
  guint index;

  if (g_ptr_array_find (self->tabs, tab, &index) && (int) index != self->selected)
    {
      sw_tabs_set_selected (self, index);
      g_signal_emit (self, tabs_changed, 0, (int) index);
    }
}

GtkWidget *
sw_tabs_new (const char *first, ...)
{
  SwTabs *self = g_object_new (SW_TYPE_TABS, NULL);
  va_list args;

  va_start (args, first);
  for (const char *text = first; text; text = va_arg (args, const char *))
    {
      GtkWidget *label = gtk_label_new (text), *tab;

      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_halign (label, GTK_ALIGN_CENTER);
      tab = vibeos_pressable_new (label);
      gtk_widget_add_css_class (tab, "sw-tab");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (tab), VIBEOS_RIPPLE_BOUNDED, 0);
      gtk_accessible_update_property (GTK_ACCESSIBLE (tab), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
      g_signal_connect (tab, "clicked", G_CALLBACK (on_tab_clicked), self);
      g_ptr_array_add (self->tabs, tab);
      gtk_box_append (GTK_BOX (self->box), tab);
    }
  va_end (args);
  sw_tabs_set_selected (self, 0);
  return GTK_WIDGET (self);
}

void
sw_tabs_set_selected (SwTabs *self, int index)
{
  self->selected = index;
  for (guint i = 0; i < self->tabs->len; i++)
    {
      GtkWidget *tab = g_ptr_array_index (self->tabs, i);

      if ((int) i == index)
        gtk_widget_add_css_class (tab, "selected");
      else
        gtk_widget_remove_css_class (tab, "selected");
      gtk_accessible_update_state (GTK_ACCESSIBLE (tab), GTK_ACCESSIBLE_STATE_SELECTED, (int) i == index, -1);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

int
sw_tabs_get_selected (SwTabs *self)
{
  return self->selected;
}

/* ---- fields ---------------------------------------------------------------------------------------------------- */

static void
on_eye_clicked (GtkWidget *eye, GtkWidget *text)
{
  gboolean shown = !gtk_text_get_visibility (GTK_TEXT (text));
  GtkWidget *image = vibeos_pressable_get_child (VIBEOS_PRESSABLE (eye));

  gtk_text_set_visibility (GTK_TEXT (text), shown);
  gtk_image_set_from_icon_name (GTK_IMAGE (image), shown ? "sw-visibility-off-symbolic" : "sw-visibility-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (eye), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  shown ? "Hide the password" : "Show the password", -1);
}

static void
on_field_pressed (GtkGestureClick *click, int n, double x, double y, GtkWidget *text)
{
  gtk_widget_grab_focus (text);
}

static void
on_field_changed (GtkEditable *text, GtkWidget *field)
{
  /* typing again clears the error */
  sw_field_set_error (field, NULL);
}

GtkWidget *
sw_field_new (const char *label, SwFieldKind kind)
{
  GtkWidget *field = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4), *pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *title = gtk_label_new (label), *text = gtk_text_new (), *error = gtk_label_new (NULL);
  GtkWidget *hint = gtk_label_new (NULL);
  GtkGesture *click = gtk_gesture_click_new ();

  gtk_widget_add_css_class (field, "sw-field");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_add_css_class (title, "sw-field-label");
  gtk_box_append (GTK_BOX (field), title);

  gtk_widget_add_css_class (pill, "sw-field-box");
  gtk_widget_set_hexpand (text, TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (text), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  switch (kind)
    {
    case SW_FIELD_EMAIL:
      gtk_text_set_input_purpose (GTK_TEXT (text), GTK_INPUT_PURPOSE_EMAIL);
      gtk_text_set_input_hints (GTK_TEXT (text), GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_LOWERCASE);
      gtk_text_set_max_length (GTK_TEXT (text), 254);
      break;
    case SW_FIELD_NAME:
      gtk_text_set_input_purpose (GTK_TEXT (text), GTK_INPUT_PURPOSE_NAME);
      gtk_text_set_input_hints (GTK_TEXT (text), GTK_INPUT_HINT_UPPERCASE_WORDS | GTK_INPUT_HINT_NO_SPELLCHECK);
      gtk_text_set_max_length (GTK_TEXT (text), 50);
      break;
    case SW_FIELD_PASSWORD:
    case SW_FIELD_NEW_PASSWORD:
      gtk_text_set_input_purpose (GTK_TEXT (text), GTK_INPUT_PURPOSE_PASSWORD);
      gtk_text_set_input_hints (GTK_TEXT (text), GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_PRIVATE);
      gtk_text_set_visibility (GTK_TEXT (text), FALSE);
      gtk_text_set_max_length (GTK_TEXT (text), 128);
      break;
    default:
      break;
    }
  gtk_box_append (GTK_BOX (pill), text);
  if (kind == SW_FIELD_PASSWORD || kind == SW_FIELD_NEW_PASSWORD)
    {
      GtkWidget *eye = sw_icon_button_new ("sw-visibility-symbolic", "Show the password");

      gtk_widget_add_css_class (eye, "sw-field-eye");
      g_signal_connect (eye, "clicked", G_CALLBACK (on_eye_clicked), text);
      gtk_box_append (GTK_BOX (pill), eye);
    }
  g_signal_connect (click, "pressed", G_CALLBACK (on_field_pressed), text);
  gtk_widget_add_controller (pill, GTK_EVENT_CONTROLLER (click));
  gtk_box_append (GTK_BOX (field), pill);

  gtk_label_set_xalign (GTK_LABEL (error), 0);
  gtk_label_set_wrap (GTK_LABEL (error), TRUE);
  gtk_widget_add_css_class (error, "sw-field-error");
  gtk_widget_set_visible (error, FALSE);
  gtk_box_append (GTK_BOX (field), error);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_widget_add_css_class (hint, "sw-field-hint");
  gtk_widget_set_visible (hint, FALSE);
  gtk_box_append (GTK_BOX (field), hint);

  g_object_set_data (G_OBJECT (field), "sw-text", text);
  g_object_set_data (G_OBJECT (field), "sw-pill", pill);
  g_object_set_data (G_OBJECT (field), "sw-error", error);
  g_object_set_data (G_OBJECT (field), "sw-hint", hint);
  g_signal_connect (text, "changed", G_CALLBACK (on_field_changed), field);
  return field;
}

GtkWidget *
sw_field_get_text_widget (GtkWidget *field)
{
  return g_object_get_data (G_OBJECT (field), "sw-text");
}

const char *
sw_field_get_text (GtkWidget *field)
{
  return gtk_editable_get_text (GTK_EDITABLE (sw_field_get_text_widget (field)));
}

void
sw_field_set_text (GtkWidget *field, const char *text)
{
  gtk_editable_set_text (GTK_EDITABLE (sw_field_get_text_widget (field)), text ? text : "");
}

static void
update_hint (GtkWidget *field)
{
  GtkWidget *error = g_object_get_data (G_OBJECT (field), "sw-error");
  GtkWidget *hint = g_object_get_data (G_OBJECT (field), "sw-hint");
  const char *text = gtk_label_get_text (GTK_LABEL (hint));

  gtk_widget_set_visible (hint, !gtk_widget_get_visible (error) && text && *text);
}

void
sw_field_set_error (GtkWidget *field, const char *message)
{
  GtkWidget *error = g_object_get_data (G_OBJECT (field), "sw-error");
  GtkWidget *pill = g_object_get_data (G_OBJECT (field), "sw-pill");
  GtkWidget *text = sw_field_get_text_widget (field);

  gtk_label_set_text (GTK_LABEL (error), message);
  gtk_widget_set_visible (error, message != NULL);
  if (message)
    gtk_widget_add_css_class (pill, "error");
  else
    gtk_widget_remove_css_class (pill, "error");
  gtk_accessible_update_state (GTK_ACCESSIBLE (text), GTK_ACCESSIBLE_STATE_INVALID,
                               message ? GTK_ACCESSIBLE_INVALID_TRUE : GTK_ACCESSIBLE_INVALID_FALSE, -1);
  update_hint (field);
}

void
sw_field_set_hint (GtkWidget *field, const char *hint)
{
  gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (field), "sw-hint")), hint);
  update_hint (field);
}

void
sw_field_grab_focus (GtkWidget *field)
{
  gtk_widget_grab_focus (sw_field_get_text_widget (field));
}

gboolean
sw_fields_show_errors (const SwApiError *error, ...)
{
  va_list args;
  gboolean shown = FALSE;
  const char *name;

  va_start (args, error);
  while ((name = va_arg (args, const char *)))
    {
      GtkWidget *field = va_arg (args, GtkWidget *);
      const char *message = sw_api_error_field (error, name);

      if (message)
        {
          sw_field_set_error (field, message);
          shown = TRUE;
        }
    }
  va_end (args);
  return shown;
}

/* ---- the code field -------------------------------------------------------------------------------------------- */

static guint code_complete, code_changed;

typedef struct {
  GtkWidget  parent_instance;
  GtkWidget *overlay, *text;
  GPtrArray *boxes;
  int        length;
  gboolean   setting;
} SwCodeField;

typedef GtkWidgetClass SwCodeFieldClass;

static GType sw_code_field_get_type (void);
G_DEFINE_TYPE (SwCodeField, sw_code_field, GTK_TYPE_WIDGET)

static void
code_update (SwCodeField *self)
{
  const char *code = gtk_editable_get_text (GTK_EDITABLE (self->text));
  int n = strlen (code);
  gboolean focus = gtk_widget_has_focus (self->text);

  for (int i = 0; i < self->length; i++)
    {
      GtkWidget *box = g_ptr_array_index (self->boxes, i);
      GtkWidget *label = gtk_widget_get_first_child (box);
      char digit[2] = { i < n ? code[i] : 0, 0 };

      gtk_label_set_text (GTK_LABEL (label), digit);
      if (focus && i == MIN (n, self->length - 1))
        gtk_widget_add_css_class (box, "current");
      else
        gtk_widget_remove_css_class (box, "current");
    }
}

static void
on_code_changed (GtkEditable *editable, SwCodeField *self)
{
  g_autofree char *digits = NULL;

  if (self->setting)
    return;
  digits = sw_digits_only (gtk_editable_get_text (editable), self->length);
  if (strcmp (digits, gtk_editable_get_text (editable)))
    {
      /* a pasted "123 456" or letters: only the digits stay */
      self->setting = TRUE;
      gtk_editable_set_text (editable, digits);
      gtk_editable_set_position (editable, -1);
      self->setting = FALSE;
    }
  sw_code_field_set_error (GTK_WIDGET (self), FALSE);
  code_update (self);
  g_signal_emit (self, code_changed, 0);
  if ((int) strlen (digits) == self->length)
    g_signal_emit (self, code_complete, 0, digits);
}

static void
on_code_focus (GtkEventControllerFocus *focus, SwCodeField *self)
{
  code_update (self);
}

static void
on_code_pressed (GtkGestureClick *click, int n, double x, double y, SwCodeField *self)
{
  gtk_widget_grab_focus (self->text);
  gtk_editable_set_position (GTK_EDITABLE (self->text), -1);
}

static void
sw_code_field_dispose (GObject *object)
{
  SwCodeField *self = (SwCodeField *) object;

  g_clear_pointer (&self->overlay, gtk_widget_unparent);
  g_clear_pointer (&self->boxes, g_ptr_array_unref);
  G_OBJECT_CLASS (sw_code_field_parent_class)->dispose (object);
}

static gboolean
sw_code_field_grab_focus (GtkWidget *widget)
{
  return gtk_widget_grab_focus (((SwCodeField *) widget)->text);
}

static void
sw_code_field_class_init (SwCodeFieldClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = sw_code_field_dispose;
  klass->grab_focus = sw_code_field_grab_focus;
  gtk_widget_class_set_layout_manager_type (klass, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (klass, "sw-code");
  code_complete = g_signal_new ("complete", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                G_TYPE_NONE, 1, G_TYPE_STRING);
  code_changed = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                               G_TYPE_NONE, 0);
}

static void
sw_code_field_init (SwCodeField *self)
{
}

GtkWidget *
sw_code_field_new (int length)
{
  SwCodeField *self = g_object_new (sw_code_field_get_type (), NULL);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  GtkGesture *click = gtk_gesture_click_new ();

  self->length = length;
  self->boxes = g_ptr_array_new ();
  self->overlay = gtk_overlay_new ();
  gtk_widget_set_parent (self->overlay, GTK_WIDGET (self));
  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);
  for (int i = 0; i < length; i++)
    {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *digit = gtk_label_new (NULL);

      gtk_widget_add_css_class (box, "sw-code-box");
      gtk_widget_set_hexpand (digit, TRUE);
      gtk_widget_set_vexpand (digit, TRUE);
      gtk_box_append (GTK_BOX (box), digit);
      gtk_box_append (GTK_BOX (row), box);
      g_ptr_array_add (self->boxes, box);
    }
  gtk_overlay_set_child (GTK_OVERLAY (self->overlay), row);

  /* the typing goes into a text field laid over the boxes, unseen: the on-screen keyboard and pasting work as in any
   * field, and the boxes show what it holds */
  self->text = gtk_text_new ();
  gtk_text_set_max_length (GTK_TEXT (self->text), length);
  gtk_text_set_input_purpose (GTK_TEXT (self->text), GTK_INPUT_PURPOSE_DIGITS);
  gtk_text_set_input_hints (GTK_TEXT (self->text), GTK_INPUT_HINT_NO_SPELLCHECK);
  gtk_widget_set_opacity (self->text, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->text), GTK_ACCESSIBLE_PROPERTY_LABEL, "The code", -1);
  gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->text);
  g_signal_connect (self->text, "changed", G_CALLBACK (on_code_changed), self);
  g_signal_connect (focus, "enter", G_CALLBACK (on_code_focus), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_code_focus), self);
  gtk_widget_add_controller (self->text, focus);
  g_signal_connect (click, "pressed", G_CALLBACK (on_code_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  code_update (self);
  return GTK_WIDGET (self);
}

char *
sw_code_field_get_code (GtkWidget *field)
{
  return g_strdup (gtk_editable_get_text (GTK_EDITABLE (((SwCodeField *) field)->text)));
}

void
sw_code_field_clear (GtkWidget *field)
{
  gtk_editable_set_text (GTK_EDITABLE (((SwCodeField *) field)->text), "");
}

void
sw_code_field_set_error (GtkWidget *field, gboolean error)
{
  SwCodeField *self = (SwCodeField *) field;

  for (guint i = 0; i < self->boxes->len; i++)
    if (error)
      gtk_widget_add_css_class (g_ptr_array_index (self->boxes, i), "error");
    else
      gtk_widget_remove_css_class (g_ptr_array_index (self->boxes, i), "error");
}

/* ---- misc ------------------------------------------------------------------------------------------------------ */

GtkWidget *
sw_divider_new (void)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (line, "sw-divider");
  return line;
}

static void
avatar_update (GtkWidget *avatar)
{
  SwAccount *account = sw_account_get_default ();
  GtkWidget *child = gtk_widget_get_first_child (avatar);
  int size = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (avatar), "sw-size"));
  const char *name = sw_account_get_name (account);

  if (child)
    gtk_box_remove (GTK_BOX (avatar), child);
  if (sw_account_is_signed_in (account) && name && *name)
    {
      g_autofree char *first = g_utf8_substring (name, 0, 1);
      g_autofree char *upper = g_utf8_strup (first, -1);

      child = gtk_label_new (upper);
      gtk_widget_add_css_class (avatar, "signed-in");
    }
  else
    {
      child = gtk_image_new_from_icon_name ("sw-person-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (child), size * 0.6);
      gtk_widget_remove_css_class (avatar, "signed-in");
    }
  gtk_widget_set_hexpand (child, TRUE);
  gtk_widget_set_vexpand (child, TRUE);
  gtk_box_append (GTK_BOX (avatar), child);
}

GtkWidget *
sw_avatar_new (int size)
{
  GtkWidget *avatar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (avatar, "sw-avatar");
  if (size >= 64)
    gtk_widget_add_css_class (avatar, "large");
  gtk_widget_set_size_request (avatar, size, size);
  /* the size asked for, not more: the child's expanding stays inside */
  gtk_widget_set_hexpand (avatar, FALSE);
  gtk_widget_set_vexpand (avatar, FALSE);
  gtk_widget_set_halign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  g_object_set_data (G_OBJECT (avatar), "sw-size", GINT_TO_POINTER (size));
  g_signal_connect_object (sw_account_get_default (), "changed", G_CALLBACK (avatar_update), avatar,
                           G_CONNECT_SWAPPED);
  avatar_update (avatar);
  return avatar;
}
