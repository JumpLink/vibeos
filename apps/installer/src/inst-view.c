/* One job on screen (inst-view.h). The view is rebuilt from the job's plan whenever its phase changes; within a phase
 * only what moves is updated (the progress, the status line, the app's name and icon as they come in). */
#include "inst-view.h"
#include "inst-demo.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <string.h>
#include <spongy-gtk.h>
#include <vibeos.h>

/* in a sheet, the other packages are named up to this many, then "and 4 more" */
#define SHEET_NAMES  4
#define PAGE_NAMES   12

struct _InstView {
  GtkWidget     parent_instance;

  InstJob      *job;
  InstViewMode  mode;
  InstPhase     shown_phase;
  InstChange    shown_change;
  gboolean      built;

  GtkWidget    *scroll, *content, *actions;
  GtkWidget    *icon_slot, *name, *sub;
  GtkWidget    *progress, *status, *percent;
  guint         pulse;
  guint         demo_answer;    /* the demo's "auto": the question answered yes, a moment after it is shown */
};

G_DEFINE_FINAL_TYPE (InstView, inst_view, GTK_TYPE_WIDGET)

enum { SIGNAL_DONE, N_SIGNALS };
static guint signals[N_SIGNALS];

char *
inst_format_size (guint64 bytes)
{
  return g_format_size (bytes);
}

/* ---- pieces --------------------------------------------------------------------------------------------------- */

static GtkWidget *
label (const char *text, const char *css_class, float xalign)
{
  GtkWidget *l = gtk_label_new (text);

  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  if (xalign == 0.5f)
    gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
  if (css_class)
    gtk_widget_add_css_class (l, css_class);
  return l;
}

GtkWidget *
inst_view_icon_new (GIcon *icon, int size)
{
  GtkWidget *image;

  if (icon)
    {
      image = gtk_image_new_from_gicon (icon);
      gtk_image_set_pixel_size (GTK_IMAGE (image), size);
      gtk_widget_add_css_class (image, "inst-app-icon");
      return image;
    }
  /* no icon of its own: the package glyph on a tile */
  image = gtk_image_new_from_icon_name ("inst-package-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (image), size * 5 / 9);
  gtk_widget_set_size_request (image, size, size);
  gtk_widget_add_css_class (image, "inst-generic-icon");
  return image;
}

static GtkWidget *
filled_button (const char *text)
{
  GtkWidget *button = vibeos_pressable_new (gtk_label_new (text));

  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
  gtk_widget_add_css_class (button, "inst-filled-button");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  return button;
}

static GtkWidget *
text_button (const char *text)
{
  GtkWidget *button = vibeos_text_button_new (text);

  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  return button;
}

/* a row of the facts card: what, and its value under it */
static void
fact (GtkWidget *list, const char *title, const char *value)
{
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), vibeos_list_row_new (title, value));
}

/* "libfoo, libbar, libbaz and 4 more" */
static char *
name_list (GPtrArray *names, guint most)
{
  GString *text = g_string_new (NULL);
  guint shown = names->len > most + 1 ? most : names->len;

  for (guint i = 0; i < shown; i++)
    {
      if (i)
        g_string_append (text, ", ");
      g_string_append (text, g_ptr_array_index (names, i));
    }
  if (shown < names->len)
    {
      guint more = names->len - shown;
      g_autofree char *list = g_string_free (text, FALSE);

      /* TRANSLATORS: %s is a list of names ("libfoo, libbar"), %u how many more there are */
      return g_strdup_printf (g_dngettext (NULL, "%s and %u more", "%s and %u more", more), list, more);
    }
  return g_string_free (text, FALSE);
}

/* ---- the header: icon, name, version -------------------------------------------------------------------------- */

