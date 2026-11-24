/* dictate-onboarding: see dictate-onboarding.h. */
#include "dictate-onboarding.h"
#include "dictate-access.h"
#include "dictate-cues.h"
#include "dictate-download-dialog.h"
#include "dictate-engine.h"
#include "dictate-enroll.h"
#include "dictate-l10n.h"
#include "dictate-models.h"
#include "dictate-stage.h"
#include "dictate-tools.h"
#include "dictate-voicematch.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>
#include <vibeos.h>

#define STEPS 6
#define LISTENS 3               /* turns listened for in a step before it waits for a tap */

typedef enum { HELLO, LANGUAGE, PACE, ACCESS, VOICE, READY } Step;

/* the speech rates the pace step goes through (as Settings' Speech rate) */
static const double RATES[] = { 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0 };

/* the languages' names in the language spoken now, besides their own (for "German" said in English) */
static const struct { const char *code, *name; } NAMES[] = {
  { "en", N_("English") }, { "de", N_("German") }, { "es", N_("Spanish") }, { "fr", N_("French") },
  { "it", N_("Italian") }, { "ru", N_("Russian") }, { "nl", N_("Dutch") },
};

/* the words each step listens for, in the language spoken: alternatives between "|" */
/* TRANSLATORS: words that go on to the next step of Vibe's introduction, alternatives separated by "|", lower case */
static const char *const WORDS_ON = N_("continue|next|go on|okay|ok|yes|done");
/* TRANSLATORS: words that go back a step in Vibe's introduction, alternatives separated by "|", lower case */
static const char *const WORDS_BACK = N_("back|go back");
/* TRANSLATORS: words that skip the rest of Vibe's introduction, alternatives separated by "|", lower case */
static const char *const WORDS_SKIP = N_("skip");
/* TRANSLATORS: words for "speak slower" in Vibe's introduction, alternatives separated by "|", lower case */
static const char *const WORDS_SLOWER = N_("slower|slow down");
/* TRANSLATORS: words for "speak faster" in Vibe's introduction, alternatives separated by "|", lower case */
static const char *const WORDS_FASTER = N_("faster|speed up|quicker");
/* TRANSLATORS: words for "this speed is fine" in Vibe's introduction, alternatives separated by "|", lower case */
static const char *const WORDS_GOOD = N_("that's good|good|fine|perfect|that's fine");
/* TRANSLATORS: words that turn something off ("turn off files"), alternatives separated by "|", lower case */
static const char *const WORDS_OFF = N_("off|turn off|switch off|disable");
/* TRANSLATORS: words that turn something on ("turn on files"), alternatives separated by "|", lower case */
static const char *const WORDS_ON_SWITCH = N_("on|turn on|switch on|enable");

typedef struct {
  GtkWidget *window, *page, *stage, *dots, *corner, *enroll;
  GSettings *settings;
  DictateTalk *talk;
  DictateEngine *engine;
  DictateOnboardingHooks hooks;
  char *language;
  Step step;
  GString *heard;               /* the turn's words */
  guint listens;
  gboolean saying, listening, over, enrolled;
  gboolean deaf;                /* the microphone or the model failed: the rest is tapped through */
  char *tapped;                 /* the step's hint for tapping, shown once it is deaf */
  GPtrArray *chips;             /* the access step's, their key in "dictate-access" */
} Intro;

static void show_step (Intro *in);
static void go (Intro *in, Step step);

static const char *
vibe (Intro *in, const char *msgid)
{
  return dictate_l10n (in->language, msgid);
}

static DictateStage *
stage (Intro *in)
{
  return DICTATE_STAGE (in->stage);
}

/* --- what is heard, matched on the phone */

/* `text` lower case, its words with single spaces between them and around them: " turn off files " */
static char *
words_of (const char *text)
{
  g_autofree char *down = g_utf8_strdown (text, -1);
  GString *out = g_string_new (" ");
  gboolean space = TRUE;

  for (const char *c = down; *c; c = g_utf8_next_char (c))
    {
      gunichar u = g_utf8_get_char (c);

      if (g_unichar_isalnum (u) || u == '\'' || u == 0x2019)
        {
          g_string_append_unichar (out, u == 0x2019 ? '\'' : u);
          space = FALSE;
        }
      else if (!space)
        {
          g_string_append_c (out, ' ');
          space = TRUE;
        }
    }
  if (!space)
    g_string_append_c (out, ' ');
  return g_string_free (out, FALSE);
}

