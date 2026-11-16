/* icon-lookup NAME…: what the launcher's icon theme lookup finds at 156 px (cc icon-lookup.c ../src/image.c, no GPU) */
#include <stdio.h>
#include <stdlib.h>
#include "image.h"

GfxTex *gfx_tex_from_bgra (int w, int h, const void *pixels, int stride) { return NULL; }

int
main (int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    char *p = image_icon_path (argv[i], 156);
    printf ("%s -> %s\n", argv[i], p ? p : "(none)");
    g_free (p);
  }
  return 0;
}
