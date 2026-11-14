/* The part of Moonshine's C API (moonshine-c-api.h, MIT, github.com/moonshine-ai/moonshine) Dictate uses: the
 * streaming transcriber. The library is loaded at run time (dictate-engine.c), so these are the structures as
 * header version 3.0.0 lays them out and the functions' types; they must match the library's exactly. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MOONSHINE_HEADER_VERSION 30000

#define MOONSHINE_MODEL_ARCH_TINY_STREAMING   2
#define MOONSHINE_MODEL_ARCH_BASE_STREAMING   3
#define MOONSHINE_MODEL_ARCH_SMALL_STREAMING  4
#define MOONSHINE_MODEL_ARCH_MEDIUM_STREAMING 5

struct moonshine_option_t {
  const char *name;
  const char *value;
};

struct transcript_word_t {
  const char *text;
  float start;
  float end;
  float confidence;
};

struct speaker_span_t {
  float start_time;
  float duration;
  uint64_t speaker_id;
  uint32_t speaker_index;
  uint64_t start_char;
  uint64_t end_char;
};

/* one stretch of speech, about a sentence; only the last line of a stream's transcript can be incomplete, and a
 * complete one never changes again */
struct transcript_line_t {
  const char *text;
  const float *audio_data;
  size_t audio_data_count;
  float start_time;
  float duration;
  uint64_t id;
  int8_t is_complete;
  int8_t is_updated;
  int8_t is_new;
  int8_t has_text_changed;
  int8_t have_speakers_changed;
  const struct speaker_span_t *speaker_spans;
  uint64_t speaker_span_count;
  uint32_t last_transcription_latency_ms;
  const struct transcript_word_t *words;
  uint64_t word_count;
};

/* owned by the transcriber, valid until its next call */
struct transcript_t {
  struct transcript_line_t *lines;
  uint64_t line_count;
};

typedef int32_t     (*MoonshineGetVersion)       (void);
typedef const char *(*MoonshineErrorToString)    (int32_t error);
typedef int32_t     (*MoonshineLoadFromFiles)    (const char *path, uint32_t model_arch,
                                                  const struct moonshine_option_t *options, uint64_t options_count,
                                                  int32_t moonshine_version);
typedef void        (*MoonshineFreeTranscriber)  (int32_t transcriber);
typedef int32_t     (*MoonshineCreateStream)     (int32_t transcriber, uint32_t flags);
typedef int32_t     (*MoonshineStreamCall)       (int32_t transcriber, int32_t stream);
/* 16 kHz float samples between -1 and 1 */
typedef int32_t     (*MoonshineAddAudio)         (int32_t transcriber, int32_t stream, const float *audio,
                                                  uint64_t length, int32_t sample_rate, uint32_t flags);
typedef int32_t     (*MoonshineTranscribeStream) (int32_t transcriber, int32_t stream, uint32_t flags,
                                                  struct transcript_t **transcript);
