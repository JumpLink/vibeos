/* The answer screen: see answer-view.h. No still shows this screen (no call was placed on the reference phone);
 * every number is from the source, under incallui/answer/impl/: AnswerFragment.java, res/layout/
 * fragment_incoming_call.xml, answermethod/FlingUpDownMethod.java and FlingUpDownTouchHandler.java,
 * answermethod/res/layout/swipe_up_down_method.xml, affordance/SwipeButtonHelper.java and SwipeButtonView.java.
 *
 * The view draws everything itself, as one surface: the puck's animations move its parts independently of any
 * layout (the hint text, the disc, the icon on it, the text under it), and the swipe fades and shrinks the
 * contact grid above them.
 *
 * The animations are FlingUpDownMethod's AnimatorSets, played by a small tween engine below: each tween is one
 * ObjectAnimator (a value, from → to, a start delay, a duration and a curve). An animator given only an end value
 * starts from wherever its value is when it starts, as Android's do. Its states are the method's: ENTRY (the puck
 * flies in), BOUNCE (it hops every 3 s, while the call rings), SWIPE (it follows the finger), SETTLE (100 ms back
 * to rest), HINT (one high jump with "Swipe down to reject" shown, after a tap) and COMPLETED (answered or declined:
 * the puck is gone). */
#include "answer-view.h"
#include "avatar.h"

#include <glib/gi18n.h>
#include <math.h>
#include <neo.h>

/* the contact grid (fragment_incoming_call.xml, values-h300dp and -h540dp) */
#define GRID_TOP        24
#define GRID_SIDE       24
#define ROW_GAP         8
#define STATUS_SIZE     18      /* Dialer.Incall.TextAppearance */
#define NAME_SIZE       54      /* .Large on the answer screen */
#define NAME_MIN_SIZE   32
#define AVATAR          172     /* only for a contact with a photo */
#define SPACER          220     /* under the column, over the puck */
#define ENTRY_MS        1000    /* answer_animate_entry_millis */

/* the puck (swipe_up_down_method.xml) */
#define METHOD_SIDE     50      /* answer_swipe_dead_zone_sides */
#define HINT_SIZE       14      /* Dialer.Incall.TextAppearance.Hint, italic */
#define SPACE_HOLDER    50
#define ANSWER_GAP      18
#define PUCK_BOX        88      /* answer_contact_puck_size_photo: the container */
#define PUCK_GAP        10
#define PUCK            72      /* answer_contact_puck_size_no_photo: the white disc */
#define PUCK_ICON       30
#define REJECT_GAP      20

/* FlingUpDownTouchHandler, FlingAnimationUtils */
#define DEAD_ZONE_TOP   150     /* max(answer_swipe_dead_zone_top, ACCEPT_THRESHOLD_DP) */
#define THRESHOLD       150     /* ACCEPT_THRESHOLD_DP, REJECT_THRESHOLD_DP */
#define FALSING         40      /* FALSING_THRESHOLD_DP */
#define TOUCH_SLOP      8       /* ViewConfiguration */
#define MIN_FLING       250     /* dp/s; twice that for a fling down */
#define HINT_MOVE       0.1     /* HINT_MOVE_THRESHOLD_RATIO */
#define FLING_RECENTER  0.1     /* PROGRESS_FLING_RECENTER */
#define SWIPE_RECENTER  0.8     /* PROGRESS_SWIPE_RECENTER */

/* the answer-and-release affordance (bottom|end, 56 dp; affordance/res/values/dimens.xml) */
#define SIDE_BOX        56
#define SIDE_ICON       32      /* ic_end_answer_32 */
#define SIDE_TOUCH      120     /* answer_affordance_touch_target_size */
#define SIDE_TRIGGER    110     /* answer_min_swipe_amount */
#define SIDE_MIN_RADIUS 30      /* answer_affordance_min_background_radius */
#define SIDE_HINT_GROW  60      /* hint_grow_amount_sideways */
#define SIDE_ALPHA      0.87    /* SWIPE_RESTING_ALPHA_AMOUNT */
#define SIDE_MIN_FLING  50      /* ViewConfiguration's minimum fling velocity, dp/s */
#define SIDE_HINT_MS    5000    /* SWIPE_HINT_TIMEOUT_MS: the puck's hint says how to use the icon this long */

/* includeFontPadding: Android's line box is the font's bounding box (head yMax/yMin), not its ascent/descent.
 * Roboto: 2163/-555 against 1900/-500 of 2048; Google Sans Flex: 2210/-605 against 1932/-572 of 2000 */
#define ROBOTO_PAD_TOP(size)    ((size) * 263 / 2048.0)
#define ROBOTO_PAD_BOTTOM(size) ((size) * 55 / 2048.0)
#define GSF_PAD_TOP(size)       ((size) * 278 / 2000.0)
#define GSF_PAD_BOTTOM(size)    ((size) * 33 / 2000.0)

/* colours: the InCallUI theme is dark and is drawn raw (no force dark; spec §8.1) */
#define BACKGROUND      0x181c1d        /* incall_background_gradient_*, night */
#define TEXT_SECONDARY  (0xdd / 255.0)  /* textColorSecondary #DDFFFFFF: white at this alpha */
#define CALL_GREEN      0x3ee0a0        /* dialer_call_green, night: the icon until the first touch */
#define ANSWER_ICON     0x00c853        /* incoming_answer_icon */
#define ACCEPT_COLOR    0x00c853        /* call_accept_background */
#define HANGUP_COLOR    0xdf0000        /* call_hangup_background */

/* ---- the tween engine ----------------------------------------------------------------------------------------- */

typedef enum
{
  EASE_LINEAR,
  EASE_FAST_OUT_SLOW_IN,
  EASE_LINEAR_OUT_SLOW_IN,
  EASE_FAST_OUT_LINEAR_IN,
  EASE_ACCELERATE_DECELERATE,   /* an ObjectAnimator's default */
  EASE_DECELERATE,
  EASE_ACCELERATE,
  EASE_PATH_0001,               /* PathInterpolator(0, 0, 0, 1) */
  EASE_PATH_4001,               /* PathInterpolator(0.4, 0, 0, 1) */
  EASE_BOUNCE,
} Ease;

typedef enum
{
  GROUP_METHOD,                 /* the method's AnimatorSets: endAnimation cancels them */
  GROUP_PROPERTY,               /* view.animate(): only a newer animation of the same value stops them */
  GROUP_FRAGMENT,               /* AnswerFragment's own: the contact grid's entry */
} Group;

typedef struct
{
  double *value;
  double from, to;              /* from NAN: the value as it is when the tween starts */
  gint64 start, duration;       /* µs, on the frame clock */
  Ease ease;
  Group group;
  gboolean started;
} Tween;

#define MAX_TWEENS 48

typedef enum
{
  STATE_NONE,
  STATE_ENTRY,
  STATE_BOUNCE,
  STATE_SWIPE,
  STATE_SETTLE,
  STATE_HINT,
  STATE_COMPLETED,
} AnimState;

typedef enum { TARGET_CENTER, TARGET_ACCEPT, TARGET_REJECT } FlingTarget;

struct _AnswerView
{
  GtkWidget parent;

  guint call;
  gboolean waiting;             /* CALL_WAITING: the background does not fade on a decline */
  gboolean other_call, can_hold, held_call;
  PangoLayout *status, *name, *bottom, *answer_text, *reject_text;
  double name_size;
  GdkTexture *photo;
  char *hint;                   /* the puck's hint (setHintText); NULL: "Swipe up to answer" */
  guint hint_restore;           /* the side icon's hint going back to it */

