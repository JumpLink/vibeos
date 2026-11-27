/* call-speaker: which Voice Call profile the speaker switches to, on cards as PipeWire describes them */
#include "../src/call-speaker.h"

#define N_MAX 8

typedef struct {
  pa_card_info card;
  pa_card_profile_info2 profiles[N_MAX], *profile_list[N_MAX];
  pa_card_port_info ports[N_MAX], *port_list[N_MAX];
  pa_card_profile_info2 *port_profiles[N_MAX][N_MAX];
} Card;

static pa_card_profile_info2 *
profile (Card *c, const char *name, unsigned priority, int available)
{
  pa_card_profile_info2 *p = &c->profiles[c->card.n_profiles];

  *p = (pa_card_profile_info2) { .name = name, .description = name, .priority = priority, .available = available };
  c->profile_list[c->card.n_profiles++] = p;
  c->card.profiles2 = c->profile_list;
  return p;
}

/* @profiles: the profiles the port is in, NULL-terminated */
static void
port (Card *c, const char *name, pa_device_port_type_t type, int available, pa_card_profile_info2 **profiles)
{
  guint32 i = c->card.n_ports;
  pa_card_port_info *p = &c->ports[i];

  *p = (pa_card_port_info) { .name = name, .description = name, .direction = PA_DIRECTION_OUTPUT, .type = type,
                             .available = available, .profiles2 = c->port_profiles[i] };
  while (profiles[p->n_profiles])
    {
      c->port_profiles[i][p->n_profiles] = profiles[p->n_profiles];
      p->n_profiles++;
    }
  c->port_list[c->card.n_ports++] = p;
  c->card.ports = c->port_list;
}

/* the OnePlus 6T's: earpiece and speaker on one PCM, so a Voice Call profile each */
static void
fajita (Card *c, pa_card_profile_info2 **hifi, pa_card_profile_info2 **earpiece, pa_card_profile_info2 **speaker)
{
  *c = (Card) { .card = { .name = "alsa_card.platform-sound" } };
  *hifi = profile (c, "HiFi", 8000, 1);
  *earpiece = profile (c, "Voice Call (Earpiece, Mic)", 6400, 1);
  *speaker = profile (c, "Voice Call (Mic, Speaker)", 6300, 1);
  port (c, "[Out] Earpiece", PA_DEVICE_PORT_TYPE_EARPIECE, PA_PORT_AVAILABLE_UNKNOWN,
        (pa_card_profile_info2 *[]) { *earpiece, NULL });
  port (c, "[Out] Speaker", PA_DEVICE_PORT_TYPE_SPEAKER, PA_PORT_AVAILABLE_UNKNOWN,
        (pa_card_profile_info2 *[]) { *hifi, *speaker, NULL });
}

static void
test_split (void)
{
  Card c;
  pa_card_profile_info2 *hifi, *earpiece, *speaker;

  fajita (&c, &hifi, &earpiece, &speaker);
  /* no call: nothing to switch, even though HiFi plays on the speaker */
  c.card.active_profile2 = hifi;
  g_assert_null (call_speaker_profile (&c.card, TRUE));
  g_assert_null (call_speaker_profile (&c.card, FALSE));
  /* on the earpiece */
  c.card.active_profile2 = earpiece;
  g_assert_cmpstr (call_speaker_profile (&c.card, TRUE), ==, "Voice Call (Mic, Speaker)");
  g_assert_null (call_speaker_profile (&c.card, FALSE));
  /* on the speaker */
  c.card.active_profile2 = speaker;
  g_assert_null (call_speaker_profile (&c.card, TRUE));
  g_assert_cmpstr (call_speaker_profile (&c.card, FALSE), ==, "Voice Call (Earpiece, Mic)");
}

/* the Galaxy S10e's: earpiece and speaker as two ports of one Voice Call sink, which callaudiod switches */
static void
test_shared (void)
{
  Card c = { .card = { .name = "alsa_card.platform-sound" } };
  pa_card_profile_info2 *hifi = profile (&c, "HiFi", 8000, 1);
  pa_card_profile_info2 *call = profile (&c, "Voice Call", 6400, 1);

  port (&c, "[Out] Earpiece", PA_DEVICE_PORT_TYPE_EARPIECE, PA_PORT_AVAILABLE_UNKNOWN,
        (pa_card_profile_info2 *[]) { call, NULL });
  port (&c, "[Out] Speaker", PA_DEVICE_PORT_TYPE_SPEAKER, PA_PORT_AVAILABLE_UNKNOWN,
        (pa_card_profile_info2 *[]) { hifi, call, NULL });
  c.card.active_profile2 = call;
  g_assert_null (call_speaker_profile (&c.card, TRUE));
  g_assert_null (call_speaker_profile (&c.card, FALSE));
}

/* off goes to the best profile without the speaker; one that is not available, or a headset not plugged in, does not
 * count */
static void
test_headset (void)
{
  Card c;
  pa_card_profile_info2 *hifi, *earpiece, *speaker, *headset, *broken;

  fajita (&c, &hifi, &earpiece, &speaker);
  headset = profile (&c, "Voice Call (Headphones, Headset)", 6500, 1);
  broken = profile (&c, "Voice Call (Bluetooth)", 9000, 0);
  port (&c, "[Out] Headphones", PA_DEVICE_PORT_TYPE_HEADPHONES, PA_PORT_AVAILABLE_YES,
        (pa_card_profile_info2 *[]) { headset, NULL });
  port (&c, "[Out] Bluetooth", PA_DEVICE_PORT_TYPE_HEADSET, PA_PORT_AVAILABLE_UNKNOWN,
        (pa_card_profile_info2 *[]) { broken, speaker, NULL });
  c.card.active_profile2 = speaker;
  /* the speaker profile plays on the Bluetooth port too, so it is not the speaker alone: off switches nothing */
  g_assert_null (call_speaker_profile (&c.card, FALSE));
  c.card.ports[3]->profiles2[1] = NULL;
  c.card.ports[3]->n_profiles = 1;
  g_assert_cmpstr (call_speaker_profile (&c.card, FALSE), ==, "Voice Call (Headphones, Headset)");
  c.card.ports[2]->available = PA_PORT_AVAILABLE_NO;
  g_assert_cmpstr (call_speaker_profile (&c.card, FALSE), ==, "Voice Call (Earpiece, Mic)");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/call-speaker/split", test_split);
  g_test_add_func ("/call-speaker/shared", test_shared);
  g_test_add_func ("/call-speaker/headset", test_headset);
  return g_test_run ();
}
