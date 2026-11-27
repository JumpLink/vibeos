/* navi-awake: see navi-awake.h */
#include "navi-awake.h"
#include "navi-inhibit.h"

#include <string.h>

NaviRun
navi_run_for (NaviMode mode, NaviSeen seen)
{
  gboolean driving = mode == NAVI_MODE_DRIVE, seen_now = seen == NAVI_SEEN;
  return (NaviRun) {
    .position = seen_now || (driving && seen != NAVI_CLOSED),
    .compass = seen_now,
    .drawing = seen_now,
    .timers = seen_now,
    .screen_on = seen_now && (driving || mode == NAVI_MODE_ARRIVED),
    .sleep_block = driving && seen != NAVI_CLOSED,
  };
}

NaviSeen
navi_seen_from (gboolean open, gboolean mapped, gboolean suspended, gboolean active, gboolean ever_active)
{
  if (!open)
    return NAVI_CLOSED;
  return mapped && !suspended && (active || !ever_active) ? NAVI_SEEN : NAVI_UNSEEN;
}

typedef struct {
  NaviAwake *awake;
  guint interval_ms;
  GSourceFunc fn;
  gpointer data;
  guint source;
} Timer;

struct _NaviAwake {
  NaviPosition *position;
  NaviAwakeHooks hooks;
  gpointer data;
  NaviSleepBlock *block;
  GPtrArray *timers;
  gboolean started; /* the first update has been */
  NaviMode mode;
  NaviSeen seen;
  NaviRun run;
};

static gboolean
timer_fired (gpointer data)
{
  Timer *t = data;
  if (t->fn (t->data) == G_SOURCE_CONTINUE)
    return G_SOURCE_CONTINUE;
  t->source = 0;
  g_ptr_array_remove (t->awake->timers, t); /* frees it */
  return G_SOURCE_REMOVE;
}

static void
timer_attach (Timer *t)
{
  if (t->source)
    return;
  t->source = t->interval_ms % 1000 == 0 ? g_timeout_add_seconds (t->interval_ms / 1000, timer_fired, t)
                                         : g_timeout_add (t->interval_ms, timer_fired, t);
}

static void
timer_detach (Timer *t)
{
  g_clear_handle_id (&t->source, g_source_remove);
}

static void
timer_free (gpointer data)
{
  timer_detach (data);
  g_free (data);
}

NaviAwake *
navi_awake_new (NaviPosition *position, const NaviAwakeHooks *hooks, gpointer data)
{
  NaviAwake *self = g_new0 (NaviAwake, 1);
  self->position = position ? g_object_ref (position) : NULL;
  if (hooks)
    self->hooks = *hooks;
  self->data = data;
  self->block = navi_sleep_block_new ();
  self->timers = g_ptr_array_new_with_free_func (timer_free);
  self->seen = NAVI_UNSEEN;
  return self;
}

static void
apply (NaviAwake *self, NaviRun want)
{
  gboolean first = !self->started;
  NaviRun had = self->run;
  self->run = want;
  self->started = TRUE;
  if (first || want.timers != had.timers) {
    for (guint i = 0; i < self->timers->len; i++)
      (want.timers ? timer_attach : timer_detach) (self->timers->pdata[i]);
  }
  if (self->position && (first || want.position != had.position)) {
    if (want.position)
      navi_position_start (self->position);
    else
      navi_position_stop (self->position);
  }
  if (first || want.sleep_block != had.sleep_block)
    navi_sleep_block_set (self->block, want.sleep_block);
  /* the window's part last: drawing again may ask for what the rest started */
  if (self->hooks.compass && (first || want.compass != had.compass))
    self->hooks.compass (want.compass, self->data);
  if (self->hooks.screen_on && (first || want.screen_on != had.screen_on))
    self->hooks.screen_on (want.screen_on, self->data);
  if (self->hooks.drawing && (first || want.drawing != had.drawing))
    self->hooks.drawing (want.drawing, self->data);
}

void
navi_awake_update (NaviAwake *self, NaviMode mode, NaviSeen seen)
{
  NaviRun want = navi_run_for (mode, seen);
  gboolean changed = !self->started || memcmp (&want, &self->run, sizeof want) != 0;
  if (self->started && (seen != self->seen || changed))
    g_debug ("awake: %s, %s: position %s, drawing %s, sleep %s", mode == NAVI_MODE_DRIVE ? "driving" : "no route",
             seen == NAVI_SEEN ? "seen" : seen == NAVI_UNSEEN ? "not seen" : "closed", want.position ? "on" : "off",
             want.drawing ? "on" : "off", want.sleep_block ? "blocked" : "allowed");
  self->mode = mode;
  self->seen = seen;
  if (changed)
    apply (self, want);
}

NaviRun
navi_awake_get (NaviAwake *self)
{
  return self->run;
}

NaviSeen
navi_awake_get_seen (NaviAwake *self)
{
  return self->seen;
}

void
navi_awake_add_timer (NaviAwake *self, guint interval_ms, GSourceFunc fn, gpointer data)
{
  Timer *t = g_new0 (Timer, 1);
  t->awake = self;
  t->interval_ms = MAX (interval_ms, 1);
  t->fn = fn;
  t->data = data;
  g_ptr_array_add (self->timers, t);
  if (self->run.timers)
    timer_attach (t);
}

guint
navi_awake_timers_attached (NaviAwake *self)
{
  guint n = 0;
  for (guint i = 0; i < self->timers->len; i++)
    n += ((Timer *) self->timers->pdata[i])->source != 0;
  return n;
}

gboolean
navi_awake_sleep_blocked (NaviAwake *self)
{
  return navi_sleep_block_is_held (self->block);
}

void
navi_awake_free (NaviAwake *self)
{
  if (!self)
    return;
  g_ptr_array_unref (self->timers);
  if (self->position) {
    navi_position_stop (self->position);
    g_object_unref (self->position);
  }
  navi_sleep_block_free (self->block);
  g_free (self);
}
