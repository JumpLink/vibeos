/* navi-guidance: see navi-guidance.h. */
#include "navi-guidance.h"

#include <math.h>
#include <string.h>

#define OFF_ROUTE_MIN_M      40.0  /* off by more than this (or the fix's accuracy) is off the route */
#define OFF_ROUTE_FIXES      3     /* for this many fixes in a row */
#define OFF_ROUTE_US         3000000
#define WRONG_WAY_DEG        120.0 /* driving against the route */
#define ARRIVE_M             25.0
#define BACK_SLACK_M         30.0  /* a fix this far behind the last place is noise, not reversing */

typedef struct {
  gboolean alert, pre, post;
} Spoken;

struct _NaviGuidance {
  NaviRoute *route;
  char *language;
  NaviProgress p;
  gboolean located;       /* a fix has been matched */
  guint off_fixes;
  gint64 off_since;
  guint wrong_fixes;
  Spoken *spoken;         /* per manoeuvre */
  gboolean started;       /* the first manoeuvre's words were said */
  double next_when_seen;  /* how far the next manoeuvre was when it became the next */
  guint last_next;
};

NaviGuidance *
navi_guidance_new (NaviRoute *route)
{
  NaviGuidance *self = g_new0 (NaviGuidance, 1);
  self->route = navi_route_ref (route);
  self->language = g_strdup (navi_route_language ());
  self->spoken = g_new0 (Spoken, MAX (route->n_maneuvers, 1u));
  self->p.next = MIN (1u, route->n_maneuvers ? route->n_maneuvers - 1 : 0);
  self->p.remaining = route->length;
  self->p.time_left = route->time;
  self->p.to_next = route->n_maneuvers > 1 ? route->maneuvers[1].s : route->length;
  self->p.on_route = TRUE;
  self->last_next = G_MAXUINT;
  return self;
}

void
navi_guidance_free (NaviGuidance *self)
{
  if (!self)
    return;
  navi_route_unref (self->route);
  g_free (self->language);
  g_free (self->spoken);
  g_free (self);
}

NaviRoute *
navi_guidance_get_route (NaviGuidance *self)
{
  return self->route;
}

const NaviProgress *
navi_guidance_get_progress (NaviGuidance *self)
{
  return &self->p;
}

const NaviProgress *
navi_guidance_update (NaviGuidance *self, double lat, double lng, double accuracy, double speed, double heading,
                      gint64 time_us)
{
  NaviRoute *r = self->route;
  NaviProgress *p = &self->p;
  double s, bearing, offset;
  if (!self->located) {
    /* the first fix: anywhere on the route, the nearest place */
    offset = navi_path_project (r->points, r->n_points, lat, lng, 0, G_MAXDOUBLE, &s, &bearing);
  } else {
    /* from a little behind the last place to as far as the car can have come since */
    double ahead = MAX (400.0, (speed > 0 ? speed : 30) * 20);
    offset = navi_path_project (r->points, r->n_points, lat, lng, p->s - 60, p->s + ahead, &s, &bearing);
    if (offset > OFF_ROUTE_MIN_M) {
      /* not there: further on (a loop, a tunnel's lost fixes), but only where it is plainly on the route */
      double s2, b2;
      double o2 = navi_path_project (r->points, r->n_points, lat, lng, p->s, G_MAXDOUBLE, &s2, &b2);
      gboolean same_way = heading < 0 || speed < 3 || fabs (navi_angle_diff (heading, b2)) < 60;
      if (o2 < OFF_ROUTE_MIN_M / 2 && same_way) {
        s = s2;
        bearing = b2;
        offset = o2;
      }
    }
    /* never backwards for a fix's noise */
    if (s < p->s && s > p->s - BACK_SLACK_M)
      s = p->s;
  }
  self->located = TRUE;
  p->s = s;
  p->offset = offset;
  p->bearing = bearing;

  double limit = MAX (OFF_ROUTE_MIN_M, MIN (accuracy, 150.0) + 15);
  p->on_route = offset <= limit;
  if (!p->on_route) {
    if (!self->off_fixes++)
      self->off_since = time_us;
  } else {
    self->off_fixes = 0;
  }
  gboolean wrong = heading >= 0 && speed > 4 && fabs (navi_angle_diff (heading, bearing)) > WRONG_WAY_DEG;
  self->wrong_fixes = wrong ? self->wrong_fixes + 1 : 0;
  p->off_route = (self->off_fixes >= OFF_ROUTE_FIXES && time_us - self->off_since >= OFF_ROUTE_US) ||
                 self->wrong_fixes >= OFF_ROUTE_FIXES + 1;

  p->remaining = MAX (0.0, r->length - s);
  p->time_left = navi_route_time_left (r, s);
  p->current = r->n_maneuvers ? navi_route_maneuver_at (r, s) : 0;
  p->next = r->n_maneuvers ? MIN (p->current + 1, r->n_maneuvers - 1) : 0;
  p->to_next = r->n_maneuvers ? MAX (0.0, r->maneuvers[p->next].s - s) : p->remaining;
  if (p->next != self->last_next) {
    self->last_next = p->next;
    self->next_when_seen = p->to_next;
  }
  if (p->remaining < ARRIVE_M && p->on_route)
    p->arrived = TRUE;
  return p;
}

