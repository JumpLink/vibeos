/* dictate-offline: see dictate-offline.h. What was said is cut into words (each kept twice: folded, lower case and
 * without accents, for matching; and as it was said, for a name to look up), numbers in words become numbers, and
 * the commands are tried in a fixed order, each a few word patterns per language. */
#include "dictate-offline.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- the languages' words ---------------- */

typedef enum { EN, DE, ES, FR, IT, RU, N_LANG } Lang;

static Lang
lang_of (const char *language)
{
  static const char *codes[] = { "en", "de", "es", "fr", "it", "ru" };

  for (guint i = 0; i < G_N_ELEMENTS (codes); i++)
    if (language && g_str_has_prefix (language, codes[i]))
      return i;
  return EN;
}

typedef struct {
  const char *word;
  int value;
} Number;

/* the numbers to 59 as words, folded; "ein", "un", "una", "uno", "une" are left out: they are articles too */
static const Number NUMBERS[N_LANG][32] = {
  [EN] = { { "zero", 0 }, { "one", 1 }, { "two", 2 }, { "three", 3 }, { "four", 4 }, { "five", 5 }, { "six", 6 },
           { "seven", 7 }, { "eight", 8 }, { "nine", 9 }, { "ten", 10 }, { "eleven", 11 }, { "twelve", 12 },
           { "thirteen", 13 }, { "fourteen", 14 }, { "fifteen", 15 }, { "sixteen", 16 }, { "seventeen", 17 },
           { "eighteen", 18 }, { "nineteen", 19 }, { "twenty", 20 }, { "thirty", 30 }, { "forty", 40 },
           { "fifty", 50 } },
  [DE] = { { "null", 0 }, { "eins", 1 }, { "eine", 1 }, { "zwei", 2 }, { "drei", 3 }, { "vier", 4 }, { "funf", 5 },
           { "sechs", 6 }, { "sieben", 7 }, { "acht", 8 }, { "neun", 9 }, { "zehn", 10 }, { "elf", 11 },
           { "zwolf", 12 }, { "dreizehn", 13 }, { "vierzehn", 14 }, { "funfzehn", 15 }, { "sechzehn", 16 },
           { "siebzehn", 17 }, { "achtzehn", 18 }, { "neunzehn", 19 }, { "zwanzig", 20 }, { "dreissig", 30 },
           { "vierzig", 40 }, { "funfzig", 50 } },
  [ES] = { { "cero", 0 }, { "dos", 2 }, { "tres", 3 }, { "cuatro", 4 }, { "cinco", 5 }, { "seis", 6 },
           { "siete", 7 }, { "ocho", 8 }, { "nueve", 9 }, { "diez", 10 }, { "once", 11 }, { "doce", 12 },
           { "trece", 13 }, { "catorce", 14 }, { "quince", 15 }, { "dieciseis", 16 }, { "diecisiete", 17 },
           { "dieciocho", 18 }, { "diecinueve", 19 }, { "veinte", 20 }, { "treinta", 30 }, { "cuarenta", 40 },
           { "cincuenta", 50 } },
  [FR] = { { "zero", 0 }, { "deux", 2 }, { "trois", 3 }, { "quatre", 4 }, { "cinq", 5 }, { "six", 6 },
           { "sept", 7 }, { "huit", 8 }, { "neuf", 9 }, { "dix", 10 }, { "onze", 11 }, { "douze", 12 },
           { "treize", 13 }, { "quatorze", 14 }, { "quinze", 15 }, { "seize", 16 }, { "vingt", 20 },
           { "trente", 30 }, { "quarante", 40 }, { "cinquante", 50 } },
  [IT] = { { "zero", 0 }, { "due", 2 }, { "tre", 3 }, { "quattro", 4 }, { "cinque", 5 }, { "sei", 6 },
           { "sette", 7 }, { "otto", 8 }, { "nove", 9 }, { "dieci", 10 }, { "undici", 11 }, { "dodici", 12 },
           { "tredici", 13 }, { "quattordici", 14 }, { "quindici", 15 }, { "sedici", 16 }, { "diciassette", 17 },
           { "diciotto", 18 }, { "diciannove", 19 }, { "venti", 20 }, { "trenta", 30 }, { "quaranta", 40 },
           { "cinquanta", 50 } },
  [RU] = { { "ноль", 0 }, { "один", 1 }, { "одна", 1 }, { "одну", 1 }, { "два", 2 }, { "две", 2 }, { "три", 3 },
           { "четыре", 4 }, { "пять", 5 }, { "шесть", 6 }, { "семь", 7 }, { "восемь", 8 }, { "девять", 9 },
           { "десять", 10 }, { "одиннадцать", 11 }, { "двенадцать", 12 }, { "тринадцать", 13 },
           { "четырнадцать", 14 }, { "пятнадцать", 15 }, { "шестнадцать", 16 }, { "семнадцать", 17 },
           { "восемнадцать", 18 }, { "девятнадцать", 19 }, { "двадцать", 20 }, { "тридцать", 30 },
           { "сорок", 40 }, { "пятьдесят", 50 } },
};

/* the word between tens and units ("treinta y cinco", "vingt et un") */
static const char *const JOINS[N_LANG] = { "and", "und", "y", "et", "e", "" };

static int
number_word (Lang l, const char *w)
{
  for (guint i = 0; i < G_N_ELEMENTS (NUMBERS[l]) && NUMBERS[l][i].word; i++)
    if (!strcmp (NUMBERS[l][i].word, w))
      return NUMBERS[l][i].value;
  return -1;
}

/* a number in one word: digits, a number word, or a compound ("funfundzwanzig", "veinticinco", "ventotto") */
static int
number_of (Lang l, const char *w)
{
  static const char *const ES_UNITS[] = { "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho",
                                          "nueve" };
  static const char *const IT_STEMS[] = { "vent", "trent", "quarant", "cinquant" };
  static const char *const IT_UNITS[] = { "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto",
                                          "nove" };
  int v;

  if (*w && strspn (w, "0123456789") == strlen (w) && strlen (w) <= 4)
    return atoi (w);
  if ((v = number_word (l, w)) >= 0)
    return v;
  if (l == DE)
    {
      const char *und = strstr (w, "und");
      if (und && und != w)
        {
          g_autofree char *unit = g_strndup (w, und - w);
          int u = number_word (l, unit), t = number_word (l, und + 3);
          if (!strcmp (unit, "ein"))
            u = 1;
          if (u > 0 && u < 10 && t >= 20 && t % 10 == 0)
            return t + u;
        }
    }
  if (l == ES && g_str_has_prefix (w, "veinti"))
    for (guint i = 0; i < G_N_ELEMENTS (ES_UNITS); i++)
      if (!strcmp (w + 6, ES_UNITS[i]))
        return 21 + i;
  if (l == IT)
    for (guint s = 0; s < G_N_ELEMENTS (IT_STEMS); s++)
      if (g_str_has_prefix (w, IT_STEMS[s]))
        {
          const char *rest = w + strlen (IT_STEMS[s]);
          for (guint i = 0; i < G_N_ELEMENTS (IT_UNITS); i++)
            if (!strcmp (rest, IT_UNITS[i]) || (rest[0] && strchr ("ai", rest[0]) && !strcmp (rest + 1, IT_UNITS[i])))
              return (s + 2) * 10 + i + 1;
        }
  return -1;
}

/* ---------------- what was said, as words ---------------- */

typedef struct {
  char *fold;                   /* lower case, no accents (Russian: lower case, ё as е) */
  char *orig;                   /* as it was said */
  int number;                   /* -1: not a number */
  gsize start, end;             /* where it is in what was said, in bytes */
} Word;

typedef struct {
  Lang lang;
  char *text;                   /* what was said */
  GArray *words;                /* Word */
} Said;