/* whether the words heard hold one of `alternatives` ("a|b c"), whole words only */
static gboolean
heard_has (const char *heard, const char *alternatives)
{
  g_auto (GStrv) each = g_strsplit (alternatives, "|", -1);

  for (guint i = 0; each[i]; i++)
    {
      g_autofree char *want = words_of (each[i]);

      if (strlen (want) > 2 && strstr (heard, want))
        return TRUE;
    }
  return FALSE;
}

static void listen (Intro *in);

/* --- the steps */

static void
finish (Intro *in, const char *said, GBytes *audio)
{
  if (in->over)
    return;
  in->over = TRUE;
  g_settings_set_boolean (in->settings, "first-run", FALSE);
  if (dictate_engine_get_state (in->engine) != DICTATE_IDLE)
    dictate_engine_stop (in->engine);
  if (in->saying && dictate_talk_get_state (in->talk) != DICTATE_TALK_IDLE)
    dictate_talk_stop (in->talk);
  in->saying = in->listening = FALSE;
  /* told last: it takes the page away */
  in->hooks.done (said, audio, in->hooks.data);
}

/* the step's words said, then listened for; with replies only written, listened for at once */
static void
say (Intro *in, const char *text)
{
  in->listens = 0;
  if (in->listening)
    {
      in->listening = FALSE;
      dictate_engine_stop (in->engine);
    }
  if (g_settings_get_boolean (in->settings, "talk-speak") && dictate_talk_get_state (in->talk) == DICTATE_TALK_IDLE)
    {
      in->saying = TRUE;
      dictate_talk_announce (in->talk, in->language, text);
    }
  else
    listen (in);
}

static void
on_talk_state (DictateTalk *talk, Intro *in)
{
  if (in->saying && dictate_talk_get_state (talk) == DICTATE_TALK_IDLE)
    {
      in->saying = FALSE;
      if (in->step != VOICE)
        listen (in);
    }
}

/* the introduction's language has a speech model: its engine is asked in that language, which it is given here, as
 * listen () gives it before a start */
static gboolean
can_listen (Intro *in)
{
  if (in->deaf)
    return FALSE;
  dictate_engine_set_language (in->engine, in->language);
  return dictate_engine_available (in->engine, NULL);
}

static void
listen (Intro *in)
{
  if (in->over || in->step == VOICE || !can_listen (in) || in->listens >= LISTENS)
    {
      dictate_orb_set_look (dictate_stage_get_orb (stage (in)), DICTATE_ORB_REST, NULL);
      return;
    }
  in->listens++;
  in->listening = TRUE;
  g_string_truncate (in->heard, 0);
  dictate_orb_set_look (dictate_stage_get_orb (stage (in)), DICTATE_ORB_LISTENING, NULL);
  dictate_cue_play (in->settings, DICTATE_CUE_LISTENING);
  dictate_engine_set_language (in->engine, in->language);
  dictate_engine_set_turns (in->engine, TRUE);
  dictate_engine_start (in->engine);
}

static void
on_engine_text (DictateEngine *engine, GStrv finished, const char *partial, Intro *in)
{
  if (!in->listening)
    return;
  for (guint i = 0; finished && finished[i]; i++)
    {
      if (in->heard->len)
        g_string_append_c (in->heard, ' ');
      g_string_append (in->heard, finished[i]);
    }
}

static void choose_language (Intro *in, const char *code);
static void set_rate (Intro *in, int step);
static void set_access (Intro *in, const char *key, gboolean on);