static void
update_header (InstView *self)
{
  const InstPlan *plan = inst_job_get_plan (self->job);
  g_autofree char *sub = NULL;
  GtkWidget *child;

  if (!self->name)
    return;
  gtk_label_set_text (GTK_LABEL (self->name), inst_job_get_display_name (self->job));
  if (plan->name && plan->package && plan->version)
    sub = g_strdup_printf ("%s · %s", plan->package, plan->version);
  else if (plan->version)
    sub = g_strdup_printf (_("Version %s"), plan->version);
  gtk_label_set_text (GTK_LABEL (self->sub), sub ? sub : "");
  gtk_widget_set_visible (self->sub, sub != NULL);

  while ((child = gtk_widget_get_first_child (self->icon_slot)))
    gtk_box_remove (GTK_BOX (self->icon_slot), child);
  gtk_box_append (GTK_BOX (self->icon_slot),
                  inst_view_icon_new (plan->icon, self->mode == INST_VIEW_SHEET ? 48 : 88));
}

static GtkWidget *
header (InstView *self)
{
  gboolean sheet = self->mode == INST_VIEW_SHEET;
  GtkWidget *box = gtk_box_new (sheet ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL, sheet ? 16 : 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  gtk_widget_add_css_class (box, "inst-header");
  self->icon_slot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign (self->icon_slot, sheet ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->icon_slot, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), self->icon_slot);
  self->name = label (NULL, "inst-name", sheet ? 0 : 0.5f);
  self->sub = label (NULL, "inst-sub", sheet ? 0 : 0.5f);
  gtk_label_set_selectable (GTK_LABEL (self->sub), FALSE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (text), self->name);
  gtk_box_append (GTK_BOX (text), self->sub);
  gtk_box_append (GTK_BOX (box), text);
  update_header (self);
  return box;
}

/* ---- the lead: the question, the progress or the outcome ----------------------------------------------------- */

/* "Installed 1.2" and "This file 1.3 · newer", one under the other */
static GtkWidget *
versions (InstView *self)
{
  const InstPlan *plan = inst_job_get_plan (self->job);
  gboolean newer = plan->change == INST_CHANGE_UPGRADE, older = plan->change == INST_CHANGE_DOWNGRADE;
  GtkWidget *grid = gtk_grid_new ();
  const char *mine = inst_job_get_kind (self->job) == INST_JOB_FILE ? _("This file") : _("Available");
  const char *rows[2][3] = {
    { _("Installed"), plan->installed_version, older ? _("newer") : newer ? _("older") : NULL },
    { mine, plan->version, newer ? _("newer") : older ? _("OLDER") : NULL },
  };

  gtk_widget_add_css_class (grid, "inst-versions");
  gtk_grid_set_column_spacing (GTK_GRID (grid), 16);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  for (int r = 0; r < 2; r++)
    {
      GtkWidget *what = label (rows[r][0], "inst-version-what", 0);
      GtkWidget *value = label (rows[r][1], "inst-version-value", 0);

      gtk_label_set_wrap (GTK_LABEL (what), FALSE);
      gtk_widget_set_hexpand (value, TRUE);
      gtk_grid_attach (GTK_GRID (grid), what, 0, r, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), value, 1, r, 1, 1);
      if (rows[r][2])
        {
          GtkWidget *tag = label (rows[r][2], "inst-version-tag", 1);

          gtk_label_set_wrap (GTK_LABEL (tag), FALSE);
          if (r == 1 && older)
            gtk_widget_add_css_class (tag, "inst-older");
          gtk_grid_attach (GTK_GRID (grid), tag, 2, r, 1, 1);
        }
    }
  return grid;
}

static GtkWidget *
outcome (const char *icon_name, const char *css_class, const char *title, const char *text, const char *details)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

  gtk_widget_add_css_class (box, "inst-outcome");
  gtk_widget_add_css_class (box, css_class);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 28);
  gtk_widget_add_css_class (icon, "inst-outcome-icon");
  gtk_box_append (GTK_BOX (head), icon);
  gtk_box_append (GTK_BOX (head), label (title, "inst-lead-title", 0));
  gtk_box_append (GTK_BOX (box), head);
  if (text)
    gtk_box_append (GTK_BOX (box), label (text, "inst-lead-text", 0));
  if (details)
    {
      GtkWidget *d = label (details, "inst-details", 0);

      gtk_label_set_selectable (GTK_LABEL (d), TRUE);
      gtk_box_append (GTK_BOX (box), d);
    }
  return box;
}

static gboolean
on_pulse (gpointer data)
{
  InstView *self = data;

  if (self->progress && inst_job_get_percent (self->job) < 0)
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->progress));
  return G_SOURCE_CONTINUE;
}

