/* Pictures into textures (see image.h). */
#include "image.h"

#include <cairo.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <string.h>

/* ---- pixbufs and textures ------------------------------------------------------------------------------------- */

GdkPixbuf *
image_load (const char *path, int w, int h, gboolean cover)
{
  int pw = 0, ph = 0;
  if (!gdk_pixbuf_get_file_info (path, &pw, &ph) || pw <= 0 || ph <= 0)
    return NULL;
  if (!cover)
    return gdk_pixbuf_new_from_file_at_scale (path, w, h, FALSE, NULL);
  /* the smallest scale that covers w × h, then the middle of it */
  double s = MAX ((double) w / pw, (double) h / ph);
  int sw = MAX (w, (int) ceil (pw * s)), sh = MAX (h, (int) ceil (ph * s));
  GdkPixbuf *big = gdk_pixbuf_new_from_file_at_scale (path, sw, sh, FALSE, NULL);
  if (!big)
    return NULL;
  sw = gdk_pixbuf_get_width (big);
  sh = gdk_pixbuf_get_height (big);
  GdkPixbuf *out = gdk_pixbuf_new_subpixbuf (big, MAX (0, (sw - w) / 2), MAX (0, (sh - h) / 2), MIN (w, sw),
                                             MIN (h, sh));
  GdkPixbuf *copy = gdk_pixbuf_copy (out);
  g_object_unref (out);
  g_object_unref (big);
  return copy;
}

GfxTex *
image_texture (GdkPixbuf *pb)
{
  int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  int n = gdk_pixbuf_get_n_channels (pb), stride = gdk_pixbuf_get_rowstride (pb);
  gboolean alpha = gdk_pixbuf_get_has_alpha (pb);
  const guint8 *src = gdk_pixbuf_read_pixels (pb);
  guint8 *bgra = g_malloc ((gsize) w * h * 4);
  for (int y = 0; y < h; y++) {
    const guint8 *s = src + (gsize) y * stride;
    guint8 *d = bgra + (gsize) y * w * 4;
    for (int x = 0; x < w; x++, s += n, d += 4) {
      guint a = alpha ? s[3] : 255;
      d[0] = (s[2] * a + 127) / 255;
      d[1] = (s[1] * a + 127) / 255;
      d[2] = (s[0] * a + 127) / 255;
      d[3] = a;
    }
  }
  GfxTex *t = gfx_tex_from_bgra (w, h, bgra, w * 4);
  g_free (bgra);
  return t;
}

/* ---- the icon theme ------------------------------------------------------------------------------------------- */

typedef enum { DIR_FIXED, DIR_SCALABLE, DIR_THRESHOLD } DirType;

typedef struct {
  char      *sub;           /* "48x48/apps" */
  int        size, scale, min, max, threshold;
  DirType    type;
  GPtrArray *files;         /* GHashTable* name -> extension, one per base dir, read on first use */
} IconDir;

typedef struct {
  char      *name;
  GPtrArray *bases;         /* the theme's directories under every icons root */
  GPtrArray *dirs;          /* IconDir* */
  char     **inherits;
} Theme;

static GHashTable *themes;     /* name -> Theme* (NULL: there is no such theme) */
static GPtrArray  *chain;      /* Theme*: the theme, what it inherits, hicolor */
static GHashTable *path_cache; /* "name@px" -> path or "" */

static GPtrArray *
icon_roots (void)
{
  GPtrArray *r = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (r, g_build_filename (g_get_home_dir (), ".icons", NULL));
  g_ptr_array_add (r, g_build_filename (g_get_user_data_dir (), "icons", NULL));
  for (const char *const *d = g_get_system_data_dirs (); *d; d++)
    g_ptr_array_add (r, g_build_filename (*d, "icons", NULL));
  return r;
}

static void
dir_free (gpointer p)
{
  IconDir *d = p;
  g_free (d->sub);
  g_ptr_array_unref (d->files);
  g_free (d);
}