  /* what the animations move (dp, alpha 0…1, degrees) */
  double text_y, text_alpha, reject_y, reject_alpha;
  double puck_x, puck_y, puck_sx, puck_sy, puck_alpha, bg_scale, icon_rotation, icon_alpha;
  GdkRGBA icon_tint;
  double puck_progress;         /* the disc's tint toward accept (+) or decline (−); 0 white */
  gint64 pivot_bottom_until;    /* the hint's squash pivots on the container's foot until then */
  double grid_alpha, grid_scale, entry, entry_alpha;
  double background;            /* the decline's fade of the window, 0…1 */

  /* the state machine */
  AnimState state, after_settle, phase;
  gint64 phase_end;             /* µs: when the running AnimatorSet ends; 0: none runs */
  gint64 vibration_start;       /* the puck's ±1 dp shiver, 0 for none */
  Tween tweens[MAX_TWEENS];
  guint n_tweens;
  guint tick;
  gboolean answer_clicked, reject_clicked;

  /* FlingUpDownTouchHandler */
  gboolean touch_enabled, tracking, slop_exceeded, above_falsing, hint_distance_exceeded;
  double initial_y, zero_y, last_y;
  double accept_line, reject_line;  /* where a swipe from here has gone all the way: up to answer, down to decline */
  double progress;
  SpongyVelocity velocity;
  gboolean flinging;
  double fling_from, fling_to;
  gint64 fling_start, fling_duration;
  Ease fling_ease;

  /* the side icon (SwipeButtonHelper / SwipeButtonView) */
  gboolean side_touch, side_slop, side_finishing, side_done;
  double side_down_x, side_down_y, side_translation, side_translation_on_down;
  double side_radius, side_icon_alpha, side_icon_scale;
  SpongyVelocity side_velocity;
  /* the circle's own animation (appear, disappear, the tap's hint, the commit) */
  gboolean side_anim;
  double side_from, side_to;
  gint64 side_start, side_duration, side_hold_until;
  Ease side_ease;
  int side_hint_phase;          /* 1 growing, 2 holding and shrinking; 0 none */
};

G_DEFINE_FINAL_TYPE (AnswerView, answer_view, GTK_TYPE_WIDGET)

static GdkRGBA
rgb (guint32 value, double alpha)
{
  return (GdkRGBA) { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0, alpha };
}

/* android.view.animation.BounceInterpolator */
static double
bounce (double t)
{
#define B(x) ((x) * (x) * 8.0)
  t *= 1.1226;
  if (t < 0.3535)
    return B (t);
  if (t < 0.7408)
    return B (t - 0.54719) + 0.7;
  if (t < 0.9644)
    return B (t - 0.8526) + 0.9;
  return B (t - 1.0435) + 0.95;
#undef B
}

static double
ease (Ease e, double t)
{
  static SpongyInterp path_4001;
  static gboolean path_ready;

  switch (e)
    {
    case EASE_FAST_OUT_SLOW_IN:
      return spongy_curve (SPONGY_FAST_OUT_SLOW_IN, t);
    case EASE_LINEAR_OUT_SLOW_IN:
      return spongy_curve (SPONGY_LINEAR_OUT_SLOW_IN, t);
    case EASE_FAST_OUT_LINEAR_IN:
      return spongy_curve (SPONGY_FAST_OUT_LINEAR_IN, t);
    case EASE_ACCELERATE_DECELERATE:
      return spongy_curve (SPONGY_ACCELERATE_DECELERATE, t);
    case EASE_DECELERATE:
      return spongy_curve (SPONGY_DECELERATE, t);
    case EASE_ACCELERATE:
      return spongy_curve (SPONGY_ACCELERATE, t);
    case EASE_PATH_0001:
      return spongy_curve (SPONGY_STANDARD_DECELERATE, t);
    case EASE_PATH_4001:
      if (!path_ready)
        {
          spongy_interp_cubic (&path_4001, 0.4f, 0, 0, 1);
          path_ready = TRUE;
        }
      return spongy_interp_get (&path_4001, t);
    case EASE_BOUNCE:
      return bounce (t);
    case EASE_LINEAR:
    default:
      return t;
    }
}

static gint64
now_us (AnswerView *self)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));

  return clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
}

static void ensure_tick (AnswerView *self);

/* an ObjectAnimator: @value from @from (NAN: from wherever it is then) to @to, @delay ms from now */
static void
tween (AnswerView *self, double *value, double from, double to, guint delay, guint duration, Ease e, Group group)
{
  Tween *t;

  if (self->n_tweens == MAX_TWEENS)
    {
      g_warning ("answer view: too many animations at once");
      return;
    }
  t = &self->tweens[self->n_tweens++];
  *t = (Tween) { value, from, to, now_us (self) + delay * 1000, MAX (duration, 1) * 1000, e, group, FALSE };
  ensure_tick (self);
}

static void
cancel_group (AnswerView *self, Group group)
{
  guint n = 0;

  for (guint i = 0; i < self->n_tweens; i++)
    if (self->tweens[i].group != group)
      self->tweens[n++] = self->tweens[i];
  self->n_tweens = n;
}

/* view.animate().cancel() for the values given */
static void
cancel_values (AnswerView *self, double *a, double *b)
{
  guint n = 0;

  for (guint i = 0; i < self->n_tweens; i++)
    {
      Tween *t = &self->tweens[i];

      if (!(t->group == GROUP_PROPERTY && (t->value == a || t->value == b)))
        self->tweens[n++] = *t;
    }
  self->n_tweens = n;
}

/* one step of every tween; FALSE once none is left */
static gboolean
run_tweens (AnswerView *self, gint64 now)
{
  guint n = 0;

  for (guint i = 0; i < self->n_tweens; i++)
    {
      Tween *t = &self->tweens[i];
      double f;

      if (now < t->start)
        {
          self->tweens[n++] = *t;
          continue;
        }
      if (!t->started)
        {
          /* the newest animation of a value wins: the ones of it already running stop */
          for (guint j = 0; j < self->n_tweens; j++)
            if (j != i && self->tweens[j].value == t->value && self->tweens[j].started)
              self->tweens[j].duration = 0;
          t->started = TRUE;
          if (isnan (t->from))
            t->from = *t->value;
        }
      if (t->duration == 0)       /* stopped by a newer one above */
        continue;
      f = CLAMP ((now - t->start) / (double) t->duration, 0, 1);
      *t->value = t->from + (t->to - t->from) * ease (t->ease, f);
      if (f < 1)
        self->tweens[n++] = *t;
    }
  self->n_tweens = n;
  return n > 0;
}

/* ---- text ----------------------------------------------------------------------------------------------------- */

static PangoLayout *
new_layout (AnswerView *self, const char *family, double size, int weight, gboolean italic)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), NULL);
  g_autoptr(PangoFontDescription) font = pango_font_description_from_string (family);

  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_font_description_set_weight (font, weight);
  if (italic)
    pango_font_description_set_style (font, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description (layout, font);
  return layout;
}

static double
layout_height (PangoLayout *layout)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return height / (double) PANGO_SCALE;
}

static double
layout_width (PangoLayout *layout)
{
  int width, height;

  pango_layout_get_size (layout, &width, &height);
  return width / (double) PANGO_SCALE;
}

static double
roboto_box (PangoLayout *layout, double size)
{
  return ROBOTO_PAD_TOP (size) + layout_height (layout) + ROBOTO_PAD_BOTTOM (size);
}

/* a single line, ellipsized at the end when it is wider than @width */
static void
single_line (PangoLayout *layout, double width)
{
  pango_layout_set_width (layout, (int) (width * PANGO_SCALE));
  pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
}