static void
word_clear (Word *w)
{
  g_free (w->fold);
  g_free (w->orig);
}

static char *
fold (Lang l, const char *s)
{
  g_autofree char *down = g_utf8_strdown (s, -1);
  GString *out;

  if (l != RU)
    return g_str_to_ascii (down, "C");
  out = g_string_new (NULL);
  for (const char *p = down; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      g_string_append_unichar (out, c == 0x0451 ? 0x0435 : c);
    }
  return g_string_free (out, FALSE);
}

/* `said` into words: letters and digits are words, "6:30" and "6.30" one word "6:30", everything else a space */
static Said *
said_new (Lang l, const char *said)
{
  Said *s = g_new0 (Said, 1);
  GString *cur = g_string_new (NULL);
  gsize start = 0;

  s->lang = l;
  s->text = g_strdup (said);
  s->words = g_array_new (FALSE, TRUE, sizeof (Word));
  g_array_set_clear_func (s->words, (GDestroyNotify) word_clear);
  for (const char *p = said;; p = g_utf8_next_char (p))
    {
      gunichar c = *p ? g_utf8_get_char (p) : 0;
      gboolean time_mark = (c == ':' || c == '.') && cur->len && g_ascii_isdigit (cur->str[cur->len - 1])
                           && g_ascii_isdigit (p[1]);

      if (c && (g_unichar_isalnum (c) || time_mark))
        {
          if (!cur->len)
            start = p - said;
          g_string_append_unichar (cur, time_mark ? ':' : c);
          continue;
        }
      if (cur->len)
        {
          Word w = { fold (l, cur->str), g_strdup (cur->str), -1, start, p - said };
          w.number = strchr (w.fold, ':') ? -1 : number_of (l, w.fold);
          g_array_append_val (s->words, w);
          g_string_truncate (cur, 0);
        }
      if (!c)
        break;
    }
  g_string_free (cur, TRUE);
  /* tens and units said apart are one number: "twenty five", "treinta y cinco", "vingt et un", "dix sept" */
  for (guint i = 0; i + 1 < s->words->len; i++)
    {
      Word *a = &g_array_index (s->words, Word, i);
      guint j = i + 1;
      Word *b = &g_array_index (s->words, Word, j);
      gboolean tens = a->number >= 20 && a->number % 10 == 0 && a->number <= 50;

      if (!tens && !(l == FR && a->number == 10))
        continue;
      if (*JOINS[l] && !strcmp (b->fold, JOINS[l]) && j + 1 < s->words->len)
        b = &g_array_index (s->words, Word, ++j);
      if (l == FR && b->number < 0 && (!strcmp (b->fold, "un") || !strcmp (b->fold, "une")))
        b->number = 1;
      if (b->number > 0 && b->number < 10 && (tens || b->number >= 7))
        {
          a->number += b->number;
          a->end = b->end;
          g_array_remove_range (s->words, i + 1, j - i);
        }
    }
  return s;
}

static void
said_free (Said *s)
{
  g_array_unref (s->words);
  g_free (s->text);
  g_free (s);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Said, said_free)

static Word *
word (Said *s, guint i)
{
  return i < s->words->len ? &g_array_index (s->words, Word, i) : NULL;
}

/* where the phrase (words apart by spaces; "*" any one word) starts, -1 for nowhere */
static int
find (Said *s, const char *phrase)
{
  g_auto (GStrv) parts = g_strsplit (phrase, " ", -1);
  guint n = g_strv_length (parts);

  for (guint i = 0; i + n <= s->words->len; i++)
    {
      guint k = 0;
      while (k < n && (!strcmp (parts[k], "*") || !strcmp (parts[k], word (s, i + k)->fold)))
        k++;
      if (k == n)
        return i;
    }
  return -1;
}

/* the first of `phrases` ("a|b c|d") found: its start, and through *end where it ends; -1 for none */
static int
find_any (Said *s, const char *phrases, guint *end)
{
  g_auto (GStrv) each = g_strsplit (phrases, "|", -1);
  int best = -1;
  guint best_end = 0;

  for (guint i = 0; each[i]; i++)
    {
      int at = find (s, each[i]);
      g_auto (GStrv) parts = g_strsplit (each[i], " ", -1);
      guint n = g_strv_length (parts);

      if (at >= 0 && (best < 0 || at < best))
        best = at, best_end = at + n;
    }
  if (end)
    *end = best_end;
  return best;
}

/* a word starting with one of `stems` ("a|b"), its index or -1: for Russian's endings and the like */
static int
find_stem (Said *s, const char *stems)
{
  g_auto (GStrv) each = g_strsplit (stems, "|", -1);

  for (guint i = 0; i < s->words->len; i++)
    for (guint k = 0; each[k]; k++)
      if (g_str_has_prefix (word (s, i)->fold, each[k]))
        return i;
  return -1;
}

static gboolean
has (Said *s, const char *phrases)
{
  return find_any (s, phrases, NULL) >= 0;
}

/* the words from `from` to `to` (exclusive) as they were said, with what is between them ("I'll", "Anna-Lena");
 * NULL when there are none */
static char *
span (Said *s, guint from, guint to)
{
  to = MIN (to, s->words->len);
  if (from >= to)
    return NULL;
  return g_strndup (s->text + word (s, from)->start, word (s, to - 1)->end - word (s, from)->start);
}

/* what was said from word `from` to the end, its last punctuation too (a message's text) */
static char *
rest (Said *s, guint from)
{
  if (from >= s->words->len)
    return NULL;
  return g_strstrip (g_strdup (s->text + word (s, from)->start));
}

/* ---------------- the commands' words, per language (folded) ---------------- */

typedef struct {
  const char *answer, *decline, *hangup;
  const char *did, *undo, *undo_bare;
  const char *wifi, *bluetooth, *on, *off;
  const char *timer, *alarm, *cancel, *time, *battery;
  const char *messages, *send, *saying, *call, *open;
  const char *at, *pm, *am;
  const char *minutes, *hours, *seconds, *half_hour, *one;
  const char *drop;             /* little words before a name: "to", "a", "an" */
} Words;