static GtkWidget *
progress (InstView *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

  gtk_widget_add_css_class (box, "inst-progress-box");
  self->status = label (NULL, "inst-status", 0);
  gtk_widget_set_hexpand (self->status, TRUE);
  self->percent = label (NULL, "inst-percent", 1);
  gtk_label_set_wrap (GTK_LABEL (self->percent), FALSE);
  gtk_box_append (GTK_BOX (line), self->status);
  gtk_box_append (GTK_BOX (line), self->percent);
  self->progress = gtk_progress_bar_new ();
  gtk_progress_bar_set_pulse_step (GTK_PROGRESS_BAR (self->progress), 0.08);
  gtk_widget_add_css_class (self->progress, "inst-progress");
  gtk_box_append (GTK_BOX (box), line);
  gtk_box_append (GTK_BOX (box), self->progress);
  if (!self->pulse)
    self->pulse = g_timeout_add (120, on_pulse, self);
  return box;
}

static void
update_progress (InstView *self)
{
  int percent = inst_job_get_percent (self->job);
  g_autofree char *text = percent >= 0 ? g_strdup_printf ("%d %%", percent) : NULL;

  if (!self->progress)
    return;
  gtk_label_set_text (GTK_LABEL (self->status), inst_job_get_status_text (self->job)
                                                    ? inst_job_get_status_text (self->job) : "");
  gtk_label_set_text (GTK_LABEL (self->percent), text ? text : "");
  if (percent >= 0)
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->progress), percent / 100.0);
}

/* a package whose apps run sandboxed: uninstalled, its apps' data goes with it (vibeos-sandbox keeps a record of
 * each in ~/.local/share/vibeos-sandbox/packages, and deletes the data once dpkg has removed the package) */
static gboolean
sandboxed (const char *package)
{
  if (!package || !*package || strchr (package, '/') || package[0] == '.')
    return FALSE;
  g_autofree char *record = g_build_filename (g_get_user_data_dir (), "vibeos-sandbox", "packages", package, NULL);
  return g_file_test (record, G_FILE_TEST_IS_REGULAR);
}

static gboolean
data_goes (const InstPlan *plan)
{
  if (sandboxed (plan->package))
    return TRUE;
  for (guint i = 0; plan->also_remove && i < plan->also_remove->len; i++)
    if (sandboxed (plan->also_remove->pdata[i]))
      return TRUE;
  return FALSE;
}

/* the question for a READY plan: a title, and a sentence when the title is not enough */
static void
question (InstView *self, GtkWidget *box)
{
  const InstPlan *plan = inst_job_get_plan (self->job);
  const char *name = inst_job_get_display_name (self->job);
  gboolean file = inst_job_get_kind (self->job) == INST_JOB_FILE;
  g_autofree char *title = NULL, *text = NULL;

  switch (plan->change)
    {
    case INST_CHANGE_NEW:
      title = g_strdup (plan->name ? _("Install this app?") : _("Install this package?"));
      break;
    case INST_CHANGE_UPGRADE:
      title = g_strdup_printf (_("Replace %s %s with %s?"), name, plan->installed_version, plan->version);
      text = g_strdup (file ? _("This file has a newer version than the one installed.")
                            : _("The software sources have a newer version than the one installed."));
      break;
    case INST_CHANGE_DOWNGRADE:
      title = g_strdup (file ? _("This file is an OLDER version") : _("The available version is OLDER"));
      text = g_strdup_printf (file ? _("%s %s is installed. This file has %s, which is older. Replace the installed "
                                       "version with the older one?")
                                   : _("%s %s is installed. The software sources have %s, which is older. Replace "
                                       "the installed version with the older one?"),
                              name, plan->installed_version, plan->version);
      break;
    case INST_CHANGE_REINSTALL:
      title = g_strdup (_("Already installed"));
      text = g_strdup_printf (_("%s %s is installed already. Reinstalling puts its files back as they came."), name,
                              plan->version);
      break;
    case INST_CHANGE_SAME:
      title = g_strdup (_("Already installed"));
      text = g_strdup_printf (_("%s %s is installed already. The same version cannot be installed over itself."),
                              name, plan->version);
      break;
    case INST_CHANGE_REMOVE:
      title = g_strdup_printf (_("Uninstall %s?"), name);
      text = g_strdup (data_goes (plan) ? _("Its data will be deleted.")
                                        : _("Its program files are removed. Your own files stay."));
      break;
    }
  gtk_box_append (GTK_BOX (box), label (title, "inst-lead-title", 0));
  if (text)
    gtk_box_append (GTK_BOX (box), label (text, "inst-lead-text", 0));
  if (plan->change == INST_CHANGE_UPGRADE || plan->change == INST_CHANGE_DOWNGRADE)
    gtk_box_append (GTK_BOX (box), versions (self));
}