/* AutoResizeTextView: the largest size from 54 sp down to 32 sp at which the name fits on one line; below that it
 * is ellipsized */
static void
fit_name (AnswerView *self, double width)
{
  g_autoptr(PangoFontDescription) font = pango_font_description_copy (pango_layout_get_font_description (self->name));
  double size;

  pango_layout_set_width (self->name, -1);
  pango_layout_set_ellipsize (self->name, PANGO_ELLIPSIZE_NONE);
  for (size = NAME_SIZE; size > NAME_MIN_SIZE; size--)
    {
      pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
      pango_layout_set_font_description (self->name, font);
      if (layout_width (self->name) <= width)
        break;
    }
  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_layout_set_font_description (self->name, font);
  single_line (self->name, width);
  self->name_size = size;
}

/* the hints may take two lines in the method view's width */
static void
hint_lines (PangoLayout *layout, double width)
{
  pango_layout_set_width (layout, (int) (width * PANGO_SCALE));
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
}

static void
draw_layout (GtkSnapshot *snapshot, PangoLayout *layout, double x, double y, double alpha)
{
  if (alpha <= 0)
    return;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (x, y));
  gtk_snapshot_append_layout (snapshot, layout, &(GdkRGBA) { 1, 1, 1, alpha });
  gtk_snapshot_restore (snapshot);
}

/* ---- where things are ----------------------------------------------------------------------------------------- */

typedef struct
{
  /* the contact grid's rows, top to bottom, each its top edge (dp, before the entry slide) */
  double status_y, name_y, bottom_y, data_y, data_bottom;
  /* the puck's resting places */
  double answer_y, puck_cy, reject_y;
  double side_x, side_y;
} Geometry;

static void
measure_geometry (AnswerView *self, Geometry *g)
{
  double height = gtk_widget_get_height (GTK_WIDGET (self)), width = gtk_widget_get_width (GTK_WIDGET (self));
  double hint_box = roboto_box (self->reject_text, HINT_SIZE);

  g->status_y = GRID_TOP;
  g->name_y = g->status_y + roboto_box (self->status, STATUS_SIZE) + ROW_GAP;
  g->bottom_y = g->name_y + GSF_PAD_TOP (self->name_size) + layout_height (self->name) + GSF_PAD_BOTTOM (self->name_size)
                + ROW_GAP;
  g->data_y = g->bottom_y + roboto_box (self->bottom, STATUS_SIZE);
  g->data_bottom = height - SPACER;

  /* the method's column is anchored at the foot: reject hint, puck, answer hint, from the bottom up */
  g->reject_y = height - REJECT_GAP - hint_box;
  g->puck_cy = g->reject_y - PUCK_GAP - PUCK_BOX / 2.0;
  g->answer_y = g->puck_cy - PUCK_BOX / 2.0 - ANSWER_GAP - roboto_box (self->answer_text, HINT_SIZE);

  g->side_x = width - SIDE_BOX / 2.0;
  g->side_y = height - SIDE_BOX / 2.0;
}

/* ---- the method's states (FlingUpDownMethod) ------------------------------------------------------------------ */

static void set_state (AnswerView *self, AnimState state);
static void side_reset (AnswerView *self, gboolean animate);
static void side_hide (AnswerView *self);
static void restore_hint (AnswerView *self);

/* endAnimation: every AnimatorSet of the method stops where it is; the shiver ends at rest */
static void
end_animation (AnswerView *self)
{
  /* a settle cut short does not lead anywhere (its onAnimationCancel) */
  if (self->phase == STATE_SETTLE && self->phase_end)
    self->after_settle = STATE_NONE;
  cancel_group (self, GROUP_METHOD);
  self->phase = STATE_NONE;
  self->phase_end = 0;
  self->vibration_start = 0;
  self->puck_x = 0;
}

static void
start_phase (AnswerView *self, AnimState phase, guint duration)
{
  self->phase = phase;
  self->phase_end = now_us (self) + duration * 1000;
  ensure_tick (self);
}

/* resetAnswerProgress: the side icon back to rest, the window's colour back */
static void
reset_answer_progress (AnswerView *self)
{
  side_reset (self, TRUE);
  self->background = 0;
}