static const Words WORDS[N_LANG] = {
  [EN] = {
    .answer = "answer|pick up|accept the call|accept it",
    .decline = "decline|reject",
    .hangup = "hang up|end the call|end call",
    .did = "what did you do|what did you just do|what have you done|what have you just done|what was the last thing",
    .undo = "undo|take that back|take it back|put it back",
    .undo_bare = "",
    .wifi = "wifi|wi fi|wireless", .bluetooth = "bluetooth",
    .on = "on|enable|switch on|turn on", .off = "off|disable|switch off|turn off",
    .timer = "timer", .alarm = "alarm|alarms|wake me",
    .cancel = "cancel|delete|remove|stop|turn off|switch off|clear",
    .time = "what time|the time|time is it", .battery = "battery|charge",
    .messages = "messages|message|texts|text|sms",
    .send = "text|message|send a message to|send a text to|send an sms to|send message to|write to",
    .saying = "saying|that says|that|to say",
    .call = "call|phone|ring|dial",
    .open = "open|launch|start",
    .at = "at|for", .pm = "pm|p m|in the evening|tonight|in the afternoon", .am = "am|a m|in the morning",
    .minutes = "minute|minutes|min|mins", .hours = "hour|hours", .seconds = "second|seconds|sec|secs",
    .half_hour = "half an hour|half hour", .one = "a|an",
    .drop = "to|the|my|app|application",
  },
  [DE] = {
    .answer = "annehmen|nimm an|nimm ab|rangehen|abheben|heb ab|antworte|annehmen",
    .decline = "ablehnen|lehne ab|lehn ab|abweisen|wegdrucken",
    .hangup = "auflegen|leg auf|lege auf|beende den anruf|anruf beenden|gesprach beenden",
    .did = "was hast du gemacht|was hast du gerade gemacht|was hast du getan|was hast du gerade getan"
           "|was war das letzte",
    .undo = "ruckgangig|mach das ruckgangig|nimm das zuruck",
    .undo_bare = "",
    .wifi = "wlan|wifi|wi fi", .bluetooth = "bluetooth",
    .on = "an|ein|einschalten|anschalten|aktivieren|schalte * ein|schalte * an",
    .off = "aus|ausschalten|abschalten|deaktivieren",
    .timer = "timer|countdown", .alarm = "wecker|weck mich|wecke mich|alarm",
    .cancel = "losch|losche|loschen|entfern|entferne|entfernen|stopp|stoppe|stoppen|abbrechen|brich|aus|beende",
    .time = "wie spat|uhrzeit|wie viel uhr|wieviel uhr", .battery = "akku|batterie|ladestand",
    .messages = "nachrichten|nachricht|sms",
    .send = "schreib|schreibe|sms an|nachricht an|schick|schicke",
    .saying = "dass|mit dem text|mit|und sag",
    .call = "ruf|rufe|anrufen|telefoniere mit",
    .open = "offne|offnen|starte|starten|mach * auf",
    .at = "um|fur|auf", .pm = "abends|am abend|nachmittags|am nachmittag", .am = "morgens|fruh|am morgen",
    .minutes = "minute|minuten|min", .hours = "stunde|stunden", .seconds = "sekunde|sekunden|sek",
    .half_hour = "halbe stunde", .one = "ein|einen",
    .drop = "an|den|die|das|dem|der|app|bitte",
  },
  [ES] = {
    .answer = "contesta|contestar|responde|responder|acepta la llamada|aceptar la llamada|coge|cogela",
    .decline = "rechaza|rechazar|rechazala",
    .hangup = "cuelga|colgar|termina la llamada|terminar la llamada",
    .did = "que hiciste|que has hecho|que acabas de hacer|que fue lo ultimo",
    .undo = "deshaz|deshacer|deshazlo",
    .undo_bare = "",
    .wifi = "wifi|wi fi", .bluetooth = "bluetooth",
    .on = "enciende|encender|activa|activar|pon", .off = "apaga|apagar|desactiva|desactivar|quita",
    .timer = "temporizador|timer", .alarm = "alarma|alarmas|despiertame|despertador",
    .cancel = "cancela|cancelar|borra|borrar|elimina|eliminar|quita|quitar|para|parar|apaga|detén|deten",
    .time = "que hora|hora es", .battery = "bateria|carga",
    .messages = "mensajes|mensaje|sms",
    .send = "escribe a|escribele a|manda un mensaje a|envia un mensaje a|manda un sms a|envia un sms a",
    .saying = "que diga|diciendo|que",
    .call = "llama a|llamar a|llama al|marca a",
    .open = "abre|abrir|inicia|iniciar",
    .at = "a las|para las|a la", .pm = "de la tarde|de la noche|por la tarde|por la noche",
    .am = "de la manana|por la manana|de la madrugada",
    .minutes = "minuto|minutos|min", .hours = "hora|horas", .seconds = "segundo|segundos",
    .half_hour = "media hora", .one = "un|una",
    .drop = "a|al|la|el|los|las|app|aplicacion|de",
  },
  [FR] = {
    .answer = "reponds|repondre|decroche|decrocher|accepte l appel|accepter l appel",
    .decline = "refuse|refuser|rejette|rejeter|refuse l appel",
    .hangup = "raccroche|raccrocher|termine l appel|terminer l appel|mets fin a l appel",
    .did = "qu est ce que tu as fait|qu as tu fait|qu est ce que tu viens de faire|qu as tu fait a l instant",
    .undo = "annule ca|annuler ca|annule le dernier|annule la derniere|defais|defaire|reviens en arriere",
    .undo_bare = "annule|annuler",
    .wifi = "wifi|wi fi", .bluetooth = "bluetooth",
    .on = "allume|allumer|active|activer|mets", .off = "eteins|eteindre|desactive|desactiver|coupe|couper",
    .timer = "minuteur|minuterie|timer|compte a rebours", .alarm = "reveil|reveils|alarme|alarmes|reveille moi",
    .cancel = "annule|annuler|supprime|supprimer|efface|effacer|arrete|arreter|enleve|enlever|desactive",
    .time = "quelle heure|l heure|heure est il", .battery = "batterie|charge",
    .messages = "messages|message|sms|textos",
    .send = "ecris a|ecrire a|envoie un message a|envoie un sms a|envoyer un message a|ecris un message a",
    .saying = "disant|qui dit|pour dire|que",
    .call = "appelle|appeler|telephone a|compose",
    .open = "ouvre|ouvrir|lance|lancer|demarre",
    .at = "a|pour", .pm = "du soir|de l apres midi|ce soir", .am = "du matin|ce matin",
    .minutes = "minute|minutes|min", .hours = "heure|heures", .seconds = "seconde|secondes",
    .half_hour = "demi heure", .one = "un|une",
    .drop = "a|au|l|la|le|les|appli|application|de",
  },
  [IT] = {
    .answer = "rispondi|rispondere|accetta la chiamata|accettare la chiamata",
    .decline = "rifiuta|rifiutare|rifiutala",
    .hangup = "riattacca|riattaccare|riaggancia|chiudi la chiamata|termina la chiamata|metti giu",
    .did = "cosa hai fatto|che hai fatto|che cosa hai fatto|cosa hai appena fatto|qual e l ultima cosa",
    .undo = "annulla quello|annulla l ultima|annulla l ultimo|torna indietro",
    .undo_bare = "annulla|annullare",
    .wifi = "wifi|wi fi", .bluetooth = "bluetooth",
    .on = "accendi|accendere|attiva|attivare", .off = "spegni|spegnere|disattiva|disattivare",
    .timer = "timer|conto alla rovescia", .alarm = "sveglia|sveglie|svegliami|allarme",
    .cancel = "cancella|cancellare|elimina|eliminare|annulla|annullare|ferma|fermare|togli|togliere|disattiva",
    .time = "che ore|che ora|ore sono", .battery = "batteria|carica",
    .messages = "messaggi|messaggio|sms",
    .send = "scrivi a|scrivi ad|scrivere a|manda un messaggio a|invia un messaggio a|manda un sms a",
    .saying = "che dice|dicendo|per dire|che",
    .call = "chiama|chiamare|telefona a",
    .open = "apri|aprire|avvia|avviare|lancia",
    .at = "alle|per le|all", .pm = "di sera|del pomeriggio|stasera", .am = "di mattina|del mattino|stamattina",
    .minutes = "minuto|minuti|min", .hours = "ora|ore", .seconds = "secondo|secondi",
    .half_hour = "mezz ora|mezzora|mezza ora", .one = "un|una|uno",
    .drop = "a|ad|al|la|il|lo|l|app|applicazione|di",
  },
  [RU] = {
    .answer = "ответь|ответить|прими|принять|возьми трубку",
    .decline = "отклони|отклонить|сбрось|сбросить",
    .hangup = "положи трубку|заверши звонок|завершить звонок|повесь трубку",
    .did = "что ты сделал|что ты сейчас сделал|что ты только что сделал|что ты сделала",
    .undo = "отмени это|отмени последнее|верни как было",
    .undo_bare = "отмени|отменить",
    .wifi = "wifi|wi fi|вайфай|вай фай|wi|фай", .bluetooth = "bluetooth|блютуз|блютус",
    .on = "включи|включить|включай", .off = "выключи|выключить|отключи|отключить",
    .timer = "таймер|таймера", .alarm = "будильник|будильника|будильники|разбуди",
    .cancel = "отмени|отменить|удали|удалить|выключи|выключить|убери|убрать|останови|остановить|сбрось",
    .time = "который час|сколько времени|какое время|время", .battery = "батарея|батареи|заряд|аккумулятор",
    .messages = "сообщения|сообщение|сообщений|смс|эсэмэс",
    .send = "напиши|написать|отправь сообщение|отправь смс|пошли сообщение|пошли смс",
    .saying = "что|с текстом",
    .call = "позвони|позвонить|набери|звони",
    .open = "открой|открыть|запусти|запустить",
    .at = "на|в", .pm = "вечера|вечером|дня", .am = "утра|утром",
    .minutes = "минуту|минуты|минут|мин", .hours = "час|часа|часов", .seconds = "секунду|секунды|секунд",
    .half_hour = "полчаса", .one = "",
    .drop = "приложение",
  },
};


