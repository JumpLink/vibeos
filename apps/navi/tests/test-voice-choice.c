/* test-voice-choice: the voice Navi speaks a language in is the one Agent speaks it in (dictate_voice_choose, from
 * Agent's settings and its voices): Russian's tiny-voca and the man or the woman Agent's Voice setting has, a Piper
 * voice for the others, talk-voice's file for all where it is there, and none where Agent has none. And Agent's name
 * and credit for each voice (dictate_voice_title, dictate_pack_for_voice), which Navi's Settings and About show.
 *
 * The voices are stand-ins (empty files: the choice looks at names, never loads a model) in the test's own data
 * directories (G_TEST_OPTION_ISOLATE_DIRS); Agent's settings are its schema, compiled in the build directory
 * (GSETTINGS_SCHEMA_DIR), on a memory backend. */
#include "dictate-download.h"
#include "dictate-voice.h"

#include <glib/gstdio.h>
#include <string.h>

static const char *const TINY_VOCA_FILES[] = { "model.onnx", "model.json", "stress.txt.gz", "accent.onnx",
                                               "omograph.onnx", "omograph_vocab.txt.gz", NULL };

static GSettings *agent;

/* an empty file at the user's data directory's vibeos-dictate/voices/LANGUAGE/NAME */
static char *
put (const char *language, const char *name)
{
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "voices", language, NULL);
  char *path = g_build_filename (dir, name, NULL);
  g_autofree char *parent = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
  g_assert_true (g_file_set_contents (path, "", 0, NULL));
  return path;
}

static char *
put_tiny_voca (void)
{
  for (guint i = 0; TINY_VOCA_FILES[i]; i++) {
    g_autofree char *name = g_build_filename ("tiny-voca", TINY_VOCA_FILES[i], NULL);
    g_free (put ("ru", name));
  }
  return g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "voices", "ru", "tiny-voca", "model.onnx", NULL);
}

static void
reset_agent (void)
{
  const char *keys[] = { "talk-voice", "talk-voice-name", "talk-voice-gender" };
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    g_settings_reset (agent, keys[i]);
}

/* Russian: tiny-voca, as the man Agent has by default, or the woman once its Voice setting says so */
static void
test_russian (void)
{
  g_autofree char *model = NULL, *name = NULL, *gender = NULL;
  g_autofree char *tiny_voca = NULL;

  reset_agent ();
  /* Piper's Denis, Russian's voice before tiny-voca: still on a phone, never chosen for Russian */
  g_free (put ("ru", "ru_RU-denis-medium.onnx"));
  g_free (put ("ru", "ru_RU-denis-medium.onnx.json"));
  model = dictate_voice_choose (agent, "ru", &name, &gender);
  g_assert_null (model); /* tiny-voca not there yet: Agent has no Russian voice, and neither has Navi */
  g_clear_pointer (&gender, g_free);

  tiny_voca = put_tiny_voca ();
  model = dictate_voice_choose (agent, "ru", &name, &gender);
  g_assert_cmpstr (model, ==, tiny_voca);
  g_assert_null (name);
  g_assert_cmpstr (gender, ==, "male");
  g_clear_pointer (&model, g_free);
  g_clear_pointer (&gender, g_free);

  g_settings_set_string (agent, "talk-voice-gender", "female");
  model = dictate_voice_choose (agent, "ru", &name, &gender);
  g_assert_cmpstr (model, ==, tiny_voca);
  g_assert_cmpstr (gender, ==, "female");
  g_clear_pointer (&model, g_free);
  g_clear_pointer (&gender, g_free);

  /* without Agent's settings (vibeos-dictate not installed): its defaults, the voice's own speaker */
  model = dictate_voice_choose (NULL, "ru", &name, &gender);
  g_assert_cmpstr (model, ==, tiny_voca);
  g_assert_null (name);
  g_assert_null (gender);
}

/* the other languages: the first Piper voice by name, with its .json beside it */
static void
test_piper (void)
{
  g_autofree char *model = NULL, *gender = NULL, *lessac = NULL;

  reset_agent ();
  g_assert_null (dictate_voice_choose (agent, "de", NULL, NULL));
  g_free (put ("en", "en_US-aa-medium.onnx")); /* first by name, but no .json beside it: not a voice */
  lessac = put ("en", "en_US-lessac-medium.onnx");
  g_free (put ("en", "en_US-lessac-medium.onnx.json"));
  g_free (put ("en", "en_US-ryan-medium.onnx"));
  g_free (put ("en", "en_US-ryan-medium.onnx.json"));
  model = dictate_voice_choose (agent, "en", NULL, &gender);
  g_assert_cmpstr (model, ==, lessac);
  g_assert_cmpstr (gender, ==, "male"); /* the setting's, which a Piper voice of one speaker ignores */
  g_assert_null (dictate_voice_choose (agent, "de", NULL, NULL));
}

/* talk-voice: Agent's own file for every language while it is there, else the language's voice */
static void
test_chosen_file (void)
{
  g_autofree char *mine = NULL, *lessac = NULL, *model = NULL, *name = NULL;

  reset_agent ();
  lessac = put ("en", "en_US-lessac-medium.onnx");
  g_free (put ("en", "en_US-lessac-medium.onnx.json"));
  mine = put ("kokoro", "model_quantized.onnx");
  g_settings_set_string (agent, "talk-voice", mine);
  g_settings_set_string (agent, "talk-voice-name", "af_heart");
  for (guint i = 0; i < 2; i++) {
    const char *language = i ? "de" : "en";
    model = dictate_voice_choose (agent, language, &name, NULL);
    g_assert_cmpstr (model, ==, mine);
    g_assert_cmpstr (name, ==, "af_heart");
    g_clear_pointer (&model, g_free);
    g_clear_pointer (&name, g_free);
  }
  g_assert_cmpint (g_remove (mine), ==, 0);
  model = dictate_voice_choose (agent, "en", &name, NULL);
  g_assert_cmpstr (model, ==, lessac);
  g_assert_cmpstr (name, ==, "af_heart");
}