static Theme *
theme_load (const char *name)
{
  if (!themes)
    themes = g_hash_table_new (g_str_hash, g_str_equal);
  gpointer found;
  if (g_hash_table_lookup_extended (themes, name, NULL, &found))
    return found;
  Theme *t = g_new0 (Theme, 1);
  t->name = g_strdup (name);
  t->bases = g_ptr_array_new_with_free_func (g_free);
  t->dirs = g_ptr_array_new_with_free_func (dir_free);
  GKeyFile *kf = NULL;
  g_autoptr (GPtrArray) roots = icon_roots ();
  for (guint i = 0; i < roots->len; i++) {
    char *base = g_build_filename (roots->pdata[i], name, NULL);
    if (!g_file_test (base, G_FILE_TEST_IS_DIR)) {
      g_free (base);
      continue;
    }
    g_ptr_array_add (t->bases, base);
    if (!kf) {
      char *index = g_build_filename (base, "index.theme", NULL);
      kf = g_key_file_new ();
      g_key_file_set_list_separator (kf, ',');      /* the theme spec's lists, not GKeyFile's ';' */
      if (!g_key_file_load_from_file (kf, index, G_KEY_FILE_NONE, NULL))
        g_clear_pointer (&kf, g_key_file_free);
      g_free (index);
    }
  }
  if (!kf) {
    g_ptr_array_unref (t->bases);
    g_ptr_array_unref (t->dirs);
    g_free (t->name);
    g_free (t);
    g_hash_table_insert (themes, g_strdup (name), NULL);
    return NULL;
  }
  t->inherits = g_key_file_get_string_list (kf, "Icon Theme", "Inherits", NULL, NULL);
  g_auto (GStrv) dirs = g_key_file_get_string_list (kf, "Icon Theme", "Directories", NULL, NULL);
  g_auto (GStrv) scaled = g_key_file_get_string_list (kf, "Icon Theme", "ScaledDirectories", NULL, NULL);
  for (int pass = 0; pass < 2; pass++) {
    GStrv list = pass ? scaled : dirs;
    for (int i = 0; list && list[i]; i++) {
      const char *s = list[i];
      if (!g_key_file_has_group (kf, s))
        continue;
      IconDir *d = g_new0 (IconDir, 1);
      d->sub = g_strdup (s);
      d->size = g_key_file_get_integer (kf, s, "Size", NULL);
      d->scale = MAX (1, g_key_file_get_integer (kf, s, "Scale", NULL));
      g_autofree char *type = g_key_file_get_string (kf, s, "Type", NULL);
      d->type = !g_strcmp0 (type, "Fixed") ? DIR_FIXED : !g_strcmp0 (type, "Scalable") ? DIR_SCALABLE : DIR_THRESHOLD;
      d->min = g_key_file_has_key (kf, s, "MinSize", NULL) ? g_key_file_get_integer (kf, s, "MinSize", NULL) : d->size;
      d->max = g_key_file_has_key (kf, s, "MaxSize", NULL) ? g_key_file_get_integer (kf, s, "MaxSize", NULL) : d->size;
      d->threshold = g_key_file_has_key (kf, s, "Threshold", NULL) ? g_key_file_get_integer (kf, s, "Threshold", NULL)
                                                                    : 2;
      d->files = g_ptr_array_new_full (t->bases->len, (GDestroyNotify) g_hash_table_unref);
      for (guint b = 0; b < t->bases->len; b++)
        g_ptr_array_add (d->files, NULL);
      g_ptr_array_add (t->dirs, d);
    }
  }
  g_key_file_free (kf);
  g_hash_table_insert (themes, g_strdup (name), t);
  return t;
}

static void
chain_add (const char *name, int depth)
{
  if (depth > 8)
    return;
  for (guint i = 0; i < chain->len; i++)
    if (!g_strcmp0 (((Theme *) chain->pdata[i])->name, name))
      return;
  Theme *t = theme_load (name);
  if (!t)
    return;
  g_ptr_array_add (chain, t);
  for (int i = 0; t->inherits && t->inherits[i]; i++)
    chain_add (t->inherits[i], depth + 1);
}