/* whether `w` is one of `list` ("a|b|c") */
static gboolean
in_list (const char *list, const char *w)
{
  g_auto (GStrv) each = g_strsplit (list, "|", -1);

  return w && g_strv_contains ((const char *const *) each, w);
}

/* minutes or seconds: what a timer's length is in ("sept heures" is a time, "sept minutes" is not) */
static gboolean
is_unit (Said *s, Word *w)
{
  const Words *ws = &WORDS[s->lang];

  return w && (in_list (ws->minutes, w->fold) || in_list (ws->seconds, w->fold));
}

/* "h", "uhr", "heures", "ore", "horas", "часов", "o" (of "o'clock"): the word after an hour */
static gboolean
is_hour_word (Word *w)
{
  return w && w->number < 0 && (!strcmp (w->fold, "h") || !strcmp (w->fold, "uhr") || !strcmp (w->fold, "o")
                                || g_str_has_prefix (w->fold, "heure") || !strcmp (w->fold, "ore")
                                || g_str_has_prefix (w->fold, "hora") || g_str_has_prefix (w->fold, "час"));
}

/* a "don't" before word `upto`: "don't answer" is no command to answer */
static gboolean
negated (Said *s, guint upto)
{
  static const char *const NOT[N_LANG] = {
    [EN] = "don t|do not|not|never|dont", [DE] = "nicht|nein|kein|keine", [ES] = "no|nunca",
    [FR] = "ne|pas|n|jamais", [IT] = "non|mai", [RU] = "не|нет",
  };
  int at = find_any (s, NOT[s->lang], NULL);

  return at >= 0 && (guint) at < upto;
}

/* the time in `s`, as HH:MM: "6:30", "6 30", "seven thirty pm", "halb sieben", "7 y media", "sept heures et demie",
 * "7 e mezza", "half past six"; FALSE when there is none */
static gboolean
time_in (Said *s, char **out)
{
  const Words *ws = &WORDS[s->lang];
  int hour = -1, minute = 0, at;
  guint after = 0, end;

  /* "half past six", "quarter to seven", "halb sieben", "viertel nach sieben" */
  if (s->lang == EN && (at = find_any (s, "half past|quarter past|quarter to", &end)) >= 0 && word (s, end)
      && word (s, end)->number > 0 && word (s, end)->number < 24)
    {
      hour = word (s, end)->number;
      if (!strcmp (word (s, at)->fold, "half"))
        minute = 30;
      else if (!strcmp (word (s, at + 1)->fold, "to"))
        minute = 45, hour = (hour + 23) % 24;
      else
        minute = 15;
    }
  else if (s->lang == DE && (at = find_any (s, "halb|viertel nach|viertel vor", &end)) >= 0 && word (s, end)
           && word (s, end)->number > 0 && word (s, end)->number < 24)
    {
      hour = word (s, end)->number;
      if (!strcmp (word (s, at)->fold, "halb"))
        hour = (hour + 23) % 24, minute = 30;
      else if (!strcmp (word (s, at + 1)->fold, "vor"))
        hour = (hour + 23) % 24, minute = 45;
      else
        minute = 15;
    }
  for (guint i = 0; i < s->words->len && hour < 0; i++)
    {
      Word *a = word (s, i);
      int h, m;
      guint j;

      if (strchr (a->fold, ':') && sscanf (a->fold, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60)
        {
          hour = h, minute = m, after = i + 1;
          break;
        }
      /* a timer's length ("10 minutes") is not a time */
      if (a->number < 0 || a->number > 23 || is_unit (s, word (s, i + 1)))
        continue;
      hour = a->number, after = j = i + 1;
      if (is_hour_word (word (s, j)))
        j++;
      if (word (s, j) && (!strcmp (word (s, j)->fold, "e") || !strcmp (word (s, j)->fold, "y")
                          || !strcmp (word (s, j)->fold, "clock")) && word (s, j + 1) && word (s, j + 1)->number >= 0)
        j++;
      if (word (s, j) && word (s, j)->number >= 0 && word (s, j)->number < 60 && !is_unit (s, word (s, j + 1)))
        minute = word (s, j)->number, after = j + 1;
    }
  if (hour < 0)
    return FALSE;
  if (minute == 0 && after)
    {
      /* "7 y media", "7 et demie", "7 e mezza", "7 y cuarto", "7 menos cuarto" just after the hour */
      if ((at = find_any (s, "y media|et demie|e mezza|e mezzo", &end)) >= 0 && at <= (int) after + 1)
        minute = 30;
      else if ((at = find_any (s, "y cuarto|et quart|e un quarto|e quarto", &end)) >= 0 && at <= (int) after + 1)
        minute = 15;
      else if ((at = find_any (s, "menos cuarto|moins le quart|moins quart|meno un quarto|meno quarto", &end)) >= 0
               && at <= (int) after + 1)
        minute = 45, hour = (hour + 23) % 24;
    }
  if (hour < 12 && has (s, ws->pm))
    hour += 12;
  else if (hour == 12 && has (s, ws->am))
    hour = 0;
  *out = g_strdup_printf ("%02d:%02d", hour, minute);
  return TRUE;
}

/* a timer's length in seconds: "10 minutes", "an hour 20 minutes", "half an hour", "полчаса"; 0 for none */
static int
length_in (Said *s)
{
  const Words *ws = &WORDS[s->lang];
  int total = has (s, ws->half_hour) ? 30 * 60 : 0;

  for (guint i = 1; i < s->words->len; i++)
    {
      Word *unit = word (s, i), *before = word (s, i - 1);
      int scale = in_list (ws->minutes, unit->fold) ? 60 : in_list (ws->hours, unit->fold) ? 3600
                  : in_list (ws->seconds, unit->fold) ? 1 : 0;
      int n = before->number;

      if (!scale)
        continue;
      if (n < 0 && *ws->one && in_list (ws->one, before->fold))
        n = 1;
      if (n > 0)
        total += n * scale;
    }
  /* "полчаса" is one word: also an hour word's prefix, so the hours above did not count it */
  return total;
}

/* the name after word `from`: the words up to `to` (or the end), the little words before it dropped */
static char *
name_from (Said *s, guint from, guint to)
{
  const Words *ws = &WORDS[s->lang];

  while (from < to && from < s->words->len && in_list (ws->drop, word (s, from)->fold))
    from++;
  while (to > from && to <= s->words->len && in_list (ws->drop, word (s, to - 1)->fold))
    to--;
  return span (s, from, to);
}

static char *
args_of (JsonBuilder *b)
{
  g_autoptr (JsonNode) root = json_builder_get_root (b);

  return json_to_string (root, FALSE);
}

static char *
one_string (const char *key, const char *value)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  if (key)
    {
      json_builder_set_member_name (b, key);
      json_builder_add_string_value (b, value);
    }
  json_builder_end_object (b);
  return args_of (b);
}

