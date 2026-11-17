/* sw-device: see sw-device.h */
#include "sw-device.h"

#include <string.h>
#include <vibeos.h>

static char *device_id, *model, *arch;
static gboolean model_done, arch_done;
static GCallback loaded_cb;
static gpointer loaded_data;

const char *
sw_device_built_arch (void)
{
#if defined(__aarch64__)
  return "arm64";
#elif defined(__x86_64__)
  return "amd64";
#elif defined(__arm__)
  return "armhf";
#elif defined(__riscv) && __riscv_xlen == 64
  return "riscv64";
#else
  return "arm64";
#endif
}

char *
sw_device_id_in (const char *dir)
{
  g_autofree char *path = g_build_filename (dir, "device-id", NULL);
  g_autofree char *contents = NULL;
  char *id;

  if (g_file_get_contents (path, &contents, NULL, NULL))
    {
      g_strstrip (contents);
      if (g_uuid_string_is_valid (contents))
        return g_steal_pointer (&contents);
    }
  id = g_uuid_string_random ();
  g_mkdir_with_parents (dir, 0700);
  if (!g_file_set_contents_full (path, id, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
    g_warning ("could not keep the device id in %s", path);
  return id;
}

char *
sw_device_clean_model (const char *raw, gsize length)
{
  g_autofree char *text = g_strndup (raw, length);
  char *clean;

  /* the device tree's strings end in a NUL, and some list several (compatible-style) */
  clean = g_strstrip (g_strdup (text));
  g_free (g_steal_pointer (&text));
  if (g_utf8_strlen (clean, -1) > 100)
    {
      char *cut = g_utf8_substring (clean, 0, 100);

      g_free (clean);
      clean = cut;
    }
  return clean;
}

const char *
sw_device_id (void)
{
  if (!device_id)
    {
      g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "vibeos-software", NULL);

      device_id = sw_device_id_in (dir);
    }
  return device_id;
}

const char *
sw_device_model (void)
{
  return model && *model ? model : "Linux device";
}

const char *
sw_device_version (void)
{
  const char *release = vibeos_release ();

  return release && *release ? release : "unknown";
}

const char *
sw_device_arch (void)
{
  return arch && *arch ? arch : sw_device_built_arch ();
}

gboolean
sw_device_is_loaded (void)
{
  return model_done && arch_done;
}

JsonNode *
sw_device_to_json (gboolean with_id)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  if (with_id)
    {
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, sw_device_id ());
    }
  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, sw_device_model ());
  json_builder_set_member_name (b, "vibeos_version");
  json_builder_add_string_value (b, sw_device_version ());
  json_builder_set_member_name (b, "arch");
  json_builder_add_string_value (b, sw_device_arch ());
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

static void
maybe_done (void)
{
  if (!sw_device_is_loaded () || !loaded_cb)
    return;
  ((void (*) (gpointer)) loaded_cb) (loaded_data);
  loaded_cb = NULL;
}

/* ---- the model ------------------------------------------------------------------------------------------------- */

static void
on_tree_model (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autofree char *contents = NULL;
  gsize length = 0;

  if (g_file_load_contents_finish (G_FILE (source), res, &contents, &length, NULL, NULL) && length)
    {
      g_free (model);
      model = sw_device_clean_model (contents, length);
    }
  model_done = TRUE;
  maybe_done ();
}

static void
read_device_tree (void)
{
  g_autoptr (GFile) file = g_file_new_for_path ("/sys/firmware/devicetree/base/model");

  g_file_load_contents_async (file, NULL, on_tree_model, NULL);
}

static void
on_hostnamed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, NULL);
  g_autoptr (GVariant) value = NULL;

  if (reply)
    {
      g_variant_get (reply, "(v)", &value);
      if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING) && *g_variant_get_string (value, NULL))
        {
          g_free (model);
          model = sw_device_clean_model (g_variant_get_string (value, NULL), strlen (g_variant_get_string (value, NULL)));
          model_done = TRUE;
          maybe_done ();
          return;
        }
    }
  read_device_tree ();
}

static void
on_system_bus (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_finish (res, NULL);

  if (!bus)
    {
      read_device_tree ();
      return;
    }
  g_dbus_connection_call (bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1",
                          "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", "org.freedesktop.hostname1", "HardwareModel"),
                          G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_hostnamed, NULL);
}

/* ---- the architecture ------------------------------------------------------------------------------------------ */

static void
on_dpkg_arch (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autofree char *out = NULL;

  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL, NULL) && out)
    {
      g_strstrip (out);
      if (*out)
        {
          g_free (arch);
          arch = g_steal_pointer (&out);
        }
    }
  arch_done = TRUE;
  maybe_done ();
}

void
sw_device_load (GCallback callback, gpointer data)
{
  g_autoptr (GSubprocess) dpkg = NULL;

  loaded_cb = callback;
  loaded_data = data;
  sw_device_id ();
  if (sw_device_is_loaded ())
    {
      maybe_done ();
      return;
    }
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, on_system_bus, NULL);
  dpkg = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                           "dpkg", "--print-architecture", NULL);
  if (dpkg)
    g_subprocess_communicate_utf8_async (dpkg, NULL, NULL, on_dpkg_arch, NULL);
  else
    {
      arch_done = TRUE;
      maybe_done ();
    }
}