/* a turn heard in the step: what it asks for, or listening again */
static void
heard (Intro *in)
{
  g_autofree char *w = words_of (in->heard->str);

  if (in->step == READY)
    {
      /* the first question to Vibe: the conversation asks it */
      if (in->heard->len)
        {
          g_autoptr (GBytes) audio = dictate_engine_take_turn_audio (in->engine);
          g_autofree char *said = g_strdup (in->heard->str);

          finish (in, said, audio);
          return;
        }
      listen (in);
      return;
    }
  if (heard_has (w, vibe (in, WORDS_SKIP)))
    {
      finish (in, NULL, NULL);
      return;
    }
  if (heard_has (w, vibe (in, WORDS_BACK)) && in->step > HELLO)
    {
      go (in, in->step - 1);
      return;
    }
  switch (in->step)
    {
    case LANGUAGE:
      {
        guint n;
        const DictateLanguage *languages = dictate_languages (&n);

        for (guint i = 0; i < n; i++)
          {
            const char *name = NULL;

            for (guint j = 0; j < G_N_ELEMENTS (NAMES); j++)
              if (!g_strcmp0 (NAMES[j].code, languages[i].code))
                name = vibe (in, NAMES[j].name);
            if (heard_has (w, languages[i].name) || (name && heard_has (w, name)))
              {
                choose_language (in, languages[i].code);
                return;
              }
          }
      }
      break;
    case PACE:
      if (heard_has (w, vibe (in, WORDS_SLOWER)))
        {
          set_rate (in, -1);
          return;
        }
      if (heard_has (w, vibe (in, WORDS_FASTER)))
        {
          set_rate (in, 1);
          return;
        }
      if (heard_has (w, vibe (in, WORDS_GOOD)))
        {
          go (in, ACCESS);
          return;
        }
      break;
    case ACCESS:
      {
        gboolean off = heard_has (w, vibe (in, WORDS_OFF)), on = !off && heard_has (w, vibe (in, WORDS_ON_SWITCH));

        if (off || on)
          for (guint i = 0; i < in->chips->len; i++)
            {
              const DictateAccess *a = g_object_get_data (in->chips->pdata[i], "dictate-access");

              if (heard_has (w, vibe (in, a->title)))
                {
                  set_access (in, a->key, on);
                  return;
                }
            }
      }
      break;
    default:
      break;
    }
  if (heard_has (w, vibe (in, WORDS_ON)))
    {
      go (in, in->step + 1);
      return;
    }
  listen (in);
}

static void
on_engine_state (DictateEngine *engine, Intro *in)
{
  if (!in->listening || in->step == VOICE || dictate_engine_get_state (engine) != DICTATE_IDLE)
    return;
  in->listening = FALSE;
  dictate_orb_set_look (dictate_stage_get_orb (stage (in)), DICTATE_ORB_REST, NULL);
  heard (in);
}

static void
on_engine_error (DictateEngine *engine, const char *message, Intro *in)
{
  /* the microphone or the model failed: the steps are tapped through. The step is not shown again: that would say it
   * again, listen again and fail again, round and round */
  in->listening = FALSE;
  in->deaf = TRUE;
  dictate_orb_set_look (dictate_stage_get_orb (stage (in)), DICTATE_ORB_REST, NULL);
  if (in->tapped)
    dictate_stage_set_hint (stage (in), in->tapped);
}

/* --- the language step */

static void
on_language_ready (const char *language, gpointer data)
{
  Intro *in = g_object_get_data (G_OBJECT (data), "dictate-onboarding");

  if (!in || in->over)
    return;
  in->hooks.set_language (language, in->hooks.data);
  g_free (in->language);
  in->language = g_strdup (language);
  go (in, PACE);
}

static void
choose_language (Intro *in, const char *code)
{
  if (in->listening)
    {
      in->listening = FALSE;
      dictate_engine_stop (in->engine);
    }
  /* what it lacks downloaded first, if the user agrees; until then the step stays */
  dictate_download_ensure (in->window, code, in->talk, on_language_ready, in->page);
}

static void
on_language_row (VibeosPressable *row, Intro *in)
{
  choose_language (in, g_object_get_data (G_OBJECT (row), "dictate-language"));
}