char *
dictate_offline_normalize (const char *language, const char *said)
{
  g_autoptr (Said) s = said_new (lang_of (language), said);
  GString *out = g_string_new (NULL);

  for (guint i = 0; i < s->words->len; i++)
    {
      if (out->len)
        g_string_append_c (out, ' ');
      if (word (s, i)->number >= 0)
        g_string_append_printf (out, "%d", word (s, i)->number);
      else
        g_string_append (out, word (s, i)->fold);
    }
  return g_string_free (out, FALSE);
}

char *
dictate_offline_parse (const char *language, const char *said, char **arguments)
{
  g_autoptr (Said) s = said_new (lang_of (language), said);
  const Words *ws = &WORDS[s->lang];
  g_autofree char *time = NULL;
  gboolean alarm, timer, cancel;
  guint end, cut;
  int at;

  *arguments = NULL;
  if (!s->words->len)
    return NULL;
  /* a call: declined (maybe with a text after it), answered, hung up; "don't" before it, none of them */
  if ((at = find_any (s, ws->decline, &end)) >= 0)
    {
      if (negated (s, at + 1))
        return NULL;
      g_autoptr (JsonBuilder) b = json_builder_new ();
      int t = find_any (s, ws->send, &cut);

      json_builder_begin_object (b);
      if (t > at)
        {
          int saying = find_any (s, ws->saying, &end);
          g_autofree char *text = rest (s, saying > t ? end : cut);
          if (text)
            {
              json_builder_set_member_name (b, "text");
              json_builder_add_string_value (b, text);
            }
        }
      json_builder_end_object (b);
      *arguments = args_of (b);
      return g_strdup ("decline_call");
    }
  if ((at = find_any (s, ws->hangup, NULL)) >= 0)
    {
      if (negated (s, at + 1))
        return NULL;
      *arguments = one_string ("action", "hang_up");
      return g_strdup ("call_control");
    }
  if ((at = find_any (s, ws->answer, NULL)) >= 0)
    {
      if (negated (s, at + 1))
        return NULL;
      *arguments = one_string (NULL, NULL);
      return g_strdup ("answer_call");
    }
  if (has (s, ws->did))
    {
      *arguments = one_string (NULL, NULL);
      return g_strdup ("get_action_log");
    }
  alarm = has (s, ws->alarm) || (s->lang == RU && find_stem (s, "будильник") >= 0);
  timer = has (s, ws->timer) || (s->lang == RU && find_stem (s, "таймер") >= 0);
  if (has (s, ws->undo) || (*ws->undo_bare && !alarm && !timer && word (s, 0) && in_list (ws->undo_bare,
                                                                                            word (s, 0)->fold)))
    {
      *arguments = one_string (NULL, NULL);
      return g_strdup ("undo_last_action");
    }
  /* Wi-Fi or Bluetooth on or off */
  {
    gboolean wifi = has (s, ws->wifi), bt = has (s, ws->bluetooth);
    if (wifi || bt)
      {
        gboolean off = has (s, ws->off), on = has (s, ws->on);
        g_autoptr (JsonBuilder) b = json_builder_new ();

        if (!off && !on)
          return NULL;
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "radio");
        json_builder_add_string_value (b, wifi ? "wifi" : "bluetooth");
        json_builder_set_member_name (b, "on");
        /* "turn off" has "turn" … "off"; "ausschalten" is off even with "an" in it somewhere */
        json_builder_add_boolean_value (b, !off);
        json_builder_end_object (b);
        *arguments = args_of (b);
        return g_strdup ("set_wifi_bluetooth");
      }
  }
  cancel = has (s, ws->cancel);
  if (timer)
    {
      int seconds = length_in (s);
      if (cancel)
        {
          *arguments = one_string (NULL, NULL);
          return g_strdup ("cancel_timer");
        }
      if (seconds > 0)
        {
          g_autoptr (JsonBuilder) b = json_builder_new ();
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "minutes");
          json_builder_add_int_value (b, seconds / 60);
          if (seconds % 60)
            {
              json_builder_set_member_name (b, "seconds");
              json_builder_add_int_value (b, seconds % 60);
            }
          json_builder_end_object (b);
          *arguments = args_of (b);
          return g_strdup ("set_timer");
        }
      *arguments = one_string (NULL, NULL);
      return g_strdup ("get_alarms");
    }
  if (alarm)
    {
      gboolean timed = time_in (s, &time);
      if (cancel)
        {
          *arguments = timed ? one_string ("time", time) : one_string (NULL, NULL);
          return g_strdup ("cancel_alarm");
        }
      if (timed)
        {
          *arguments = one_string ("time", time);
          return g_strdup ("set_alarm");
        }
      *arguments = one_string (NULL, NULL);
      return g_strdup ("get_alarms");
    }
  /* "text Anna saying I'm on my way": before "call" ("send a message" has no "call", but "messages" does) */
  if ((at = find_any (s, ws->send, &cut)) >= 0 && (at == 0 || s->lang != EN || strcmp (word (s, at)->fold, "text")))
    {
      int saying = -1;
      guint after = 0;
      g_autoptr (JsonBuilder) b = json_builder_new ();
      g_autofree char *to = NULL, *text = NULL;

      for (guint i = cut + 1; i < s->words->len && saying < 0; i++)
        if (in_list (ws->saying, word (s, i)->fold))
          saying = i, after = i + 1;
      /* two-word connectors: "that says", "que diga", "qui dit", "che dice", "с текстом" */
      if (saying >= 0 && word (s, after) && in_list ("says|diga|dit|dice|текстом|text", word (s, after)->fold))
        after++;
      to = name_from (s, cut, saying >= 0 ? (guint) saying : s->words->len);
      text = saying >= 0 ? rest (s, after) : NULL;
      /* "read my messages" is not a message to "my" */
      if (to && !in_list ("me|my|mir|meine|mis|mes|miei|mie|мне|мои", word (s, cut) ? word (s, cut)->fold : ""))
        {
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "to");
          json_builder_add_string_value (b, to);
          if (text)
            {
              json_builder_set_member_name (b, "text");
              json_builder_add_string_value (b, text);
            }
          json_builder_end_object (b);
          *arguments = args_of (b);
          return g_strdup ("propose_message");
        }
    }
  if (has (s, ws->messages) || (s->lang == RU && find_stem (s, "сообщен|смс") >= 0))
    {
      *arguments = one_string (NULL, NULL);
      return g_strdup ("read_messages");
    }
  if ((at = find_any (s, ws->call, &cut)) >= 0)
    {
      /* German: "ruf Anna an" */
      guint to = s->words->len;
      g_autofree char *who = NULL;

      if (s->lang == DE && to > cut && !strcmp (word (s, to - 1)->fold, "an"))
        to--;
      who = name_from (s, cut, to);
      if (!who && s->lang == DE && at > 0)
        who = name_from (s, 0, at);
      if (who && !in_list ("me|him|her|them|mich|ihn|sie|me|lo|la|le|lui|leur|mi|gli|меня|ему|ей|им", who))
        {
          *arguments = one_string ("to", who);
          return g_strdup ("propose_call");
        }
      return NULL;
    }
  if ((at = find_any (s, ws->open, &cut)) >= 0)
    {
      g_autofree char *app = name_from (s, cut, s->words->len);
      if (s->lang == DE && app && g_str_has_suffix (app, " auf"))
        app[strlen (app) - 4] = '\0';
      if (app)
        {
          *arguments = one_string ("name", app);
          return g_strdup ("open_app");
        }
      return NULL;
    }
  if (has (s, ws->battery) || (s->lang == RU && find_stem (s, "батаре|заряд") >= 0))
    {
      *arguments = one_string (NULL, NULL);
      return g_strdup ("get_battery");
    }
  if (has (s, ws->time) || (s->lang == DE && find (s, "uhr ist") >= 0))
    {
      *arguments = one_string (NULL, NULL);
      return g_strdup ("get_time");
    }
  return NULL;
}

