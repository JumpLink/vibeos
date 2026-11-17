/* sw-json: reading the store's JSON safely, and the few formats the screens share.
 *
 * Every getter takes a NULL object and a missing, null or wrongly typed member as "none": the API promises every
 * documented field, but a screen should still draw when one is not what it should be. */
#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

const char *sw_json_string  (JsonObject *object, const char *member);
gint64      sw_json_int     (JsonObject *object, const char *member, gint64 fallback);
/* NAN for none */
double      sw_json_double  (JsonObject *object, const char *member);
gboolean    sw_json_bool    (JsonObject *object, const char *member, gboolean fallback);
JsonObject *sw_json_object  (JsonObject *object, const char *member);
JsonArray  *sw_json_array   (JsonObject *object, const char *member);
/* the node's object, or NULL */
JsonObject *sw_json_node_object (JsonNode *node);
JsonArray  *sw_json_node_array  (JsonNode *node);
/* element `index` of `array` as an object, or NULL */
JsonObject *sw_json_array_object (JsonArray *array, guint index);

/* An app's icon URL for a picture `pixels` wide: the 96 one up to 96, else the 192 one (or whichever there is) */
const char *sw_app_icon_url (JsonObject *app, int pixels);
/* the package row of an AppDetail for `arch` (else "all"), or NULL */
JsonObject *sw_app_package_for_arch (JsonObject *detail, const char *arch);

/* "5.6 MB", "820 kB" with a no-break space (GLib's decimal units); "" for 0 */
char       *sw_format_size  (guint64 bytes);
/* "14 Mar 2025" from an ISO-8601 time, or NULL */
char       *sw_format_date  (const char *iso);
/* "4.6" (one decimal, a point whatever the locale) */
char       *sw_format_rating (double rating);
/* "132 ratings", "1 rating", "No ratings yet" */
char       *sw_format_ratings_count (gint64 count);
/* a text's length as the server counts it: Unicode code points, after trimming */
glong       sw_text_length  (const char *text);
/* "a•••@example.org": an address with the name hidden but its first letter */
char       *sw_mask_email   (const char *email);
/* the digits of `text`, at most `max` */
char       *sw_digits_only  (const char *text, gsize max);

G_END_DECLS