static GtkWidget *
languages_new (Intro *in)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  guint n;
  const DictateLanguage *languages = dictate_languages (&n);

  gtk_widget_add_css_class (box, "dictate-intro-choices");
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  for (guint i = 0; i < n; i++)
    {
      gboolean chosen = !g_strcmp0 (languages[i].code, in->language);
      g_autofree char *note = dictate_download_note (languages[i].code, in->talk);
      GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8), *name = gtk_label_new (languages[i].name);
      GtkWidget *state = gtk_label_new (note ? note : _("On the phone")), *row;

      gtk_widget_add_css_class (name, "dictate-intro-choice-name");
      gtk_label_set_xalign (GTK_LABEL (name), 0);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_box_append (GTK_BOX (line), name);
      gtk_widget_add_css_class (state, "dictate-intro-choice-state");
      gtk_box_append (GTK_BOX (line), state);
      if (chosen)
        {
          GtkWidget *check = gtk_image_new_from_icon_name ("dictate-check-symbolic");

          gtk_image_set_pixel_size (GTK_IMAGE (check), 18);
          gtk_box_append (GTK_BOX (line), check);
        }
      row = vibeos_pressable_new (line);
      gtk_widget_add_css_class (row, "dictate-intro-choice");
      if (chosen)
        gtk_widget_add_css_class (row, "dictate-intro-chosen");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 20);
      g_object_set_data (G_OBJECT (row), "dictate-language", (gpointer) languages[i].code);
      g_signal_connect (row, "clicked", G_CALLBACK (on_language_row), in);
      gtk_box_append (GTK_BOX (box), row);
    }
  return box;
}

/* --- the pace step */

static guint
rate_index (Intro *in)
{
  double speed = g_settings_get_double (in->settings, "talk-speed");
  guint best = 1;

  for (guint i = 0; i < G_N_ELEMENTS (RATES); i++)
    if (fabs (RATES[i] - speed) < fabs (RATES[best] - speed))
      best = i;
  return best;
}

static void
set_rate (Intro *in, int step)
{
  int i = (int) rate_index (in) + step;

  if (i < 0 || i >= (int) G_N_ELEMENTS (RATES))
    {
      listen (in);
      return;
    }
  /* the window's handler gives the talk the new speed */
  g_settings_set_double (in->settings, "talk-speed", RATES[i]);
  show_step (in);
}

static void
on_slower (VibeosPressable *button, Intro *in)
{
  set_rate (in, -1);
}

static void
on_faster (VibeosPressable *button, Intro *in)
{
  set_rate (in, 1);
}

static void
on_good (VibeosPressable *button, Intro *in)
{
  go (in, ACCESS);
}

/* --- the access step: a chip each for the areas Vibe has tools for */

static void
show_chip (Intro *in, GtkWidget *chip, gboolean on)
{
  const DictateAccess *a = g_object_get_data (G_OBJECT (chip), "dictate-access");
  /* TRANSLATORS: a switched-off area in Vibe's introduction: "Files · off" */
  g_autofree char *off = g_strdup_printf (vibe (in, N_("%s · off")), vibe (in, a->title));

  gtk_label_set_text (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (chip))),
                      on ? vibe (in, a->title) : off);
  if (on)
    gtk_widget_remove_css_class (chip, "dictate-intro-chip-off");
  else
    gtk_widget_add_css_class (chip, "dictate-intro-chip-off");
}

static void
set_access (Intro *in, const char *key, gboolean on)
{
  g_settings_set_boolean (in->settings, key, on);
  for (guint i = 0; i < in->chips->len; i++)
    {
      const DictateAccess *a = g_object_get_data (in->chips->pdata[i], "dictate-access");

      if (!g_strcmp0 (a->key, key))
        show_chip (in, in->chips->pdata[i], on);
    }
  listen (in);
}

static void
on_chip (VibeosPressable *chip, Intro *in)
{
  const DictateAccess *a = g_object_get_data (G_OBJECT (chip), "dictate-access");

  set_access (in, a->key, !g_settings_get_boolean (in->settings, a->key));
}

static GtkWidget *
chips_new (Intro *in)
{
  GtkWidget *flow = gtk_flow_box_new ();
  guint n;
  const DictateAccess *access = dictate_access_list (&n);

  gtk_widget_add_css_class (flow, "dictate-intro-chips");
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), FALSE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 8);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 8);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 8);
  gtk_widget_set_valign (flow, GTK_ALIGN_CENTER);
  g_ptr_array_set_size (in->chips, 0);
  for (guint i = 0; i < n; i++)
    {
      GtkWidget *chip;

      if (!dictate_tools_area_has_tools (access[i].key))
        continue;
      chip = vibeos_pressable_new (gtk_label_new (NULL));
      gtk_widget_add_css_class (chip, "dictate-intro-chip");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (chip), VIBEOS_RIPPLE_BOUNDED, 24);
      g_object_set_data (G_OBJECT (chip), "dictate-access", (gpointer) &access[i]);
      show_chip (in, chip, g_settings_get_boolean (in->settings, access[i].key));
      g_signal_connect (chip, "clicked", G_CALLBACK (on_chip), in);
      gtk_flow_box_append (GTK_FLOW_BOX (flow), chip);
      g_ptr_array_add (in->chips, chip);
    }
  return flow;
}

