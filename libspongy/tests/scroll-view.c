/* SpongyScrollView laid out both ways: a vertical one as it always was, a horizontal one along the width.
 * Needs a display (GDK_BACKEND=broadway with gtk4-broadwayd will do); without one it is skipped. */
#include "spongy-gtk.h"

#include <math.h>
#include <stdio.h>

static int bad;

#define CHECK(cond, ...) do { if (!(cond)) { bad++; printf ("FAILED: " __VA_ARGS__); printf ("\n"); } } while (0)

/* a box of n fixed-size boxes */
static GtkWidget *
strip (GtkOrientation orientation, int n, int w, int h, int spacing)
{
  GtkWidget *box = gtk_box_new (orientation, spacing);
  for (int i = 0; i < n; i++) {
    GtkWidget *tile = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request (tile, w, h);
    gtk_box_append (GTK_BOX (box), tile);
  }
  return box;
}

/* where the child's origin is, in the view */
static graphene_point_t
child_origin (GtkWidget *view, GtkWidget *child)
{
  graphene_point_t p;
  if (!gtk_widget_compute_point (child, view, &GRAPHENE_POINT_INIT (0, 0), &p))
    p = GRAPHENE_POINT_INIT (NAN, NAN);
  return p;
}

static void
vertical (void)
{
  GtkWidget *view = spongy_scroll_view_new ();
  GtkWidget *child = strip (GTK_ORIENTATION_VERTICAL, 10, 100, 100, 0);
  int min, nat;

  g_object_ref_sink (view);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (view), child);
  CHECK (gtk_orientable_get_orientation (GTK_ORIENTABLE (view)) == GTK_ORIENTATION_VERTICAL, "vertical by default");
  CHECK (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (view)) != NULL, "a vertical view has a vadjustment");
  CHECK (spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (view)) == NULL, "and no hadjustment");

  gtk_widget_measure (view, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  CHECK (nat == 100, "vertical: natural width %d, want 100", nat);
  gtk_widget_measure (view, GTK_ORIENTATION_VERTICAL, 100, &min, &nat, NULL, NULL);
  CHECK (min == 0 && nat == 1000, "vertical: height %d..%d, want 0..1000", min, nat);

  gtk_widget_allocate (view, 100, 300, -1, NULL);
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (view));
  CHECK (gtk_adjustment_get_upper (adj) == 1000 && gtk_adjustment_get_page_size (adj) == 300,
         "vertical: adjustment %g of %g, want 300 of 1000", gtk_adjustment_get_page_size (adj),
         gtk_adjustment_get_upper (adj));
  CHECK (gtk_widget_get_height (child) == 1000 && gtk_widget_get_width (child) == 100,
         "vertical: child %dx%d, want 100x1000", gtk_widget_get_width (child), gtk_widget_get_height (child));

  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (view), 250);
  gtk_widget_allocate (view, 100, 300, -1, NULL);
  graphene_point_t p = child_origin (view, child);
  CHECK (p.x == 0 && p.y == -250, "vertical: scrolled 250, child at %g,%g", p.x, p.y);
  CHECK (gtk_adjustment_get_value (adj) == 250, "vertical: adjustment at %g", gtk_adjustment_get_value (adj));

  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (view), 5000);
  CHECK (spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)) == 700, "vertical: past the end, at %g",
         spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)));
  g_object_unref (view);
}

