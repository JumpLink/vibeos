/* The app in a package (inst-catalog.h): from the AppStream catalog, or from the installed desktop file */
#include "inst-catalog.h"
#include "inst-demo.h"
#include "inst-dpkg.h"

#include <appstream.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

void
inst_app_info_free (InstAppInfo *info)
{
  if (!info)
    return;
  g_free (info->name);
  g_free (info->summary);
  g_free (info->description);
  g_free (info->homepage);
  g_clear_object (&info->icon);
  g_free (info->desktop_id);
  g_free (info);
}

/* the demo's app: name= and icon= say what it is called and looks like, app= what it is in the app drawer */
static InstAppInfo *
demo_info (const char *package)
{
  const char *name = inst_demo_get_for ("name", package), *icon = inst_demo_get_for ("icon", package);
  const char *app = inst_demo_get_for ("app", package);
  InstAppInfo *info;

  if (!name)
    return NULL;
  info = g_new0 (InstAppInfo, 1);
  info->name = g_strdup (name);
  info->summary = g_strdup ("A sample app for trying App Installer");
  info->description = g_strdup ("This package is not real: App Installer's demo shows it to try its screens.\n\n"
                                "Nothing is installed or removed while the demo runs.");
  info->homepage = g_strdup ("https://vibeos.top");
  if (icon)
    info->icon = g_themed_icon_new (icon);
  info->desktop_id = g_strdup (app);
  return info;
}

/* ---- the AppStream catalog ------------------------------------------------------------------------------------ */

static GMutex pool_lock;
static AsPool *pool;

/* the largest icon that is a file here (the catalog's cache), else the stock icon's name */
static GIcon *
pick_icon (AsComponent *cpt)
{
  GPtrArray *icons = as_component_get_icons (cpt);
  AsIcon *best = NULL, *stock = NULL;
  guint best_size = 0;

  for (guint i = 0; icons && i < icons->len; i++)
    {
      AsIcon *icon = g_ptr_array_index (icons, i);
      AsIconKind kind = as_icon_get_kind (icon);
      guint size = as_icon_get_width (icon) * MAX (as_icon_get_scale (icon), 1u);

      if (kind == AS_ICON_KIND_STOCK && !stock)
        stock = icon;
      else if ((kind == AS_ICON_KIND_CACHED || kind == AS_ICON_KIND_LOCAL) && as_icon_get_filename (icon)
               && g_file_test (as_icon_get_filename (icon), G_FILE_TEST_IS_REGULAR) && size >= best_size)
        {
          best = icon;
          best_size = size;
        }
    }
  /* a stock icon in the theme is the app's own, and scalable: once it is installed it is there */
  if (best && best_size >= 64)
    {
      g_autoptr (GFile) file = g_file_new_for_path (as_icon_get_filename (best));

      return g_file_icon_new (file);
    }
  if (stock)
    return g_themed_icon_new (as_icon_get_name (stock));
  if (best)
    {
      g_autoptr (GFile) file = g_file_new_for_path (as_icon_get_filename (best));

      return g_file_icon_new (file);
    }
  return NULL;
}

/* AppStream's description markup (<p>, <ul>/<ol> with <li>) as plain text */
static char *
plain_description (const char *markup)
{
  g_autoptr (GError) error = NULL;
  char *text;

  if (!markup || !*markup)
    return NULL;
  text = as_markup_convert (markup, AS_MARKUP_KIND_TEXT, &error);
  if (!text)
    return NULL;
  return g_strstrip (text);
}

InstAppInfo *
inst_catalog_lookup (const char *package)
{
  g_autoptr (AsComponentBox) box = NULL;
  AsComponent *found = NULL;
  InstAppInfo *info;
  const char *url;

  if (inst_demo_active ())
    return demo_info (package);
  g_mutex_lock (&pool_lock);
  if (!pool)
    {
      g_autoptr (GError) error = NULL;

      pool = as_pool_new ();
      /* the catalog the package lists brought, not the desktop files of what is installed */
      as_pool_set_flags (pool, AS_POOL_FLAG_LOAD_OS_CATALOG);
      if (!as_pool_load (pool, NULL, &error))
        g_debug ("AppStream: %s", error->message);
    }
  box = as_pool_get_components_by_kind (pool, AS_COMPONENT_KIND_DESKTOP_APP);
  for (guint i = 0; box && i < as_component_box_get_size (box); i++)
    {
      AsComponent *cpt = as_component_box_index_safe (box, i);

      if (!g_strcmp0 (as_component_get_pkgname (cpt), package))
        {
          found = cpt;
          break;
        }
    }
  if (!found)
    {
      g_mutex_unlock (&pool_lock);
      return NULL;
    }
  info = g_new0 (InstAppInfo, 1);
  info->name = g_strdup (as_component_get_name (found));
  info->summary = g_strdup (as_component_get_summary (found));
  info->description = plain_description (as_component_get_description (found));
  url = as_component_get_url (found, AS_URL_KIND_HOMEPAGE);
  info->homepage = g_strdup (url);
  info->icon = pick_icon (found);
  g_mutex_unlock (&pool_lock);
  return info;
}

/* ---- the installed desktop file ------------------------------------------------------------------------------- */

char *
inst_catalog_pick_desktop (const char *const *files, const char *package)
{
  g_autofree char *lower_package = g_ascii_strdown (package ? package : "", -1);
  const char *best = NULL;
  int best_score = 0;

  for (guint i = 0; files && files[i]; i++)
    {
      const char *path = files[i];
      g_autoptr (GDesktopAppInfo) app = NULL;
      g_autofree char *base = NULL, *lower = NULL;
      int score;

      if (!g_str_has_prefix (path, "/usr/share/applications/") || !g_str_has_suffix (path, ".desktop"))
        continue;
      app = g_desktop_app_info_new_from_filename (path);
      /* the drawer's test: an app, shown here, with something to run */
      if (!app || !g_app_info_should_show (G_APP_INFO (app)) || !g_app_info_get_executable (G_APP_INFO (app)))
        continue;
      base = g_path_get_basename (path);
      lower = g_ascii_strdown (base, -1);
      score = strstr (lower, lower_package) ? 2 : 1;
      if (score > best_score)
        {
          best = path;
          best_score = score;
        }
    }
  return g_strdup (best);
}

InstAppInfo *
inst_catalog_installed (const char *package)
{
  g_auto (GStrv) files = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GDesktopAppInfo) app = NULL;
  InstAppInfo *info;

  if (inst_demo_active ())
    return demo_info (package);
  files = inst_dpkg_files (package);
  path = inst_catalog_pick_desktop ((const char *const *) files, package);
  if (!path || !(app = g_desktop_app_info_new_from_filename (path)))
    return NULL;
  info = g_new0 (InstAppInfo, 1);
  info->name = g_strdup (g_app_info_get_display_name (G_APP_INFO (app)));
  info->summary = g_strdup (g_app_info_get_description (G_APP_INFO (app)));
  if (g_app_info_get_icon (G_APP_INFO (app)))
    info->icon = g_object_ref (g_app_info_get_icon (G_APP_INFO (app)));
  info->desktop_id = g_path_get_basename (path);
  return info;
}