static void
chain_build (void)
{
  chain = g_ptr_array_new ();
  g_autoptr (GSettingsSchema) schema =
      g_settings_schema_source_lookup (g_settings_schema_source_get_default (), "org.gnome.desktop.interface", TRUE);
  char *name = NULL;
  if (schema) {
    GSettings *s = g_settings_new ("org.gnome.desktop.interface");
    name = g_settings_get_string (s, "icon-theme");
    g_object_unref (s);
  }
  if (name && *name)
    chain_add (name, 0);
  g_free (name);
  chain_add ("hicolor", 0);
}

/* the names in one theme directory, read once: name -> "png" / "svg" */
static GHashTable *
dir_files (Theme *t, IconDir *d, guint base)
{
  GHashTable *h = d->files->pdata[base];
  if (h)
    return h;
  h = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  char *path = g_build_filename (t->bases->pdata[base], d->sub, NULL);
  GDir *dir = g_dir_open (path, 0, NULL);
  if (dir) {
    const char *f;
    while ((f = g_dir_read_name (dir))) {
      const char *dot = strrchr (f, '.');
      if (!dot)
        continue;
      const char *ext = !strcmp (dot, ".png") ? "png" : !strcmp (dot, ".svg") ? "svg" : NULL;
      if (!ext)
        continue;
      char *n = g_strndup (f, dot - f);
      const char *had = g_hash_table_lookup (h, n);
      if (!had || !strcmp (ext, "svg"))         /* an SVG draws sharp at any size */
        g_hash_table_replace (h, n, (gpointer) ext);
      else
        g_free (n);
    }
    g_dir_close (dir);
  }
  g_free (path);
  d->files->pdata[base] = h;
  return h;
}

static int
dir_distance (IconDir *d, int px)
{
  int size = px;
  switch (d->type) {
  case DIR_FIXED:
    return abs (d->size * d->scale - size);
  case DIR_SCALABLE:
    if (size < d->min * d->scale)
      return d->min * d->scale - size;
    if (size > d->max * d->scale)
      return size - d->max * d->scale;
    return 0;
  default:
    if (size < (d->size - d->threshold) * d->scale)
      return d->min * d->scale - size;
    if (size > (d->size + d->threshold) * d->scale)
      return size - d->max * d->scale;
    return 0;
  }
}

static char *
theme_lookup (Theme *t, const char *name, int px)
{
  char *best = NULL;
  int best_d = G_MAXINT;
  gboolean best_big = FALSE;
  for (guint i = 0; i < t->dirs->len; i++) {
    IconDir *d = t->dirs->pdata[i];
    for (guint b = 0; b < t->bases->len; b++) {
      const char *ext = g_hash_table_lookup (dir_files (t, d, b), name);
      if (!ext)
        continue;
      int dist = dir_distance (d, px);
      /* between two the same way off, the bigger one, which scales down sharper */
      gboolean big = d->size * d->scale >= px || !strcmp (ext, "svg");
      if (dist < best_d || (dist == best_d && big && !best_big)) {
        g_free (best);
        best = g_strdup_printf ("%s/%s/%s.%s", (char *) t->bases->pdata[b], d->sub, name, ext);
        best_d = dist;
        best_big = big;
      }
      if (dist == 0 && big)
        return best;
    }
  }
  return best;
}

char *
image_icon_path (const char *name, int px)
{
  if (!chain)
    chain_build ();
  if (!path_cache)
    path_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  char *key = g_strdup_printf ("%s@%d", name, px);
  const char *cached = g_hash_table_lookup (path_cache, key);
  if (cached) {
    g_free (key);
    return *cached ? g_strdup (cached) : NULL;
  }
  char *path = NULL;
  for (guint i = 0; i < chain->len && !path; i++)
    path = theme_lookup (chain->pdata[i], name, px);
  for (const char *const *e = (const char *const[]) { "png", "svg", NULL }; !path && *e; e++) {
    char *p = g_strdup_printf ("/usr/share/pixmaps/%s.%s", name, *e);
    if (g_file_test (p, G_FILE_TEST_EXISTS))
      path = p;
    else
      g_free (p);
  }
  g_hash_table_insert (path_cache, key, g_strdup (path ? path : ""));
  return path;
}

