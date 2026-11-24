/* term-window: see term-window.h */
#include "term-window.h"
#include "term-session.h"
#include "term-view.h"

#include <glib/gi18n.h>
#include <vibeos.h>

typedef struct {
  GtkWindow    *window;
  VibeosTopBar *bar;
  TermView     *view;
  TermSession  *session;
  GtkWidget    *extra_keys;
} Window;

static const VibeosAboutSection term_legal[] = {
  { "VTE", "The terminal itself is <a href=\"https://gitlab.gnome.org/GNOME/vte\">VTE</a>, GNOME's terminal widget, "
           "under the GNU Lesser General Public License, version 3 or later." },
  { NULL }
};

static const VibeosAboutInfo term_about = { .name = "Terminal", .icon_name = "vibeos.Terminal",
                                            .legal = term_legal };

static void
window_free (gpointer data)
{
  Window *self = data;

  if (self->session)
    g_signal_handlers_disconnect_by_data (self->session, self);
  g_clear_object (&self->session);
  g_free (self);
}

static void
update_title (Window *self)
{
  const char *title = vte_terminal_get_termprop_string (VTE_TERMINAL (self->view), VTE_TERMPROP_XTERM_TITLE, NULL);

  if (!title || !*title)
    title = _("Terminal");
  gtk_window_set_title (self->window, title);
  vibeos_top_bar_set_title (self->bar, title);
}

static void
on_title_changed (VteTerminal *terminal, const char *name, Window *self)
{
  update_title (self);
}

static void
on_exited (TermSession *session, int status, Window *self)
{
  /* the shell is done: so is its window, as in GNOME Console */
  gtk_window_destroy (self->window);
}

static void
menu_new_window (gpointer data)
{
  Window *self = data;

  gtk_window_present (GTK_WINDOW (term_window_new (gtk_window_get_application (self->window))));
}

static void
menu_paste (gpointer data)
{
  Window *self = data;

  term_view_paste (self->view);
}

static void
on_more (GtkWidget *button, Window *self)
{
  VibeosMenu *menu = vibeos_menu_new ();

  vibeos_menu_append (menu, _("New window"), menu_new_window, self);
  vibeos_menu_append (menu, _("Paste"), menu_paste, self);
  vibeos_menu_append_about (menu, &term_about);
  vibeos_menu_present (menu, button);
}

static gboolean
on_key_pressed (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, Window *self)
{
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();

  if ((keyval == GDK_KEY_Escape || keyval == GDK_KEY_Back) && mods == 0)
    return term_view_end_selection (self->view);
  if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
    if (keyval == GDK_KEY_C || keyval == GDK_KEY_c) {
      term_view_copy (self->view);
      return TRUE;
    }
    if (keyval == GDK_KEY_V || keyval == GDK_KEY_v) {
      term_view_paste (self->view);
      return TRUE;
    }
  }
  return FALSE;
}

GtkWidget *
term_window_new (GtkApplication *app)
{
  Window *self = g_new0 (Window, 1);
  GtkWidget *window = vibeos_window_new (app);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *more;
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_autoptr (GError) error = NULL;

  self->window = GTK_WINDOW (window);
  g_object_set_data_full (G_OBJECT (window), "term-window", self, window_free);
  gtk_widget_add_css_class (window, "terminal");

  self->bar = VIBEOS_TOP_BAR (vibeos_top_bar_new (_("Terminal")));
  more = vibeos_top_bar_add_action (self->bar, "term-more-symbolic");
  g_signal_connect (more, "clicked", G_CALLBACK (on_more), self);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->bar));

  self->view = term_view_new ();
  gtk_widget_set_vexpand (GTK_WIDGET (self->view), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->view), TRUE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->view));

  /* the extra keys' row, to come: between the terminal and the keyboard */
  self->extra_keys = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->extra_keys, "term-extra-keys");
  gtk_widget_set_visible (self->extra_keys, FALSE);
  gtk_box_append (GTK_BOX (content), self->extra_keys);

  vibeos_window_set_content (self->window, content);
  update_title (self);
  g_signal_connect (self->view, "termprop-changed::" VTE_TERMPROP_XTERM_TITLE, G_CALLBACK (on_title_changed), self);

  /* before the terminal: capture */
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (window, keys);

  self->session = term_session_new (VTE_TERMINAL (self->view));
  term_view_set_session (self->view, self->session);
  g_signal_connect (self->session, "exited", G_CALLBACK (on_exited), self);
  if (!term_session_start (self->session, NULL, &error)) {
    g_autofree char *text = g_strdup_printf ("%s\r\n", error->message);
    vte_terminal_feed (VTE_TERMINAL (self->view), text, -1);
  }
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  return window;
}
