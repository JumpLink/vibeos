/* sw-json: see sw-json.h */
#include "sw-json.h"

#include <math.h>
#include <string.h>

static JsonNode *
member (JsonObject *object, const char *name)
{
  JsonNode *node;

  if (!object || !name)
    return NULL;
  node = json_object_get_member (object, name);
  return node && !JSON_NODE_HOLDS_NULL (node) ? node : NULL;
}

const char *
sw_json_string (JsonObject *object, const char *name)
{
  JsonNode *node = member (object, name);

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
         ? json_node_get_string (node) : NULL;
}

gint64
sw_json_int (JsonObject *object, const char *name, gint64 fallback)
{
  JsonNode *node = member (object, name);

  if (!node || !JSON_NODE_HOLDS_VALUE (node))
    return fallback;
  if (json_node_get_value_type (node) == G_TYPE_INT64)
    return json_node_get_int (node);
  if (json_node_get_value_type (node) == G_TYPE_DOUBLE)
    return (gint64) json_node_get_double (node);
  return fallback;
}

double
sw_json_double (JsonObject *object, const char *name)
{
  JsonNode *node = member (object, name);

  if (!node || !JSON_NODE_HOLDS_VALUE (node))
    return NAN;
  if (json_node_get_value_type (node) == G_TYPE_DOUBLE)
    return json_node_get_double (node);
  if (json_node_get_value_type (node) == G_TYPE_INT64)
    return (double) json_node_get_int (node);
  return NAN;
}

gboolean
sw_json_bool (JsonObject *object, const char *name, gboolean fallback)
{
  JsonNode *node = member (object, name);

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_BOOLEAN
         ? json_node_get_boolean (node) : fallback;
}

JsonObject *
sw_json_object (JsonObject *object, const char *name)
{
  JsonNode *node = member (object, name);

  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

JsonArray *
sw_json_array (JsonObject *object, const char *name)
{
  JsonNode *node = member (object, name);

  return node && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

JsonObject *
sw_json_node_object (JsonNode *node)
{
  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

JsonArray *
sw_json_node_array (JsonNode *node)
{
  return node && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

JsonObject *
sw_json_array_object (JsonArray *array, guint index)
{
  if (!array || index >= json_array_get_length (array))
    return NULL;
  return sw_json_node_object (json_array_get_element (array, index));
}

const char *
sw_app_icon_url (JsonObject *app, int pixels)
{
  JsonObject *icon = sw_json_object (app, "icon");
  const char *small = sw_json_string (icon, "96"), *large = sw_json_string (icon, "192");

  if (pixels <= 96)
    return small ? small : large;
  return large ? large : small;
}

JsonObject *
sw_app_package_for_arch (JsonObject *detail, const char *arch)
{
  JsonArray *packages = sw_json_array (detail, "packages");
  JsonObject *all = NULL;

  for (guint i = 0; packages && i < json_array_get_length (packages); i++)
    {
      JsonObject *p = sw_json_array_object (packages, i);
      const char *a = sw_json_string (p, "arch");

      if (arch && !g_strcmp0 (a, arch))
        return p;
      if (!g_strcmp0 (a, "all"))
        all = p;
    }
  return all;
}

char *
sw_format_size (guint64 bytes)
{
  if (!bytes)
    return g_strdup ("");
  return g_format_size (bytes);
}

char *
sw_format_date (const char *iso)
{
  g_autoptr (GDateTime) time = iso ? g_date_time_new_from_iso8601 (iso, NULL) : NULL;
  static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
                                  "Dec" };

  if (!time)
    return NULL;
  /* the month in English whatever the locale: the store's texts are English */
  return g_strdup_printf ("%d %s %d", g_date_time_get_day_of_month (time),
                          months[g_date_time_get_month (time) - 1], g_date_time_get_year (time));
}

char *
sw_format_rating (double rating)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  if (isnan (rating))
    return g_strdup ("");
  return g_strdup (g_ascii_formatd (buffer, sizeof buffer, "%.1f", rating));
}

char *
sw_format_ratings_count (gint64 count)
{
  if (count <= 0)
    return g_strdup ("No ratings yet");
  if (count == 1)
    return g_strdup ("1 rating");
  return g_strdup_printf ("%" G_GINT64_FORMAT " ratings", count);
}

glong
sw_text_length (const char *text)
{
  g_autofree char *copy = g_strstrip (g_strdup (text ? text : ""));

  return g_utf8_strlen (copy, -1);
}

char *
sw_mask_email (const char *email)
{
  const char *at = email ? strchr (email, '@') : NULL;
  g_autofree char *first = NULL;

  if (!at || at == email)
    return g_strdup (email ? email : "");
  first = g_utf8_substring (email, 0, 1);
  return g_strdup_printf ("%s•••%s", first, at);
}

char *
sw_digits_only (const char *text, gsize max)
{
  GString *digits = g_string_new (NULL);

  for (const char *p = text; p && *p && digits->len < max; p++)
    if (g_ascii_isdigit (*p))
      g_string_append_c (digits, *p);
  return g_string_free (digits, FALSE);
}