static GtkWidget *
lead (InstView *self)
{
  InstPhase phase = inst_job_get_phase (self->job);
  gboolean removing = inst_job_get_kind (self->job) == INST_JOB_REMOVE;
  GtkWidget *box;

  switch (phase)
    {
    case INST_PHASE_READING:
    case INST_PHASE_CHECKING:
    case INST_PHASE_RUNNING:
      box = progress (self);
      update_progress (self);
      return box;
    case INST_PHASE_READY:
      box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
      gtk_widget_add_css_class (box, "inst-question");
      if (inst_job_get_plan (self->job)->change == INST_CHANGE_DOWNGRADE)
        gtk_widget_add_css_class (box, "inst-warning");
      question (self, box);
      return box;
    case INST_PHASE_DONE:
      return outcome ("inst-done-symbolic", "inst-success", inst_job_get_title (self->job),
                      inst_job_get_text (self->job), NULL);
    case INST_PHASE_CANCELLED:
      return outcome ("inst-info-symbolic", "inst-neutral", inst_job_get_title (self->job),
                      inst_job_get_text (self->job), NULL);
    case INST_PHASE_REFUSED:
    case INST_PHASE_FAILED:
      return outcome ("inst-error-symbolic", "inst-failure",
                      inst_job_get_title (self->job) ? inst_job_get_title (self->job)
                                                     : removing ? _("Not uninstalled") : _("Not installed"),
                      inst_job_get_text (self->job), inst_job_get_details (self->job));
    }
  return gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
}

/* ---- the facts: size, other packages, the package ------------------------------------------------------------- */

static GtkWidget *
facts (InstView *self)
{
  const InstPlan *plan = inst_job_get_plan (self->job);
  InstJobKind kind = inst_job_get_kind (self->job);
  InstPhase phase = inst_job_get_phase (self->job);
  gboolean sheet = self->mode == INST_VIEW_SHEET, checked = phase != INST_PHASE_READING
                                                           && phase != INST_PHASE_CHECKING;
  GtkWidget *list = vibeos_card_list_new ();
  guint most = sheet ? SHEET_NAMES : PAGE_NAMES;
  gboolean any = FALSE;

  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 0, 0);
  if (kind == INST_JOB_REMOVE)
    {
      if (plan->freed_size || plan->installed_size)
        {
          g_autofree char *size = inst_format_size (plan->freed_size ? plan->freed_size : plan->installed_size);

          fact (list, _("Frees"), size);
          any = TRUE;
        }
    }
  else if (!sheet || plan->change == INST_CHANGE_NEW)
    {
      g_autofree char *installed = plan->installed_size ? inst_format_size (plan->installed_size) : NULL;
      gboolean is_file = kind == INST_JOB_FILE && plan->file_size;
      g_autofree char *file = is_file ? inst_format_size (plan->file_size) : NULL;
      g_autofree char *download = !is_file && plan->download_size ? inst_format_size (plan->download_size) : NULL;
      g_autofree char *text = NULL;

      /* each a whole sentence: the package file's size or what it downloads, the space it takes once installed */
      if (file && installed)
        /* TRANSLATORS: a package file's size, then the space it takes once installed: "4 MB file · 12 MB installed" */
        text = g_strdup_printf (_("%s file · %s installed"), file, installed);
      else if (download && installed)
        /* TRANSLATORS: what installing downloads, then the space it takes: "4 MB download · 12 MB installed" */
        text = g_strdup_printf (_("%s download · %s installed"), download, installed);
      else if (installed)
        text = g_strdup_printf (_("%s installed"), installed);
      else if (file)
        text = g_strdup_printf (_("%s file"), file);
      else if (download)
        text = g_strdup_printf (_("%s download"), download);
      if (text)
        {
          fact (list, _("Size"), text);
          any = TRUE;
        }
    }
  if (checked && plan->also_install->len)
    {
      g_autofree char *names = name_list (plan->also_install, most);
      g_autofree char *title = g_strdup_printf (g_dngettext (NULL, "Also installs %u other package",
                                                             "Also installs %u other packages",
                                                             plan->also_install->len),
                                                plan->also_install->len);
      g_autofree char *text = NULL;

      if (plan->also_size)
        {
          g_autofree char *size = inst_format_size (plan->also_size);

          text = g_strdup_printf (_("%s to download: %s"), size, names);
        }
      fact (list, title, text ? text : names);
      any = TRUE;
    }
  if (checked && plan->also_remove->len)
    {
      g_autofree char *names = name_list (plan->also_remove, most);
      g_autofree char *title = g_strdup_printf (g_dngettext (NULL, "Also removes %u other package",
                                                             "Also removes %u other packages",
                                                             plan->also_remove->len),
                                                plan->also_remove->len);

      fact (list, title, names);
      any = TRUE;
    }
  if (!sheet && plan->package && plan->version)
    {
      g_autofree char *text = plan->architecture
                                  ? g_strdup_printf ("%s %s · %s", plan->package, plan->version, plan->architecture)
                                  : g_strdup_printf ("%s %s", plan->package, plan->version);

      fact (list, _("Package"), text);
      any = TRUE;
    }
  if (!sheet && plan->homepage && *plan->homepage)
    {
      fact (list, _("Website"), plan->homepage);
      any = TRUE;
    }
  if (!any)
    {
      g_object_ref_sink (list);
      g_object_unref (list);
      return NULL;
    }
  return list;
}

