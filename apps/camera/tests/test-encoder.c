/* The encoder is read from the cache and never probed unless asked: opening the app must not touch the hardware
 * encoder (the OnePlus 6T's Venus driver hangs in its STREAMOFF). */
#include "cam-encoder.h"

#include <gst/gst.h>
#include <sys/utsname.h>

static void
test_no_cache_no_probe (void)
{
  g_assert_null (cam_encoder_get ());
  g_assert_null (cam_encoder_get ());
  g_assert_cmpuint (cam_encoder_probes_started (), ==, 0);
}

static void
test_cache_hit_no_probe (void)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "vibeos-camera", NULL);
  g_autofree char *path = g_build_filename (dir, "encoder.ini", NULL);
  g_autofree char *key = NULL;
  const CamEncoder *encoder;
  struct utsname u;

  uname (&u);
  key = g_strdup_printf ("%s %s", u.release, gst_version_string ());
  g_key_file_set_string (file, "encoder", "key", key);
  g_key_file_set_string (file, "encoder", "element", "x264enc");
  g_key_file_set_boolean (file, "encoder", "hd", TRUE);
  g_key_file_set_boolean (file, "encoder", "fhd", FALSE);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_key_file_save_to_file (file, path, NULL));

  encoder = cam_encoder_get ();
  g_assert_nonnull (encoder);
  g_assert_cmpstr (encoder->element, ==, "x264enc");
  g_assert_true (encoder->hd);
  g_assert_false (encoder->fhd);
  g_assert_cmpuint (cam_encoder_probes_started (), ==, 0);
}

int
main (int argc, char **argv)
{
  g_autofree char *tmp = g_dir_make_tmp ("vibeos-camera-test-XXXXXX", NULL);

  g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/encoder/no-cache-no-probe", test_no_cache_no_probe);
  g_test_add_func ("/encoder/cache-hit-no-probe", test_cache_hit_no_probe);
  return g_test_run ();
}
