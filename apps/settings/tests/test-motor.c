/* ns_motor_present_in on made-up /sys/class/input, /sys/class/leds and udev database trees: each of the three signs
 * alone, each of them absent, and the game controller that rumbles but is not a motor. extension/tests/sounds.js
 * checks the same trees with the shell's copy of the rule. */
#include "../src/ns-motor.h"

#include <glib/gstdio.h>
#include <stdlib.h>

typedef struct {
  char *root;
  char *input, *leds, *udev;
} Tree;

static void
put (const char *path, const char *text)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}

static Tree *
tree_new (void)
{
  Tree *t = g_new0 (Tree, 1);
  t->root = g_dir_make_tmp ("motor-XXXXXX", NULL);
  t->input = g_build_filename (t->root, "input", NULL);
  t->leds = g_build_filename (t->root, "leds", NULL);
  t->udev = g_build_filename (t->root, "udev", NULL);
  g_assert_cmpint (g_mkdir_with_parents (t->input, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (t->leds, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (t->udev, 0755), ==, 0);
  return t;
}

static void
tree_free (Tree *t)
{
  g_autofree char *cmd = g_strdup_printf ("rm -rf '%s'", t->root);
  g_assert_cmpint (system (cmd), ==, 0);
  g_free (t->root);
  g_free (t->input);
  g_free (t->leds);
  g_free (t->udev);
  g_free (t);
}

static void
event (Tree *t, const char *node, const char *ff, const char *abs)
{
  g_autofree char *a = g_strdup_printf ("%s/%s/device/capabilities/ff", t->input, node);
  g_autofree char *b = g_strdup_printf ("%s/%s/device/capabilities/abs", t->input, node);
  put (a, ff);
  put (b, abs);
}

static gboolean
present (Tree *t)
{
  return ns_motor_present_in (t->input, t->leds, t->udev);
}

static void
test_nothing (void)
{
  Tree *t = tree_new ();
  g_assert_false (present (t));
  /* a keyboard and a touchscreen: capabilities, but no force feedback */
  event (t, "event0", "0\n", "0\n");
  event (t, "event1", "0\n", "3\n");
  g_assert_false (present (t));
  /* trees that are not there at all */
  g_assert_false (ns_motor_present_in ("/nonexistent/a", "/nonexistent/b", "/nonexistent/c"));
  tree_free (t);
}

static void
test_force_feedback (void)
{
  Tree *t = tree_new ();
  event (t, "event0", "0\n", "0\n");
  event (t, "event4", "0 0 0 10000\n", "0\n");
  g_assert_true (present (t));
  tree_free (t);
}

static void
test_game_controller (void)
{
  Tree *t = tree_new ();
  /* rumble, with axes: a controller, which comes and goes */
  event (t, "event7", "10000\n", "3003f\n");
  g_assert_false (present (t));
  tree_free (t);
}

static void
test_led (void)
{
  Tree *t = tree_new ();
  g_autofree char *a = g_build_filename (t->leds, "input3::capslock", "brightness", NULL);
  put (a, "0\n");
  g_assert_false (present (t));
  g_autofree char *b = g_build_filename (t->leds, "vibrator", "brightness", NULL);
  put (b, "0\n");
  g_assert_true (present (t));
  tree_free (t);
}

static void
test_feedbackd (void)
{
  Tree *t = tree_new ();
  g_autofree char *a = g_build_filename (t->udev, "c13:64", NULL);
  put (a, "I:1\nE:ID_INPUT=1\nG:uaccess\n");
  g_assert_false (present (t));
  g_autofree char *b = g_build_filename (t->udev, "c13:65", NULL);
  put (b, "I:1\nE:ID_INPUT=1\nE:FEEDBACKD_TYPE=vibra\nG:uaccess\n");
  g_assert_true (present (t));
  /* an LED feedbackd drives is not a haptic device */
  g_autofree char *c = g_build_filename (t->udev, "c13:65", NULL);
  put (c, "E:FEEDBACKD_TYPE=led\n");
  g_assert_false (present (t));
  tree_free (t);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/motor/nothing", test_nothing);
  g_test_add_func ("/motor/force-feedback", test_force_feedback);
  g_test_add_func ("/motor/game-controller", test_game_controller);
  g_test_add_func ("/motor/led", test_led);
  g_test_add_func ("/motor/feedbackd", test_feedbackd);
  return g_test_run ();
}