/* "Packages run with full rights": a .deb file can do anything to the device */
static GtkWidget *
trust_note (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new_from_icon_name ("inst-shield-symbolic"), *text;

  gtk_widget_add_css_class (box, "inst-trust");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 20);
  gtk_widget_set_valign (icon, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), icon);
  text = label (_("A package can change anything on this device: it installs with full rights. Install only what "
                  "you trust."), "inst-trust-text", 0);
  /* the rest of the line: a wrapping label asks only for a width that gives it a pleasing shape */
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (box), text);
  return box;
}

static GtkWidget *
description (InstView *self)
{
  const InstPlan *plan = inst_job_get_plan (self->job);
  GtkWidget *box;

  if ((!plan->summary || !*plan->summary) && (!plan->description || !*plan->description))
    return NULL;
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (box, "inst-about");
  if (plan->summary && *plan->summary)
    gtk_box_append (GTK_BOX (box), label (plan->summary, "inst-summary", 0));
  if (plan->description && *plan->description)
    gtk_box_append (GTK_BOX (box), label (plan->description, "inst-description", 0));
  return box;
}

/* ---- the buttons ---------------------------------------------------------------------------------------------- */

static void
on_start (GtkWidget *button, InstView *self)
{
  if (inst_job_can_start (self->job))
    inst_job_start (self->job);
}

static gboolean
demo_answer (gpointer data)
{
  InstView *self = data;

  self->demo_answer = 0;
  on_start (NULL, self);
  return G_SOURCE_REMOVE;
}

static void
on_cancel (GtkWidget *button, InstView *self)
{
  gboolean running = inst_job_get_phase (self->job) == INST_PHASE_RUNNING;

  inst_job_cancel (self->job);
  /* a running job stops in PackageKit's time, and the view shows how it ended */
  if (!running)
    g_signal_emit (self, signals[SIGNAL_DONE], 0);
}

static void
on_done (GtkWidget *button, InstView *self)
{
  g_signal_emit (self, signals[SIGNAL_DONE], 0);
}

static void
on_open (GtkWidget *button, InstView *self)
{
  const char *id = inst_job_get_plan (self->job)->desktop_id;
  g_autoptr (GDesktopAppInfo) app = id ? g_desktop_app_info_new (id) : NULL;
  g_autoptr (GdkAppLaunchContext) context = gdk_display_get_app_launch_context (gtk_widget_get_display (button));

  if (app)
    g_app_info_launch (G_APP_INFO (app), NULL, G_APP_LAUNCH_CONTEXT (context), NULL);
  g_signal_emit (self, signals[SIGNAL_DONE], 0);
}