/* --- the voice step: the recording (dictate-enroll.h) in the page's place */

static void
on_enrolled (gboolean saved, gpointer data)
{
  Intro *in = data;

  in->enrolled = saved;
  go (in, READY);
}

/* --- the last step: the rule, and things to try */

static void
on_try (VibeosPressable *button, Intro *in)
{
  finish (in, g_object_get_data (G_OBJECT (button), "dictate-said"), NULL);
}

static GtkWidget *
tries_new (Intro *in)
{
  /* each only when what it needs is on and has tools */
  static const struct { const char *area, *said; } TRIES[] = {
    { NULL, N_("What time is it?") },
    { "access-alarms", N_("Wake me at six thirty.") },
    { "access-messages", N_("Do I have new messages?") },
    { "access-location", N_("What's the weather tomorrow?") },
  };
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8), *label = gtk_label_new (vibe (in, N_("Try saying")));

  gtk_widget_add_css_class (box, "dictate-intro-choices");
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (label, "dictate-intro-try");
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_box_append (GTK_BOX (box), label);
  for (guint i = 0; i < G_N_ELEMENTS (TRIES); i++)
    {
      const char *said = vibe (in, TRIES[i].said);
      /* TRANSLATORS: a thing to say to Vibe, in quotation marks */
      g_autofree char *quoted = g_strdup_printf (vibe (in, N_("“%s”")), said);
      GtkWidget *text, *button;

      if (TRIES[i].area && (!dictate_tools_area_has_tools (TRIES[i].area)
                            || !g_settings_get_boolean (in->settings, TRIES[i].area)))
        continue;
      text = gtk_label_new (quoted);
      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_label_set_wrap (GTK_LABEL (text), TRUE);
      button = vibeos_pressable_new (text);
      gtk_widget_add_css_class (button, "dictate-intro-suggestion");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
      g_object_set_data_full (G_OBJECT (button), "dictate-said", g_strdup (said), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_try), in);
      gtk_box_append (GTK_BOX (box), button);
    }
  return box;
}

/* the rule as it is kept: the card waits for a tap (dictate-window.c), or, with outward actions on a countdown
 * (outward-confirm, kept by the model side), a message or a call goes after three seconds unless it is cancelled */
static const char *
rule (Intro *in)
{
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autofree char *outward = NULL;

  g_object_get (in->settings, "settings-schema", &schema, NULL);
  if (g_settings_schema_has_key (schema, "outward-confirm"))
    outward = g_settings_get_string (in->settings, "outward-confirm");
  if (!g_strcmp0 (outward, "countdown"))
    return vibe (in, N_("Before I call or send anything, I read it back, and it goes after three seconds unless you "
                        "cancel it. Nothing is deleted until you tap to confirm."));
  return vibe (in, N_("Before I call, send or delete anything, I show you exactly what will happen, and nothing "
                      "happens until you confirm it."));
}

/* --- the steps on the stage */

static void
on_corner (VibeosPressable *button, Intro *in)
{
  if (in->step == HELLO)
    finish (in, NULL, NULL);
  else
    go (in, in->step - 1);
}

static void
on_tapped (DictateStage *s, Intro *in)
{
  switch (in->step)
    {
    case HELLO:
    case ACCESS:
      go (in, in->step + 1);
      break;
    case READY:
      /* "Tap anywhere and speak": the conversation listens */
      finish (in, "", NULL);
      break;
    default:
      break;
    }
}

/* the hint under a step: what can be said, while Vibe listens for it; else what can be tapped */
static void
hint (Intro *in, const char *said, const char *tapped)
{
  g_free (in->tapped);
  in->tapped = g_strdup (tapped);
  dictate_stage_set_hint (stage (in), can_listen (in) && in->listens < LISTENS ? said : tapped);
}

