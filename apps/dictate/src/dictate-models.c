/* dictate-models: see dictate-models.h. */
#include "dictate-models.h"
#include "moonshine-api.h"

#include <string.h>

/* by their names, as Android lists languages */
static const DictateLanguage LANGUAGES[] = {
  { "de", "Deutsch" },
  { "en", "English" },
  { "es", "Español" },
  { "fr", "Français" },
  { "it", "Italiano" },
  { "ru", "Русский" },
};

/* Moonshine's streaming models, in the order they are preferred: the tiny one keeps up with speech on both reference
 * phones, the small one only on the faster */
static const struct { const char *prefix; guint arch; } MOONSHINE[] = {
  { "tiny-streaming-", MOONSHINE_MODEL_ARCH_TINY_STREAMING },
  { "small-streaming-", MOONSHINE_MODEL_ARCH_SMALL_STREAMING },
  { "base-streaming-", MOONSHINE_MODEL_ARCH_BASE_STREAMING },
  { "medium-streaming-", MOONSHINE_MODEL_ARCH_MEDIUM_STREAMING },
};
#define VOSK_RANK G_N_ELEMENTS (MOONSHINE)

const DictateLanguage *
dictate_languages (guint *n)
{
  *n = G_N_ELEMENTS (LANGUAGES);
  return LANGUAGES;
}

const DictateLanguage *
dictate_language_lookup (const char *code)
{
  for (guint i = 0; code && i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_str_equal (LANGUAGES[i].code, code))
      return &LANGUAGES[i];
  return NULL;
}

static gboolean
has_files (const char *dir, const char *const *files)
{
  for (; *files; files++)
    {
      g_autofree char *path = g_build_filename (dir, *files, NULL);

      if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return FALSE;
    }
  return TRUE;
}

/* the language in a model's name: the part of it, between dashes, that is a language's code */
static const DictateLanguage *
name_language (const char *name)
{
  g_auto (GStrv) parts = g_strsplit (name, "-", -1);

  for (guint i = 0; parts[i]; i++)
    {
      const DictateLanguage *language = dictate_language_lookup (parts[i]);

      if (language)
        return language;
    }
  return NULL;
}

/* how preferred the model in `dir` named `name` is (lower first), filling in `model`; -1 for none Dictate can run */
static int
rank (const char *dir, const char *name, DictateModel *model)
{
  static const char *const MOONSHINE_FILES[] = { "streaming_config.json", "tokenizer.bin", "encoder.ort",
                                                 "decoder_kv.ort", NULL };
  static const char *const VOSK_FILES[] = { "am/final.mdl", "conf/model.conf", NULL };

  for (guint i = 0; i < G_N_ELEMENTS (MOONSHINE); i++)
    if (g_str_has_prefix (name, MOONSHINE[i].prefix) && has_files (dir, MOONSHINE_FILES))
      {
        model->kind = DICTATE_MOONSHINE;
        model->arch = MOONSHINE[i].arch;
        return i;
      }
  if (g_str_has_prefix (name, "vosk-") && has_files (dir, VOSK_FILES))
    {
      model->kind = DICTATE_VOSK;
      model->arch = 0;
      return VOSK_RANK;
    }
  return -1;
}

/* the best model for each language, by its code; `only`: just that language's */
static GHashTable *
scan (const char *only)
{
  GHashTable *best = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) dictate_model_free);
  g_autoptr (GHashTable) ranks = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) roots = g_ptr_array_new ();

  g_ptr_array_add (roots, (gpointer) g_get_user_data_dir ());
  for (const char *const *d = g_get_system_data_dirs (); *d; d++)
    g_ptr_array_add (roots, (gpointer) *d);

  for (guint r = 0; r < roots->len; r++)
    {
      g_autofree char *models = g_build_filename (roots->pdata[r], "neo-dictate", "models", NULL);
      g_autoptr (GDir) list = g_dir_open (models, 0, NULL);
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      const char *name;

      if (!list)
        continue;
      while ((name = g_dir_read_name (list)))
        g_ptr_array_add (names, g_strdup (name));
      /* the directory's order is the file system's: the same choice every time all the same */
      g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
      for (guint i = 0; i < names->len; i++)
        {
          const char *entry = names->pdata[i];
          const DictateLanguage *language = name_language (entry);
          g_autofree char *dir = NULL;
          DictateModel found = { 0 };
          int got;

          if (!language || (only && !g_str_equal (language->code, only)))
            continue;
          dir = g_build_filename (models, entry, NULL);
          got = rank (dir, entry, &found);
          /* an earlier root's model of the same rank stays: the user's comes first */
          if (got < 0 || (g_hash_table_contains (ranks, language->code) &&
                          GPOINTER_TO_INT (g_hash_table_lookup (ranks, language->code)) <= got))
            continue;
          found.language = g_strdup (language->code);
          found.dir = g_steal_pointer (&dir);
          g_hash_table_insert (ranks, (gpointer) language->code, GINT_TO_POINTER (got));
          g_hash_table_insert (best, (gpointer) language->code, dictate_model_copy (&found));
          g_free (found.language);
          g_free (found.dir);
        }
    }
  return best;
}

GPtrArray *
dictate_models_installed (void)
{
  g_autoptr (GHashTable) best = scan (NULL);
  GPtrArray *models = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_model_free);

  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    {
      DictateModel *model = g_hash_table_lookup (best, LANGUAGES[i].code);

      if (model)
        g_ptr_array_add (models, dictate_model_copy (model));
    }
  return models;
}

DictateModel *
dictate_model_find (const char *code)
{
  g_autoptr (GHashTable) best = NULL;
  DictateModel *model;

  if (!dictate_language_lookup (code))
    return NULL;
  best = scan (code);
  model = g_hash_table_lookup (best, code);
  return model ? dictate_model_copy (model) : NULL;
}

DictateModel *
dictate_model_copy (const DictateModel *model)
{
  DictateModel *copy = g_new (DictateModel, 1);

  *copy = *model;
  copy->language = g_strdup (model->language);
  copy->dir = g_strdup (model->dir);
  return copy;
}

void
dictate_model_free (DictateModel *model)
{
  if (!model)
    return;
  g_free (model->language);
  g_free (model->dir);
  g_free (model);
}

char *
dictate_language_default (void)
{
  const char *const *locales = g_get_language_names ();
  g_autoptr (GPtrArray) models = dictate_models_installed ();

  if (models->len == 0)
    return NULL;
  /* "de_DE.UTF-8" is de */
  for (guint i = 0; locales[i]; i++)
    {
      g_autofree char *code = g_ascii_strdown (locales[i], strcspn (locales[i], "_.@"));

      for (guint m = 0; m < models->len; m++)
        if (g_str_equal (((DictateModel *) models->pdata[m])->language, code))
          return g_steal_pointer (&code);
    }
  for (guint m = 0; m < models->len; m++)
    if (g_str_equal (((DictateModel *) models->pdata[m])->language, "en"))
      return g_strdup ("en");
  return g_strdup (((DictateModel *) models->pdata[0])->language);
}
