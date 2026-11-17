/* vibeos-original-colors: the theme's colours as Material 3's original colour rules (2021) give them.
 *
 * Some apps draw part of themselves by the rules Material 3 started with rather than the current ones: the same
 * seed, style, contrast and light or dark as the theme, each role at its original tone, the accents in the theme's
 * palettes and the surfaces in the original neutral ones. In dark mode that makes the surfaces a little lighter and
 * greyer and the text a little brighter than the theme's (a surface at tone 6 rather than 4, its containers at 10,
 * 12, 17 and 22), and the primary and tertiary containers deeper; in light mode the containers a touch bluer. A messaging app's conversation, drawn in these, reads as a calmer page than its teal list.
 *
 *   GdkRGBA bubble = vibeos_original_color (VIBEOS_SURFACE_CONTAINER);
 *
 * Stylesheets use them as @vibeos_original_surface, @vibeos_original_on_surface_variant, and so on (every role of
 * VibeosColor, "vibeos_" replaced by "vibeos_original_"), once vibeos_original_colors_init () has run. They follow
 * the theme: read them as you draw, as with vibeos_color (). The fixed colours (VIBEOS_CALL ...) are the same as
 * vibeos_color () gives. */
#pragma once

#include <gtk/gtk.h>

#include "vibeos-style.h"

G_BEGIN_DECLS

GdkRGBA vibeos_original_color       (VibeosColor color);
GdkRGBA vibeos_original_color_alpha (VibeosColor color, float alpha);

/* defines the @vibeos_original_* colours for the whole display and keeps them up to date with the theme. Call after
 * vibeos_init; safe to call more than once */
void    vibeos_original_colors_init (void);

G_END_DECLS
