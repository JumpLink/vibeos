/* The speaker in a call (call-speaker.h), through PulseAudio's API (PipeWire's pulse server). */
#include "call-speaker.h"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

/* alsa-lib's SND_USE_CASE_VERB_VOICECALL: PipeWire names the verb's profiles after it */
#define VOICE_CALL "Voice Call"

struct _CallSpeaker {
  pa_glib_mainloop *loop;
  pa_context *pa;
  gboolean speaker;             /* what was asked for last */
  gboolean pending;             /* asked for before the sound server was reached */
};

static gboolean
is_voice_call (const pa_card_profile_info2 *profile)
{
  return profile && profile->available && g_str_has_prefix (profile->name, VOICE_CALL);
}

static gboolean
port_in (const pa_card_port_info *port, const pa_card_profile_info2 *profile)
{
  for (guint32 i = 0; i < port->n_profiles; i++)
    if (g_strcmp0 (port->profiles2[i]->name, profile->name) == 0)
      return TRUE;
  return FALSE;
}

/* where @profile plays: on the speaker, and on anything else (the earpiece, a headset); unplugged ports do not count */
static void
outputs (const pa_card_info *card, const pa_card_profile_info2 *profile, gboolean *speaker, gboolean *other)
{
  *speaker = *other = FALSE;
  for (guint32 i = 0; i < card->n_ports; i++)
    {
      const pa_card_port_info *port = card->ports[i];

      if (port->direction != PA_DIRECTION_OUTPUT || port->available == PA_PORT_AVAILABLE_NO || !port_in (port, profile))
        continue;
      if (port->type == PA_DEVICE_PORT_TYPE_SPEAKER)
        *speaker = TRUE;
      else
        *other = TRUE;
    }
}

const char *
call_speaker_profile (const pa_card_info *card, gboolean speaker)
{
  const pa_card_profile_info2 *best = NULL;
  gboolean on_speaker, on_other;

  if (!is_voice_call (card->active_profile2))
    return NULL;
  outputs (card, card->active_profile2, &on_speaker, &on_other);
  /* off: only a profile that plays on the speaker alone; with the earpiece in it too, the daemon switches the port */
  if (speaker ? on_speaker : !on_speaker || on_other)
    return NULL;
  for (guint32 i = 0; i < card->n_profiles; i++)
    {
      const pa_card_profile_info2 *profile = card->profiles2[i];

      if (!is_voice_call (profile))
        continue;
      outputs (card, profile, &on_speaker, &on_other);
      if ((speaker ? on_speaker : on_other && !on_speaker) && (!best || profile->priority > best->priority))
        best = profile;
    }
  return best ? best->name : NULL;
}

static void
on_card (pa_context *pa, const pa_card_info *card, int eol, void *data)
{
  CallSpeaker *self = data;
  const char *profile;

  if (eol || !card || !(profile = call_speaker_profile (card, self->speaker)))
    return;
  g_message ("speaker %s: card %s to profile '%s'", self->speaker ? "on" : "off", card->name, profile);
  pa_operation_unref (pa_context_set_card_profile_by_index (pa, card->index, profile, NULL, NULL));
}

static void
apply (CallSpeaker *self)
{
  self->pending = FALSE;
  pa_operation_unref (pa_context_get_card_info_list (self->pa, on_card, self));
}

/* the speaker asked for before the call's profile was there (WirePlumber switches to it as the call starts) goes on
 * once it is */
static void
on_pa_event (pa_context *pa, pa_subscription_event_type_t type, uint32_t index, void *data)
{
  CallSpeaker *self = data;

  if (self->speaker && (type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_CARD
      && (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE)
    pa_operation_unref (pa_context_get_card_info_by_index (pa, index, on_card, self));
}

static void
on_pa_state (pa_context *pa, void *data)
{
  CallSpeaker *self = data;

  if (pa_context_get_state (pa) != PA_CONTEXT_READY)
    return;
  pa_context_set_subscribe_callback (pa, on_pa_event, self);
  pa_operation_unref (pa_context_subscribe (pa, PA_SUBSCRIPTION_MASK_CARD, NULL, NULL));
  if (self->pending)
    apply (self);
}

CallSpeaker *
call_speaker_new (void)
{
  CallSpeaker *self = g_new0 (CallSpeaker, 1);

  self->loop = pa_glib_mainloop_new (NULL);
  return self;
}

void
call_speaker_set (CallSpeaker *self, gboolean speaker)
{
  self->speaker = speaker;
  if (self->pa && pa_context_get_state (self->pa) == PA_CONTEXT_READY)
    {
      apply (self);
      return;
    }
  self->pending = TRUE;
  if (self->pa && PA_CONTEXT_IS_GOOD (pa_context_get_state (self->pa)))
    return;
  g_clear_pointer (&self->pa, pa_context_unref);
  self->pa = pa_context_new (pa_glib_mainloop_get_api (self->loop), "Phone speaker");
  pa_context_set_state_callback (self->pa, on_pa_state, self);
  if (pa_context_connect (self->pa, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
    g_warning ("speaker: cannot reach the sound server: %s", pa_strerror (pa_context_errno (self->pa)));
}

void
call_speaker_reset (CallSpeaker *self)
{
  self->speaker = self->pending = FALSE;
}

void
call_speaker_free (CallSpeaker *self)
{
  if (self->pa)
    {
      pa_context_set_state_callback (self->pa, NULL, NULL);
      pa_context_disconnect (self->pa);
      pa_context_unref (self->pa);
    }
  pa_glib_mainloop_free (self->loop);
  g_free (self);
}