static void
add_button (InstView *self, GtkWidget *button, GCallback callback)
{
  g_signal_connect (button, "clicked", callback, self);
  gtk_box_append (GTK_BOX (self->actions), button);
}

static void
build_actions (InstView *self)
{
  const InstPlan *plan = inst_job_get_plan (self->job);
  InstPhase phase = inst_job_get_phase (self->job);
  g_autoptr (GDesktopAppInfo) app = plan->desktop_id ? g_desktop_app_info_new (plan->desktop_id) : NULL;
  gboolean openable = app != NULL;
  GtkWidget *child, *primary = NULL;

  while ((child = gtk_widget_get_first_child (self->actions)))
    gtk_box_remove (GTK_BOX (self->actions), child);

  switch (phase)
    {
    case INST_PHASE_READING:
    case INST_PHASE_CHECKING:
      add_button (self, text_button (_("Cancel")), G_CALLBACK (on_cancel));
      primary = filled_button (inst_job_get_kind (self->job) == INST_JOB_REMOVE ? _("Uninstall") : _("Install"));
      gtk_widget_set_sensitive (primary, FALSE);
      gtk_box_append (GTK_BOX (self->actions), primary);
      break;
    case INST_PHASE_READY:
      if (plan->change == INST_CHANGE_SAME)
        {
          if (openable)
            add_button (self, text_button (_("Open")), G_CALLBACK (on_open));
          add_button (self, filled_button (_("Done")), G_CALLBACK (on_done));
          break;
        }
      add_button (self, text_button (_("Cancel")), G_CALLBACK (on_cancel));
      switch (plan->change)
        {
        case INST_CHANGE_UPGRADE:
        case INST_CHANGE_DOWNGRADE:
          {
            g_autofree char *text = g_strdup_printf (_("Replace with %s"), plan->version);

            primary = filled_button (text);
            break;
          }
        case INST_CHANGE_REINSTALL:
          primary = filled_button (_("Reinstall"));
          break;
        case INST_CHANGE_REMOVE:
          primary = filled_button (_("Uninstall"));
          gtk_widget_add_css_class (primary, "inst-destructive");
          break;
        default:
          primary = filled_button (_("Install"));
          break;
        }
      add_button (self, primary, G_CALLBACK (on_start));
      break;
    case INST_PHASE_RUNNING:
      /* Stop only while PackageKit can (before dpkg starts): otherwise there is nothing to press */
      if (inst_job_can_cancel (self->job))
        add_button (self, text_button (_("Stop")), G_CALLBACK (on_cancel));
      break;
    case INST_PHASE_DONE:
      if (inst_job_get_kind (self->job) != INST_JOB_REMOVE && openable)
        {
          add_button (self, text_button (_("Done")), G_CALLBACK (on_done));
          add_button (self, filled_button (_("Open")), G_CALLBACK (on_open));
        }
      else
        add_button (self, filled_button (_("Done")), G_CALLBACK (on_done));
      break;
    case INST_PHASE_REFUSED:
    case INST_PHASE_FAILED:
    case INST_PHASE_CANCELLED:
      add_button (self, filled_button (_("Close")), G_CALLBACK (on_done));
      break;
    }
  gtk_widget_set_visible (self->actions, gtk_widget_get_first_child (self->actions) != NULL);
}

/* ---- the whole view ------------------------------------------------------------------------------------------- */

