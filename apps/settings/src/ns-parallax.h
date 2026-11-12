/* The parallax wallpapers Neo can show: every directory with a parallax.json in /usr/share/neo-launcher/parallax and
 * ~/.local/share/neo-launcher/parallax, as a grid of previews. A set is GNOME's wallpaper (org.gnome.desktop.background)
 * set to the picture in its folder, which the launcher plays as that set. */
#pragma once

#include <gtk/gtk.h>

GtkWidget *ns_parallax_grid (GSettings *bg, GSettings *iface);