/* ---- the voice ---- */

static gboolean
is_destination (NaviTurn t)
{
  return t == NAVI_TURN_DESTINATION || t == NAVI_TURN_DESTINATION_LEFT || t == NAVI_TURN_DESTINATION_RIGHT;
}

/* where the alert comes, by speed: town, main roads, motorway */
static double
alert_distance (double speed)
{
  if (speed < 0)
    speed = 10;
  return speed < 15 ? 400 : speed < 25 ? 800 : 1500;
}

double
navi_guidance_pre_distance (double speed)
{
  if (speed < 0)
    speed = 10;
  return CLAMP (speed * 6, 50.0, 300.0);
}


/* Navi's own words for each language it speaks, in navi_route_language's order of codes; English first */
static const struct {
  const char *code;
  const char *words[NAVI_SAY_DANGER_ZONE + 1];
} SAYINGS[] = {
  { "en", { "You have arrived at your destination.", "You have reached your stop.", "Speed camera ahead.",
            "Red light camera ahead.", "Section control ahead.", "Danger zone ahead." } },
  { "de", { "Du hast dein Ziel erreicht.", "Du hast deinen Zwischenstopp erreicht.", "Blitzer voraus.",
            "Rotlichtblitzer voraus.", "Abschnittskontrolle voraus.", "Gefahrenzone voraus." } },
  { "fr", { "Vous êtes arrivé à destination.", "Vous avez atteint votre étape.", "Radar en approche.",
            "Radar de feu rouge en approche.", "Radar tronçon en approche.", "Zone de danger en approche." } },
  { "it", { "Sei arrivato a destinazione.", "Hai raggiunto la tua tappa.", "Autovelox più avanti.",
            "Telecamera semaforica più avanti.", "Tutor più avanti.", "Zona di pericolo più avanti." } },
  { "es", { "Has llegado a tu destino.", "Has llegado a tu parada.", "Radar de velocidad más adelante.",
            "Cámara de semáforo más adelante.", "Control de tramo más adelante.", "Zona de peligro más adelante." } },
  { "ru", { "Вы прибыли в пункт назначения.", "Вы прибыли в промежуточную точку.",
            "Впереди камера контроля скорости.", "Впереди камера на светофоре.",
            "Впереди контроль средней скорости.", "Впереди опасный участок." } },
};

static guint
saying_language (const char *language)
{
  for (guint i = 0; language && i < G_N_ELEMENTS (SAYINGS); i++)
    if (!g_ascii_strncasecmp (language, SAYINGS[i].code, 2))
      return i;
  return 0;
}

const char *
navi_guidance_saying (NaviSaying what, const char *language)
{
  g_return_val_if_fail (what <= NAVI_SAY_DANGER_ZONE, NULL);
  return SAYINGS[saying_language (language)].words[what];
}

/* Russian's count words: 1 километр, 2 километра, 5 километров; a fraction takes the second (1,5 километра) */
static const char *
ru_count (double n, const char *one, const char *few, const char *many)
{
  if (n != floor (n))
    return few;
  long k = lround (n) % 100;
  if (k % 10 == 1 && k != 11)
    return one;
  if (k % 10 >= 2 && k % 10 <= 4 && (k < 12 || k > 14))
    return few;
  return many;
}

/* a number as `code` writes it: 1.5 or 1,5 */
static char *
number (const char *code, double n, const char *format)
{
  char *s = g_strdup_printf (format, n);
  if (strcmp (code, "en"))
    g_strdelimit (s, ".", ',');
  return s;
}