/* ---------------- the replies ---------------- */

typedef struct {
  const char *notice, *unknown, *failed, *time, *battery, *charging, *alarm_set, *alarm_gone, *alarms, *no_alarms,
             *off, *timers, *no_timers, *timer_set, *timer_gone, *minutes, *radio_on, *radio_off, *wifi, *app,
             *call_card, *message_card, *message_text, *messages, *no_messages, *from, *undone, *no_undo, *did,
             *did_nothing, *answered, *declined, *declined_text, *hung_up;
  /* what an action in the log was, by tool */
  const char *did_alarm, *did_alarm_gone, *did_timer, *did_timer_gone, *did_radio, *did_app, *did_call,
             *did_message, *did_undo, *did_answer, *did_decline, *did_call_control;
} Replies;

static const Replies REPLIES[N_LANG] = {
  [EN] = {
    "I can't reach the server, so I'm working offline with the basics.",
    "Offline I can tell the time and the battery, set alarms and timers, switch Wi-Fi and Bluetooth, open apps, call, "
    "text, and read your messages.",
    "That didn't work.", "It's %s.", "The battery is at %d percent%s.", ", charging",
    "Alarm set for %s. Say undo to remove it.", "The %s alarm is deleted. Say undo to put it back.",
    "Your alarms: %s.", "You have no alarms.", "off", "Timers running: %s.", "No timer is running.",
    "Timer set for %s. Say undo to stop it.", "The timer is stopped. Say undo to start it again.", "%d minutes",
    "%s is on. Say undo to switch it back.", "%s is off. Say undo to switch it back.", "Wi-Fi", "Opening %s.",
    "The call to %s is ready: tap Call to start it.", "The message to %s is ready: tap Send to send it.",
    "What should the message say? Say, for example: text Anna saying I'm on my way.",
    "Unread messages: %d.", "No new messages.", "The newest, from %s: %s", "Undone.", "There's nothing to undo.",
    "The last thing I did was %s.", "I haven't done anything yet.", "Answered.", "Declined.",
    "Declined. The message to %s is ready: tap Send to send it.", "Hung up.",
    "set an alarm", "delete an alarm", "start a timer", "stop a timer", "switch Wi-Fi or Bluetooth",
    "open an app", "get a call ready", "get a message ready", "undo the last thing", "answer a call",
    "decline a call", "change the call",
  },
  [DE] = {
    "Ich erreiche den Server nicht und arbeite offline, nur mit dem Nötigsten.",
    "Offline kann ich die Uhrzeit und den Akku sagen, Wecker und Timer stellen, WLAN und Bluetooth schalten, Apps "
    "öffnen, anrufen, Nachrichten schreiben und vorlesen.",
    "Das hat nicht geklappt.", "Es ist %s Uhr.", "Der Akku hat %d Prozent%s.", " und lädt",
    "Wecker auf %s gestellt. Sag „rückgängig“, um ihn zu löschen.",
    "Der Wecker um %s ist gelöscht. Sag „rückgängig“, um ihn zurückzuholen.",
    "Deine Wecker: %s.", "Du hast keine Wecker.", "aus", "Laufende Timer: %s.", "Es läuft kein Timer.",
    "Timer auf %s gestellt. Sag „rückgängig“, um ihn zu stoppen.",
    "Der Timer ist gestoppt. Sag „rückgängig“, um ihn wieder zu starten.", "%d Minuten",
    "%s ist an. Sag „rückgängig“, um es zurückzustellen.", "%s ist aus. Sag „rückgängig“, um es zurückzustellen.",
    "WLAN", "Ich öffne %s.", "Der Anruf an %s ist bereit: Tippe auf Anrufen.",
    "Die Nachricht an %s ist bereit: Tippe auf Senden.",
    "Was soll in der Nachricht stehen? Sag zum Beispiel: Schreib Anna, dass ich unterwegs bin.",
    "Ungelesene Nachrichten: %d.", "Keine neuen Nachrichten.", "Die neueste, von %s: %s", "Rückgängig gemacht.",
    "Es gibt nichts rückgängig zu machen.", "Zuletzt habe ich %s.", "Ich habe noch nichts getan.", "Angenommen.",
    "Abgelehnt.", "Abgelehnt. Die Nachricht an %s ist bereit: Tippe auf Senden.", "Aufgelegt.",
    "einen Wecker gestellt", "einen Wecker gelöscht", "einen Timer gestartet", "einen Timer gestoppt",
    "WLAN oder Bluetooth geschaltet", "eine App geöffnet", "einen Anruf vorbereitet", "eine Nachricht vorbereitet",
    "das Letzte rückgängig gemacht", "einen Anruf angenommen", "einen Anruf abgelehnt", "den Anruf geändert",
  },
  [ES] = {
    "No puedo conectar con el servidor, así que funciono sin conexión, solo con lo básico.",
    "Sin conexión puedo decirte la hora y la batería, poner alarmas y temporizadores, activar el Wi-Fi y el "
    "Bluetooth, abrir apps, llamar, escribir y leer tus mensajes.",
    "No ha funcionado.", "Son las %s.", "La batería está al %d por ciento%s.", " y cargando",
    "Alarma puesta a las %s. Di «deshacer» para quitarla.",
    "He quitado la alarma de las %s. Di «deshacer» para recuperarla.",
    "Tus alarmas: %s.", "No tienes alarmas.", "desactivada", "Temporizadores en marcha: %s.",
    "No hay ningún temporizador en marcha.", "Temporizador de %s. Di «deshacer» para pararlo.",
    "He parado el temporizador. Di «deshacer» para reanudarlo.", "%d minutos",
    "%s activado. Di «deshacer» para volver atrás.", "%s desactivado. Di «deshacer» para volver atrás.", "Wi-Fi",
    "Abriendo %s.", "La llamada a %s está lista: toca Llamar.", "El mensaje para %s está listo: toca Enviar.",
    "¿Qué debe decir el mensaje? Di, por ejemplo: escribe a Anna que voy de camino.",
    "Mensajes sin leer: %d.", "No hay mensajes nuevos.",
    "El más reciente, de %s: %s", "Deshecho.", "No hay nada que deshacer.",
    "Lo último que hice fue %s.", "Todavía no he hecho nada.", "Contestada.", "Rechazada.",
    "Rechazada. El mensaje para %s está listo: toca Enviar.", "Colgado.",
    "poner una alarma", "quitar una alarma", "poner un temporizador", "parar un temporizador",
    "cambiar el Wi-Fi o el Bluetooth", "abrir una app", "preparar una llamada", "preparar un mensaje",
    "deshacer lo último", "contestar una llamada", "rechazar una llamada", "cambiar la llamada",
  },
  [FR] = {
    "Je n'arrive pas à joindre le serveur, je fonctionne hors ligne avec l'essentiel.",
    "Hors ligne, je peux donner l'heure et la batterie, régler des réveils et des minuteurs, activer le Wi-Fi et le "
    "Bluetooth, ouvrir des applis, appeler, écrire et lire vos messages.",
    "Ça n'a pas marché.", "Il est %s.", "La batterie est à %d pour cent%s.", ", en charge",
    "Réveil réglé à %s. Dites « annuler » pour le supprimer.",
    "Le réveil de %s est supprimé. Dites « annuler » pour le remettre.",
    "Vos réveils : %s.", "Vous n'avez aucun réveil.", "désactivé", "Minuteurs en cours : %s.",
    "Aucun minuteur en cours.", "Minuteur de %s lancé. Dites « annuler » pour l'arrêter.",
    "Le minuteur est arrêté. Dites « annuler » pour le relancer.", "%d minutes",
    "%s est activé. Dites « annuler » pour revenir en arrière.",
    "%s est désactivé. Dites « annuler » pour revenir en arrière.", "Le Wi-Fi", "J'ouvre %s.",
    "L'appel à %s est prêt : touchez Appeler.", "Le message pour %s est prêt : touchez Envoyer.",
    "Que doit dire le message ? Dites par exemple : écris à Anna que j'arrive.",
    "Messages non lus : %d.", "Aucun nouveau message.",
    "Le plus récent, de %s : %s", "C'est annulé.", "Il n'y a rien à annuler.",
    "La dernière chose que j'ai faite : %s.", "Je n'ai encore rien fait.", "Appel pris.", "Appel refusé.",
    "Appel refusé. Le message pour %s est prêt : touchez Envoyer.", "Raccroché.",
    "régler un réveil", "supprimer un réveil", "lancer un minuteur", "arrêter un minuteur",
    "changer le Wi-Fi ou le Bluetooth", "ouvrir une appli", "préparer un appel", "préparer un message",
    "annuler la dernière action", "prendre un appel", "refuser un appel", "changer l'appel",
  },
  [IT] = {
    "Non riesco a raggiungere il server, quindi lavoro offline con le funzioni di base.",
    "Offline posso dirti l'ora e la batteria, impostare sveglie e timer, gestire Wi-Fi e Bluetooth, aprire app, "
    "chiamare, scrivere e leggere i messaggi.",
    "Non ha funzionato.", "Sono le %s.", "La batteria è al %d per cento%s.", ", in carica",
    "Sveglia impostata alle %s. Di' «annulla» per toglierla.",
    "Ho tolto la sveglia delle %s. Di' «annulla» per rimetterla.",
    "Le tue sveglie: %s.", "Non hai sveglie.", "disattivata", "Timer in corso: %s.", "Nessun timer in corso.",
    "Timer di %s avviato. Di' «annulla» per fermarlo.", "Ho fermato il timer. Di' «annulla» per farlo ripartire.",
    "%d minuti", "%s è attivo. Di' «annulla» per tornare indietro.",
    "%s è spento. Di' «annulla» per tornare indietro.", "Il Wi-Fi", "Apro %s.",
    "La chiamata a %s è pronta: tocca Chiama.", "Il messaggio per %s è pronto: tocca Invia.",
    "Cosa deve dire il messaggio? Di', per esempio: scrivi ad Anna che arrivo.",
    "Messaggi non letti: %d.", "Nessun messaggio nuovo.",
    "Il più recente, da %s: %s", "Annullato.", "Non c'è niente da annullare.",
    "L'ultima cosa che ho fatto: %s.", "Non ho ancora fatto niente.", "Risposto.", "Rifiutata.",
    "Rifiutata. Il messaggio per %s è pronto: tocca Invia.", "Ho riattaccato.",
    "impostare una sveglia", "togliere una sveglia", "avviare un timer", "fermare un timer",
    "cambiare il Wi-Fi o il Bluetooth", "aprire un'app", "preparare una chiamata", "preparare un messaggio",
    "annullare l'ultima azione", "rispondere a una chiamata", "rifiutare una chiamata", "cambiare la chiamata",
  },
  [RU] = {
    "Сервер недоступен, поэтому я работаю офлайн и умею только самое нужное.",
    "Офлайн я могу сказать время и заряд, ставить будильники и таймеры, включать Wi-Fi и Bluetooth, открывать "
    "приложения, звонить, писать и читать сообщения.",
    "Не получилось.", "Сейчас %s.", "Заряд батареи %d процентов%s.", ", идёт зарядка",
    "Будильник на %s. Скажите «отмени», чтобы убрать его.",
    "Будильник на %s удалён. Скажите «отмени», чтобы вернуть его.",
    "Ваши будильники: %s.", "Будильников нет.", "выключен", "Идут таймеры: %s.", "Таймеров нет.",
    "Таймер на %s. Скажите «отмени», чтобы остановить его.",
    "Таймер остановлен. Скажите «отмени», чтобы запустить его снова.", "%d мин",
    "%s включён. Скажите «отмени», чтобы вернуть как было.",
    "%s выключен. Скажите «отмени», чтобы вернуть как было.", "Wi-Fi", "Открываю %s.",
    "Звонок готов, %s: нажмите «Позвонить».", "Сообщение готово, %s: нажмите «Отправить».",
    "Что написать? Скажите, например: напиши Анне, что я еду.",
    "Непрочитанных сообщений: %d.", "Новых сообщений нет.", "Последнее, от %s: %s", "Отменено.", "Отменять нечего.",
    "Последнее, что я сделал: %s.", "Я пока ничего не делал.", "Ответил.", "Отклонил.",
    "Отклонил. Сообщение готово, %s: нажмите «Отправить».", "Положил трубку.",
    "поставил будильник", "удалил будильник", "запустил таймер", "остановил таймер",
    "переключил Wi-Fi или Bluetooth", "открыл приложение", "подготовил звонок", "подготовил сообщение",
    "отменил последнее действие", "ответил на звонок", "отклонил звонок", "изменил звонок",
  },
};

