/* test-offline: Agent's offline grammar (dictate-offline.c) in its six languages: each command to its tool and
 * arguments, numbers in words, and the replies written from made-up tool results. */
#include "dictate-offline.h"

#include <string.h>

typedef struct {
  const char *lang, *said, *tool, *args;      /* args: a piece the arguments' JSON must have */
} Case;

static const Case CASES[] = {
  { "en", "What time is it?", "get_time", NULL },
  { "en", "How much battery do I have?", "get_battery", NULL },
  { "en", "Set an alarm for 6:30", "set_alarm", "\"time\":\"06:30\"" },
  { "en", "Wake me up at seven thirty pm", "set_alarm", "\"time\":\"19:30\"" },
  { "en", "Set an alarm for half past six.", "set_alarm", "\"time\":\"06:30\"" },
  { "en", "Cancel my 6:45 alarm.", "cancel_alarm", "\"time\":\"06:45\"" },
  { "en", "What alarms do I have?", "get_alarms", NULL },
  { "en", "Set a timer for 10 minutes.", "set_timer", "\"minutes\":10" },
  { "en", "Set a timer for twenty five minutes", "set_timer", "\"minutes\":25" },
  { "en", "Stop the timer.", "cancel_timer", NULL },
  { "en", "Turn off Wi-Fi.", "set_wifi_bluetooth", "\"on\":false" },
  { "en", "Turn on Bluetooth", "set_wifi_bluetooth", "\"radio\":\"bluetooth\",\"on\":true" },
  { "en", "Open the camera.", "open_app", "\"name\":\"camera\"" },
  { "en", "Call Anna.", "propose_call", "\"to\":\"Anna\"" },
  { "en", "Text Anna saying I'm on my way.", "propose_message", "\"to\":\"Anna\",\"text\":\"I'm on my way.\"" },
  { "en", "Send a message to Anna-Lena saying see you", "propose_message", "\"to\":\"Anna-Lena\"" },
  { "en", "Read my messages.", "read_messages", NULL },
  { "en", "Undo that.", "undo_last_action", NULL },
  { "en", "What did you just do?", "get_action_log", NULL },
  { "en", "Answer.", "answer_call", NULL },
  { "en", "Decline and text I'll call you back.", "decline_call", "\"text\":\"I'll call you back.\"" },
  { "en", "Hang up.", "call_control", "hang_up" },
  { "en", "Tell me a joke.", NULL, NULL },
  { "en", "Don't answer it.", NULL, NULL },
  { "de", "Nicht auflegen!", NULL, NULL },
  { "es", "No contestes.", NULL, NULL },
  { "de", "Wie spät ist es?", "get_time", NULL },
  { "de", "Wie viel Akku habe ich noch?", "get_battery", NULL },
  { "de", "Stell einen Wecker auf halb sieben.", "set_alarm", "\"time\":\"06:30\"" },
  { "de", "Weck mich um 7 Uhr 15", "set_alarm", "\"time\":\"07:15\"" },
  { "de", "Lösch den Wecker um 6:45.", "cancel_alarm", "\"time\":\"06:45\"" },
  { "de", "Timer für fünfundzwanzig Minuten", "set_timer", "\"minutes\":25" },
  { "de", "Schalte das WLAN aus.", "set_wifi_bluetooth", "\"radio\":\"wifi\",\"on\":false" },
  { "de", "Schalte Bluetooth ein.", "set_wifi_bluetooth", "\"radio\":\"bluetooth\",\"on\":true" },
  { "de", "Öffne die Kamera.", "open_app", "\"name\":\"Kamera\"" },
  { "de", "Ruf Anna an.", "propose_call", "\"to\":\"Anna\"" },
  { "de", "Schreib Anna, dass ich später komme.", "propose_message", "\"text\":\"ich später komme.\"" },
  { "de", "Lies meine Nachrichten vor.", "read_messages", NULL },
  { "de", "Mach das rückgängig.", "undo_last_action", NULL },
  { "de", "Was hast du gerade gemacht?", "get_action_log", NULL },
  { "de", "Ablehnen.", "decline_call", NULL },
  { "de", "Leg auf.", "call_control", "hang_up" },
  { "es", "¿Qué hora es?", "get_time", NULL },
  { "es", "Pon una alarma a las siete y media.", "set_alarm", "\"time\":\"07:30\"" },
  { "es", "Pon un temporizador de cinco minutos.", "set_timer", "\"minutes\":5" },
  { "es", "Temporizador de treinta y cinco minutos", "set_timer", "\"minutes\":35" },
  { "es", "Apaga el wifi.", "set_wifi_bluetooth", "\"on\":false" },
  { "es", "Llama a Carlos.", "propose_call", "\"to\":\"Carlos\"" },
  { "es", "Escribe a Anna que llego tarde.", "propose_message", "\"to\":\"Anna\",\"text\":\"llego tarde.\"" },
  { "es", "Deshaz eso.", "undo_last_action", NULL },
  { "es", "¿Qué acabas de hacer?", "get_action_log", NULL },
  { "es", "Abre la cámara.", "open_app", "\"name\":\"cámara\"" },
  { "es", "Lee mis mensajes.", "read_messages", NULL },
  { "es", "Contesta.", "answer_call", NULL },
  { "fr", "Quelle heure est-il ?", "get_time", NULL },
  { "fr", "Mets un réveil à sept heures trente", "set_alarm", "\"time\":\"07:30\"" },
  { "fr", "Réveille-moi à six heures et demie", "set_alarm", "\"time\":\"06:30\"" },
  { "fr", "Lance un minuteur de dix minutes", "set_timer", "\"minutes\":10" },
  { "fr", "Minuteur de vingt et une minutes", "set_timer", "\"minutes\":21" },
  { "fr", "Éteins le Wi-Fi.", "set_wifi_bluetooth", "\"on\":false" },
  { "fr", "Appelle Anna.", "propose_call", "\"to\":\"Anna\"" },
  { "fr", "Écris à Anna que j'arrive.", "propose_message", "\"text\":\"j'arrive.\"" },
  { "fr", "Annule.", "undo_last_action", NULL },
  { "fr", "Annule le réveil de 6 h 45", "cancel_alarm", "\"time\":\"06:45\"" },
  { "fr", "Qu'est-ce que tu as fait ?", "get_action_log", NULL },
  { "fr", "Lis mes messages.", "read_messages", NULL },
  { "fr", "Batterie ?", "get_battery", NULL },
  { "it", "Che ore sono?", "get_time", NULL },
  { "it", "Imposta una sveglia alle sette e mezza.", "set_alarm", "\"time\":\"07:30\"" },
  { "it", "Timer di venticinque minuti", "set_timer", "\"minutes\":25" },
  { "it", "Accendi il bluetooth.", "set_wifi_bluetooth", "\"radio\":\"bluetooth\",\"on\":true" },
  { "it", "Chiama Marco.", "propose_call", "\"to\":\"Marco\"" },
  { "it", "Scrivi a Anna che arrivo.", "propose_message", "\"text\":\"arrivo.\"" },
  { "it", "Annulla.", "undo_last_action", NULL },
  { "it", "Cosa hai fatto?", "get_action_log", NULL },
  { "it", "Apri la fotocamera.", "open_app", "\"name\":\"fotocamera\"" },
  { "it", "Leggi i messaggi.", "read_messages", NULL },
  { "ru", "Который час?", "get_time", NULL },
  { "ru", "Поставь будильник на семь тридцать", "set_alarm", "\"time\":\"07:30\"" },
  { "ru", "Таймер на пять минут", "set_timer", "\"minutes\":5" },
  { "ru", "Выключи вайфай", "set_wifi_bluetooth", "\"radio\":\"wifi\",\"on\":false" },
  { "ru", "Позвони Анне", "propose_call", "\"to\":\"Анне\"" },
  { "ru", "Напиши Анне, что я еду", "propose_message", "\"to\":\"Анне\",\"text\":\"я еду\"" },
  { "ru", "Отмени", "undo_last_action", NULL },
  { "ru", "Что ты сделал?", "get_action_log", NULL },
  { "ru", "Открой камеру", "open_app", "\"name\":\"камеру\"" },
  { "ru", "Прочитай сообщения", "read_messages", NULL },
  { "ru", "Сколько заряда?", "get_battery", NULL },
};

