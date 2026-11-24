/* call-audio-ucm: what a sound card's UCM says about recording a call, read from the configurations for no card in
 * tests/ucm2 (ALSA_CONFIG_UCM2) */
#include "../src/call-audio-ucm.h"

static void
test_record_and_announce (void)
{
  CallAudioUcm ucm;

  g_assert_true (call_audio_ucm_probe_card ("RecordingPhone", &ucm));
  g_assert_cmpstr (call_audio_ucm_pcm_path (ucm.capture_pcm), ==, "hw:0,4");
  g_assert_cmpstr (call_audio_ucm_pcm_path (ucm.playback_pcm), ==, "hw:0,5");
  call_audio_ucm_clear (&ucm);
  g_assert_null (ucm.capture_pcm);
}

static void
test_record_only (void)
{
  CallAudioUcm ucm;

  /* a recording, but no way to play the announcement to the other person */
  g_assert_true (call_audio_ucm_probe_card ("NoAnnouncePhone", &ucm));
  g_assert_cmpstr (call_audio_ucm_pcm_path (ucm.capture_pcm), ==, "hw:0,4");
  g_assert_null (ucm.playback_pcm);
  call_audio_ucm_clear (&ucm);
}

static void
test_cannot_record (void)
{
  CallAudioUcm ucm;

  /* calls with nothing that records them; Play Voice alone counts for nothing */
  g_assert_false (call_audio_ucm_probe_card ("CallOnlyPhone", &ucm));
  g_assert_null (ucm.capture_pcm);
  g_assert_null (ucm.playback_pcm);
  /* no Voice Call verb */
  g_assert_false (call_audio_ucm_probe_card ("Tablet", &ucm));
  g_assert_null (ucm.capture_pcm);
  /* no configuration at all */
  g_assert_false (call_audio_ucm_probe_card ("-NoSuchCard", &ucm));
  g_assert_null (ucm.capture_pcm);
}

static void
test_pcm_path (void)
{
  /* alsa-lib's prefix for UCM's private configuration goes; a name without it stays */
  g_assert_cmpstr (call_audio_ucm_pcm_path ("_ucm0001.hw:S10e,4"), ==, "hw:S10e,4");
  g_assert_cmpstr (call_audio_ucm_pcm_path ("_ucm00AF.hw:0,2"), ==, "hw:0,2");
  g_assert_cmpstr (call_audio_ucm_pcm_path ("hw:0,2"), ==, "hw:0,2");
  g_assert_cmpstr (call_audio_ucm_pcm_path ("_ucmx.hw:0,2"), ==, "_ucmx.hw:0,2");
  g_assert_null (call_audio_ucm_pcm_path (NULL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/call-audio-ucm/record-and-announce", test_record_and_announce);
  g_test_add_func ("/call-audio-ucm/record-only", test_record_only);
  g_test_add_func ("/call-audio-ucm/cannot-record", test_cannot_record);
  g_test_add_func ("/call-audio-ucm/pcm-path", test_pcm_path);
  return g_test_run ();
}