/* the instruction after the distance, its first letter lower case: "In 400 meters, turn left ..." */
static char *
lower_first (const char *text)
{
  if (!text || !*text || !g_utf8_validate (text, -1, NULL))
    return g_strdup (text ? text : "");
  gunichar c = g_utf8_get_char (text);
  if (!g_unichar_isupper (c))
    return g_strdup (text);
  char first[8];
  first[g_unichar_to_utf8 (g_unichar_tolower (c), first)] = '\0';
  return g_strconcat (first, g_utf8_next_char (text), NULL);
}

char *
navi_guidance_alert_text (const char *language, double metres, const char *instruction)
{
  const char *code = SAYINGS[saying_language (language)].code;
  gboolean de = !strcmp (code, "de"), fr = !strcmp (code, "fr"), it = !strcmp (code, "it"),
           es = !strcmp (code, "es"), ru = !strcmp (code, "ru");
  /* spoken as people say it: 50 m steps below a kilometre, half kilometres above; in miles, 100 ft steps below a
   * quarter mile, quarters up to a mile, halves above */
  g_autofree char *amount = NULL;
  if (navi_format_uses_miles ()) {
    double mi = metres / 1609.344;
    if (mi < 0.2) {
      long ft = MAX (100L, lround (metres * 3.2808399 / 100) * 100);
      amount = g_strdup_printf ("%ld %s", ft, de ? "Fuß" : fr ? "pieds" : it ? "piedi" : es ? "pies"
                                               : ru ? ru_count (ft, "фут", "фута", "футов") : "feet");
    } else {
      double q = mi < 0.875 ? round (mi * 4) / 4 : round (mi * 2) / 2;
      g_autofree char *n = number (code, q, q == floor (q) ? "%.0f" : "%.2g");
      if (de)
        amount = q == 1 ? g_strdup ("einer Meile") : g_strdup_printf ("%s Meilen", n);
      else if (fr)
        amount = g_strdup_printf (q < 2 ? "%s mile" : "%s miles", n);
      else if (it)
        amount = g_strdup_printf (q == 1 ? "%s miglio" : "%s miglia", n);
      else if (es)
        amount = g_strdup_printf (q == 1 ? "%s milla" : "%s millas", n);
      else if (ru)
        amount = g_strdup_printf ("%s %s", n, ru_count (q, "милю", "мили", "миль"));
      else if (q == 0.25)
        amount = g_strdup ("a quarter mile");
      else if (q == 0.5)
        amount = g_strdup ("half a mile");
      else if (q == 0.75)
        amount = g_strdup ("three quarters of a mile");
      else if (q == 1)
        amount = g_strdup ("1 mile");
      else
        amount = g_strdup_printf (q == floor (q) ? "%.0f miles" : "%.1f miles", q);
    }
  } else if (metres < 950) {
    long m = MAX (50L, lround (metres / 50) * 50);
    amount = g_strdup_printf ("%ld %s", m, de ? "Metern" : fr ? "mètres" : it ? "metri" : es ? "metros"
                                          : ru ? ru_count (m, "метр", "метра", "метров") : "meters");
  } else {
    double km = round (metres / 500) * 0.5;
    g_autofree char *n = number (code, km, km == floor (km) ? "%.0f" : "%.1f");
    if (de)
      amount = km == 1 ? g_strdup ("einem Kilometer") : g_strdup_printf ("%s Kilometern", n);
    else if (fr)
      amount = g_strdup_printf (km < 2 ? "%s kilomètre" : "%s kilomètres", n);
    else if (it)
      amount = g_strdup_printf (km == 1 ? "%s chilometro" : "%s chilometri", n);
    else if (es)
      amount = g_strdup_printf (km == 1 ? "%s kilómetro" : "%s kilómetros", n);
    else if (ru)
      amount = g_strdup_printf ("%s %s", n, ru_count (km, "километр", "километра", "километров"));
    else
      amount = g_strdup_printf (km == 1 ? "%s kilometer" : "%s kilometers", n);
  }
  if (de)
    return g_strdup_printf ("In %s: %s", amount, instruction);
  /* "Turn left onto X." follows "In 400 meters, " in lower case */
  g_autofree char *rest = lower_first (instruction);
  if (ru)
    return g_strdup_printf ("Через %s %s", amount, rest);
  return g_strdup_printf ("%s %s, %s", fr ? "Dans" : it ? "Tra" : es ? "En" : "In", amount, rest);
}

static void
add_unique (GPtrArray *out, char *text)
{
  if (!text || !*text || g_ptr_array_find_with_equal_func (out, text, g_str_equal, NULL))
    g_free (text);
  else
    g_ptr_array_add (out, text);
}

