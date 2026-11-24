/* navi-mvt: see navi-mvt.h. The format: https://github.com/mapbox/vector-tile-spec (2.1). */
#include "navi-mvt.h"

#include <math.h>
#include <string.h>

typedef struct {
  const guint8 *p, *end;
  gboolean bad;
} Reader;

static guint64
varint (Reader *r)
{
  guint64 v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (r->p >= r->end) {
      r->bad = TRUE;
      return 0;
    }
    guint8 b = *r->p++;
    v |= (guint64) (b & 0x7f) << shift;
    if (!(b & 0x80))
      return v;
  }
  r->bad = TRUE;
  return 0;
}

/* the next field: its number, wire type, and for length-delimited ones the bytes */
static gboolean
next_field (Reader *r, guint *field, guint *wire, Reader *sub, guint64 *value)
{
  if (r->p >= r->end || r->bad)
    return FALSE;
  guint64 key = varint (r);
  *field = key >> 3;
  *wire = key & 7;
  switch (*wire) {
  case 0:
    *value = varint (r);
    break;
  case 1:
    if (r->end - r->p < 8)
      r->bad = TRUE;
    else {
      memcpy (value, r->p, 8);
      r->p += 8;
    }
    break;
  case 2: {
    guint64 n = varint (r);
    if (n > (guint64) (r->end - r->p))
      r->bad = TRUE;
    else {
      sub->p = r->p;
      sub->end = r->p + n;
      sub->bad = FALSE;
      r->p += n;
    }
    break;
  }
  case 5:
    if (r->end - r->p < 4)
      r->bad = TRUE;
    else {
      guint32 v32;
      memcpy (&v32, r->p, 4);
      *value = v32;
      r->p += 4;
    }
    break;
  default:
    r->bad = TRUE;
  }
  return !r->bad;
}

static gint64
zigzag (guint64 v)
{
  return (gint64) (v >> 1) ^ -(gint64) (v & 1);
}

typedef struct {
  GPtrArray *keys;   /* char* */
  GPtrArray *values; /* char*, the value as text */
} Layer;

struct _NaviMvtFeature {
  Layer *layer;
  GArray *tags;   /* guint32 pairs */
  NaviMvtType type;
  GArray *points; /* int pairs */
  GArray *parts;  /* guint */
  guint extent;
};

const char *
navi_mvt_feature_get (const NaviMvtFeature *f, const char *key)
{
  for (guint i = 0; i + 1 < f->tags->len; i += 2) {
    guint k = g_array_index (f->tags, guint32, i), v = g_array_index (f->tags, guint32, i + 1);
    if (k < f->layer->keys->len && v < f->layer->values->len && !strcmp (f->layer->keys->pdata[k], key))
      return f->layer->values->pdata[v];
  }
  return NULL;
}

NaviMvtType
navi_mvt_feature_type (const NaviMvtFeature *f)
{
  return f->type;
}

const int *
navi_mvt_feature_points (const NaviMvtFeature *f, guint *n, const guint **parts, guint *n_parts)
{
  *n = f->points->len / 2;
  if (parts)
    *parts = (const guint *) f->parts->data;
  if (n_parts)
    *n_parts = f->parts->len ? f->parts->len - 1 : 0;
  return (const int *) f->points->data;
}

guint
navi_mvt_feature_extent (const NaviMvtFeature *f)
{
  return f->extent;
}

static char *
value_text (Reader *r)
{
  guint field, wire;
  guint64 v = 0;
  Reader sub;
  char *text = NULL;
  while (next_field (r, &field, &wire, &sub, &v)) {
    g_free (text);
    switch (field) {
    case 1: text = g_strndup ((const char *) sub.p, sub.end - sub.p); break;
    case 2: {
      float f;
      guint32 v32 = (guint32) v;
      memcpy (&f, &v32, 4);
      text = g_strdup_printf ("%g", f);
      break;
    }
    case 3: {
      double d;
      memcpy (&d, &v, 8);
      text = g_strdup_printf ("%g", d);
      break;
    }
    case 4: text = g_strdup_printf ("%" G_GINT64_FORMAT, (gint64) v); break;
    case 5: text = g_strdup_printf ("%" G_GUINT64_FORMAT, v); break;
    case 6: text = g_strdup_printf ("%" G_GINT64_FORMAT, zigzag (v)); break;
    case 7: text = g_strdup (v ? "true" : "false"); break;
    default: text = NULL;
    }
  }
  return text ? text : g_strdup ("");
}