const char *
dictate_offline_notice (const char *language)
{
  return REPLIES[lang_of (language)].notice;
}

static JsonObject *
parse_result (const char *json, JsonNode **keep)
{
  *keep = json ? json_from_string (json, NULL) : NULL;
  return *keep && JSON_NODE_HOLDS_OBJECT (*keep) ? json_node_get_object (*keep) : NULL;
}

static const char *
member (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) && JSON_NODE_HOLDS_VALUE (json_object_get_member (o, name))
         ? json_object_get_string_member (o, name) : NULL;
}

/* what the log's tool did, in the user's language */
static const char *
did_of (const Replies *r, const char *tool)
{
  static const struct { const char *tool; gsize offset; } DID[] = {
    { "set_alarm", G_STRUCT_OFFSET (Replies, did_alarm) },
    { "cancel_alarm", G_STRUCT_OFFSET (Replies, did_alarm_gone) },
    { "set_timer", G_STRUCT_OFFSET (Replies, did_timer) },
    { "cancel_timer", G_STRUCT_OFFSET (Replies, did_timer_gone) },
    { "set_wifi_bluetooth", G_STRUCT_OFFSET (Replies, did_radio) },
    { "open_app", G_STRUCT_OFFSET (Replies, did_app) },
    { "propose_call", G_STRUCT_OFFSET (Replies, did_call) },
    { "propose_message", G_STRUCT_OFFSET (Replies, did_message) },
    { "undo_last_action", G_STRUCT_OFFSET (Replies, did_undo) },
    { "answer_call", G_STRUCT_OFFSET (Replies, did_answer) },
    { "decline_call", G_STRUCT_OFFSET (Replies, did_decline) },
    { "call_control", G_STRUCT_OFFSET (Replies, did_call_control) },
  };

  for (guint i = 0; i < G_N_ELEMENTS (DID); i++)
    if (!g_strcmp0 (tool, DID[i].tool))
      return G_STRUCT_MEMBER (const char *, r, DID[i].offset);
  return NULL;
}