char **
navi_guidance_upcoming_prompts (NaviGuidance *self, double speed)
{
  NaviRoute *r = self->route;
  GPtrArray *out = g_ptr_array_new ();
  if (r->n_maneuvers) {
    guint first = self->started ? self->p.next : 0;
    double alert = alert_distance (speed);
    /* the speed then may be a little off now's, over the edge of a band ("in 400" or "in 800 meters"): both */
    double speeds[3] = { speed, speed < 0 ? speed : speed * 0.8, speed < 0 ? speed : speed * 1.25 };
    for (guint i = first; i < MIN (first + 2, r->n_maneuvers); i++) {
      NaviManeuver *m = &r->maneuvers[i];
      Spoken *sp = &self->spoken[i];
      if (!sp->pre || (i == 0 && !self->started))
        add_unique (out, g_strdup (m->verbal_pre ? m->verbal_pre : m->instruction));
      /* the alert comes where the speed's alert distance is, on a stretch long enough for one */
      double stretch = i > 0 ? m->s - r->maneuvers[i - 1].s : 0;
      const char *words = m->verbal_alert ? m->verbal_alert : m->instruction;
      for (guint k = 0; k < G_N_ELEMENTS (speeds); k++) {
        double band = alert_distance (speeds[k]);
        /* a turn that became the next one closer than the band is alerted at once, where the next fix is: here less
         * a second's driving */
        double at[2] = { band, band };
        if (i == self->p.next && self->next_when_seen < band) {
          at[0] = self->p.to_next;
          at[1] = self->p.to_next - MAX (speeds[k], 0.0);
        }
        for (guint j = 0; j < 2; j++)
          if (i > 0 && !sp->alert && words && stretch > at[j] &&
              at[j] > navi_guidance_pre_distance (speeds[k]) + 100)
            add_unique (out, is_destination (m->type) ? g_strdup (words)
                                                      : navi_guidance_alert_text (self->language, at[j], words));
      }
      if (m->verbal_post && i + 1 < r->n_maneuvers && r->maneuvers[i + 1].s - m->s > MAX (2000.0, alert + 500))
        add_unique (out, g_strdup (m->verbal_post));
    }
  }
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

char *
navi_guidance_take_prompt (NaviGuidance *self, double speed)
{
  NaviRoute *r = self->route;
  NaviProgress *p = &self->p;
  if (!self->located || !r->n_maneuvers || !p->on_route)
    return NULL;
  if (!self->started) {
    /* "Drive south. Then turn left onto ..." */
    self->started = TRUE;
    self->spoken[0].pre = TRUE;
    if (r->maneuvers[0].verbal_pre)
      return g_strdup (r->maneuvers[0].verbal_pre);
  }
  if (p->arrived) {
    guint last = r->n_maneuvers - 1;
    if (!self->spoken[last].pre) {
      self->spoken[last].pre = self->spoken[last].alert = TRUE;
      return g_strdup (r->maneuvers[last].verbal_pre ? r->maneuvers[last].verbal_pre : r->maneuvers[last].instruction);
    }
    return NULL;
  }
  guint i = p->next;
  NaviManeuver *m = &r->maneuvers[i];
  Spoken *sp = &self->spoken[i];
  double pre = navi_guidance_pre_distance (speed), alert = alert_distance (speed);
  /* a turn followed at once by another is announced with it ("Then ..."): its own alert would only crowd it */
  if (!sp->pre && p->to_next <= pre) {
    sp->pre = sp->alert = TRUE;
    if (i > 0)
      self->spoken[i - 1].post = TRUE;
    const char *words = m->verbal_pre ? m->verbal_pre : m->instruction;
    return words ? g_strdup (words) : NULL;
  }
  if (!sp->alert && p->to_next <= alert && p->to_next > pre + 100 && self->next_when_seen > p->to_next + 1) {
    sp->alert = TRUE;
    const char *words = m->verbal_alert ? m->verbal_alert : m->instruction;
    if (!words)
      return NULL;
    if (is_destination (m->type))
      return g_strdup (words);
    return navi_guidance_alert_text (self->language, p->to_next, words);
  }
  /* just after a manoeuvre, on a long stretch: how far to keep going */
  if (i > 0 && !self->spoken[i - 1].post) {
    NaviManeuver *done = &r->maneuvers[i - 1];
    self->spoken[i - 1].post = TRUE;
    if (p->current == i - 1 && p->s - done->s < 150 && p->to_next > MAX (2000.0, alert + 500) && done->verbal_post)
      return g_strdup (done->verbal_post);
  }
  return NULL;
}
