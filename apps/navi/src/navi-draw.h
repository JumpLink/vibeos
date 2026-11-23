/* navi-draw: Navi's own pictures, drawn with cairo: the turn arrows of the banner, the car, the pins, and the
 * icons that have no symbolic icon in the theme (re-centre, routes, compass). All of it in a box `size` wide,
 * its top left at the cairo origin. */
#pragma once

#include <gtk/gtk.h>

#include "navi-route.h"

G_BEGIN_DECLS

/* the turn arrow for a manoeuvre, in `color` (the banner's white); a roundabout shows its exit number */
void navi_draw_turn (cairo_t *cr, double size, NaviTurn type, int roundabout_exit, const GdkRGBA *color);
/* the car: an arrow pointing up, turned by `rotation` degrees, flattened to `squash` of its height (a car lying on a
 * tilted map), on its soft shadow */
void navi_draw_car (cairo_t *cr, double size, double rotation, double squash, gboolean night);
/* the destination's pin, its point at the bottom middle */
void navi_draw_pin (cairo_t *cr, double width, double height);
/* the finish flag at a route's end (a chequered disc) */
void navi_draw_finish (cairo_t *cr, double size);
/* the re-centre crosshair */
void navi_draw_crosshair (cairo_t *cr, double size, const GdkRGBA *color);
/* two roads forking: the routes */
void navi_draw_routes (cairo_t *cr, double size, const GdkRGBA *color);
/* the compass needle, north turned to `bearing` degrees clockwise on the screen */
void navi_draw_compass (cairo_t *cr, double size, double bearing, const GdkRGBA *color);

/* A GtkDrawingArea that draws one of these at its size: the turn, the routes, the crosshair */
typedef enum {
  NAVI_ICON_TURN,
  NAVI_ICON_CROSSHAIR,
  NAVI_ICON_ROUTES,
  NAVI_ICON_FINISH,
  NAVI_ICON_FUEL,    /* a fuel pump */
  NAVI_ICON_PARKING, /* a P on a sign */
  NAVI_ICON_WORK,    /* a briefcase */
  NAVI_ICON_PIN,     /* the destination's pin */
} NaviIconKind;

GtkWidget *navi_icon_new (NaviIconKind kind, int size);
/* a turn icon's manoeuvre */
void navi_icon_set_turn (GtkWidget *icon, NaviTurn type, int roundabout_exit);
/* its colour (white unless set) */
void navi_icon_set_color (GtkWidget *icon, const GdkRGBA *color);

G_END_DECLS
