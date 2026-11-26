/* look: whether the shade blurs (look.h) */
#include "look.h"

#include <json-glib/json-glib.h>

char *
look_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "vibeos", "shade-look.json", NULL);
}

bool
look_blurs (const char *path)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return true;
  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return true;
  JsonObject *o = json_node_get_object (root);
  JsonNode *version = json_object_get_member (o, "version"), *blur = json_object_get_member (o, "blur");
  if (!version || !JSON_NODE_HOLDS_VALUE (version) || json_node_get_value_type (version) != G_TYPE_INT64 ||
      json_node_get_int (version) != 1 || !blur || !JSON_NODE_HOLDS_VALUE (blur) ||
      json_node_get_value_type (blur) != G_TYPE_BOOLEAN)
    return true;
  return json_node_get_boolean (blur);
}
