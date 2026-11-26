/* look: whether the shade blurs, which a tablet's drawer sheet follows. extension/shadelook.js measures whether the
 * GPU holds the frame rate under the shade's blur and keeps its verdict in shade-look.json, in the user's state
 * directory: version 1, {"version": 1, "blur": true or false, …}. No file, or one not understood: the blur, which the
 * shade starts with. */
#pragma once

#include <stdbool.h>

char *look_path   (void);                  /* the user's file; g_free it */
bool  look_blurs  (const char *path);