static void
show_step (Intro *in)
{
  DictateStage *s = stage (in);
  DictateOrb *orb = dictate_stage_get_orb (s);
  const char *headline = NULL, *sub = NULL;
  g_autofree char *who = NULL, *spoken = NULL;

  dictate_stage_reset (s);
  dictate_stage_clear_buttons (s);
  dictate_stage_set_middle (s, NULL);
  dictate_stage_set_text_first (s, FALSE);
  dictate_orb_set_small (orb, FALSE);
  dictate_dots_set (in->dots, in->step);
  gtk_label_set_label (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (in->corner))),
                       in->step == HELLO ? _("Skip") : _("Back"));
  switch (in->step)
    {
    case HELLO:
      headline = vibe (in, N_("Hello. I'm Vibe."));
      sub = vibe (in, N_("I run this phone by voice. Everything I say is also written here, and everything can be "
                         "tapped."));
      hint (in, vibe (in, N_("Tap anywhere, or say “continue”")), vibe (in, N_("Tap anywhere to go on")));
      break;
    case LANGUAGE:
      {
        const char *name = in->language;

        for (guint j = 0; j < G_N_ELEMENTS (NAMES); j++)
          if (!g_strcmp0 (NAMES[j].code, in->language))
            name = vibe (in, NAMES[j].name);
        headline = vibe (in, N_("Which language shall we speak?"));
        dictate_stage_set_text_first (s, TRUE);
        dictate_stage_set_middle (s, languages_new (in));
        {
          /* TRANSLATORS: the language step's hint; %s is the language spoken now: English */
          g_autofree char *said = g_strdup_printf (vibe (in, N_("Say a language, or “continue” for %s")), name);

          hint (in, said, vibe (in, N_("Tap a language")));
        }
      }
      break;
    case PACE:
      {
        static const char *const LABELS[] = { N_("Slower"), N_("Normal"), "1.25×", "1.5×", "2×", "2.5×", "3×" };
        guint i = rate_index (in);
        GtkWidget *b;

        /* TRANSLATORS: over the pace step; %s is the speech rate: 1.25× */
        who = g_strdup_printf (vibe (in, N_("Vibe says · at %s")), i < 2 ? vibe (in, LABELS[i]) : LABELS[i]);
        headline = vibe (in, N_("Is this speed comfortable?"));
        sub = vibe (in, N_("I can go up to three times faster."));
        dictate_orb_set_small (orb, TRUE);
        b = dictate_stage_add_button (s, vibe (in, N_("Slower")), DICTATE_STAGE_BUTTON);
        g_signal_connect (b, "clicked", G_CALLBACK (on_slower), in);
        b = dictate_stage_add_button (s, vibe (in, N_("That's good")), DICTATE_STAGE_BUTTON_PRIMARY);
        g_signal_connect (b, "clicked", G_CALLBACK (on_good), in);
        b = dictate_stage_add_button (s, vibe (in, N_("Faster")), DICTATE_STAGE_BUTTON);
        g_signal_connect (b, "clicked", G_CALLBACK (on_faster), in);
        hint (in, vibe (in, N_("Say “slower”, “faster” or “that's good”")), vibe (in, N_("Tap a choice")));
      }
      break;
    case ACCESS:
      headline = vibe (in, N_("What may I use on this phone?"));
      sub = vibe (in, N_("What I read goes only to VibeOS's own model server, never to anyone else. For the "
                         "weather, the town's rough position goes to Open-Meteo."));
      dictate_stage_set_text_first (s, TRUE);
      dictate_stage_set_middle (s, chips_new (in));
      hint (in, vibe (in, N_("Say “turn off files”, or “continue”")),
            vibe (in, N_("Tap one to turn it off or on, or anywhere else to go on")));
      break;
    case READY:
      headline = vibe (in, N_("I'm ready. One rule:"));
      sub = rule (in);
      dictate_stage_set_text_first (s, TRUE);
      dictate_stage_set_middle (s, tries_new (in));
      dictate_stage_set_hint (s, _("Tap anywhere and speak"));
      break;
    case VOICE:
      return;
    }
  dictate_stage_set_who (s, who ? who : vibe (in, N_("Vibe says")), FALSE);
  dictate_stage_set_text (s, headline, sub);
  spoken = sub ? g_strconcat (headline, " ", sub, NULL) : g_strdup (headline);
  say (in, spoken);
}