/* resetTouchState: the puck's colours back, everything it touched animating home */
static void
reset_touch_state (AnswerView *self)
{
  tween (self, &self->puck_sx, NAN, 1, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
  tween (self, &self->puck_sy, NAN, 1, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
  tween (self, &self->bg_scale, NAN, 1, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
  self->puck_progress = 0;
  self->icon_tint = rgb (ANSWER_ICON, 1);
  tween (self, &self->icon_rotation, NAN, 0, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
  reset_answer_progress (self);
  tween (self, &self->text_alpha, NAN, 1, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
  tween (self, &self->puck_alpha, NAN, 1, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
  tween (self, &self->icon_alpha, NAN, 1, 0, 300, EASE_ACCELERATE_DECELERATE, GROUP_PROPERTY);
}

/* ENTRY: the puck flies up from below the screen, overshoots and settles; the hint text rides up with it */
static void
start_entry (AnswerView *self)
{
  end_animation (self);
  /* textUp, which a slip in the source gives FastOutSlowIn, and textDown, left on the default curve */
  tween (self, &self->text_y, 192, -20, 0, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->text_y, -20, 0, 1500, 1333, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  /* rejectTextShow, after puckUp and 333 ms */
  self->reject_alpha = 0;
  tween (self, &self->reject_alpha, NAN, 1, 1833, 667, EASE_FAST_OUT_LINEAR_IN, GROUP_METHOD);
  tween (self, &self->reject_y, -8, 0, 1833, 667, EASE_FAST_OUT_LINEAR_IN, GROUP_METHOD);
  /* puckUp, puckDown */
  tween (self, &self->puck_y, 400, -12, 0, 1500, EASE_PATH_0001, GROUP_METHOD);
  tween (self, &self->puck_y, -12, 0, 1500, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  /* puckScaleUp, puckScaleDown */
  tween (self, &self->bg_scale, 0.33, 1.1, 0, 1333, EASE_PATH_4001, GROUP_METHOD);
  tween (self, &self->bg_scale, 1.1, 1, 1500, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  self->vibration_start = now_us (self);
  start_phase (self, STATE_ENTRY, 1500 + 1333);
}

/* BOUNCE, one 3 s breath (createBreatheAnimation): the puck and its text hop 42 dp and back, the disc swells, the
 * reject hint fades out and back in. It starts again when it ends, for as long as the state lasts. */
static void
start_bounce (AnswerView *self)
{
  end_animation (self);
  /* up, after 167 ms */
  tween (self, &self->text_y, 0, -42, 167, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->reject_alpha, NAN, 0, 167, 667, EASE_FAST_OUT_LINEAR_IN, GROUP_METHOD);
  tween (self, &self->reject_y, 0, -8, 167, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->puck_y, NAN, -42, 167, 1500, EASE_PATH_4001, GROUP_METHOD);
  tween (self, &self->bg_scale, 1, 1.0625, 167, 1333, EASE_PATH_4001, GROUP_METHOD);
  /* down, after puckUp */
  tween (self, &self->puck_y, NAN, 0, 1667, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->text_y, -42, 0, 1667, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->bg_scale, 1.0625, 1, 1667, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->reject_alpha, NAN, 1, 1667 + 333, 667, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->reject_y, -8, 0, 1667, 1333, EASE_FAST_OUT_SLOW_IN, GROUP_METHOD);
  self->vibration_start = now_us (self);
  start_phase (self, STATE_BOUNCE, 3000);
}

/* SETTLE: 100 ms to rest after a swipe, then on to the bounce or the hint */
static void
start_settle (AnswerView *self)
{
  end_animation (self);
  tween (self, &self->bg_scale, NAN, 1, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->icon_rotation, NAN, 0, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->text_alpha, NAN, 1, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->puck_alpha, NAN, 1, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->icon_alpha, NAN, 1, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->puck_x, NAN, 0, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->puck_y, NAN, 0, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  start_phase (self, STATE_SETTLE, 100);
}

/* HINT, after a tap: the puck squashes, jumps 60 dp and bounces back, and "Swipe down to reject" shows for 2 s */
static void
start_hint (AnswerView *self)
{
  double text_offset = 60 + (1.15 - 1) * PUCK;

  end_animation (self);
  reset_touch_state (self);
  /* puckAnticipate (REVERSE once, pivot at the foot), puckResetTranslation */
  tween (self, &self->puck_sy, NAN, 0.95, 0, 100, EASE_DECELERATE, GROUP_METHOD);
  tween (self, &self->puck_sy, 0.95, 1, 100, 100, EASE_ACCELERATE, GROUP_METHOD);
  tween (self, &self->puck_sx, NAN, 1.05, 0, 100, EASE_DECELERATE, GROUP_METHOD);
  tween (self, &self->puck_sx, 1.05, 1, 100, 100, EASE_ACCELERATE, GROUP_METHOD);
  self->pivot_bottom_until = now_us (self) + 200 * 1000;
  tween (self, &self->puck_y, NAN, 0, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  tween (self, &self->puck_x, NAN, 0, 0, 100, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  /* up, after the anticipation (config_shortAnimTime 200 ms) */
  tween (self, &self->text_y, NAN, -text_offset, 200, 200, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->puck_y, NAN, -60, 200, 200, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->bg_scale, 1, 1.15, 200, 200, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->reject_y, NAN, 8, 200, 200, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->reject_alpha, NAN, 1, 200, 200, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  /* down, after puckUp (config_mediumAnimTime 400 ms; the puck with BounceInterpolator) */
  tween (self, &self->text_y, NAN, 0, 400, 400, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->puck_y, NAN, 0, 400, 400, EASE_BOUNCE, GROUP_METHOD);
  tween (self, &self->bg_scale, 1.15, 1, 400, 200, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  tween (self, &self->reject_y, NAN, 0, 400, 400, EASE_LINEAR_OUT_SLOW_IN, GROUP_METHOD);
  /* rejectHintHide, after HINT_REJECT_SHOW_DURATION_MILLIS */
  tween (self, &self->reject_alpha, NAN, 0, 2000, 300, EASE_ACCELERATE_DECELERATE, GROUP_METHOD);
  start_phase (self, STATE_HINT, 2000 + 300);
}

/* setAnimationState: HINT and BOUNCE out of a swipe go through SETTLE first; nothing follows COMPLETED */
static void
set_state (AnswerView *self, AnimState state)
{
  if (state != STATE_HINT && self->state == state)
    return;
  if (self->state == STATE_COMPLETED)
    return;
  if ((state == STATE_HINT || state == STATE_BOUNCE) && self->state == STATE_SWIPE)
    {
      self->after_settle = state;
      state = STATE_SETTLE;
    }
  self->state = state;
  switch (state)
    {
    case STATE_ENTRY:
      start_entry (self);
      break;
    case STATE_BOUNCE:
      start_bounce (self);
      break;
    case STATE_SWIPE:
      /* startSwipeToAnswerSwipeAnimation */
      reset_touch_state (self);
      end_animation (self);
      break;
    case STATE_SETTLE:
      start_settle (self);
      break;
    case STATE_HINT:
      start_hint (self);
      break;
    case STATE_COMPLETED:
      end_animation (self);
      break;
    case STATE_NONE:
    default:
      break;
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* an AnimatorSet ran to its end */
static void
on_phase_end (AnswerView *self)
{
  AnimState phase = self->phase, next;

  self->phase = STATE_NONE;
  self->phase_end = 0;
  switch (phase)
    {
    case STATE_ENTRY:
      if (self->state == STATE_ENTRY)
        set_state (self, STATE_BOUNCE);
      break;
    case STATE_BOUNCE:
      /* AnimatorSet does not repeat: a new breath once the last has ended */
      if (self->state == STATE_BOUNCE)
        start_bounce (self);
      break;
    case STATE_SETTLE:
      next = self->after_settle;
      self->after_settle = STATE_NONE;
      if (next != STATE_NONE)
        set_state (self, next);
      break;
    case STATE_HINT:
      if (self->state == STATE_HINT)
        set_state (self, STATE_BOUNCE);
      break;
    default:
      break;
    }
}

/* VibrateInterpolator: a ±1 dp shiver sideways, ramping up at 583 ms, full until 1583 ms, gone by 1833 ms. Its
 * sine runs at 80 radians a millisecond, far above the frame rate, so what shows is a jitter. */
static double
vibration (double ms)
{
  double slider = 0;

  if (ms > 583 && ms < 750)
    slider = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, (ms - 583) / 167);
  else if (ms >= 750 && ms <= 1583)
    slider = 1;
  else if (ms > 1583 && ms < 1833)
    slider = 1 - spongy_curve (SPONGY_FAST_OUT_SLOW_IN, (ms - 1583) / 250);
  return slider * sin (ms * 80 * slider);
}

/* ---- the swipe ------------------------------------------------------------------------------------------------ */

static double
lerp (double a, double b, double t)
{
  return a + (b - a) * t;
}

/* AnswerFragment.onAnswerProgressUpdate: the window fades on a decline, the contact grid fades and shrinks from a
 * quarter of the way on, and the side icon goes */
static void
on_answer_progress (AnswerView *self, double p)
{
  double header = MAX (0, (fabs (p) - 1) / (1 - 0.25) + 1);

  self->background = !self->waiting && p < 0 ? -p : 0;
  self->grid_alpha = lerp (self->grid_alpha, 1 - header, 0.5);
  self->grid_scale = lerp (self->grid_scale, lerp (1, 0.75, header), 0.5);
  if (fabs (p) >= 0.0001)
    {
      side_hide (self);
      restore_hint (self);
    }
}

/* updateSwipeTextAndPuckForTouch: each value moves halfway toward where the progress puts it */
static void
update_swipe (AnswerView *self)
{
  double p = CLAMP (self->progress, -1, 1), a = fabs (p);
  gboolean accepting = p >= 0;
  double text_alpha = MAX (0, 1 - a * 9);

  cancel_values (self, &self->text_alpha, &self->text_y);
  cancel_values (self, &self->icon_rotation, &self->icon_alpha);
  self->text_alpha = lerp (self->text_alpha, text_alpha, 0.5);
  self->reject_alpha = lerp (self->reject_alpha, MIN (text_alpha, self->reject_alpha), 0.5);
  self->text_y = lerp (self->text_y, 0, 0.5);
  self->puck_progress = p;
  self->icon_rotation = lerp (self->icon_rotation, accepting ? 0 : a * 135, 0.5);
  self->icon_tint = rgb (ANSWER_ICON, 1 - MIN (1, a * 4));
  self->puck_y = lerp (self->puck_y, -p * (accepting ? THRESHOLD : 24), 0.5);
  on_answer_progress (self, p);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
set_progress (AnswerView *self, double progress)
{
  if (fabs (progress) > HINT_MOVE)
    self->hint_distance_exceeded = TRUE;
  self->progress = progress;
  if (self->state == STATE_SWIPE)
    update_swipe (self);
}

/* performAccept / performReject: the puck goes, the call is answered or declined (once) */
static void
perform (AnswerView *self, gboolean accept)
{
  set_state (self, STATE_COMPLETED);
  if (accept && !self->answer_clicked)
    {
      self->answer_clicked = TRUE;
      calls_client_answer (calls_client_get (), self->call);
    }
  else if (!accept && !self->reject_clicked)
    {
      self->reject_clicked = TRUE;
      calls_client_decline (calls_client_get (), self->call);
    }
}

/* onMoveReset / onMoveFinish */
static void
on_move_ended (AnswerView *self)
{
  if (self->progress == 0)
    {
      set_state (self, self->hint_distance_exceeded ? STATE_BOUNCE : STATE_HINT);
      reset_touch_state (self);
      reset_answer_progress (self);
    }
  else
    {
      self->touch_enabled = FALSE;
      perform (self, self->progress > 0);
    }
}

static double
y_to_progress (AnswerView *self, double y)
{
  gboolean below = y > self->zero_y;
  double nearest = below ? self->reject_line : self->accept_line;

  if (nearest == self->zero_y)
    return 0;
  return CLAMP ((y - self->zero_y) / (nearest - self->zero_y) * (below ? -1 : 1), -1, 1);
}

static FlingTarget
fling_target (AnswerView *self, double y, double velocity)
{
  double progress = y_to_progress (self, y), min = MIN_FLING;
  gboolean same_direction;

  if (velocity > 0)
    min *= 2;                   /* REJECT_FLING_THRESHOLD_MODIFIER */
  if (fabs (velocity) < min)
    {
      if (fabs (progress) > SWIPE_RECENTER)
        return progress > 0 ? TARGET_ACCEPT : TARGET_REJECT;
      return TARGET_CENTER;
    }
  same_direction = (velocity < 0) == (progress > 0);
  if (!same_direction && fabs (progress) >= FLING_RECENTER)
    return TARGET_CENTER;
  return velocity < 0 ? TARGET_ACCEPT : TARGET_REJECT;
}

/* the progress animated to its target. FlingAnimationUtils measures this distance in progress against a speed in
 * pixels a second, so a real fling takes a frame; a release without speed takes 350 ms */
static void
fling (AnswerView *self, double velocity, FlingTarget target, gboolean falsing)
{
  self->flinging = TRUE;
  self->fling_from = self->progress;
  self->fling_to = target == TARGET_ACCEPT ? 1 : target == TARGET_REJECT ? -1 : 0;
  self->fling_start = now_us (self);
  if (velocity == 0 || (target == TARGET_CENTER && falsing))
    {
      self->fling_duration = 350 * 1000;
      self->fling_ease = EASE_FAST_OUT_SLOW_IN;
    }
  else
    {
      self->fling_duration = 1;
      self->fling_ease = EASE_LINEAR_OUT_SLOW_IN;
    }
  ensure_tick (self);
}

static void
end_motion (AnswerView *self, double y, double t_ms, gboolean cancel)
{
  if ((self->tracking && self->slop_exceeded) || fabs (y - self->initial_y) > TOUCH_SLOP || cancel)
    {
      float vx, vy;
      double vector;
      gboolean false_touch = !self->above_falsing;

      spongy_velocity_get (&self->velocity, t_ms, &vx, &vy);
      vector = copysign (hypot (vx, vy), vy);
      self->tracking = FALSE;
      fling (self, vy, false_touch || !self->slop_exceeded || cancel ? TARGET_CENTER : fling_target (self, y, vector),
             false_touch);
    }
  else
    {
      self->tracking = FALSE;
      set_progress (self, 0);
      on_move_ended (self);
    }
}

/* ---- the side icon -------------------------------------------------------------------------------------------- */

static gboolean
side_visible (AnswerView *self)
{
  return self->other_call && !self->side_done;
}

/* SwipeButtonView's circle animated to @radius */
static void
side_animate (AnswerView *self, double radius, guint delay, guint duration, Ease e)
{
  self->side_anim = TRUE;
  self->side_from = self->side_radius;
  self->side_to = radius;
  self->side_start = now_us (self) + delay * 1000;
  self->side_duration = MAX (duration, 1) * 1000;
  self->side_ease = e;
  ensure_tick (self);
}

/* setCircleRadius: the circle comes in over 80 ms and goes over up to 200 ms; in between it follows at once */
static void
side_set_radius (AnswerView *self, double radius)
{
  gboolean hidden = !self->side_anim && self->side_radius == 0, now_hidden = radius == 0;

  if (hidden == now_hidden && !self->side_anim)
    self->side_radius = radius;
  else if (hidden != now_hidden)
    side_animate (self, radius, 0, now_hidden ? 200 : 80,
                  now_hidden ? EASE_FAST_OUT_LINEAR_IN : EASE_LINEAR_OUT_SLOW_IN);
  else
    self->side_to = radius;
}

/* updateIconAlpha / getScale: the icon grows and brightens as the swipe goes on */
static void
side_set_icon (AnswerView *self, double alpha)
{
  self->side_icon_scale = MIN (alpha / SIDE_ALPHA * 0.2 + 0.8, 1.5);
  self->side_icon_alpha = MIN (alpha, 1);
}

static double
side_radius_for (double translation)
{
  return translation <= TOUCH_SLOP ? 0 : (translation - TOUCH_SLOP) * 0.25 + SIDE_MIN_RADIUS;
}

static void
side_set_translation (AnswerView *self, double translation)
{
  double amount = fabs (translation), alpha = amount / SIDE_TRIGGER;

  self->side_translation = MIN (0, translation);
  side_set_radius (self, side_radius_for (amount));
  side_set_icon (self, alpha + MAX (0, 1 - alpha) * SIDE_ALPHA);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* reset: the icon back at rest, its circle gone */
static void
side_reset (AnswerView *self, gboolean animate)
{
  if (self->side_finishing)
    return;
  self->side_hint_phase = 0;
  self->side_translation = 0;
  if (animate)
    side_set_radius (self, 0);
  else
    {
      self->side_anim = FALSE;
      self->side_radius = 0;
    }
  side_set_icon (self, SIDE_ALPHA);
}

/* animateHideLeftRightIcon: a swipe of the puck has begun */
static void
side_hide (AnswerView *self)
{
  if (self->side_finishing)
    return;
  self->side_hint_phase = 0;
  side_set_radius (self, 0);
  side_set_icon (self, 0);
}

/* finishAnimation: the circle covers the screen, the icon fades, then the call is answered and the other ended */
static void
side_commit (AnswerView *self)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self)), height = gtk_widget_get_height (GTK_WIDGET (self));
  Geometry g;

  measure_geometry (self, &g);
  self->side_finishing = TRUE;
  self->side_hint_phase = 0;
  side_animate (self, hypot (MAX (g.side_x, width - g.side_x), MAX (g.side_y, height - g.side_y)), 0, 300,
                EASE_FAST_OUT_SLOW_IN);
  self->side_icon_alpha = 0;
}

static gboolean
on_hint_restore (gpointer data)
{
  AnswerView *self = data;

  self->hint_restore = 0;
  restore_hint (self);
  return G_SOURCE_REMOVE;
}

/* onIconClicked: the circle pulses out to 60 dp and back, and the puck's hint says what the icon does */
static void
side_hint (AnswerView *self)
{
  self->side_hint_phase = 1;
  side_animate (self, SIDE_HINT_GROW, 0, 200, EASE_LINEAR_OUT_SLOW_IN);
  g_free (self->hint);
  self->hint = g_strdup (_("Swipe from icon to answer and end ongoing call"));
  pango_layout_set_text (self->answer_text, self->hint, -1);
  g_clear_handle_id (&self->hint_restore, g_source_remove);
  self->hint_restore = g_timeout_add (SIDE_HINT_MS, on_hint_restore, self);
}

/* one frame of the circle's own animation */
static void
side_frame (AnswerView *self, gint64 now)
{
  double f;

  if (!self->side_anim || now < self->side_start)
    return;
  f = CLAMP ((now - self->side_start) / (double) self->side_duration, 0, 1);
  self->side_radius = lerp (self->side_from, self->side_to, ease (self->side_ease, f));
  if (self->side_hint_phase)
    {
      /* getAnimatorToRadius: the icon follows the circle as if it were being swiped */
      double translation = self->side_radius > SIDE_MIN_RADIUS ? (self->side_radius - SIDE_MIN_RADIUS) / 0.25 + TOUCH_SLOP
                                                               : 0;
      double alpha = translation / SIDE_TRIGGER;

      side_set_icon (self, alpha + MAX (0, 1 - alpha) * SIDE_ALPHA);
    }
  if (f < 1)
    return;
  self->side_anim = FALSE;
  if (self->side_hint_phase == 1)
    {
      /* HINT_CIRCLE_OPEN_DURATION held, then HINT_PHASE2_DURATION back */
      self->side_hint_phase = 2;
      side_animate (self, 0, 500, 350, EASE_FAST_OUT_LINEAR_IN);
    }
  else if (self->side_hint_phase == 2)
    self->side_hint_phase = 0;
  else if (self->side_finishing)
    {
      self->side_finishing = FALSE;
      self->side_done = TRUE;
      self->touch_enabled = FALSE;
      set_state (self, STATE_COMPLETED);
      if (!self->answer_clicked)
        {
          self->answer_clicked = TRUE;
          calls_client_answer_and_release (calls_client_get (), self->call);
        }
    }
}

/* flingWithCurrentVelocity: past 110 dp it commits, unless flicked back; otherwise it snaps home */
static void
side_end (AnswerView *self, double x, double y, double t_ms, gboolean cancel)
{
  float vx, vy;
  double bx = x - self->side_down_x, by = y - self->side_down_y, length = hypot (bx, by), velocity;
  gboolean snap_back, wrong_way;

  spongy_velocity_get (&self->side_velocity, t_ms, &vx, &vy);
  /* the speed along the swipe, positive toward the left (the right icon's way) */
  velocity = length > 0 ? -(vx * bx + vy * by) / length : 0;
  snap_back = fabs (self->side_translation) < fabs (self->side_translation_on_down) + SIDE_TRIGGER;
  wrong_way = velocity * self->side_translation < 0;
  snap_back |= fabs (velocity) > SIDE_MIN_FLING && wrong_way;
  self->side_touch = FALSE;
  if (snap_back || cancel)
    side_reset (self, TRUE);
  else
    side_commit (self);
  /* a tap: the hint */
  if (!self->side_slop && !cancel)
    side_hint (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* restoreSwipeHintTexts: what the puck's hint says while another call is up */
static void
restore_hint (AnswerView *self)
{
  g_clear_handle_id (&self->hint_restore, g_source_remove);
  g_clear_pointer (&self->hint, g_free);
  if (self->other_call)
    {
      if (self->held_call)
        self->hint = g_strdup (_("Swipe up to answer and end call on hold"));
      else if (self->can_hold)
        self->hint = g_strdup (_("Swipe up to answer and hold ongoing call"));
    }
  pango_layout_set_text (self->answer_text, self->hint ? self->hint : _("Swipe up to answer"), -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- touch ---------------------------------------------------------------------------------------------------- */

static double
event_ms (GtkGesture *gesture)
{
  return gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (gesture));
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, AnswerView *self)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self));
  Geometry g;

  measure_geometry (self, &g);
  if (!self->touch_enabled || self->state == STATE_COMPLETED)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  /* AffordanceHolderLayout takes a touch on the side icon before the puck's view sees it */
  if (side_visible (self) && !self->side_finishing && hypot (x - g.side_x, y - g.side_y) <= SIDE_TOUCH / 2.0)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      self->side_touch = TRUE;
      self->side_slop = FALSE;
      self->side_hint_phase = 0;
      self->side_anim = FALSE;
      self->side_down_x = x;
      self->side_down_y = y;
      self->side_translation_on_down = self->side_translation;
      spongy_velocity_clear (&self->side_velocity);
      spongy_velocity_add (&self->side_velocity, event_ms (GTK_GESTURE (gesture)), x, y);
      return;
    }
  /* the method view: inside its side margins, below the dead zone at the top */
  if (x < METHOD_SIDE || x > width - METHOD_SIDE || y < DEAD_ZONE_TOP)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  /* startMotion */
  self->initial_y = self->last_y = y;
  self->hint_distance_exceeded = FALSE;
  if (self->progress <= 0.25)
    {
      self->accept_line = MAX (0, y - THRESHOLD);
      self->reject_line = MIN (gtk_widget_get_height (GTK_WIDGET (self)), y + THRESHOLD);
      self->zero_y = y;
    }
  self->above_falsing = FALSE;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, event_ms (GTK_GESTURE (gesture)), x, y);
  /* a touch that catches the puck in flight is a swipe already */
  self->slop_exceeded = self->flinging;
  self->flinging = FALSE;
  self->tracking = TRUE;
  set_state (self, STATE_SWIPE);
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, AnswerView *self)
{
  double x, y;

  gtk_gesture_drag_get_start_point (gesture, &x, &y);
  x += dx;
  y += dy;
  if (self->side_touch)
    {
      double distance = hypot (x - self->side_down_x, y - self->side_down_y);

      spongy_velocity_add (&self->side_velocity, event_ms (GTK_GESTURE (gesture)), x, y);
      if (distance > TOUCH_SLOP)
        self->side_slop = TRUE;
      side_set_translation (self, MIN (0, self->side_translation_on_down - distance));
      return;
    }
  if (!self->tracking)
    return;
  self->last_y = y;
  if (fabs (y - self->initial_y) > TOUCH_SLOP)
    self->slop_exceeded = TRUE;
  if (fabs (y - self->initial_y) >= FALSING)
    self->above_falsing = TRUE;
  set_progress (self, y_to_progress (self, y));
  spongy_velocity_add (&self->velocity, event_ms (GTK_GESTURE (gesture)), x, y);
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, AnswerView *self)
{
  double x, y;

  gtk_gesture_drag_get_start_point (gesture, &x, &y);
  x += dx;
  y += dy;
  if (self->side_touch)
    {
      spongy_velocity_add (&self->side_velocity, event_ms (GTK_GESTURE (gesture)), x, y);
      side_end (self, x, y, event_ms (GTK_GESTURE (gesture)), FALSE);
      return;
    }
  if (!self->tracking)
    return;
  spongy_velocity_add (&self->velocity, event_ms (GTK_GESTURE (gesture)), x, y);
  end_motion (self, y, event_ms (GTK_GESTURE (gesture)), FALSE);
}

static void
on_drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, AnswerView *self)
{
  if (self->side_touch)
    side_end (self, self->side_down_x, self->side_down_y, event_ms (gesture), TRUE);
  else if (self->tracking)
    end_motion (self, self->last_y, event_ms (gesture), TRUE);
}

/* ---- the frame ------------------------------------------------------------------------------------------------ */

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  AnswerView *self = ANSWER_VIEW (widget);
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  gboolean running;

  running = run_tweens (self, now);
  if (self->vibration_start)
    {
      double ms = (now - self->vibration_start) / 1000.0;

      self->puck_x = ms < 1833 ? vibration (ms) : 0;
      if (ms >= 1833)
        self->vibration_start = 0;
    }
  if (self->flinging)
    {
      double f = CLAMP ((now - self->fling_start) / (double) self->fling_duration, 0, 1);

      set_progress (self, lerp (self->fling_from, self->fling_to, ease (self->fling_ease, f)));
      if (f >= 1)
        {
          self->flinging = FALSE;
          on_move_ended (self);
        }
    }
  side_frame (self, now);
  if (self->phase_end && now >= self->phase_end)
    on_phase_end (self);
  gtk_widget_queue_draw (widget);

  running = running || self->n_tweens > 0 || self->phase_end || self->vibration_start || self->flinging
            || self->side_anim;
  if (!running)
    self->tick = 0;
  return running ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
ensure_tick (AnswerView *self)
{
  if (!self->tick && gtk_widget_get_mapped (GTK_WIDGET (self)))
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void
draw_centered (GtkSnapshot *snapshot, PangoLayout *layout, double width, double y, double alpha)
{
  PangoRectangle logical;

  /* the layout centres its line in its own width already: its logical box says where the line starts */
  pango_layout_get_pixel_extents (layout, NULL, &logical);
  draw_layout (snapshot, layout, round ((width - logical.width) / 2) - logical.x, y, alpha);
}

static void
draw_grid (AnswerView *self, GtkSnapshot *snapshot, const Geometry *g, double width, double height)
{
  /* createTranslation: each row slides up from half its offset in the column */
  double slide = 1 - self->entry;
  double alpha = self->grid_alpha * self->entry_alpha;
  double cy = GRID_TOP + (height - GRID_TOP) / 2;

  if (alpha <= 0)
    return;
  gtk_snapshot_save (snapshot);
  /* the contact grid's container scales about its centre */
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (width / 2, cy));
  gtk_snapshot_scale (snapshot, self->grid_scale, self->grid_scale);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-width / 2, -cy));
  gtk_snapshot_push_opacity (snapshot, alpha);
  draw_centered (snapshot, self->status, width, g->status_y + ROBOTO_PAD_TOP (STATUS_SIZE), TEXT_SECONDARY);
  draw_centered (snapshot, self->name, width,
                 g->name_y + (g->name_y - GRID_TOP) * 0.5 * slide + GSF_PAD_TOP (self->name_size), 1);
  draw_centered (snapshot, self->bottom, width,
                 g->bottom_y + (g->bottom_y - GRID_TOP) * 0.5 * slide + ROBOTO_PAD_TOP (STATUS_SIZE), TEXT_SECONDARY);
  if (self->photo && g->data_bottom - g->data_y >= AVATAR)
    avatar_snapshot (snapshot, GTK_WIDGET (self), width / 2,
                     (g->data_y + g->data_bottom) / 2 + (g->data_y - GRID_TOP) * 0.5 * slide, AVATAR, NULL, NULL,
                     self->photo);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static void
draw_puck (AnswerView *self, GtkSnapshot *snapshot, const Geometry *g, double width)
{
  double cx = width / 2 + self->puck_x, cy = g->puck_cy + self->puck_y;
  double pivot = self->pivot_bottom_until > now_us (self) ? PUCK_BOX / 2.0 : 0;
  double r = PUCK / 2.0 * self->bg_scale, a = fabs (self->puck_progress);
  GdkRGBA white = { 1, 1, 1, 1 };
  GdkRGBA tint = rgb (self->puck_progress >= 0 ? ACCEPT_COLOR : HANGUP_COLOR, 1);
  GdkRGBA disc = neo_rgba_mix (&white, &tint, a);
  graphene_rect_t box = GRAPHENE_RECT_INIT (-r, -r, 2 * r, 2 * r);
  GskRoundedRect shape;
  GdkRGBA icon = self->icon_tint;

  if (self->puck_alpha <= 0)
    return;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx, cy + pivot));
  gtk_snapshot_scale (snapshot, self->puck_sx, self->puck_sy);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (0, -pivot));
  gtk_snapshot_push_opacity (snapshot, self->puck_alpha);

  /* the disc at elevation 8 dp, 12 while the finger is on it (activated_button_elevation) */
  gsk_rounded_rect_init_from_rect (&shape, &box, r);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.18 }, 0, self->tracking ? 6 : 4, 1,
                                     self->tracking ? 12 : 8);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &(GdkRGBA) { 0, 0, 0, 0.3 }, 0, 1, 0, 3);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &disc, &box);
  gtk_snapshot_pop (snapshot);

  /* the phone, turning toward "hang up" on a decline */
  icon.alpha *= self->icon_alpha;
  if (icon.alpha > 0)
    {
      gtk_snapshot_rotate (snapshot, self->icon_rotation);
      neo_snapshot_icon (snapshot, GTK_WIDGET (self), "dialer-call-symbolic", -PUCK_ICON / 2.0, -PUCK_ICON / 2.0,
                         PUCK_ICON, &icon);
    }
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

/* ArgbEvaluator: the channels mixed in linear light (gamma 2.2) */
static GdkRGBA
argb_evaluate (const GdkRGBA *a, const GdkRGBA *b, double t)
{
#define MIX(c) pow (pow (a->c, 2.2) + (pow (b->c, 2.2) - pow (a->c, 2.2)) * t, 1 / 2.2)
  return (GdkRGBA) { MIX (red), MIX (green), MIX (blue), a->alpha + (b->alpha - a->alpha) * t };
#undef MIX
}

static void
draw_side (AnswerView *self, GtkSnapshot *snapshot, const Geometry *g)
{
  GdkRGBA white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 }, icon;
  double size = SIDE_ICON * self->side_icon_scale;

  if (!self->other_call || (self->side_done && !self->side_finishing))
    return;
  if (self->side_radius > 0)
    {
      /* updateCircleColor: half-white at the smallest circle, opaque from 1.5 times that */
      double fraction = 0.5 + 0.5 * CLAMP ((self->side_radius - SIDE_MIN_RADIUS) / (0.5 * SIDE_MIN_RADIUS), 0, 1);
      graphene_rect_t box = GRAPHENE_RECT_INIT (g->side_x - self->side_radius, g->side_y - self->side_radius,
                                                2 * self->side_radius, 2 * self->side_radius);
      GskRoundedRect circle;

      gsk_rounded_rect_init_from_rect (&circle, &box, self->side_radius);
      gtk_snapshot_push_rounded_clip (snapshot, &circle);
      gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 1, 1, 1, fraction }, &box);
      gtk_snapshot_pop (snapshot);
    }
  /* updateIconColor: white to black as the circle grows to its smallest size */
  icon = argb_evaluate (&white, &black, MIN (1, self->side_radius / SIDE_MIN_RADIUS));
  icon.alpha = self->side_icon_alpha;
  if (icon.alpha > 0)
    neo_snapshot_icon (snapshot, GTK_WIDGET (self), "dialer-end-answer-symbolic", g->side_x - size / 2,
                       g->side_y - size / 2, size, &icon);
}

static void
answer_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  AnswerView *self = ANSWER_VIEW (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  graphene_rect_t all = GRAPHENE_RECT_INIT (0, 0, width, height);
  GdkRGBA background = rgb (BACKGROUND, 1);
  Geometry g;

  measure_geometry (self, &g);
  /* updateWindowBackgroundColor: blendARGB toward #66000000, alpha too, over what is behind (here black) */
  if (self->background > 0)
    {
      double t = self->background;

      gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 0, 0, 0, 1 }, &all);
      background = (GdkRGBA) { background.red * (1 - t), background.green * (1 - t), background.blue * (1 - t),
                               1 - t + 0.4 * t };
    }
  gtk_snapshot_append_color (snapshot, &background, &all);

  draw_grid (self, snapshot, &g, width, height);
  if (self->state != STATE_COMPLETED)
    {
      draw_layout (snapshot, self->answer_text, METHOD_SIDE,
                   g.answer_y + self->text_y + ROBOTO_PAD_TOP (HINT_SIZE), self->text_alpha * TEXT_SECONDARY);
      draw_puck (self, snapshot, &g, width);
    }
  draw_layout (snapshot, self->reject_text, METHOD_SIDE, g.reject_y + self->reject_y + ROBOTO_PAD_TOP (HINT_SIZE),
               self->reject_alpha * TEXT_SECONDARY);
  draw_side (self, snapshot, &g);
}

