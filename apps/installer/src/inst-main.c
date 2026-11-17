/* vibeos-installer: App Installer, which installs .deb packages, and installs and uninstalls packages from the
 * software sources for other apps.
 *
 *   vibeos-installer                     the start page: what the app does, and "Choose a package"
 *   vibeos-installer FILE.deb            the package's page: install it (Files opens .deb files with it)
 *   vibeos-installer --install NAME      install the package NAME from the software sources
 *   vibeos-installer --uninstall NAME    uninstall the package NAME
 *
 * vibeos.Installer is D-Bus activatable, and the default app for .deb files in the VibeOS session
 * (/etc/xdg/gnome-mimeapps.list). Other apps use its D-Bus interface, vibeos.Installer1 (inst-service.h,
 * data/vibeos.Installer1.xml). PackageKit does the installing (inst-pk.h), polkit asks for the password.
 *
 * VIBEOS_INSTALLER_DEMO in the environment plays canned states instead, for pictures and tests: nothing is installed
 * or removed and PackageKit is never asked (inst-demo.h has the settings). */
#include "inst-demo.h"
#include "inst-job.h"
#include "inst-service.h"
#include "inst-window.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <locale.h>
#include <vibeos.h>

#define INST_APP_ID "vibeos.Installer"

#define INST_TYPE_APP (inst_app_get_type ())
G_DECLARE_FINAL_TYPE (InstApp, inst_app, INST, APP, GtkApplication)

struct _InstApp {
  GtkApplication parent_instance;
  char          *pending_action;   /* --install or --uninstall in the first instance: done at activation */
  char          *pending_name;
};

G_DEFINE_FINAL_TYPE (InstApp, inst_app, GTK_TYPE_APPLICATION)

/* the window for a new job: the one there is, unless it is busy with a running one */
static GtkWidget *
window_for_job (GtkApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  if (!window || inst_window_is_busy (GTK_WIDGET (window)))
    return inst_window_new (app);
  return GTK_WIDGET (window);
}

static void
show_job (GtkApplication *app, InstJob *job)
{
  GtkWidget *window = window_for_job (app);

  inst_window_show_job (window, job, TRUE);
  gtk_window_present (GTK_WINDOW (window));
}

static void
on_install (GSimpleAction *action, GVariant *param, gpointer app)
{
  g_autoptr (InstJob) job = inst_job_new_package (g_variant_get_string (param, NULL));

  show_job (app, job);
}

static void
on_uninstall (GSimpleAction *action, GVariant *param, gpointer app)
{
  g_autoptr (InstJob) job = inst_job_new_remove (g_variant_get_string (param, NULL));

  show_job (app, job);
}

/* a notification's default action: the window again */
static void
on_show (GSimpleAction *action, GVariant *param, gpointer app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  if (window)
    gtk_window_present (window);
  else
    g_application_activate (app);
}

/* a notification's Open: the app just installed */
static void
on_open_app (GSimpleAction *action, GVariant *param, gpointer app)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (g_variant_get_string (param, NULL));

  if (info)
    g_app_info_launch (G_APP_INFO (info), NULL, NULL, NULL);
}

static void
inst_app_startup (GApplication *app)
{
  static const GActionEntry actions[] = {
    { "install", on_install, "s", NULL, NULL, { 0 } },
    { "uninstall", on_uninstall, "s", NULL, NULL, { 0 } },
    { "show", on_show, NULL, NULL, NULL, { 0 } },
    { "open-app", on_open_app, "s", NULL, NULL, { 0 } },
  };

  G_APPLICATION_CLASS (inst_app_parent_class)->startup (app);
  /* the text follows Font size and Bold text; the stylesheet in sp and dp, over libvibeos's */
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_resource ("/vibeos/Installer/installer.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
}

static void
inst_app_activate (GApplication *app)
{
  InstApp *self = INST_APP (app);
  GtkWindow *window;

  if (self->pending_action)
    {
      g_autofree char *action = g_steal_pointer (&self->pending_action);
      g_autofree char *name = g_steal_pointer (&self->pending_name);

      g_action_group_activate_action (G_ACTION_GROUP (app), action, g_variant_new_string (name));
      return;
    }
  /* the demo acting as another app: only what that app's call would show */
  if (inst_demo_get ("api"))
    {
      static gboolean called;

      if (!called)
        inst_service_demo_call (GTK_APPLICATION (app), inst_demo_get ("api"));
      called = TRUE;
      return;
    }
  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!window)
    {
      window = GTK_WINDOW (inst_window_new (GTK_APPLICATION (app)));
      inst_window_show_start (GTK_WIDGET (window));
    }
  gtk_window_present (window);
}

