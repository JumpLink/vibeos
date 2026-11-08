/* Messaging's bitmaps at their size in dp (msg-image.h). */
#include "msg-image.h"

/* a texture that says it is @width × @height: GtkPicture's natural size is its paintable's intrinsic one */
#define MSG_TYPE_SIZED_PAINTABLE (msg_sized_paintable_get_type ())
G_DECLARE_FINAL_TYPE (MsgSizedPaintable, msg_sized_paintable, MSG, SIZED_PAINTABLE, GObject)

struct _MsgSizedPaintable {
  GObject parent;
  GdkPaintable *image;
  int width, height;
};

static void
msg_sized_paintable_snapshot (GdkPaintable *paintable, GdkSnapshot *snapshot, double width, double height)
{
  gdk_paintable_snapshot (MSG_SIZED_PAINTABLE (paintable)->image, snapshot, width, height);
}

static int
msg_sized_paintable_get_intrinsic_width (GdkPaintable *paintable)
{
  return MSG_SIZED_PAINTABLE (paintable)->width;
}

static int
msg_sized_paintable_get_intrinsic_height (GdkPaintable *paintable)
{
  return MSG_SIZED_PAINTABLE (paintable)->height;
}

static void
msg_sized_paintable_iface_init (GdkPaintableInterface *iface)
{
  iface->snapshot = msg_sized_paintable_snapshot;
  iface->get_intrinsic_width = msg_sized_paintable_get_intrinsic_width;
  iface->get_intrinsic_height = msg_sized_paintable_get_intrinsic_height;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (MsgSizedPaintable, msg_sized_paintable, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE, msg_sized_paintable_iface_init))

static void
msg_sized_paintable_dispose (GObject *object)
{
  g_clear_object (&MSG_SIZED_PAINTABLE (object)->image);
  G_OBJECT_CLASS (msg_sized_paintable_parent_class)->dispose (object);
}

static void
msg_sized_paintable_class_init (MsgSizedPaintableClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = msg_sized_paintable_dispose;
}

static void
msg_sized_paintable_init (MsgSizedPaintable *self)
{
}

GtkWidget *
msg_image_new (const char *resource, int width, int height)
{
  MsgSizedPaintable *sized = g_object_new (MSG_TYPE_SIZED_PAINTABLE, NULL);
  GtkWidget *picture;

  sized->image = GDK_PAINTABLE (gdk_texture_new_from_resource (resource));
  sized->width = width;
  sized->height = height;
  picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (sized));
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
  g_object_unref (sized);
  return picture;
}