void
image_theme_reset (void)
{
  g_clear_pointer (&chain, g_ptr_array_unref);
  g_clear_pointer (&path_cache, g_hash_table_unref);
}

/* ---- icons ---------------------------------------------------------------------------------------------------- */

static GdkPixbuf *
svg_render (const char *path, int px)
{
  RsvgHandle *h = rsvg_handle_new_from_file (path, NULL);
  if (!h)
    return NULL;
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  cairo_t *cr = cairo_create (s);
  RsvgRectangle vp = { 0, 0, px, px };
  rsvg_handle_render_document (h, cr, &vp, NULL);
  cairo_destroy (cr);
  g_object_unref (h);
  cairo_surface_flush (s);
  /* cairo's premultiplied BGRA back to a straight RGBA pixbuf */
  GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, px, px);
  const guint8 *src = cairo_image_surface_get_data (s);
  int ss = cairo_image_surface_get_stride (s), ds = gdk_pixbuf_get_rowstride (pb);
  guint8 *dst = gdk_pixbuf_get_pixels (pb);
  for (int y = 0; y < px; y++)
    for (int x = 0; x < px; x++) {
      const guint8 *p = src + (gsize) y * ss + x * 4;
      guint8 *d = dst + (gsize) y * ds + x * 4;
      guint a = p[3];
      d[0] = a ? MIN (255u, (p[2] * 255u + a / 2) / a) : 0;
      d[1] = a ? MIN (255u, (p[1] * 255u + a / 2) / a) : 0;
      d[2] = a ? MIN (255u, (p[0] * 255u + a / 2) / a) : 0;
      d[3] = a;
    }
  cairo_surface_destroy (s);
  return pb;
}

static GdkPixbuf *
file_icon (const char *path, int px)
{
  if (g_str_has_suffix (path, ".svg") || g_str_has_suffix (path, ".svgz"))
    return svg_render (path, px);
  GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale (path, px, px, TRUE, NULL);
  if (!pb)
    return NULL;
  int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
  if (w == px && h == px)
    return pb;
  /* not square: centred on a px × px canvas */
  GdkPixbuf *sq = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, px, px);
  gdk_pixbuf_fill (sq, 0);
  GdkPixbuf *rgba = gdk_pixbuf_get_has_alpha (pb) ? g_object_ref (pb) : gdk_pixbuf_add_alpha (pb, FALSE, 0, 0, 0);
  gdk_pixbuf_copy_area (rgba, 0, 0, w, h, sq, (px - w) / 2, (px - h) / 2);
  g_object_unref (rgba);
  g_object_unref (pb);
  return sq;
}

GdkPixbuf *
image_icon (GIcon *icon, int px)
{
  if (!icon)
    return NULL;
  if (G_IS_FILE_ICON (icon)) {
    char *path = g_file_get_path (g_file_icon_get_file (G_FILE_ICON (icon)));
    GdkPixbuf *pb = path ? file_icon (path, px) : NULL;
    g_free (path);
    return pb;
  }
  if (G_IS_THEMED_ICON (icon)) {
    const char *const *names = g_themed_icon_get_names (G_THEMED_ICON (icon));
    for (int i = 0; names && names[i]; i++) {
      if (g_path_is_absolute (names[i]))
        return file_icon (names[i], px);
      char *path = image_icon_path (names[i], px);
      if (path) {
        GdkPixbuf *pb = file_icon (path, px);
        g_free (path);
        if (pb)
          return pb;
      }
    }
  }
  return NULL;
}
