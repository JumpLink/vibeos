/* What the device has (see ns-caps.h). A modem is any object ModemManager has with its Modem interface, followed
 * through its object manager (a modem that shows up late, or goes, changes the rows); ModemManager is not started for
 * this. A battery is UPower's display device when it is present and a battery. A side button is a key an input device
 * reports as KEY_APPSELECT, in the key bits the kernel gives each input device in sysfs. */
#include "ns-caps.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <vibeos.h>

#define NS_TYPE_CAPS_WATCH (ns_caps_watch_get_type ())
G_DECLARE_FINAL_TYPE (NsCapsWatch, ns_caps_watch, NS, CAPS_WATCH, GObject)

struct _NsCapsWatch {
  GObject             parent;
  NsCap               caps;
  GDBusObjectManager *modems;
  GDBusProxy         *battery;
};

G_DEFINE_FINAL_TYPE (NsCapsWatch, ns_caps_watch, G_TYPE_OBJECT)

enum { PROP_0, PROP_MODEM, PROP_BATTERY, N_PROPS };
static GParamSpec *props[N_PROPS];

static NsCapsWatch *caps;

static void
caps_get_property (GObject *o, guint id, GValue *value, GParamSpec *ps)
{
  NsCapsWatch *self = NS_CAPS_WATCH (o);
  g_value_set_boolean (value, (self->caps & (id == PROP_MODEM ? NS_CAP_MODEM : NS_CAP_BATTERY)) != 0);
}

static void
ns_caps_watch_class_init (NsCapsWatchClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->get_property = caps_get_property;
  props[PROP_MODEM] = g_param_spec_boolean ("modem", NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_BATTERY] = g_param_spec_boolean ("battery", NULL, NULL, FALSE,
                                              G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (oc, N_PROPS, props);
  g_signal_new ("changed", NS_TYPE_CAPS_WATCH, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ns_caps_watch_init (NsCapsWatch *self)
{
}

static void
set (NsCap cap, gboolean on)
{
  NsCap was = caps->caps;
  caps->caps = on ? caps->caps | cap : caps->caps & ~cap;
  if (was == caps->caps)
    return;
  g_object_notify_by_pspec (G_OBJECT (caps), props[cap == NS_CAP_MODEM ? PROP_MODEM : PROP_BATTERY]);
  g_signal_emit_by_name (caps, "changed");
}

/* ---- a modem ---- */

static void
modems_count (void)
{
  gboolean any = FALSE;
  g_autolist (GDBusObject) objects = caps->modems ? g_dbus_object_manager_get_objects (caps->modems) : NULL;
  for (GList *l = objects; l && !any; l = l->next) {
    g_autoptr (GDBusInterface) i = g_dbus_object_get_interface (l->data, "org.freedesktop.ModemManager1.Modem");
    any = i != NULL;
  }
  set (NS_CAP_MODEM, any);
}

static void
modems_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  caps->modems = g_dbus_object_manager_client_new_for_bus_finish (res, &err);
  if (!caps->modems) {
    g_debug ("ModemManager: %s", err->message);
    return;
  }
  /* a modem added or gone, ModemManager started or stopped */
  g_signal_connect_swapped (caps->modems, "object-added", G_CALLBACK (modems_count), NULL);
  g_signal_connect_swapped (caps->modems, "object-removed", G_CALLBACK (modems_count), NULL);
  g_signal_connect_swapped (caps->modems, "interface-added", G_CALLBACK (modems_count), NULL);
  g_signal_connect_swapped (caps->modems, "interface-removed", G_CALLBACK (modems_count), NULL);
  g_signal_connect_swapped (caps->modems, "notify::name-owner", G_CALLBACK (modems_count), NULL);
  modems_count ();
}

/* ---- a battery ---- */

static void
battery_read (void)
{
  g_autoptr (GVariant) present = g_dbus_proxy_get_cached_property (caps->battery, "IsPresent");
  g_autoptr (GVariant) type = g_dbus_proxy_get_cached_property (caps->battery, "Type");
  /* UPower's device type 2 is a battery */
  set (NS_CAP_BATTERY, present && type && g_variant_get_boolean (present) && g_variant_get_uint32 (type) == 2);
}

static void
battery_got (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) err = NULL;
  caps->battery = g_dbus_proxy_new_for_bus_finish (res, &err);
  if (!caps->battery) {
    g_debug ("UPower: %s", err->message);
    return;
  }
  g_signal_connect_swapped (caps->battery, "g-properties-changed", G_CALLBACK (battery_read), NULL);
  battery_read ();
}

/* ---- a side button ---- */

/* whether `code` is among an input device's capabilities/key: hexadecimal longs, the most significant first, the
 * leading zero ones left out */
static gboolean
has_key_bit (char *bits, guint code)
{
  g_auto (GStrv) words = g_strsplit (g_strstrip (bits), " ", -1);
  guint n = g_strv_length (words), per = sizeof (unsigned long) * 8, w = code / per;
  if (w >= n)
    return FALSE;
  return (strtoul (words[n - 1 - w], NULL, 16) >> (code % per)) & 1;
}

static gboolean
side_key_present (void)
{
  g_autoptr (GDir) dir = g_dir_open ("/sys/class/input", 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir))) {
    if (!g_str_has_prefix (name, "input"))
      continue;
    g_autofree char *path = g_build_filename ("/sys/class/input", name, "capabilities", "key", NULL);
    g_autofree char *bits = NULL;
    if (g_file_get_contents (path, &bits, NULL, NULL) && has_key_bit (bits, KEY_APPSELECT))
      return TRUE;
  }
  return FALSE;
}