static void
answer_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  AnswerView *self = ANSWER_VIEW (widget);

  single_line (self->status, width - 2 * GRID_SIDE);
  single_line (self->bottom, width - 2 * GRID_SIDE);
  fit_name (self, width - 2 * GRID_SIDE);
  hint_lines (self->answer_text, width - 2 * METHOD_SIDE);
  hint_lines (self->reject_text, width - 2 * METHOD_SIDE);
}

/* ---- the call ------------------------------------------------------------------------------------------------- */

static void
on_entry (AnswerView *self)
{
  /* animateEntry: the grid fades in (its alpha on the default curve) as its rows slide up (LinearOutSlowIn) */
  self->entry = 0;
  self->entry_alpha = 0;
  tween (self, &self->entry, 0, 1, 0, ENTRY_MS, EASE_LINEAR_OUT_SLOW_IN, GROUP_FRAGMENT);
  tween (self, &self->entry_alpha, 0, 1, 0, ENTRY_MS, EASE_ACCELERATE_DECELERATE, GROUP_FRAGMENT);
}

static void
start_over (AnswerView *self)
{
  self->n_tweens = 0;
  self->state = STATE_NONE;
  self->after_settle = STATE_NONE;
  self->phase = STATE_NONE;
  self->phase_end = 0;
  self->answer_clicked = self->reject_clicked = FALSE;
  self->touch_enabled = TRUE;
  self->tracking = self->flinging = FALSE;
  self->progress = 0;
  self->side_done = self->side_finishing = self->side_touch = FALSE;
  self->text_y = self->reject_y = self->puck_x = self->puck_y = self->icon_rotation = 0;
  self->text_alpha = self->puck_alpha = self->icon_alpha = 1;
  self->reject_alpha = 0;
  self->puck_sx = self->puck_sy = self->bg_scale = 1;
  self->icon_tint = rgb (CALL_GREEN, 1);
  self->puck_progress = 0;
  self->grid_alpha = self->grid_scale = 1;
  self->background = 0;
  side_reset (self, FALSE);
  restore_hint (self);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      on_entry (self);
      set_state (self, STATE_ENTRY);
    }
}