static void
inst_app_open (GApplication *app, GFile **files, int n_files, const char *hint)
{
  for (int i = 0; i < n_files; i++)
    {
      g_autoptr (InstJob) job = inst_job_new_file (files[i]);
      GtkWidget *window = window_for_job (GTK_APPLICATION (app));

      inst_job_set_sandbox (job, TRUE);
      /* a file tapped in Files: its page alone, Back leaves */
      inst_window_show_job (window, job, FALSE);
      gtk_window_present (GTK_WINDOW (window));
    }
}

static int
inst_app_handle_local_options (GApplication *app, GVariantDict *options)
{
  InstApp *self = INST_APP (app);
  const char *install = NULL, *uninstall = NULL, *action, *name;
  g_autoptr (GError) error = NULL;

  if (g_variant_dict_contains (options, "version"))
    {
      g_print ("vibeos-installer %s\n", INST_VERSION);
      return 0;
    }
  g_variant_dict_lookup (options, "install", "&s", &install);
  g_variant_dict_lookup (options, "uninstall", "&s", &uninstall);
  if (!install && !uninstall)
    return -1;
  action = install ? "install" : "uninstall";
  name = install ? install : uninstall;
  if (!g_application_register (app, NULL, &error))
    {
      g_printerr ("vibeos-installer: %s\n", error->message);
      return 1;
    }
  /* App Installer is running already: it does it */
  if (g_application_get_is_remote (app))
    {
      g_action_group_activate_action (G_ACTION_GROUP (app), action, g_variant_new_string (name));
      return 0;
    }
  self->pending_action = g_strdup (action);
  self->pending_name = g_strdup (name);
  return -1;
}

static gboolean
inst_app_dbus_register (GApplication *app, GDBusConnection *connection, const char *path, GError **error)
{
  if (!G_APPLICATION_CLASS (inst_app_parent_class)->dbus_register (app, connection, path, error))
    return FALSE;
  return inst_service_register (GTK_APPLICATION (app), connection, path, error);
}

static void
inst_app_dbus_unregister (GApplication *app, GDBusConnection *connection, const char *path)
{
  inst_service_unregister ();
  G_APPLICATION_CLASS (inst_app_parent_class)->dbus_unregister (app, connection, path);
}

static void
inst_app_finalize (GObject *object)
{
  InstApp *self = INST_APP (object);

  g_free (self->pending_action);
  g_free (self->pending_name);
  G_OBJECT_CLASS (inst_app_parent_class)->finalize (object);
}

static void
inst_app_class_init (InstAppClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = inst_app_finalize;
  app_class->startup = inst_app_startup;
  app_class->activate = inst_app_activate;
  app_class->open = inst_app_open;
  app_class->handle_local_options = inst_app_handle_local_options;
  app_class->dbus_register = inst_app_dbus_register;
  app_class->dbus_unregister = inst_app_dbus_unregister;
}

static void
inst_app_init (InstApp *self)
{
  static const GOptionEntry entries[] = {
    { "install", 0, 0, G_OPTION_ARG_STRING, NULL, N_("Install the package NAME from the software sources"),
      N_("NAME") },
    { "uninstall", 0, 0, G_OPTION_ARG_STRING, NULL, N_("Uninstall the package NAME"), N_("NAME") },
    { "version", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Print the version"), NULL },
    { NULL }
  };

  g_application_add_main_option_entries (G_APPLICATION (self), entries);
  g_application_set_option_context_parameter_string (G_APPLICATION (self), N_("[FILE.deb…]"));
}

int
main (int argc, char **argv)
{
  g_autoptr (InstApp) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = g_object_new (INST_TYPE_APP, "application-id", INST_APP_ID, "flags", G_APPLICATION_HANDLES_OPEN,
                      /* started by D-Bus for another app's call: it stays a while for that app's next call (Software
                       * asks about one package after another) */
                      "inactivity-timeout", 10000, NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
