/* dictate-output: see dictate-output.h. One query to the sound server, on a main loop of its own. */
#include "dictate-output.h"

#include <pulse/pulseaudio.h>
#include <string.h>

#define ASK_TIMEOUT_US (1 * G_USEC_PER_SEC)

typedef struct {
  pa_context *context;
  char *default_sink;
  gboolean headphones;
  char *sink;
  int pending;                  /* answers still awaited; -1 failed */
} Ask;

/* what a port or a device's form says about it: worn on the head (or held to the ear) */
static gboolean
worn (const pa_sink_info *info)
{
  const char *form = pa_proplist_gets (info->proplist, PA_PROP_DEVICE_FORM_FACTOR);
  const char *bus = pa_proplist_gets (info->proplist, PA_PROP_DEVICE_BUS);
  int type = info->active_port ? (int) info->active_port->type : PA_DEVICE_PORT_TYPE_UNKNOWN;

  if (form)
    return !strcmp (form, "headphone") || !strcmp (form, "headset") || !strcmp (form, "hands-free")
           || !strcmp (form, "handset");
  switch (type)
    {
    case PA_DEVICE_PORT_TYPE_HEADPHONES:
    case PA_DEVICE_PORT_TYPE_HEADSET:
    case PA_DEVICE_PORT_TYPE_HANDSFREE:
    case PA_DEVICE_PORT_TYPE_EARPIECE:
    case PA_DEVICE_PORT_TYPE_HANDSET:
      return TRUE;
    case PA_DEVICE_PORT_TYPE_SPEAKER:
    case PA_DEVICE_PORT_TYPE_CAR:
    case PA_DEVICE_PORT_TYPE_HIFI:
    case PA_DEVICE_PORT_TYPE_HDMI:
    case PA_DEVICE_PORT_TYPE_TV:
      return FALSE;
    default:
      /* a Bluetooth device that says nothing about itself is most likely earbuds */
      return bus && !strcmp (bus, "bluetooth");
    }
}

static void
on_sink (pa_context *c, const pa_sink_info *info, int eol, void *data)
{
  Ask *ask = data;

  if (eol)
    {
      ask->pending--;
      return;
    }
  if (info && worn (info))
    {
      ask->headphones = TRUE;
      ask->sink = g_strdup (info->name);
    }
}

static void
on_server (pa_context *c, const pa_server_info *info, void *data)
{
  Ask *ask = data;

  ask->pending--;
  if (!info || !info->default_sink_name)
    {
      ask->pending = -1;
      return;
    }
  ask->default_sink = g_strdup (info->default_sink_name);
  ask->pending++;
  pa_operation_unref (pa_context_get_sink_info_by_name (c, info->default_sink_name, on_sink, ask));
}

gboolean
dictate_output_headphones (char **sink)
{
  const char *test = g_getenv ("DICTATE_TEST_ROUTE");
  pa_mainloop *loop;
  Ask ask = { 0 };
  gint64 deadline = g_get_monotonic_time () + ASK_TIMEOUT_US;
  gboolean asked = FALSE;

  if (sink)
    *sink = NULL;
  if (test)
    return !strcmp (test, "headphones");
  loop = pa_mainloop_new ();
  ask.context = pa_context_new (pa_mainloop_get_api (loop), "Vibe");
  if (pa_context_connect (ask.context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
    ask.pending = -1;
  while (ask.pending >= 0 && g_get_monotonic_time () < deadline)
    {
      pa_context_state_t state = pa_context_get_state (ask.context);

      if (!PA_CONTEXT_IS_GOOD (state))
        {
          ask.pending = -1;
          break;
        }
      if (state == PA_CONTEXT_READY && !asked)
        {
          asked = TRUE;
          ask.pending = 1;
          pa_operation_unref (pa_context_get_server_info (ask.context, on_server, &ask));
        }
      if (asked && ask.pending == 0)
        break;
      /* at most 50 ms at a time, so that the deadline holds */
      pa_mainloop_prepare (loop, 50 * 1000);
      pa_mainloop_poll (loop);
      pa_mainloop_dispatch (loop);
    }
  if (ask.pending != 0)
    g_debug ("output: the sound server did not answer: taken as a speaker");
  pa_context_disconnect (ask.context);
  pa_context_unref (ask.context);
  pa_mainloop_free (loop);
  g_free (ask.default_sink);
  if (ask.pending != 0 || !ask.headphones)
    {
      g_free (ask.sink);
      return FALSE;
    }
  if (sink)
    *sink = ask.sink;
  else
    g_free (ask.sink);
  return TRUE;
}