static void
horizontal (void)
{
  GtkWidget *view = g_object_new (SPONGY_TYPE_SCROLL_VIEW, "orientation", GTK_ORIENTATION_HORIZONTAL, NULL);
  GtkWidget *child = strip (GTK_ORIENTATION_HORIZONTAL, 10, 96, 50, 4);
  int min, nat;

  g_object_ref_sink (view);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (view), child);
  CHECK (spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (view)) != NULL,
         "a horizontal view has an hadjustment");
  CHECK (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (view)) == NULL, "and no vadjustment");

  gtk_widget_measure (view, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  CHECK (min == 0 && nat == 996, "horizontal: width %d..%d, want 0..996", min, nat);
  gtk_widget_measure (view, GTK_ORIENTATION_VERTICAL, 300, &min, &nat, NULL, NULL);
  CHECK (min == 50 && nat == 50, "horizontal: height %d..%d, want 50..50", min, nat);

  gtk_widget_allocate (view, 300, 50, -1, NULL);
  GtkAdjustment *adj = spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (view));
  CHECK (gtk_adjustment_get_upper (adj) == 996 && gtk_adjustment_get_page_size (adj) == 300,
         "horizontal: adjustment %g of %g, want 300 of 996", gtk_adjustment_get_page_size (adj),
         gtk_adjustment_get_upper (adj));
  CHECK (gtk_widget_get_width (child) == 996 && gtk_widget_get_height (child) == 50,
         "horizontal: child %dx%d, want 996x50", gtk_widget_get_width (child), gtk_widget_get_height (child));

  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (view), 250);
  gtk_widget_allocate (view, 300, 50, -1, NULL);
  graphene_point_t p = child_origin (view, child);
  CHECK (p.x == -250 && p.y == 0, "horizontal: scrolled 250, child at %g,%g", p.x, p.y);
  CHECK (gtk_adjustment_get_value (adj) == 250, "horizontal: adjustment at %g", gtk_adjustment_get_value (adj));

  /* the adjustment's value scrolls it, as GtkScrolledWindow's does */
  gtk_adjustment_set_value (adj, 100);
  gtk_widget_allocate (view, 300, 50, -1, NULL);
  p = child_origin (view, child);
  CHECK (spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)) == 100 && p.x == -100,
         "horizontal: adjustment set to 100, offset %g, child at %g", spongy_scroll_view_get_offset
         (SPONGY_SCROLL_VIEW (view)), p.x);

  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (view), 5000);
  CHECK (spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)) == 696, "horizontal: past the end, at %g",
         spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)));

  /* a child that could be wider than it needs (a label that ellipsizes): laid out at its minimum width, as
   * GtkScrolledWindow lays it out */
  {
    GtkWidget *wide = spongy_scroll_view_new (), *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *label = gtk_label_new ("a line of text much longer than the column it is in, cut short at its end");
    int cmin, cnat;

    g_object_ref_sink (wide);
    gtk_orientable_set_orientation (GTK_ORIENTABLE (wide), GTK_ORIENTATION_HORIZONTAL);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request (column, 150, 20);
    gtk_box_append (GTK_BOX (column), label);
    spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (wide), column);
    gtk_widget_measure (column, GTK_ORIENTATION_HORIZONTAL, -1, &cmin, &cnat, NULL, NULL);
    gtk_widget_allocate (wide, 100, 20, -1, NULL);
    CHECK (cnat > cmin && gtk_widget_get_width (column) == cmin, "horizontal: a column %d..%d wide laid out %d wide",
           cmin, cnat, gtk_widget_get_width (column));
    g_object_unref (wide);
  }

  /* a strip narrower than the view: laid out at the view's width, nothing to scroll */
  gtk_widget_allocate (view, 1200, 50, -1, NULL);
  CHECK (gtk_widget_get_width (child) == 1200 && spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)) == 0,
         "horizontal, wide view: child %d wide, offset %g", gtk_widget_get_width (child),
         spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)));
  g_object_unref (view);
}

static void
pager (void)
{
  GtkWidget *view = g_object_new (SPONGY_TYPE_SCROLL_VIEW, "orientation", GTK_ORIENTATION_HORIZONTAL, NULL);
  GtkWidget *child = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  g_object_ref_sink (view);
  gtk_box_set_homogeneous (GTK_BOX (child), TRUE);
  for (int i = 0; i < 5; i++) {
    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (page, TRUE);
    gtk_box_append (GTK_BOX (child), page);
  }
  /* each page as wide as the view: the box is given five views' width */
  gtk_widget_set_size_request (child, 5 * 300, 100);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (view), child);
  spongy_scroll_view_set_snap (SPONGY_SCROLL_VIEW (view), 0, TRUE);
  gtk_widget_allocate (view, 300, 100, -1, NULL);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (view), 600);
  gtk_widget_allocate (view, 300, 100, -1, NULL);
  CHECK (spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)) == 600, "pager: on page 3, at %g",
         spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)));
  /* turned: wider pages, the same one showing */
  gtk_widget_set_size_request (child, 5 * 400, 100);
  gtk_widget_allocate (view, 400, 100, -1, NULL);
  CHECK (spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)) == 800, "pager: turned, at %g, want 800",
         spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (view)));
  g_object_unref (view);
}

static void
orientable (void)
{
  GtkWidget *view = spongy_scroll_view_new ();
  GtkWidget *child = strip (GTK_ORIENTATION_HORIZONTAL, 4, 100, 40, 0);
  int min, nat;

  g_object_ref_sink (view);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (view), child);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (view), GTK_ORIENTATION_HORIZONTAL);
  GtkOrientation o;
  g_object_get (view, "orientation", &o, NULL);
  CHECK (o == GTK_ORIENTATION_HORIZONTAL, "turned horizontal, reads %d", o);
  CHECK (spongy_scroll_view_get_child (SPONGY_SCROLL_VIEW (view)) == child, "keeps its child when turned");
  gtk_widget_measure (view, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  CHECK (min == 0 && nat == 400, "turned: width %d..%d, want 0..400", min, nat);
  g_object_unref (view);
}

int
main (void)
{
  if (!gtk_init_check ()) {
    printf ("no display: skipped\n");
    return 77;
  }
  vertical ();
  horizontal ();
  pager ();
  orientable ();
  printf (bad ? "FAILED %d\n" : "ok\n", bad);
  return bad != 0;
}
