/* Moonshine's streaming transcriber as a DictateRecogniser (dictate-recogniser.h).
 *
 * libmoonshine.so comes with the ONNX Runtime it is built against, libonnxruntime.so.1, next to it. The stream hears
 * speech in lines that end at pauses; asked for its transcript, it gives all of them, only the last one possibly
 * unfinished, and a finished one never changes. A transcript update is the costly part (up to 0.7 s on a POCO X3 with
 * the tiny English model), so the engine asks for one only every UPDATE_MS. */
#include "dictate-recogniser.h"
#include "moonshine-api.h"

#include <dlfcn.h>
#include <string.h>

static struct {
  void                     *library;
  MoonshineGetVersion       get_version;
  MoonshineErrorToString    error_to_string;
  MoonshineLoadFromFiles    load;
  MoonshineFreeTranscriber  free_transcriber;
  MoonshineCreateStream     create_stream;
  MoonshineStreamCall       free_stream, start_stream, stop_stream;
  MoonshineAddAudio         add_audio;
  MoonshineTranscribeStream transcribe;
} ms;
G_LOCK_DEFINE_STATIC (ms);

struct _DictateRecogniser {
  int32_t transcriber;
};

struct _DictateStream {
  int32_t transcriber, stream;
  uint64_t done;        /* the lines already passed on as finished */
};

static gboolean
installed (void)
{
  return dictate_library_installed ("libmoonshine.so");
}

static gboolean
load_library (char **error)
{
  g_autofree char *path = dictate_library_path ("libmoonshine.so");
  void *library;
  int32_t version;

  G_LOCK (ms);
  if (ms.library)
    {
      G_UNLOCK (ms);
      return TRUE;
    }
  library = dlopen (path, RTLD_NOW | RTLD_LOCAL);
  if (!library)
    {
      *error = g_strdup (dlerror ());
      G_UNLOCK (ms);
      return FALSE;
    }
#define SYMBOL(field, name) \
  if (!(*(void **) &ms.field = dlsym (library, name))) { *error = g_strdup_printf ("no %s", name); goto fail; }
  SYMBOL (get_version, "moonshine_get_version");
  SYMBOL (error_to_string, "moonshine_error_to_string");
  SYMBOL (load, "moonshine_load_transcriber_from_files");
  SYMBOL (free_transcriber, "moonshine_free_transcriber");
  SYMBOL (create_stream, "moonshine_create_stream");
  SYMBOL (free_stream, "moonshine_free_stream");
  SYMBOL (start_stream, "moonshine_start_stream");
  SYMBOL (stop_stream, "moonshine_stop_stream");
  SYMBOL (add_audio, "moonshine_transcribe_add_audio_to_stream");
  SYMBOL (transcribe, "moonshine_transcribe_stream");
#undef SYMBOL
  /* moonshine-api.h's structures are those of version 3 */
  version = ms.get_version ();
  if (version < 30000 || version >= 40000)
    {
      *error = g_strdup_printf ("version %d, not 3", version);
      goto fail;
    }
  ms.library = library;
  G_UNLOCK (ms);
  return TRUE;

fail:
  dlclose (library);
  memset (&ms, 0, sizeof ms);
  G_UNLOCK (ms);
  return FALSE;
}

static DictateRecogniser *
load (const DictateModel *model, char **error)
{
  DictateRecogniser *recogniser;
  int32_t transcriber;

  if (!load_library (error))
    return NULL;
  transcriber = ms.load (model->dir, model->arch, NULL, 0, MOONSHINE_HEADER_VERSION);
  if (transcriber < 0)
    {
      *error = g_strdup_printf ("%s: %s", model->dir, ms.error_to_string (transcriber));
      return NULL;
    }
  recogniser = g_new (DictateRecogniser, 1);
  recogniser->transcriber = transcriber;
  return recogniser;
}

static void
unload (DictateRecogniser *recogniser)
{
  ms.free_transcriber (recogniser->transcriber);
  g_free (recogniser);
}

static DictateStream *
begin (DictateRecogniser *recogniser, char **error)
{
  int32_t stream = ms.create_stream (recogniser->transcriber, 0);
  DictateStream *self;

  if (stream < 0 || ms.start_stream (recogniser->transcriber, stream) != 0)
    {
      *error = g_strdup_printf ("no stream: %s", ms.error_to_string (stream < 0 ? stream : -1));
      if (stream >= 0)
        ms.free_stream (recogniser->transcriber, stream);
      return NULL;
    }
  self = g_new0 (DictateStream, 1);
  self->transcriber = recogniser->transcriber;
  self->stream = stream;
  return self;
}

/* the lines finished since the last time onto `finished`, the unfinished one into *partial (if not NULL) */
static void
report (DictateStream *self, GPtrArray *finished, char **partial)
{
  struct transcript_t *transcript = NULL;
  const char *heard = "";

  if (ms.transcribe (self->transcriber, self->stream, 0, &transcript) == 0 && transcript)
    for (uint64_t i = self->done; i < transcript->line_count; i++)
      {
        const struct transcript_line_t *line = &transcript->lines[i];
        const char *text = line->text ? line->text : "";

        if (line->is_complete)
          {
            char *sentence = g_strstrip (g_strdup (text));

            /* speech it made nothing of is an empty line */
            if (*sentence)
              g_ptr_array_add (finished, sentence);
            else
              g_free (sentence);
            self->done = i + 1;
          }
        else
          heard = text;
      }
  if (partial)
    *partial = g_strstrip (g_strdup (heard));
}

static void
hear (DictateStream *self, const float *samples, guint n, GPtrArray *finished, char **partial)
{
  ms.add_audio (self->transcriber, self->stream, samples, n, DICTATE_RATE, 0);
  report (self, finished, partial);
}

static void
end (DictateStream *self, GPtrArray *finished)
{
  g_autofree char *last = NULL;

  /* the end of the stream finishes its last line; one left unfinished all the same is what was heard of it */
  ms.stop_stream (self->transcriber, self->stream);
  report (self, finished, &last);
  if (*last)
    g_ptr_array_add (finished, g_steal_pointer (&last));
  ms.free_stream (self->transcriber, self->stream);
  g_free (self);
}

const DictateRecogniserClass dictate_moonshine = {
  .name = "Moonshine",
  .package = "neo-dictate-moonshine",
  .installed = installed,
  .load = load,
  .unload = unload,
  .begin = begin,
  .hear = hear,
  .end = end,
};