/* the voice step on a screen of its own, with the introduction's dots; the other steps on the stage */
static void
go (Intro *in, Step step)
{
  /* no sample can be recorded with no microphone, or before Voice match runs on this phone; one recorded already is
   * kept */
  if (step == VOICE && (!can_listen (in) || !dictate_voice_match_ready () || dictate_voice_match_enrolled ()))
    step = in->step < VOICE ? READY : ACCESS;
  if (in->listening)
    {
      in->listening = FALSE;
      dictate_engine_stop (in->engine);
    }
  if (in->saying)
    {
      in->saying = FALSE;
      dictate_talk_stop (in->talk);
    }
  in->step = step;
  if (in->enroll)
    {
      gtk_box_remove (GTK_BOX (in->page), in->enroll);
      in->enroll = NULL;
      gtk_widget_set_visible (in->stage, TRUE);
    }
  if (step == VOICE)
    {
      GtkWidget *dots = dictate_dots_new (STEPS);

      dictate_dots_set (dots, VOICE);
      in->enroll = dictate_enroll_new (in->settings, in->talk, in->engine, in->language, _("Skip"), on_enrolled, in);
      dictate_enroll_set_dots (in->enroll, dots);
      gtk_widget_set_vexpand (in->enroll, TRUE);
      gtk_widget_set_visible (in->stage, FALSE);
      gtk_box_append (GTK_BOX (in->page), in->enroll);
      dictate_enroll_start (in->enroll);
      return;
    }
  show_step (in);
}

static void
intro_free (gpointer data)
{
  Intro *in = data;

  /* however it ends, it is not shown again */
  g_settings_set_boolean (in->settings, "first-run", FALSE);
  g_signal_handlers_disconnect_by_data (in->engine, in);
  g_signal_handlers_disconnect_by_data (in->talk, in);
  dictate_engine_stop (in->engine);
  if (in->saying)
    dictate_talk_stop (in->talk);
  g_clear_object (&in->engine);
  g_clear_object (&in->talk);
  g_clear_object (&in->settings);
  g_ptr_array_unref (in->chips);
  g_string_free (in->heard, TRUE);
  g_free (in->tapped);
  g_free (in->language);
  g_free (in);
}

GtkWidget *
dictate_onboarding_new (GtkWidget *window, GSettings *settings, DictateTalk *talk,
                        const DictateOnboardingHooks *hooks)
{
  Intro *in = g_new0 (Intro, 1);

  in->window = window;
  in->settings = g_object_ref (settings);
  in->talk = g_object_ref (talk);
  in->hooks = *hooks;
  in->language = g_strdup (hooks->language (hooks->data));
  in->heard = g_string_new (NULL);
  in->chips = g_ptr_array_new ();
  in->engine = dictate_engine_new ();
  in->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (in->page, "dictate-stage");
  in->stage = dictate_stage_new (in->engine);
  gtk_widget_set_vexpand (in->stage, TRUE);
  in->dots = dictate_dots_new (STEPS);
  dictate_stage_set_header_start (stage (in), in->dots);
  in->corner = vibeos_text_button_new (_("Skip"));
  g_signal_connect (in->corner, "clicked", G_CALLBACK (on_corner), in);
  dictate_stage_set_header_end (stage (in), in->corner);
  g_signal_connect (in->stage, "tapped", G_CALLBACK (on_tapped), in);
  gtk_box_append (GTK_BOX (in->page), in->stage);
  g_signal_connect (in->engine, "text", G_CALLBACK (on_engine_text), in);
  g_signal_connect (in->engine, "state-changed", G_CALLBACK (on_engine_state), in);
  g_signal_connect (in->engine, "error", G_CALLBACK (on_engine_error), in);
  g_signal_connect (in->talk, "state-changed", G_CALLBACK (on_talk_state), in);
  g_object_set_data_full (G_OBJECT (in->page), "dictate-onboarding", in, intro_free);
  return in->page;
}

void
dictate_onboarding_start (GtkWidget *onboarding)
{
  Intro *in = g_object_get_data (G_OBJECT (onboarding), "dictate-onboarding");

  in->step = HELLO;
  show_step (in);
}