/* ---- the app's side ---- */

void
ns_caps_init (void)
{
  if (caps)
    return;
  caps = g_object_new (NS_TYPE_CAPS_WATCH, NULL);
  const char *fake = g_getenv ("NS_CAPABILITIES");
  if (fake) {
    g_auto (GStrv) names = g_strsplit (fake, ",", -1);
    caps->caps = (g_strv_contains ((const char *const *) names, "modem") ? NS_CAP_MODEM : 0)
                 | (g_strv_contains ((const char *const *) names, "battery") ? NS_CAP_BATTERY : 0)
                 | (g_strv_contains ((const char *const *) names, "side-key") ? NS_CAP_SIDE_KEY : 0);
    return;
  }
  if (side_key_present ())
    caps->caps |= NS_CAP_SIDE_KEY;
  g_dbus_object_manager_client_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                            "org.freedesktop.ModemManager1", "/org/freedesktop/ModemManager1", NULL,
                                            NULL, NULL, NULL, modems_got, NULL);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.UPower",
                            "/org/freedesktop/UPower/devices/DisplayDevice", "org.freedesktop.UPower.Device", NULL,
                            battery_got, NULL);
}

gboolean
ns_caps_has (NsCap cap)
{
  return caps && (caps->caps & cap) == cap;
}

void
ns_caps_bind_visible (GtkWidget *widget, NsCap cap)
{
  ns_caps_init ();
  g_object_bind_property (caps, cap == NS_CAP_MODEM ? "modem" : "battery", widget, "visible", G_BINDING_SYNC_CREATE);
}

void
ns_caps_on_changed (GCallback cb, gpointer object)
{
  ns_caps_init ();
  g_signal_connect_object (caps, "changed", cb, object, G_CONNECT_SWAPPED);
}

/* ---- phone or tablet ---- */

static gboolean
is_tablet (void)
{
  GApplication *app = g_application_get_default ();
  GtkWindow *win = GTK_IS_APPLICATION (app) ? gtk_application_get_active_window (GTK_APPLICATION (app)) : NULL;
  if (!win) {
    GList *wins = GTK_IS_APPLICATION (app) ? gtk_application_get_windows (GTK_APPLICATION (app)) : NULL;
    win = wins ? wins->data : NULL;
  }
  return win && vibeos_layout_is_tablet (vibeos_layout_get_for_window (win));
}

const char *
ns_device_noun (void)
{
  return is_tablet () ? "tablet" : "phone";
}

const char *
ns_about_title (void)
{
  return is_tablet () ? "About tablet" : "About phone";
}