static void
decode_geometry (Reader *r, NaviMvtFeature *f)
{
  int x = 0, y = 0;
  g_array_set_size (f->points, 0);
  g_array_set_size (f->parts, 0);
  while (r->p < r->end && !r->bad) {
    guint32 cmd = (guint32) varint (r);
    guint id = cmd & 7, count = cmd >> 3;
    if (id == 1 || id == 2) {
      if (id == 1) {
        guint start = f->points->len / 2;
        g_array_append_val (f->parts, start);
      }
      for (guint i = 0; i < count && !r->bad; i++) {
        x += zigzag (varint (r));
        y += zigzag (varint (r));
        int pt[2] = { x, y };
        g_array_append_vals (f->points, pt, 2);
      }
    } else if (id != 7) {
      break; /* not a command: give up on this geometry */
    }
  }
  guint end = f->points->len / 2;
  g_array_append_val (f->parts, end);
}

static void
read_layer (Reader *lr, const char *const *wanted, NaviMvtFunc func, gpointer data)
{
  /* the name, keys and values can come after the features: a first pass for them */
  Reader r = *lr;
  guint field, wire;
  guint64 v;
  Reader sub;
  g_autofree char *name = NULL;
  guint extent = 4096;
  Layer layer = { g_ptr_array_new_with_free_func (g_free), g_ptr_array_new_with_free_func (g_free) };
  while (next_field (&r, &field, &wire, &sub, &v)) {
    if (field == 1 && wire == 2)
      name = g_strndup ((const char *) sub.p, sub.end - sub.p);
    else if (field == 3 && wire == 2)
      g_ptr_array_add (layer.keys, g_strndup ((const char *) sub.p, sub.end - sub.p));
    else if (field == 4 && wire == 2)
      g_ptr_array_add (layer.values, value_text (&sub));
    else if (field == 5 && wire == 0)
      extent = (guint) v;
  }
  gboolean want = !wanted;
  for (guint i = 0; wanted && wanted[i] && name; i++)
    if (!strcmp (wanted[i], name))
      want = TRUE;
  if (want && name) {
    NaviMvtFeature f = { &layer, g_array_new (FALSE, FALSE, sizeof (guint32)), 0,
                         g_array_new (FALSE, FALSE, sizeof (int)), g_array_new (FALSE, FALSE, sizeof (guint)), extent };
    r = *lr;
    while (next_field (&r, &field, &wire, &sub, &v)) {
      if (field != 2 || wire != 2)
        continue;
      g_array_set_size (f.tags, 0);
      g_array_set_size (f.points, 0);
      g_array_set_size (f.parts, 0);
      f.type = NAVI_MVT_UNKNOWN;
      guint ff, fw;
      guint64 fv;
      Reader fs;
      while (next_field (&sub, &ff, &fw, &fs, &fv)) {
        if (ff == 2 && fw == 2) {
          while (fs.p < fs.end && !fs.bad) {
            guint32 t = (guint32) varint (&fs);
            g_array_append_val (f.tags, t);
          }
        } else if (ff == 3 && fw == 0) {
          f.type = (NaviMvtType) fv;
        } else if (ff == 4 && fw == 2) {
          decode_geometry (&fs, &f);
        }
      }
      if (!sub.bad)
        func (name, &f, data);
    }
    g_array_unref (f.tags);
    g_array_unref (f.points);
    g_array_unref (f.parts);
  }
  g_ptr_array_unref (layer.keys);
  g_ptr_array_unref (layer.values);
}

gboolean
navi_mvt_read (const guint8 *data, gsize length, const char *const *layers, NaviMvtFunc func, gpointer user_data)
{
  Reader r = { data, data + length, FALSE };
  guint field, wire;
  guint64 v;
  Reader sub;
  gboolean any = FALSE;
  while (next_field (&r, &field, &wire, &sub, &v)) {
    if (field == 3 && wire == 2) {
      any = TRUE;
      read_layer (&sub, layers, func, user_data);
    }
  }
  return any && !r.bad;
}

void
navi_mvt_to_lat_lng (int z, int x, int y, guint extent, double px, double py, double *lat, double *lng)
{
  double n = exp2 (z);
  double wx = (x + px / extent) / n, wy = (y + py / extent) / n;
  *lng = wx * 360 - 180;
  *lat = atan (sinh (G_PI * (1 - 2 * wy))) * 180 / G_PI;
}

void
navi_mvt_tile_at (int z, double lat, double lng, int *x, int *y)
{
  double n = exp2 (z), r = CLAMP (lat, -85.0511, 85.0511) * G_PI / 180;
  *x = (int) floor ((lng + 180) / 360 * n);
  *y = (int) floor ((1 - log (tan (r) + 1 / cos (r)) / G_PI) / 2 * n);
  *x = CLAMP (*x, 0, (int) n - 1);
  *y = CLAMP (*y, 0, (int) n - 1);
}