static void
test_parse (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (CASES); i++)
    {
      const Case *c = &CASES[i];
      g_autofree char *args = NULL;
      g_autofree char *tool = dictate_offline_parse (c->lang, c->said, &args);

      if (g_strcmp0 (tool, c->tool) != 0 || (c->args && (!args || !strstr (args, c->args))))
        g_error ("[%s] \"%s\": %s %s, wanted %s %s", c->lang, c->said, tool ? tool : "(none)", args ? args : "",
                 c->tool ? c->tool : "(none)", c->args ? c->args : "");
    }
}

static void
test_numbers (void)
{
  static const char *const cases[][3] = {
    { "en", "twenty five past", "25 past" }, { "de", "dreiundvierzig", "43" }, { "es", "veintidós", "22" },
    { "fr", "dix sept heures", "17 heures" }, { "it", "ventotto", "28" }, { "ru", "двадцать пять", "25" },
    { "en", "6.30", "6:30" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *got = dictate_offline_normalize (cases[i][0], cases[i][1]);
      g_assert_cmpstr (got, ==, cases[i][2]);
    }
}

/* made-up tool results, as the tools write them */
static char *
fake_run (const char *name, const char *arguments, gpointer data)
{
  GPtrArray *ran = data;

  g_ptr_array_add (ran, g_strdup (name));
  if (!strcmp (name, "get_time"))
    return g_strdup ("{\"time\":\"14:05\",\"date\":\"2026-11-23\"}");
  if (!strcmp (name, "set_alarm"))
    return g_strdup ("{\"alarm_set\":\"06:30\",\"rings\":\"tomorrow at 06:30\",\"can_undo\":true}");
  if (!strcmp (name, "get_messages") && strstr (arguments, "contact"))
    return g_strdup ("{\"with\":\"Anna\",\"messages\":[{\"from\":\"Anna\",\"text\":\"Bin gleich da\"}]}");
  if (!strcmp (name, "get_messages"))
    return g_strdup ("{\"conversations\":[{\"with\":\"Anna\",\"unread\":1}]}");
  if (!strcmp (name, "undo_last_action"))
    return g_strdup ("{\"error\":\"Agent has done nothing in the last day that could be undone.\"}");
  return g_strdup ("{\"error\":\"no\"}");
}

static void
test_replies (void)
{
  g_autoptr (GPtrArray) ran = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *time = dictate_offline_reply ("de", "Wie spät ist es?", FALSE, fake_run, ran);
  g_autofree char *alarm = dictate_offline_reply ("fr", "Réveil à six heures trente", FALSE, fake_run, ran);
  g_autofree char *read = dictate_offline_reply ("en", "Read my messages", FALSE, fake_run, ran);
  g_autofree char *quiet = dictate_offline_reply ("en", "Read my messages", TRUE, fake_run, ran);
  g_autofree char *undo = dictate_offline_reply ("it", "Annulla", FALSE, fake_run, ran);
  g_autofree char *unknown = dictate_offline_reply ("ru", "Расскажи анекдот", FALSE, fake_run, ran);
  g_autofree char *text = dictate_offline_reply ("en", "Text Anna", FALSE, fake_run, ran);

  g_assert_cmpstr (time, ==, "Es ist 14:05 Uhr.");
  g_assert_cmpstr (alarm, ==, "Réveil réglé à 06:30. Dites « annuler » pour le supprimer.");
  g_assert_cmpstr (read, ==, "Unread messages: 1. The newest, from Anna: Bin gleich da.");
  /* private mode on the speaker: no names, no texts */
  g_assert_cmpstr (quiet, ==, "Unread messages: 1.");
  g_assert_cmpstr (undo, ==, "Non c'è niente da annullare.");
  g_assert_nonnull (strstr (unknown, "Офлайн"));
  /* no text: asked for, nothing run */
  g_assert_nonnull (strstr (text, "What should the message say"));
  g_assert_cmpuint (ran->len, ==, 6);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/offline/parse", test_parse);
  g_test_add_func ("/offline/numbers", test_numbers);
  g_test_add_func ("/offline/replies", test_replies);
  return g_test_run ();
}