/* "read my messages": how many are unread, and the newest one (in private mode on the speaker, only how many) */
static char *
read_messages (const Replies *r, gboolean private_speaker, DictateOfflineRun run, gpointer data)
{
  g_autofree char *list = run ("get_messages", "{\"unread_only\": true, \"count\": 3}", data);
  g_autoptr (JsonNode) keep = NULL;
  JsonObject *o = parse_result (list, &keep);
  JsonArray *conversations = o && json_object_has_member (o, "conversations")
                             ? json_object_get_array_member (o, "conversations") : NULL;
  GString *out;
  guint n = conversations ? json_array_get_length (conversations) : 0;

  if (!o || member (o, "error"))
    return g_strdup (r->failed);
  if (!n)
    return g_strdup (r->no_messages);
  out = g_string_new (NULL);
  {
    gint64 unread = 0;
    for (guint i = 0; i < n; i++)
      unread += json_object_get_int_member_with_default (json_array_get_object_element (conversations, i), "unread",
                                                         1);
    g_string_append_printf (out, r->messages, (int) MAX (unread, n));
  }
  for (guint i = 0; i < n && i < 1 && !private_speaker; i++)
    {
      JsonObject *c = json_array_get_object_element (conversations, i);
      const char *with = member (c, "with");
      g_autoptr (JsonBuilder) b = json_builder_new ();
      g_autofree char *args = NULL, *got = NULL;
      g_autoptr (JsonNode) keep2 = NULL;
      JsonObject *m;
      JsonArray *messages;
      const char *text = NULL;

      if (!with)
        continue;
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "contact");
      json_builder_add_string_value (b, with);
      json_builder_set_member_name (b, "count");
      json_builder_add_int_value (b, 1);
      json_builder_set_member_name (b, "unread_only");
      json_builder_add_boolean_value (b, TRUE);
      json_builder_set_member_name (b, "include_text");
      json_builder_add_boolean_value (b, TRUE);
      json_builder_end_object (b);
      args = args_of (b);
      got = run ("get_messages", args, data);
      m = parse_result (got, &keep2);
      messages = m && json_object_has_member (m, "messages") ? json_object_get_array_member (m, "messages") : NULL;
      if (messages && json_array_get_length (messages))
        text = member (json_array_get_object_element (messages, json_array_get_length (messages) - 1), "text");
      if (text)
        {
          g_string_append_c (out, ' ');
          g_string_append_printf (out, r->from, with, text);
          if (!g_str_has_suffix (out->str, ".") && !g_str_has_suffix (out->str, "!")
              && !g_str_has_suffix (out->str, "?"))
            g_string_append_c (out, '.');
        }
    }
  return g_string_free (out, FALSE);
}

char *
dictate_offline_reply (const char *language, const char *said, gboolean private_speaker, DictateOfflineRun run,
                       gpointer data)
{
  const Replies *r = &REPLIES[lang_of (language)];
  g_autofree char *arguments = NULL;
  g_autofree char *tool = dictate_offline_parse (language, said, &arguments);
  g_autofree char *result = NULL;
  g_autoptr (JsonNode) keep = NULL;
  JsonObject *o;
  const char *s;

  if (!tool)
    return g_strdup (r->unknown);
  if (!strcmp (tool, "read_messages"))
    return read_messages (r, private_speaker, run, data);
  if (!strcmp (tool, "propose_message") && !strstr (arguments, "\"text\""))
    return g_strdup (r->message_text);
  result = run (tool, arguments, data);
  o = parse_result (result, &keep);
  if (!o || member (o, "error"))
    {
      if (!strcmp (tool, "undo_last_action"))
        return g_strdup (r->no_undo);
      return g_strdup (r->failed);
    }
  if (!strcmp (tool, "get_time"))
    return g_strdup_printf (r->time, member (o, "time") ? member (o, "time") : "?");
  if (!strcmp (tool, "get_battery"))
    return g_strdup_printf (r->battery, (int) json_object_get_int_member_with_default (o, "percent", 0),
                            json_object_get_boolean_member_with_default (o, "charging", FALSE) ? r->charging : "");
  if (!strcmp (tool, "set_alarm"))
    return g_strdup_printf (r->alarm_set, (s = member (o, "alarm_set")) ? s : "?");
  if (!strcmp (tool, "cancel_alarm"))
    return g_strdup_printf (r->alarm_gone, (s = member (o, "alarm_removed")) ? s : "?");
  if (!strcmp (tool, "set_timer"))
    {
      g_autoptr (JsonNode) args = json_from_string (arguments, NULL);
      double minutes = json_object_get_double_member_with_default (json_node_get_object (args), "minutes", 0);
      g_autofree char *length = g_strdup_printf (r->minutes, MAX (1, (int) minutes));
      return g_strdup_printf (r->timer_set, length);
    }
  if (!strcmp (tool, "cancel_timer"))
    return g_strdup (r->timer_gone);
  if (!strcmp (tool, "get_alarms"))
    {
      JsonArray *alarms = json_object_has_member (o, "alarms") ? json_object_get_array_member (o, "alarms") : NULL;
      JsonArray *running = json_object_has_member (o, "running_timers")
                           ? json_object_get_array_member (o, "running_timers") : NULL;
      g_autoptr (Said) sd = said_new (lang_of (language), said);
      GString *list = g_string_new (NULL);
      char *reply;

      if (has (sd, WORDS[sd->lang].timer) || (sd->lang == RU && find_stem (sd, "таймер") >= 0))
        {
          for (guint i = 0; running && i < json_array_get_length (running); i++)
            g_string_append_printf (list, "%s%s", list->len ? ", " : "",
                                    member (json_array_get_object_element (running, i), "left"));
          reply = list->len ? g_strdup_printf (r->timers, list->str) : g_strdup (r->no_timers);
        }
      else
        {
          for (guint i = 0; alarms && i < json_array_get_length (alarms); i++)
            {
              JsonObject *a = json_array_get_object_element (alarms, i);
              g_string_append_printf (list, "%s%s%s%s%s", list->len ? ", " : "", member (a, "time"),
                                      json_object_get_boolean_member_with_default (a, "on", TRUE) ? "" : " (",
                                      json_object_get_boolean_member_with_default (a, "on", TRUE) ? "" : r->off,
                                      json_object_get_boolean_member_with_default (a, "on", TRUE) ? "" : ")");
            }
          reply = list->len ? g_strdup_printf (r->alarms, list->str) : g_strdup (r->no_alarms);
        }
      g_string_free (list, TRUE);
      return reply;
    }
  if (!strcmp (tool, "set_wifi_bluetooth"))
    {
      g_autoptr (JsonNode) args = json_from_string (arguments, NULL);
      JsonObject *a = json_node_get_object (args);
      const char *radio = json_object_get_string_member (a, "radio");
      const char *name = !strcmp (radio, "wifi") ? r->wifi : "Bluetooth";
      return g_strdup_printf (json_object_get_boolean_member (a, "on") ? r->radio_on : r->radio_off, name);
    }
  if (!strcmp (tool, "open_app"))
    return g_strdup_printf (r->app, (s = member (o, "opened")) ? s : "");
  if (!strcmp (tool, "propose_call") || !strcmp (tool, "propose_message"))
    {
      g_autoptr (JsonNode) args = json_from_string (arguments, NULL);
      const char *to = json_object_get_string_member (json_node_get_object (args), "to");
      return g_strdup_printf (!strcmp (tool, "propose_call") ? r->call_card : r->message_card,
                              private_speaker ? "…" : to);
    }
  if (!strcmp (tool, "undo_last_action"))
    return g_strdup (r->undone);
  if (!strcmp (tool, "get_action_log"))
    {
      JsonArray *actions = json_object_has_member (o, "actions") ? json_object_get_array_member (o, "actions") : NULL;
      JsonObject *last = actions && json_array_get_length (actions) ? json_array_get_object_element (actions, 0)
                                                                    : NULL;
      const char *did = last ? did_of (r, member (last, "tool")) : NULL;

      if (!last)
        return g_strdup (r->did_nothing);
      return g_strdup_printf (r->did, did ? did : member (last, "what"));
    }
  if (!strcmp (tool, "answer_call"))
    return g_strdup (r->answered);
  if (!strcmp (tool, "decline_call"))
    {
      if (member (o, "message_to"))
        return g_strdup_printf (r->declined_text, private_speaker ? "…" : member (o, "message_to"));
      return g_strdup (r->declined);
    }
  if (!strcmp (tool, "call_control"))
    return g_strdup (r->hung_up);
  return g_strdup (r->failed);
}