void
answer_view_set_call (AnswerView *self, const CallInfo *call, const Caller *caller)
{
  g_autofree char *bottom = caller_label_and_number (caller);
  gboolean same = self->call == call->id;

  self->call = call->id;
  self->waiting = call->state == CALL_STATE_CALL_WAITING;
  /* TopRow: "Call from"; BottomRow: the number's kind and the number, for a contact */
  pango_layout_set_text (self->status, _("Call from"), -1);
  pango_layout_set_text (self->name, caller->name, -1);
  pango_layout_set_text (self->bottom, bottom ? bottom : "", -1);
  g_clear_object (&self->photo);
  self->photo = avatar_texture_from_bytes (caller->photo);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  if (!same)
    start_over (self);
}

guint
answer_view_get_call (AnswerView *self)
{
  return self->call;
}

void
answer_view_set_other_call (AnswerView *self, gboolean other_call, gboolean can_hold, gboolean held_call)
{
  if (self->other_call == other_call && self->can_hold == can_hold && self->held_call == held_call)
    return;
  self->other_call = other_call;
  self->can_hold = can_hold;
  self->held_call = held_call;
  if (!self->hint_restore)
    restore_hint (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
answer_view_reset (AnswerView *self)
{
  start_over (self);
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
answer_view_map (GtkWidget *widget)
{
  AnswerView *self = ANSWER_VIEW (widget);

  GTK_WIDGET_CLASS (answer_view_parent_class)->map (widget);
  /* onViewCreated / onStart: the entry, or the puck back at rest after a swipe the screen lost */
  if (self->state == STATE_NONE)
    {
      on_entry (self);
      set_state (self, STATE_ENTRY);
    }
  else if (self->state == STATE_SWIPE || self->state == STATE_HINT)
    {
      self->tracking = self->flinging = FALSE;
      self->progress = 0;
      set_state (self, STATE_BOUNCE);
      reset_touch_state (self);
    }
  ensure_tick (self);
}

static void
answer_view_unmap (GtkWidget *widget)
{
  AnswerView *self = ANSWER_VIEW (widget);

  if (self->tick)
    gtk_widget_remove_tick_callback (widget, self->tick);
  self->tick = 0;
  GTK_WIDGET_CLASS (answer_view_parent_class)->unmap (widget);
}

static void
answer_view_dispose (GObject *object)
{
  AnswerView *self = ANSWER_VIEW (object);

  g_clear_handle_id (&self->hint_restore, g_source_remove);
  g_clear_object (&self->status);
  g_clear_object (&self->name);
  g_clear_object (&self->bottom);
  g_clear_object (&self->answer_text);
  g_clear_object (&self->reject_text);
  g_clear_object (&self->photo);
  g_clear_pointer (&self->hint, g_free);
  G_OBJECT_CLASS (answer_view_parent_class)->dispose (object);
}

static void
answer_view_class_init (AnswerViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = answer_view_dispose;
  widget_class->snapshot = answer_view_snapshot;
  widget_class->size_allocate = answer_view_size_allocate;
  widget_class->map = answer_view_map;
  widget_class->unmap = answer_view_unmap;
  gtk_widget_class_set_css_name (widget_class, "answer-view");
}

static void
answer_view_init (AnswerView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  /* the status and bottom rows in Roboto at 18 sp, the name in Google Sans Flex at 54 sp, the hints at 14 sp
   * italic: all at 400, which Bold text makes 700 */
  self->status = new_layout (self, "Neo Roboto", STATUS_SIZE, 700, FALSE);
  self->name = new_layout (self, "Google Sans Flex, Neo Roboto", NAME_SIZE, 700, FALSE);
  self->bottom = new_layout (self, "Neo Roboto", STATUS_SIZE, 700, FALSE);
  self->answer_text = new_layout (self, "Neo Roboto", HINT_SIZE, 700, TRUE);
  self->reject_text = new_layout (self, "Neo Roboto", HINT_SIZE, 700, TRUE);
  pango_layout_set_text (self->answer_text, _("Swipe up to answer"), -1);
  pango_layout_set_text (self->reject_text, _("Swipe down to reject"), -1);
  self->name_size = NAME_SIZE;
  self->touch_enabled = TRUE;
  self->grid_alpha = self->grid_scale = self->entry = self->entry_alpha = 1;
  self->text_alpha = self->puck_alpha = self->icon_alpha = 1;
  self->puck_sx = self->puck_sy = self->bg_scale = 1;
  self->icon_tint = rgb (CALL_GREEN, 1);
  self->side_icon_alpha = SIDE_ALPHA;
  self->side_icon_scale = 1;

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 0);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

GtkWidget *
answer_view_new (void)
{
  return g_object_new (ANSWER_TYPE_VIEW, NULL);
}