static void
rebuild (InstView *self)
{
  InstPhase phase = inst_job_get_phase (self->job);
  gboolean sheet = self->mode == INST_VIEW_SHEET;
  GtkWidget *child, *piece;

  self->shown_phase = phase;
  self->shown_change = inst_job_get_plan (self->job)->change;
  self->built = TRUE;
  self->progress = self->status = self->percent = NULL;
  while ((child = gtk_widget_get_first_child (self->content)))
    gtk_box_remove (GTK_BOX (self->content), child);

  gtk_box_append (GTK_BOX (self->content), header (self));
  gtk_box_append (GTK_BOX (self->content), lead (self));
  if ((piece = facts (self)))
    gtk_box_append (GTK_BOX (self->content), piece);
  if (inst_job_get_kind (self->job) == INST_JOB_FILE && !inst_job_is_finished (self->job)
      && phase != INST_PHASE_RUNNING)
    gtk_box_append (GTK_BOX (self->content), trust_note ());
  if (!sheet && (piece = description (self)))
    gtk_box_append (GTK_BOX (self->content), piece);
  if (phase != INST_PHASE_RUNNING && phase != INST_PHASE_READING && phase != INST_PHASE_CHECKING)
    g_clear_handle_id (&self->pulse, g_source_remove);
  build_actions (self);
  if (phase == INST_PHASE_READY && inst_job_can_start (self->job) && inst_demo_has ("auto") && !self->demo_answer)
    self->demo_answer = g_timeout_add (2 * inst_demo_number ("delay", 600), demo_answer, self);
}

static void
on_job_changed (InstJob *job, InstView *self)
{
  /* a new phase (or a plan that changed its mind about what it is): the view again */
  if (!self->built || inst_job_get_phase (job) != self->shown_phase
      || inst_job_get_plan (job)->change != self->shown_change)
    {
      rebuild (self);
      return;
    }
  /* within a phase: the name and icon as they come in, the progress, and Stop as PackageKit allows it */
  update_header (self);
  update_progress (self);
  if (inst_job_get_phase (job) == INST_PHASE_RUNNING
      && (gtk_widget_get_first_child (self->actions) != NULL) != inst_job_can_cancel (job))
    build_actions (self);
  if (inst_job_get_phase (job) == INST_PHASE_READING || inst_job_get_phase (job) == INST_PHASE_CHECKING)
    {
      /* sizes and the description arrive during the check */
      rebuild (self);
    }
}

InstJob *
inst_view_get_job (InstView *self)
{
  return self->job;
}

static void
inst_view_dispose (GObject *object)
{
  InstView *self = INST_VIEW (object);
  GtkWidget *child;

  g_clear_handle_id (&self->pulse, g_source_remove);
  g_clear_handle_id (&self->demo_answer, g_source_remove);
  if (self->job)
    g_signal_handlers_disconnect_by_data (self->job, self);
  g_clear_object (&self->job);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (inst_view_parent_class)->dispose (object);
}

static void
inst_view_class_init (InstViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = inst_view_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "inst-view");
  signals[SIGNAL_DONE] = g_signal_new ("done", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 0);
}

static void
inst_view_init (InstView *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (gtk_widget_get_layout_manager (GTK_WIDGET (self))),
                                  GTK_ORIENTATION_VERTICAL);
}

GtkWidget *
inst_view_new (InstJob *job, InstViewMode mode)
{
  InstView *self = g_object_new (INST_TYPE_VIEW, NULL);

  self->job = g_object_ref (job);
  self->mode = mode;
  self->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, mode == INST_VIEW_SHEET ? 16 : 20);
  gtk_widget_add_css_class (self->content, "inst-content");
  self->actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (self->actions, GTK_ALIGN_END);
  gtk_widget_add_css_class (self->actions, "inst-actions");
  if (mode == INST_VIEW_PAGE)
    {
      self->scroll = spongy_scroll_view_new ();
      spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->content);
      gtk_widget_set_vexpand (self->scroll, TRUE);
      gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));
      /* the bar under the content, full width, with the buttons at its end */
      {
        GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_add_css_class (bar, "inst-action-bar");
        gtk_widget_set_hexpand (self->actions, TRUE);
        gtk_box_append (GTK_BOX (bar), self->actions);
        gtk_widget_set_parent (bar, GTK_WIDGET (self));
      }
    }
  else
    {
      gtk_widget_set_parent (self->content, GTK_WIDGET (self));
      gtk_widget_set_parent (self->actions, GTK_WIDGET (self));
      gtk_widget_add_css_class (GTK_WIDGET (self), "inst-sheet-view");
    }
  g_signal_connect (job, "changed", G_CALLBACK (on_job_changed), self);
  rebuild (self);
  return GTK_WIDGET (self);
}