/* a language Navi is asked for that no voice dictate_voice_find () reads can be: nothing outside the voices */
static void
test_no_voice (void)
{
  reset_agent ();
  g_assert_null (dictate_voice_choose (agent, "", NULL, NULL));
  g_assert_null (dictate_voice_choose (agent, "../en", NULL, NULL));
  g_assert_null (dictate_voice_choose (agent, NULL, NULL, NULL));
  g_assert_null (dictate_voice_choose (NULL, "it", NULL, NULL));
}

/* the keys whose change Navi follows (its prompts made again) */
static void
test_keys (void)
{
  g_assert_true (dictate_voice_is_choice_key ("talk-voice"));
  g_assert_true (dictate_voice_is_choice_key ("talk-voice-name"));
  g_assert_true (dictate_voice_is_choice_key ("talk-voice-gender"));
  g_assert_false (dictate_voice_is_choice_key ("talk-speed"));
  g_assert_false (dictate_voice_is_choice_key ("language"));
  g_assert_false (dictate_voice_is_choice_key (NULL));
}

/* a voice's name and credit are Agent's: its pack's title, licence and source, wherever the voice is */
static void
test_titles (void)
{
  g_autofree char *tiny_voca = put_tiny_voca ();
  g_autofree char *lessac = put ("en", "en_US-lessac-medium.onnx");
  const DictatePack *pack = dictate_pack_for_voice (lessac);
  g_autofree char *title = dictate_voice_title (lessac);

  g_assert_nonnull (pack);
  g_assert_cmpstr (pack->language, ==, "en");
  g_assert_cmpstr (title, ==, "Piper voice Lessac");
  g_clear_pointer (&title, g_free);
  pack = dictate_pack_for_voice (tiny_voca);
  g_assert_nonnull (pack);
  g_assert_cmpstr (pack->language, ==, "ru");
  title = dictate_voice_title (tiny_voca);
  g_assert_cmpstr (title, ==, pack->title);
  g_assert_nonnull (strstr (title, "tiny-voca"));
  g_clear_pointer (&title, g_free);
  /* packaged, in the system's data directory */
  pack = dictate_pack_for_voice ("/usr/share/vibeos-dictate/voices/de/de_DE-thorsten-medium.onnx");
  g_assert_nonnull (pack);
  g_assert_cmpstr (pack->language, ==, "de");

  /* no pack's: the name on disk */
  struct { const char *model, *title; } other[] = {
    { "/usr/share/vibeos-dictate/voices/ru/ru_RU-denis-medium.onnx", "ru_RU-denis-medium" }, /* Russian's before */
    { "/home/someone/kokoro/model_quantized.onnx", "kokoro" },
    { "/home/someone/en_US-amy-medium.onnx", "en_US-amy-medium" },
    { "/home/someone/voices/en/en_US-lessac-medium.onnx", "en_US-lessac-medium" }, /* not under vibeos-dictate */
  };
  for (guint i = 0; i < G_N_ELEMENTS (other); i++) {
    g_assert_null (dictate_pack_for_voice (other[i].model));
    title = dictate_voice_title (other[i].model);
    g_assert_cmpstr (title, ==, other[i].title);
    g_clear_pointer (&title, g_free);
  }
  g_assert_null (dictate_pack_for_voice (NULL));
  g_assert_null (dictate_voice_title (NULL));

  /* every voice pack is found from its own model, and has what About credits */
  guint n;
  const DictatePack *packs = dictate_packs (&n);
  guint voices = 0;
  for (guint i = 0; i < n; i++) {
    if (packs[i].kind != DICTATE_PACK_VOICE)
      continue;
    voices++;
    g_assert_true (packs[i].title && *packs[i].title && packs[i].licence && *packs[i].licence);
    g_assert_true (g_str_has_prefix (packs[i].source, "https://"));
    guint models = 0;
    for (const DictateFile *f = packs[i].files; f->name; f++) {
      if (!g_str_has_suffix (f->name, ".onnx"))
        continue;
      g_autofree char *model = g_build_filename ("/usr/share/vibeos-dictate", packs[i].place, f->name, NULL);
      g_assert_true (dictate_pack_for_voice (model) == &packs[i]);
      models++;
    }
    g_assert_cmpuint (models, >, 0);
  }
  g_assert_cmpuint (voices, ==, 6);
}

int
main (int argc, char **argv)
{
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  if (!g_getenv ("GSETTINGS_SCHEMA_DIR"))
    g_setenv ("GSETTINGS_SCHEMA_DIR", g_getenv ("G_TEST_BUILDDIR") ? g_getenv ("G_TEST_BUILDDIR") : ".", TRUE);
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  agent = dictate_voice_settings ();
  g_assert_nonnull (agent);
  g_test_add_func ("/voice-choice/russian", test_russian);
  g_test_add_func ("/voice-choice/piper", test_piper);
  g_test_add_func ("/voice-choice/chosen-file", test_chosen_file);
  g_test_add_func ("/voice-choice/no-voice", test_no_voice);
  g_test_add_func ("/voice-choice/keys", test_keys);
  g_test_add_func ("/voice-choice/titles", test_titles);
  int status = g_test_run ();
  g_object_unref (agent);
  return status;
}
