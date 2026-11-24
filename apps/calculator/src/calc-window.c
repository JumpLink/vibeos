/* calc-window: see calc-window.h.
 *
 * CalcMain is the window's content: the history frame, the display, the advanced pad and the input pad, placed by
 * the transition's progress between two layouts:
 *  - start: the display at the top (30 % of the height; 40 % in landscape), the pads under it, the history frame
 *    0 high above the window;
 *  - end: the pads' bottom on the bottom of the animation helper (70 % of a height below the window; 60 %), the
 *    display on top of them, the history frame filling the window above the display.
 * Every edge goes from its start to its end place in step with the progress, rounded to the device pixel. The
 * progress follows a drag down from the display (anchored at the display's bottom: one display travel is the whole
 * transition), and after the finger lifts runs on to 0 or 1 under a speed and acceleration limit (StopLogic); the
 * menu's History and Back run it over the transition's 1000 ms, accelerate-decelerate. The history is created when
 * the transition starts and destroyed when it is back at the start.
 *
 * The rest: the display's states (INPUT, EVALUATE, INIT, INIT_FOR_RESULT, RESULT, ERROR), the buttons, the keys,
 * pasting, the result taking the formula's place, the menus and dialogs.
 *
 * Back at the app's root leaves it running (the shell minimizes the window), so the display is as it was when the
 * app comes back; a new start has an empty display and the history, memory and angle mode of before. A rotation
 * builds the layout again from the saved state.
 *
 * Left out: haptics (no vibrator API here), accessibility announcements ("cleared", the result read out), and a
 * system clipboard overlay after a copy (the app's own "Text copied" toast is here). */
#include "calc-window.h"
#include "calc-button.h"
#include "calc-display.h"
#include "calc-history.h"
#include "calc-licenses.h"
#include "calc-pad.h"
#include "calc-popup-menu.h"
#include "calc-text-toolbar.h"
#include "calc-toast.h"
#include "evaluator.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>
#include <vibeos.h>

#define DURATION          1.0       /* s: the transition's duration */
#define MAX_ACCELERATION  1.2       /* progress / s²: the settle after a drag */
#define MAX_VELOCITY      4.0       /* progress / s */
#define TOUCH_SLOP        8         /* px: the move that starts a drag */
#define ERROR_COLOR       VIBEOS_ERROR      /* the formula and the result after an error */
#define N_INVERTIBLE      6

typedef enum {
  STATE_NONE = -1,        /* before the first set_state */
  STATE_INPUT,
  STATE_EVALUATE,
  STATE_INIT,
  STATE_INIT_FOR_RESULT,
  STATE_RESULT,
  STATE_ERROR,
} CalcState;

/* the buttons that are not keys of the expression (the keys are their CalcKey) */
enum {
  BTN_EQ = 1000,
  BTN_DEL,
  BTN_CLR,
  BTN_TOGGLE_INV,
  BTN_TOGGLE_MODE,
  BTN_PAREN,
};

typedef enum {
  MOTION_NONE,
  MOTION_TIMED,           /* to the end or the start, over DURATION */
  MOTION_STOP_LOGIC,      /* after a drag */
} MotionKind;

/* the settle after a drag: from where the finger left it, at its speed, to 0 or 1 with at most MAX_ACCELERATION
 * and MAX_VELOCITY, in up to three stages of constant acceleration */
typedef struct {
  double   start;
  gboolean backwards;
  int      stages;
  double   v1, v2, v3;    /* the speed at the start of each stage */
  double   t1, t2, t3;    /* their durations, s */
  double   e1, e2, e3;    /* the distance done at the end of each */
} StopLogic;

/* what a rebuild keeps */
typedef struct {
  CalcState state;
  char     *unprocessed;
  GBytes   *eval_state;
  gboolean  inverse;
  gboolean  history;      /* the history was open (or opening) */
} SavedState;

#define CALC_TYPE_MAIN (calc_main_get_type ())
G_DECLARE_FINAL_TYPE (CalcMain, calc_main, CALC, MAIN, GtkWidget)

struct _CalcMain {
  GtkWidget          parent;
  Evaluator         *evaluator;
  gboolean           one_line;          /* landscape: the one-line display and pads */
  gboolean           tablet;            /* a tablet: the same layouts with a tablet's sizes */
  VibeosLayout      *layout;            /* the window's, while the view is in it */
  /* the owner of the listener the evaluator calls back: a new one for each layout, so that what was asked for
   * an old layout does not reach the new one */
  GObject           *generation;
  EvaluationListener listener;

  GtkWidget         *display;
  GtkWidget         *advanced;
  GtkWidget         *input;
  GtkWidget         *history;
  CalcFormula       *formula;
  CalcResult        *result;
  GtkWidget         *inv_toggle;
  GtkWidget         *mode_toggle;
  GtkWidget         *invertible[N_INVERTIBLE];
  GtkWidget         *inverse[N_INVERTIBLE];

  CalcState          state;
  char              *unprocessed;       /* typed characters that are no key, shown in red */
  gboolean           inverse_mode;

  /* the transition */
  double             progress;
  double             travel;            /* px the display moves from start to end */
  graphene_rect_t    display_box;       /* where the display is now, px */
  double             history_height;    /* px */
  MotionKind         motion;
  guint              tick;
  gint64             motion_start;
  double             linear_from;
  double             goal;
  StopLogic          stop;
  /* the drag */
  gboolean           drag_inside;
  gboolean           dragging;
  double             drag_x, drag_y;
  double             drag_progress;
  SpongyVelocity     velocity;

  /* over it */
  CalcTextToolbar   *toolbar;           /* weak */
  CalcResult        *toolbar_result;    /* weak: the result the toolbar is about; NULL: the formula's */
  guint              paste_serial;
  char              *paste_text;
  char              *paste_tag;
  GtkWidget         *licenses;
  guint              rebuild_idle;
  guint              key_down;          /* the key whose release is the app's */
};

G_DEFINE_FINAL_TYPE (CalcMain, calc_main, GTK_TYPE_WIDGET)

static void on_button_click (CalcMain *self, int id);
static void add_chars (CalcMain *self, const char *more, gboolean explicit);
static void set_state (CalcMain *self, CalcState state);
static gboolean on_back_pressed (CalcMain *self);

/* ------------------------------------------------------------------------------------------------------------ */
/* little things */

static CalcExpr *
main_expr (CalcMain *self)
{
  return evaluator_get_expr (self->evaluator, EVALUATOR_MAIN_INDEX);
}

static gboolean
have_unprocessed (CalcMain *self)
{
  return self->unprocessed && *self->unprocessed;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* dialogs: an optional title, the message, Dismiss and maybe one more */

typedef void (*DialogAction) (CalcMain *self);

static void
on_dialog_response (VibeosDialog *dialog, int response, CalcMain *self)
{
  DialogAction action = g_object_get_data (G_OBJECT (dialog), "calc-action");

  if (response == VIBEOS_RESPONSE_OK && action)
    action (self);
}

static void
message_dialog (CalcMain *self, const char *title, const char *message, const char *positive, DialogAction action)
{
  VibeosDialog *dialog = vibeos_dialog_new (title && *title ? title : NULL);
  GtkWidget *label = gtk_label_new (message);

  /* the message is the dialog's content with its own padding, the content area itself having none
   * (calculator.css). It is not selectable: a selectable GtkLabel selects all of itself when the dialog gives it
   * the focus. */
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "calc-dialog-message");
  if (!(title && *title))
    gtk_widget_add_css_class (label, "untitled");
  vibeos_dialog_set_content (dialog, label);
  vibeos_dialog_add_button (dialog, _("Dismiss"), VIBEOS_RESPONSE_CANCEL);
  if (positive)
    vibeos_dialog_add_button (dialog, positive, VIBEOS_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "calc-action", action);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_dialog_response), self, 0);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the floating toolbar over the formula or a result */

static gboolean
stop_action_mode (CalcMain *self)
{
  if (!self->toolbar)
    return FALSE;
  calc_text_toolbar_dismiss (self->toolbar);   /* "closed" clears what it was about */
  return TRUE;
}

static void
on_toolbar_closed (CalcTextToolbar *toolbar, CalcMain *self)
{
  if (toolbar != self->toolbar)
    return;
  /* the result it was about loses its highlight */
  if (self->toolbar_result) {
    calc_result_set_highlight (self->toolbar_result, FALSE);
    g_object_remove_weak_pointer (G_OBJECT (self->toolbar_result), (gpointer *) &self->toolbar_result);
    self->toolbar_result = NULL;
  }
  g_object_remove_weak_pointer (G_OBJECT (self->toolbar), (gpointer *) &self->toolbar);
  self->toolbar = NULL;
}

static CalcTextToolbar *
new_toolbar (CalcMain *self, CalcResult *about)
{
  CalcTextToolbar *toolbar = calc_text_toolbar_new ();

  stop_action_mode (self);
  self->toolbar = toolbar;
  g_object_add_weak_pointer (G_OBJECT (toolbar), (gpointer *) &self->toolbar);
  if (about) {
    self->toolbar_result = about;
    g_object_add_weak_pointer (G_OBJECT (about), (gpointer *) &self->toolbar_result);
  }
  g_signal_connect_object (toolbar, "closed", G_CALLBACK (on_toolbar_closed), self, 0);
  return toolbar;
}

/* the result toolbar's memory buttons */
static gboolean
action_memory_store (gpointer data)
{
  CalcMain *self = data;

  if (self->toolbar_result)
    calc_result_memory_store (self->toolbar_result);
  return TRUE;
}

static gboolean
action_memory_add (gpointer data)
{
  CalcMain *self = data;

  if (self->toolbar_result)
    calc_result_memory_add (self->toolbar_result);
  return TRUE;
}

static gboolean
action_memory_subtract (gpointer data)
{
  CalcMain *self = data;

  if (self->toolbar_result)
    calc_result_memory_subtract (self->toolbar_result);
  return TRUE;
}

static gboolean
action_copy (gpointer data)
{
  CalcMain *self = data;

  /* refused while the digits are still being computed: the toolbar stays */
  if (!self->toolbar_result || !calc_result_copy (self->toolbar_result))
    return FALSE;
  calc_result_set_highlight (self->toolbar_result, FALSE);
  calc_toast_show (GTK_WIDGET (self), _("Text copied"));
  return TRUE;
}

/* a long press on a valid result, the display's or a history row's: MS, M+ and M− (with something in memory),
 * Copy, the result highlighted while the toolbar is up */
static void
show_result_toolbar (CalcMain *self, CalcResult *result)
{
  CalcTextToolbar *toolbar = new_toolbar (self, result);
  graphene_rect_t bounds;

  calc_text_toolbar_append (toolbar, "MS", action_memory_store, self);
  if (evaluator_get_memory_index (self->evaluator) != 0) {
    calc_text_toolbar_append (toolbar, "M+", action_memory_add, self);
    calc_text_toolbar_append (toolbar, "M−", action_memory_subtract, self);
  }
  calc_text_toolbar_append (toolbar, _("Copy"), action_copy, self);
  calc_result_set_highlight (result, TRUE);
  calc_result_get_text_bounds (result, &bounds);
  calc_text_toolbar_present (toolbar, GTK_WIDGET (result), &bounds);
}

static void
on_result_long_pressed (CalcResult *result, CalcMain *self)
{
  show_result_toolbar (self, result);
}

static void
on_history_result_long_pressed (CalcHistory *history, CalcResult *result, CalcMain *self)
{
  show_result_toolbar (self, result);
}

static void
redisplay_after_formula_change (CalcMain *self);

static void
clear_if_not_input_state (CalcMain *self)
{
  if (self->state == STATE_ERROR || self->state == STATE_RESULT) {
    set_state (self, STATE_INPUT);
    evaluator_clear_main (self->evaluator);
  }
}

/* the formula toolbar's MR */
static gboolean
action_memory_recall (gpointer data)
{
  CalcMain *self = data;
  long memory = evaluator_get_memory_index (self->evaluator);

  clear_if_not_input_state (self);
  if (memory != 0) {
    evaluator_append_expr (self->evaluator, memory);
    redisplay_after_formula_change (self);
  }
  return TRUE;
}

/* Paste: a result copied from here (its tag is the last one saved) comes back as itself, with all its digits;
 * anything else as typed characters */
static gboolean
action_paste (gpointer data)
{
  CalcMain *self = data;

  if (self->paste_tag && evaluator_is_last_saved (self->evaluator, self->paste_tag)) {
    clear_if_not_input_state (self);
    evaluator_append_expr (self->evaluator, evaluator_get_saved_index (self->evaluator));
    redisplay_after_formula_change (self);
  } else if (self->paste_text) {
    add_chars (self, self->paste_text, FALSE);
  }
  return TRUE;
}

/* the formula's toolbar: MR with something in memory, Paste with text on the clipboard; none with neither */
static void
show_formula_toolbar (CalcMain *self)
{
  gboolean memory = evaluator_get_memory_index (self->evaluator) != 0;
  gboolean paste = self->paste_text && *self->paste_text;
  CalcTextToolbar *toolbar;
  double w, h, top, bottom, end, right;
  graphene_rect_t rect;

  if (!memory && !paste)
    return;
  toolbar = new_toolbar (self, NULL);
  if (memory)
    calc_text_toolbar_append (toolbar, "MR", action_memory_recall, self);
  if (paste)
    calc_text_toolbar_append (toolbar, _("Paste"), action_paste, self);
  /* the formula scrolls to its end */
  calc_formula_scroll_to_end (self->formula);
  /* the toolbar points at the formula inside its padding, over its rightmost 10 % */
  w = gtk_widget_get_width (GTK_WIDGET (self->formula));
  h = gtk_widget_get_height (GTK_WIDGET (self->formula));
  calc_display_get_formula_padding (CALC_DISPLAY (self->display), &top, &bottom, &end);
  right = w - end;
  graphene_rect_init (&rect, right * 0.9, top, right * 0.1, MAX (0, h - top - bottom));
  calc_text_toolbar_present (toolbar, GTK_WIDGET (self->formula), &rect);
}

/* the clipboard is read before the toolbar shows: whether there is text to paste decides whether Paste is on it
 * and the tag of a result copied from here comes with it */
typedef struct {
  CalcMain *self;
  guint     serial;
} PasteRead;

static void
paste_read_free (PasteRead *read)
{
  g_object_unref (read->self);
  g_free (read);
}

static gboolean
paste_read_current (PasteRead *read)
{
  return read->serial == read->self->paste_serial && read->self->formula;
}

static void
on_tag_bytes (GObject *source, GAsyncResult *result, gpointer data)
{
  PasteRead *read = data;
  g_autoptr (GBytes) bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, NULL);

  if (paste_read_current (read)) {
    if (bytes && g_bytes_get_size (bytes) > 0)
      read->self->paste_tag = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
    show_formula_toolbar (read->self);
  }
  paste_read_free (read);
}

static void
on_tag_stream (GObject *source, GAsyncResult *result, gpointer data)
{
  PasteRead *read = data;
  g_autoptr (GInputStream) stream = gdk_clipboard_read_finish (GDK_CLIPBOARD (source), result, NULL, NULL);

  if (!stream || !paste_read_current (read)) {
    if (paste_read_current (read))
      show_formula_toolbar (read->self);
    paste_read_free (read);
    return;
  }
  g_input_stream_read_bytes_async (stream, 256, G_PRIORITY_DEFAULT, NULL, on_tag_bytes, read);
}

static void
on_paste_text (GObject *source, GAsyncResult *result, gpointer data)
{
  PasteRead *read = data;
  GdkClipboard *clipboard = GDK_CLIPBOARD (source);
  char *text = gdk_clipboard_read_text_finish (clipboard, result, NULL);

  if (!paste_read_current (read)) {
    g_free (text);
    paste_read_free (read);
    return;
  }
  read->self->paste_text = text;
  if (gdk_content_formats_contain_mime_type (gdk_clipboard_get_formats (clipboard), CALC_CLIPBOARD_TAG_MIME)) {
    static const char *mime_types[] = { CALC_CLIPBOARD_TAG_MIME, NULL };

    gdk_clipboard_read_async (clipboard, mime_types, G_PRIORITY_DEFAULT, NULL, on_tag_stream, read);
    return;
  }
  show_formula_toolbar (read->self);
  paste_read_free (read);
}

static void
on_formula_long_pressed (CalcFormula *formula, CalcMain *self)
{
  GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
  GdkContentFormats *formats = gdk_clipboard_get_formats (clipboard);
  PasteRead *read;

  self->paste_serial++;
  g_clear_pointer (&self->paste_text, g_free);
  g_clear_pointer (&self->paste_tag, g_free);
  if (!gdk_content_formats_contain_gtype (formats, G_TYPE_STRING)) {
    show_formula_toolbar (self);
    return;
  }
  read = g_new0 (PasteRead, 1);
  read->self = g_object_ref (self);
  read->serial = self->paste_serial;
  gdk_clipboard_read_text_async (clipboard, NULL, on_paste_text, read);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the display's states */

static void
redisplay_formula (CalcMain *self)
{
  g_autofree char *text = calc_expr_to_string (main_expr (self));

  /* the characters that could not be taken in, after it in red */
  calc_formula_set_text (self->formula, text, self->unprocessed);
}

static void
restore_display_positions (CalcMain *self)
{
  calc_result_clear (self->result);
  calc_display_restore_positions (CALC_DISPLAY (self->display));
}

/* the colours, and the one-line display's visibilities */
static void
set_state (CalcMain *self, CalcState state)
{
  GdkRGBA formula_color = vibeos_color (VIBEOS_ON_SURFACE), result_color = vibeos_color (VIBEOS_TERTIARY);
  GdkRGBA error = vibeos_color (ERROR_COLOR);

  if (self->state == state)
    return;
  if (state == STATE_INPUT) {
    /* evaluations are asked for explicitly from now on */
    calc_result_set_should_evaluate (self->result, CALC_RESULT_SHOULD_NOT_EVALUATE, NULL);
    restore_display_positions (self);
  }
  self->state = state;
  if (self->one_line) {
    if (state == STATE_RESULT || state == STATE_EVALUATE)
      calc_display_set_visible (CALC_DISPLAY (self->display), TRUE, TRUE);
    else if (state == STATE_ERROR)
      calc_display_set_visible (CALC_DISPLAY (self->display), FALSE, TRUE);
    else
      calc_display_set_visible (CALC_DISPLAY (self->display), TRUE, FALSE);
  }
  if (state == STATE_ERROR) {
    calc_formula_set_color (self->formula, &error);
    calc_result_set_color (self->result, &error);
  } else if (state != STATE_RESULT) {
    /* the normal colours */
    calc_formula_set_color (self->formula, &formula_color);
    calc_result_set_color (self->result, &result_color);
  }
}

static CalcState
map_from_saved (CalcState saved)
{
  switch (saved) {
  case STATE_RESULT:
  case STATE_INIT_FOR_RESULT:
    return STATE_INIT_FOR_RESULT;   /* the evaluation is expected to end normally */
  case STATE_ERROR:
  case STATE_INIT:
    return STATE_INIT;
  default:
    return saved;
  }
}

static void
on_inverse_toggled (CalcMain *self, gboolean show_inverse)
{
  self->inverse_mode = show_inverse;
  for (int i = 0; i < N_INVERTIBLE; i++) {
    gtk_widget_set_visible (self->invertible[i], !show_inverse);
    gtk_widget_set_visible (self->inverse[i], show_inverse);
  }
}

/* the toolbar shows the mode, the toggle the other one */
static void
on_mode_changed (CalcMain *self, gboolean degree_mode)
{
  calc_display_set_mode (CALC_DISPLAY (self->display), degree_mode ? "DEG" : "RAD");
  calc_button_set_label (CALC_BUTTON (self->mode_toggle), degree_mode ? "RAD" : "DEG");
}

/* from RESULT, an operator goes on after the result, anything else starts again */
static void
switch_to_input (CalcMain *self, CalcKey key)
{
  if (calc_key_is_binary (key) || calc_key_is_suffix (key))
    evaluator_collapse (self->evaluator, evaluator_get_max_index (self->evaluator));
  else
    evaluator_clear_main (self->evaluator);
  set_state (self, STATE_INPUT);
}

static void
add_key_to_expr (CalcMain *self, CalcKey key)
{
  if (self->state == STATE_ERROR)
    set_state (self, STATE_INPUT);
  else if (self->state == STATE_RESULT)
    switch_to_input (self, key);
  evaluator_append (self->evaluator, key);
}

/* a key typed or tapped: a minus replaces the + or − before it */
static void
add_explicit_key_to_expr (CalcMain *self, CalcKey key)
{
  if (self->state == STATE_INPUT && key == CALC_KEY_OP_SUB)
    calc_expr_remove_trailing_additive_operators (main_expr (self));
  add_key_to_expr (self, key);
}

static void
evaluate_instant_if_necessary (CalcMain *self)
{
  if (self->state == STATE_INPUT && calc_expr_has_interesting_ops (main_expr (self)))
    evaluator_evaluate_and_notify (self->evaluator, EVALUATOR_MAIN_INDEX, &self->listener, calc_result_char_metrics,
                                   self->result);
}

static void
redisplay_after_formula_change (CalcMain *self)
{
  redisplay_formula (self);
  set_state (self, STATE_INPUT);
  calc_result_clear (self->result);
  if (have_unprocessed (self))
    evaluator_touch (self->evaluator);   /* evaluated again once the characters are deleted */
  else
    evaluate_instant_if_necessary (self);
}

static gboolean
cancel_if_evaluating (CalcMain *self, gboolean quiet)
{
  if (self->state != STATE_EVALUATE)
    return FALSE;
  evaluator_cancel (self->evaluator, EVALUATOR_MAIN_INDEX, quiet);
  return TRUE;
}

static void
cancel_unrequested (CalcMain *self)
{
  if (self->state == STATE_INPUT)
    evaluator_cancel (self->evaluator, EVALUATOR_MAIN_INDEX, TRUE);
}

static void
on_error (CalcMain *self, long index, CalcFailure failure)
{
  if (self->state == STATE_EVALUATE || self->state == STATE_INIT || self->state == STATE_INIT_FOR_RESULT) {
    set_state (self, STATE_ERROR);
    calc_result_on_error (self->result, index, failure);
  } else {
    calc_result_clear (self->result);
  }
}

static void
on_equals (CalcMain *self)
{
  if (self->state != STATE_INPUT)
    return;
  if (have_unprocessed (self)) {
    set_state (self, STATE_EVALUATE);
    on_error (self, EVALUATOR_MAIN_INDEX, CALC_SYNTAX_ERROR);
  } else if (calc_expr_has_interesting_ops (main_expr (self))) {
    set_state (self, STATE_EVALUATE);
    evaluator_require_result (self->evaluator, EVALUATOR_MAIN_INDEX, &self->listener, calc_result_char_metrics,
                              self->result);
  }
}

static void
on_delete (CalcMain *self)
{
  /* an evaluation asked for is only cancelled */
  if (cancel_if_evaluating (self, FALSE))
    return;
  set_state (self, STATE_INPUT);
  if (have_unprocessed (self))
    *g_utf8_find_prev_char (self->unprocessed, self->unprocessed + strlen (self->unprocessed)) = '\0';
  else
    evaluator_delete (self->evaluator);
  redisplay_after_formula_change (self);
}

static void
on_clear_end (CalcMain *self)
{
  g_clear_pointer (&self->unprocessed, g_free);
  calc_result_clear (self->result);
  evaluator_clear_main (self->evaluator);
  set_state (self, STATE_INPUT);
  redisplay_formula (self);
}

static void
on_clear (CalcMain *self)
{
  if (calc_expr_is_empty (main_expr (self)) && !have_unprocessed (self))
    return;
  cancel_if_evaluating (self, TRUE);
  on_clear_end (self);
}

/* the result takes the formula's place, at the size the formula would show it (its smallest for one
 * that scrolls), in the formula's colour, and goes to the history */
static void
on_result (CalcMain *self, gboolean preserved)
{
  double size = calc_formula_get_min_text_size (self->formula);
  GdkRGBA color = calc_formula_get_color (self->formula);

  if (!calc_result_is_scrollable (self->result))
    size = calc_formula_get_variable_text_size (self->formula, calc_result_get_text (self->result));
  if (preserved)
    evaluator_represerve (self->evaluator);
  else
    evaluator_preserve (self->evaluator, EVALUATOR_MAIN_INDEX, TRUE);
  calc_display_show_result (CALC_DISPLAY (self->display), size / calc_result_get_text_size (self->result));
  calc_result_set_color (self->result, &color);
  set_state (self, STATE_RESULT);
}

/* characters typed or pasted, as the keys they stand for. A pasted number may carry an exponent
 * ("1.5E-7"), and does not run on into a number before it (a × goes between). Characters that are no key stay
 * at the end, unprocessed, in red, until deleted. */
static void
add_chars (CalcMain *self, const char *more, gboolean explicit)
{
  g_autofree char *all = g_strconcat (self->unprocessed ? self->unprocessed : "", more, NULL);
  g_autofree gunichar *s = NULL;
  glong len = 0;
  int current = 0;
  gboolean last_was_digit = FALSE;
  gunichar grouping = g_utf8_get_char (calc_grouping_separator ());

  s = g_utf8_to_ucs4_fast (all, -1, &len);
  if (self->state == STATE_RESULT && len != 0)
    /* the display is cleared at once for a function name still being typed */
    switch_to_input (self, calc_key_for_char (s[0]));
  while (current < len) {
    gunichar c = s[current];
    GUnicodeType type = g_unichar_type (c);
    CalcKey k, f;

    if (type == G_UNICODE_SPACE_SEPARATOR || type == G_UNICODE_LINE_SEPARATOR
        || type == G_UNICODE_PARAGRAPH_SEPARATOR || c == grouping) {
      current++;
      continue;
    }
    k = calc_key_for_char (c);
    if (!explicit) {
      int exp_end;

      if (last_was_digit && current != (exp_end = calc_exponent_end (s, len, current))) {
        /* scientific notation with an E, when pasting */
        evaluator_add_exponent (self->evaluator, s, current, exp_end);
        current = exp_end;
        last_was_digit = FALSE;
        continue;
      } else {
        gboolean is_digit = k != CALC_KEY_NONE && calc_key_digit_value (k) != CALC_NOT_DIGIT;

        if (current == 0 && (is_digit || k == CALC_KEY_DEC_POINT) && calc_expr_has_trailing_constant (main_expr (self)))
          add_key_to_expr (self, CALC_KEY_OP_MUL);
        last_was_digit = is_digit || (last_was_digit && k == CALC_KEY_DEC_POINT);
      }
    }
    if (k != CALC_KEY_NONE) {
      if (explicit)
        add_explicit_key_to_expr (self, k);
      else
        add_key_to_expr (self, k);
      current++;
      continue;
    }
    {
      g_autofree char *rest = g_ucs4_to_utf8 (s + current, len - current, NULL, NULL, NULL);

      f = rest ? calc_key_for_function_name (rest) : CALC_KEY_NONE;
    }
    if (f != CALC_KEY_NONE) {
      int paren = current;

      if (explicit)
        add_explicit_key_to_expr (self, f);
      else
        add_key_to_expr (self, f);
      if (f == CALC_KEY_OP_SQRT)
        add_key_to_expr (self, CALC_KEY_LPAREN);   /* sqrt( keeps its parenthesis */
      while (paren < len && s[paren] != '(')
        paren++;
      current = paren + 1;
      continue;
    }
    /* what is left cannot be turned into keys */
    g_free (self->unprocessed);
    self->unprocessed = g_ucs4_to_utf8 (s + current, len - current, NULL, NULL, NULL);
    redisplay_after_formula_change (self);
    return;
  }
  g_clear_pointer (&self->unprocessed, g_free);
  redisplay_after_formula_change (self);
}

static void
on_button_click (CalcMain *self, int id)
{
  CalcExpr *expr;

  stop_action_mode (self);
  cancel_unrequested (self);
  switch (id) {
  case BTN_EQ:
    on_equals (self);
    return;
  case BTN_DEL:
    on_delete (self);
    return;
  case BTN_CLR:
    on_clear (self);
    return;
  case BTN_TOGGLE_INV:
    on_inverse_toggled (self, !self->inverse_mode);
    if (self->state == STATE_RESULT)
      calc_result_redisplay (self->result);   /* in case a reevaluation was cancelled */
    return;
  case BTN_TOGGLE_MODE: {
    gboolean mode = !evaluator_get_degree_mode (self->evaluator, EVALUATOR_MAIN_INDEX);

    cancel_if_evaluating (self, FALSE);
    if (self->state == STATE_RESULT && calc_expr_has_trig_funcs (main_expr (self))) {
      /* the result as computed in the old mode */
      evaluator_collapse (self->evaluator, evaluator_get_max_index (self->evaluator));
      redisplay_formula (self);
    }
    /* in input, the trigonometric functions typed are read in the new mode */
    evaluator_set_degree_mode (self->evaluator, mode);
    on_mode_changed (self, mode);
    set_state (self, STATE_INPUT);
    calc_result_clear (self->result);
    if (!have_unprocessed (self))
      evaluate_instant_if_necessary (self);
    return;
  }
  case BTN_PAREN:
    /* a ")" after something that can end a parenthesis with one open; a "(" otherwise */
    expr = main_expr (self);
    if (!calc_expr_has_trailing_left_paren (expr) && calc_expr_has_open_parentheses (expr)
        && (calc_expr_has_trailing_right_paren (expr) || calc_expr_has_trailing_constant (expr)
            || calc_expr_has_trailing_suffix (expr)))
      add_explicit_key_to_expr (self, CALC_KEY_RPAREN);
    else
      add_explicit_key_to_expr (self, CALC_KEY_LPAREN);
    redisplay_after_formula_change (self);
    return;
  default:
    cancel_if_evaluating (self, FALSE);
    if (have_unprocessed (self)) {
      /* after unprocessed characters, a key is one more character */
      add_chars (self, calc_key_to_string ((CalcKey) id), TRUE);
    } else {
      add_explicit_key_to_expr (self, (CalcKey) id);
      redisplay_after_formula_change (self);
    }
    return;
  }
}

static void
on_text_size_changed (CalcFormula *formula, double old_size, CalcMain *self)
{
  /* only what the user types animates */
  if (self->state == STATE_INPUT)
    calc_formula_animate_size_change (formula, old_size);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the main result's listener: what the evaluator reports for EVALUATOR_MAIN_INDEX */

static void
listener_on_evaluate (gpointer data, long index, int init_prec_offset, int msd_index, int lsd_offset,
                      const char *truncated_whole_part)
{
  CalcMain *self = data;

  calc_result_on_evaluate (self->result, index, init_prec_offset, msd_index, lsd_offset, truncated_whole_part);
  if (self->state != STATE_INPUT)
    on_result (self, self->state == STATE_INIT_FOR_RESULT || self->state == STATE_RESULT);
}

static void
listener_on_cancelled (gpointer data, long index)
{
  CalcMain *self = data;

  set_state (self, STATE_INPUT);
  calc_result_on_cancelled (self->result, index);
}

static void
listener_on_error (gpointer data, long index, CalcFailure failure)
{
  on_error (data, index, failure);
}

static void
listener_on_reevaluate (gpointer data, long index)
{
  CalcMain *self = data;

  calc_result_on_reevaluate (self->result, index);
}

static const EvaluationListenerFuncs listener_funcs = {
  .on_cancelled = listener_on_cancelled,
  .on_error = listener_on_error,
  .on_evaluate = listener_on_evaluate,
  .on_reevaluate = listener_on_reevaluate,
};

/* Evaluator.Callback */

static void
callback_memory_state_changed (gpointer data)
{
  /* the formula's toolbar asks for the memory when it opens: nothing to update */
}

static void
callback_show_cancelled (gpointer data)
{
  message_dialog (data, NULL, _("Computation cancelled."), NULL, NULL);
}

static void
use_long_timeout (CalcMain *self)
{
  evaluator_set_long_timeout (self->evaluator);
}

static void
callback_show_timeout (gpointer data, gboolean long_timeout)
{
  message_dialog (data, _("Timeout"), _("Value may be infinite or undefined."),
                  long_timeout ? NULL : _("Use longer timeouts"), use_long_timeout);
}

static const EvaluatorCallback evaluator_callback = {
  .memory_state_changed = callback_memory_state_changed,
  .show_cancelled = callback_show_cancelled,
  .show_timeout = callback_show_timeout,
};

/* ------------------------------------------------------------------------------------------------------------ */
/* the transition */

static void
stop_logic_setup (StopLogic *s, double velocity, double distance, double max_acc, double max_vel, double max_time)
{
  double min_time_to_stop, stop_distance;

  if (velocity == 0)
    velocity = 0.0001;
  s->v1 = velocity;
  s->v2 = s->v3 = 0;
  s->t1 = s->t2 = s->t3 = 0;
  s->e1 = s->e2 = s->e3 = distance;
  min_time_to_stop = velocity / max_acc;
  stop_distance = min_time_to_stop * velocity / 2;

  if (velocity < 0) {
    /* moving away: stop, then come back */
    double time_to_zero = -velocity / max_acc;
    double reverse_distance = time_to_zero * velocity / 2;
    double total = distance - reverse_distance;
    double peak = sqrt (max_acc * total);

    if (peak < max_vel) {   /* backward accelerate, decelerate */
      s->stages = 2;
      s->v2 = peak;
      s->t1 = (peak - velocity) / max_acc;
      s->t2 = peak / max_acc;
      s->e1 = (velocity + peak) * s->t1 / 2;
      return;
    }
    /* backward accelerate cruise decelerate */
    s->stages = 3;
    s->v2 = s->v3 = max_vel;
    s->t1 = (max_vel - velocity) / max_acc;
    s->t3 = max_vel / max_acc;
    s->e1 = (velocity + max_vel) * s->t1 / 2;
    s->e2 = distance - max_vel * s->t3 / 2;
    s->t2 = (s->e2 - s->e1) / max_vel;
    return;
  }
  if (stop_distance >= distance) {   /* hard stop: it cannot brake in time */
    s->stages = 1;
    s->t1 = 2 * distance / velocity;
    return;
  }
  {
    double after = distance - stop_distance;
    double cruise = after / velocity;
    double peak;

    if (cruise + min_time_to_stop < max_time) {   /* cruise decelerate */
      s->stages = 2;
      s->v2 = velocity;
      s->e1 = after;
      s->t1 = cruise;
      s->t2 = velocity / max_acc;
      return;
    }
    peak = sqrt (max_acc * distance + velocity * velocity / 2);
    if (peak < max_vel) {   /* accelerate decelerate */
      s->stages = 2;
      s->v2 = peak;
      s->t1 = (peak - velocity) / max_acc;
      s->t2 = peak / max_acc;
      s->e1 = (velocity + peak) * s->t1 / 2;
      return;
    }
    /* accelerate cruise decelerate */
    s->stages = 3;
    s->v2 = s->v3 = max_vel;
    s->t1 = (max_vel - velocity) / max_acc;
    s->t3 = max_vel / max_acc;
    s->e1 = (velocity + max_vel) * s->t1 / 2;
    s->e2 = distance - max_vel * s->t3 / 2;
    s->t2 = (s->e2 - s->e1) / max_vel;
  }
}

static void
stop_logic_config (StopLogic *s, double current, double destination, double velocity)
{
  s->start = current;
  s->backwards = current > destination;
  if (s->backwards)
    stop_logic_setup (s, -velocity, current - destination, MAX_ACCELERATION, MAX_VELOCITY, DURATION);
  else
    stop_logic_setup (s, velocity, destination - current, MAX_ACCELERATION, MAX_VELOCITY, DURATION);
}

static double
stop_logic_duration (const StopLogic *s)
{
  return s->t1 + (s->stages > 1 ? s->t2 : 0) + (s->stages > 2 ? s->t3 : 0);
}

/* the distance done after t seconds */
static double
stop_logic_distance (const StopLogic *s, double t)
{
  if (t <= s->t1)
    return s->t1 > 0 ? s->v1 * t + (s->v2 - s->v1) * t * t / (2 * s->t1) : 0;
  if (s->stages == 1)
    return s->e1;
  t -= s->t1;
  if (t < s->t2)
    return s->e1 + s->v2 * t + (s->v3 - s->v2) * t * t / (2 * s->t2);
  if (s->stages == 2)
    return s->e2;
  t -= s->t2;
  if (t <= s->t3)
    return s->e2 + s->v3 * t - s->v3 * t * t / (2 * s->t3);
  return s->e3;
}

static double
stop_logic_position (const StopLogic *s, double t)
{
  double y = stop_logic_distance (s, t);

  return s->backwards ? s->start - y : s->start + y;
}

/* before the history opens: what is being computed for "=" is cancelled; while a restored result is being computed
 * again (INIT) the history does not open */
static gboolean
prepare_for_history (CalcMain *self)
{
  if (self->state == STATE_EVALUATE) {
    cancel_if_evaluating (self, TRUE);
    set_state (self, STATE_INPUT);
    return TRUE;
  }
  return self->state != STATE_INIT;
}

static void
on_history_back (CalcHistory *history, CalcMain *self)
{
  on_back_pressed (self);
}

static void
clear_history (CalcMain *self)
{
  evaluator_clear_everything (self->evaluator);
  on_clear_end (self);
  on_back_pressed (self);
}

static void
menu_clear (gpointer data)
{
  /* the history's Clear: the message, no title */
  message_dialog (data, NULL, _("Clear history and memory?"), _("Clear"), clear_history);
}

static void
on_history_menu (CalcHistory *history, CalcMain *self)
{
  CalcPopupMenu *menu = calc_popup_menu_new ();

  calc_popup_menu_append (menu, _("Clear"), menu_clear, self);
  calc_popup_menu_present (menu, calc_history_get_overflow (history));
}

/* the history is made when the transition starts */
static void
show_history (CalcMain *self)
{
  if (self->history || !prepare_for_history (self))
    return;
  stop_action_mode (self);
  self->history = calc_history_new (self->evaluator);
  g_signal_connect (self->history, "back", G_CALLBACK (on_history_back), self);
  g_signal_connect (self->history, "menu", G_CALLBACK (on_history_menu), self);
  g_signal_connect (self->history, "result-long-pressed", G_CALLBACK (on_history_result_long_pressed), self);
  /* under the display and the pads */
  gtk_widget_insert_after (self->history, GTK_WIDGET (self), NULL);
}

/* and dropped when it is back at the start */
static void
remove_history (CalcMain *self)
{
  if (!self->history)
    return;
  if (self->toolbar_result && gtk_widget_is_ancestor (GTK_WIDGET (self->toolbar_result), self->history))
    stop_action_mode (self);
  g_clear_pointer (&self->history, gtk_widget_unparent);
}

static void
set_progress (CalcMain *self, double progress)
{
  progress = CLAMP (progress, 0, 1);
  if (progress == self->progress)
    return;
  /* the transition starts */
  if (progress > 0 && !self->history)
    show_history (self);
  self->progress = progress;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
stop_motion (CalcMain *self)
{
  if (self->tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
  self->tick = 0;
  self->motion = MOTION_NONE;
}

/* the transition has stopped */
static void
settle (CalcMain *self)
{
  if (self->progress <= 0)
    remove_history (self);
}

static gboolean
motion_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalcMain *self = CALC_MAIN (widget);
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  gboolean done = FALSE;
  double t, p;

  if (!self->motion_start)
    self->motion_start = now;
  t = (now - self->motion_start) / (double) G_USEC_PER_SEC;
  if (self->motion == MOTION_TIMED) {
    double dir = self->goal > self->linear_from ? 1 : -1;
    double linear = self->linear_from + dir * t / DURATION;

    if ((dir > 0 && linear >= self->goal) || (dir < 0 && linear <= self->goal)) {
      linear = self->goal;
      done = TRUE;
    }
    p = spongy_curve (SPONGY_ACCELERATE_DECELERATE, linear);
  } else {
    p = stop_logic_position (&self->stop, t);
    done = t >= stop_logic_duration (&self->stop);
  }
  if (done)
    p = self->goal;
  set_progress (self, p);
  if (done) {
    self->tick = 0;
    self->motion = MOTION_NONE;
    settle (self);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
start_motion (CalcMain *self, MotionKind kind)
{
  stop_motion (self);
  self->motion = kind;
  self->motion_start = 0;
  self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), motion_tick, NULL, NULL);
}

/* to the end or the start: over the transition's duration, accelerate-decelerate, from where it is */
static void
transition_to (CalcMain *self, double goal)
{
  if (self->progress == goal && self->motion == MOTION_NONE)
    return;
  if (goal > 0 && !self->history)
    show_history (self);
  self->goal = goal;
  /* the linear time the curve is at now */
  self->linear_from = acos (CLAMP (1 - 2 * self->progress, -1, 1)) / G_PI;
  start_motion (self, MOTION_TIMED);
}

/* the finger lifts: a third of a second at its speed decides where it goes */
static void
release_drag (CalcMain *self, double velocity_y)
{
  double velocity = self->travel > 0 ? velocity_y / self->travel : 0;   /* progress / s */
  double pos = self->progress + velocity / 3;

  if (pos != 0 && pos != 1) {
    self->goal = pos < 0.5 ? 0 : 1;
    stop_logic_config (&self->stop, self->progress, self->goal, velocity);
    start_motion (self, MOTION_STOP_LOGIC);
  } else {
    settle (self);
  }
}

static gboolean
at_end_state (CalcMain *self)
{
  return self->progress >= 1 && self->motion == MOTION_NONE && !self->dragging;
}

/* Back inside the app: the toolbar closes, or the history goes up. FALSE: nothing to go back to here */
static gboolean
on_back_pressed (CalcMain *self)
{
  if (stop_action_mode (self))
    return TRUE;
  if (at_end_state (self) && self->history) {
    transition_to (self, 0);
    return TRUE;
  }
  return FALSE;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* touch: a move down or up of more than the touch slop that started on the display drags the transition;
 * any touch first closes the toolbar */

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CalcMain *self)
{
  guint32 time = gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (drag));

  stop_action_mode (self);
  self->dragging = FALSE;
  self->drag_x = x;
  self->drag_y = y;
  self->drag_inside = graphene_rect_contains_point (&self->display_box, &GRAPHENE_POINT_INIT (x, y));
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, time, x, y);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CalcMain *self)
{
  guint32 time = gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (drag));

  spongy_velocity_add (&self->velocity, time, self->drag_x + dx, self->drag_y + dy);
  if (!self->dragging) {
    if (!self->drag_inside || fabs (dy) <= TOUCH_SLOP)
      return;
    self->dragging = TRUE;
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    stop_motion (self);
    self->drag_progress = self->progress;
  }
  /* the whole move from the touch down counts, once the drag has started */
  set_progress (self, self->drag_progress + (self->travel > 0 ? dy / self->travel : 0));
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, CalcMain *self)
{
  guint32 time = gtk_event_controller_get_current_event_time (GTK_EVENT_CONTROLLER (drag));
  float vx = 0, vy = 0;

  if (!self->dragging)
    return;
  self->dragging = FALSE;
  if (time) {
    spongy_velocity_add (&self->velocity, time, self->drag_x + dx, self->drag_y + dy);
    spongy_velocity_get (&self->velocity, time, &vx, &vy);
  }
  release_drag (self, vy);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the menus */

static void
menu_history (gpointer data)
{
  transition_to (data, 1);
}

static void
menu_leading (gpointer data)
{
  CalcMain *self = data;
  g_autofree char *full = calc_result_get_full_text (self->result, TRUE);
  g_autofree char *message = g_strdup_printf ("%s %s", full, calc_result_full_text_is_exact (self->result)
                                                             ? _("(exact)") : _("(±1 in last digit)"));

  message_dialog (self, _("Answer with leading digits"), message, NULL, NULL);
}

static void
menu_fraction (gpointer data)
{
  CalcMain *self = data;
  UnifiedReal *result = evaluator_get_result (self->evaluator, EVALUATOR_MAIN_INDEX);
  g_autofree char *nice = NULL;
  g_autofree char *message = NULL;

  if (!result)
    return;
  nice = real_to_nice_string (result);
  message = calc_translate_result (nice);
  message_dialog (self, _("Answer as fraction"), message, NULL, NULL);
}

static void
on_licenses_closed (CalcLicenses *licenses, CalcMain *self)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (GTK_WIDGET (self));

  if (self->licenses != GTK_WIDGET (licenses))
    return;
  self->licenses = NULL;
  if (overlay)
    gtk_overlay_remove_overlay (overlay, GTK_WIDGET (licenses));
}

static void
menu_licenses (gpointer data)
{
  CalcMain *self = data;
  GtkOverlay *overlay = vibeos_window_get_overlay (GTK_WIDGET (self));

  if (!overlay || self->licenses)
    return;
  self->licenses = calc_licenses_new ();
  g_signal_connect (self->licenses, "closed", G_CALLBACK (on_licenses_closed), self);
  gtk_overlay_add_overlay (overlay, self->licenses);
  gtk_overlay_set_measure_overlay (overlay, self->licenses, FALSE);
  gtk_widget_grab_focus (self->licenses);
}

static const VibeosAboutSection calc_legal[] = {
  { "CRCalc", "The exact real arithmetic is Hans-J. Boehm's constructive reals (com.hp.creals, as Android's "
              "Calculator uses them): copyright 1999 Silicon Graphics, Inc., 2001-2004 Hewlett-Packard Development "
              "Company, L.P., under its own permissive licence (Open source licenses in the ⋮ menu)." },
  { NULL }
};

static const VibeosAboutInfo calc_about = { .name = "Calculator", .icon_name = "vibeos.Calculator",
                                            .legal = calc_legal };

static void
menu_about (gpointer data)
{
  vibeos_about_present (GTK_WIDGET (data), &calc_about);
}

/* the ⋮: History; with a result, its leading digits, and its fraction when it is rational; then the licences and
 * About, VibeOS's About sheet */
static void
on_overflow_clicked (GtkWidget *button, CalcMain *self)
{
  CalcPopupMenu *menu = calc_popup_menu_new ();
  UnifiedReal *result = evaluator_get_result (self->evaluator, EVALUATOR_MAIN_INDEX);

  calc_popup_menu_append (menu, _("History"), menu_history, self);
  if (self->state == STATE_RESULT)
    calc_popup_menu_append (menu, _("Answer with leading digits"), menu_leading, self);
  if (self->state == STATE_RESULT && result && real_exactly_displayable (result))
    calc_popup_menu_append (menu, _("Answer as fraction"), menu_fraction, self);
  calc_popup_menu_append (menu, _("Open source licenses"), menu_licenses, self);
  calc_popup_menu_append (menu, _("About"), menu_about, self);
  calc_popup_menu_present (menu, button);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* building it */

typedef struct {
  int             id;
  const char     *label;
  CalcButtonTheme theme;
  int             column, row;              /* portrait */
  int             land_column, land_row;
  int             invertible, inverse;      /* the index in the INV groups, or -1 */
} Key;

/* the advanced pad (inv and the modes in capitals) */
static const Key advanced_keys[] = {
  { CALC_KEY_OP_SQRT,    "√",                  CALC_BUTTON_ADVANCED, 0, 0, 1, 0, 5, -1 },
  { CALC_KEY_OP_SQR,     "x²",            CALC_BUTTON_ADVANCED, 0, 0, 1, 0, -1, 5 },
  { CALC_KEY_CONST_PI,   "π",                  CALC_BUTTON_ADVANCED, 1, 0, 2, 0, -1, -1 },
  { CALC_KEY_OP_POW,     "^",                  CALC_BUTTON_ADVANCED, 2, 0, 1, 1, -1, -1 },
  { CALC_KEY_OP_FACT,    "!",                  CALC_BUTTON_ADVANCED, 3, 0, 2, 1, -1, -1 },
  { BTN_TOGGLE_MODE,     "DEG",                CALC_BUTTON_ADVANCED, 0, 1, 0, 0, -1, -1 },
  { CALC_KEY_FUN_SIN,    "sin",                CALC_BUTTON_ADVANCED, 1, 1, 0, 2, 0, -1 },
  { CALC_KEY_FUN_ARCSIN, "sin⁻¹",    CALC_BUTTON_ADVANCED, 1, 1, 0, 2, -1, 0 },
  { CALC_KEY_FUN_COS,    "cos",                CALC_BUTTON_ADVANCED, 2, 1, 1, 2, 1, -1 },
  { CALC_KEY_FUN_ARCCOS, "cos⁻¹",    CALC_BUTTON_ADVANCED, 2, 1, 1, 2, -1, 1 },
  { CALC_KEY_FUN_TAN,    "tan",                CALC_BUTTON_ADVANCED, 3, 1, 2, 2, 2, -1 },
  { CALC_KEY_FUN_ARCTAN, "tan⁻¹",    CALC_BUTTON_ADVANCED, 3, 1, 2, 2, -1, 2 },
  { BTN_TOGGLE_INV,      "INV",                CALC_BUTTON_ADVANCED, 0, 2, 0, 1, -1, -1 },
  { CALC_KEY_CONST_E,    "e",                  CALC_BUTTON_ADVANCED, 1, 2, 0, 3, -1, -1 },
  { CALC_KEY_FUN_LN,     "ln",                 CALC_BUTTON_ADVANCED, 2, 2, 1, 3, 3, -1 },
  { CALC_KEY_FUN_EXP,    "eˣ",            CALC_BUTTON_ADVANCED, 2, 2, 1, 3, -1, 3 },
  { CALC_KEY_FUN_LOG,    "log",                CALC_BUTTON_ADVANCED, 3, 2, 2, 3, 4, -1 },
  { CALC_KEY_FUN_10POW,  "10ˣ",           CALC_BUTTON_ADVANCED, 3, 2, 2, 3, -1, 4 },
};

/* the input pad */
static const Key input_keys[] = {
  { BTN_CLR,             "AC",       CALC_BUTTON_ACTION,   0, 0, 4, 3, -1, -1 },
  { BTN_PAREN,           "( )",      CALC_BUTTON_OPERATOR, 1, 0, 4, 2, -1, -1 },
  { CALC_KEY_OP_PCT,     "%",        CALC_BUTTON_OPERATOR, 2, 0, 4, 1, -1, -1 },
  { CALC_KEY_OP_DIV,     "÷",        CALC_BUTTON_OPERATOR, 3, 0, 4, 0, -1, -1 },
  { CALC_KEY_DIGIT_7,    "7",        CALC_BUTTON_NUMERIC,  0, 1, 0, 0, -1, -1 },
  { CALC_KEY_DIGIT_8,    "8",        CALC_BUTTON_NUMERIC,  1, 1, 1, 0, -1, -1 },
  { CALC_KEY_DIGIT_9,    "9",        CALC_BUTTON_NUMERIC,  2, 1, 2, 0, -1, -1 },
  { CALC_KEY_OP_MUL,     "×",        CALC_BUTTON_OPERATOR, 3, 1, 3, 0, -1, -1 },
  { CALC_KEY_DIGIT_4,    "4",        CALC_BUTTON_NUMERIC,  0, 2, 0, 1, -1, -1 },
  { CALC_KEY_DIGIT_5,    "5",        CALC_BUTTON_NUMERIC,  1, 2, 1, 1, -1, -1 },
  { CALC_KEY_DIGIT_6,    "6",        CALC_BUTTON_NUMERIC,  2, 2, 2, 1, -1, -1 },
  { CALC_KEY_OP_SUB,     "−",   CALC_BUTTON_OPERATOR, 3, 2, 3, 1, -1, -1 },
  { CALC_KEY_DIGIT_1,    "1",        CALC_BUTTON_NUMERIC,  0, 3, 0, 2, -1, -1 },
  { CALC_KEY_DIGIT_2,    "2",        CALC_BUTTON_NUMERIC,  1, 3, 1, 2, -1, -1 },
  { CALC_KEY_DIGIT_3,    "3",        CALC_BUTTON_NUMERIC,  2, 3, 2, 2, -1, -1 },
  { CALC_KEY_OP_ADD,     "+",        CALC_BUTTON_OPERATOR, 3, 3, 3, 2, -1, -1 },
  { CALC_KEY_DIGIT_0,    "0",        CALC_BUTTON_NUMERIC,  0, 4, 0, 3, -1, -1 },
  { CALC_KEY_DEC_POINT,  NULL,       CALC_BUTTON_NUMERIC,  1, 4, 1, 3, -1, -1 },   /* the locale's */
  { BTN_DEL,             NULL,       CALC_BUTTON_CLEAR,    2, 4, 2, 3, -1, -1 },   /* the icon */
  { BTN_EQ,              "=",        CALC_BUTTON_EQUALS,   3, 4, 3, 3, -1, -1 },
};

static void
on_key_button_clicked (GtkWidget *button, CalcMain *self)
{
  on_button_click (self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "calc-id")));
}

/* the delete key held: everything goes */
static gboolean
on_delete_long_pressed (GtkWidget *button, CalcMain *self)
{
  stop_action_mode (self);
  on_clear (self);
  return TRUE;
}

static GtkWidget *
make_pad (CalcMain *self, const Key *keys, int n_keys, gboolean advanced)
{
  static const double portrait_advanced_columns[] = { 0.25, 0.5, 0.75 }, portrait_advanced_rows[] = { 0.33, 0.67 };
  static const double portrait_input_columns[] = { 0.25, 0.5, 0.75 }, portrait_input_rows[] = { 0.2, 0.4, 0.6, 0.8 };
  static const double land_advanced_columns[] = { 0.33, 0.67 }, land_advanced_rows[] = { 0.25, 0.5, 0.75 };
  static const double land_input_columns[] = { 0.2, 0.4, 0.6, 0.8 }, land_input_rows[] = { 0.25, 0.5, 0.75 };
  gboolean land = self->one_line;
  /* the pads' edge and inner paddings, the keys' margins and the delete icon: a tablet's are larger, the same
   * upright and sideways */
  int edge = self->tablet ? 16 : 8, intra = self->tablet ? 8 : 4;
  int small_margin = self->tablet ? 4 : land ? 0 : 2, key_margin = self->tablet ? 6 : 4;
  int icon = self->tablet ? 42 : land ? 22 : 48;
  GtkWidget *pad;
  int margin;

  if (advanced) {
    pad = land ? calc_pad_new (land_advanced_columns, 3, land_advanced_rows, 4)
               : calc_pad_new (portrait_advanced_columns, 4, portrait_advanced_rows, 3);
    /* the advanced pad's padding, and the keys' margin (none in landscape on a phone) */
    if (land)
      calc_pad_set_padding (CALC_PAD (pad), edge, edge, edge, intra);
    else
      calc_pad_set_padding (CALC_PAD (pad), edge, intra, edge, edge);
    margin = small_margin;
  } else {
    pad = land ? calc_pad_new (land_input_columns, 5, land_input_rows, 4)
               : calc_pad_new (portrait_input_columns, 4, portrait_input_rows, 5);
    /* the input pad's padding, and the keys' margin */
    if (land)
      calc_pad_set_padding (CALC_PAD (pad), edge, edge, intra, edge);
    else
      calc_pad_set_padding (CALC_PAD (pad), intra, edge, edge, edge);
    margin = key_margin;
  }

  for (int i = 0; i < n_keys; i++) {
    const Key *key = &keys[i];
    GtkWidget *button;

    if (key->id == BTN_DEL) {
      /* the icon smaller in landscape, where the keys are shorter */
      button = calc_button_new_icon ("calc-del-symbolic", icon, key->theme);
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("delete"), -1);
      g_signal_connect (button, "long-pressed", G_CALLBACK (on_delete_long_pressed), self);
    } else {
      button = calc_button_new (key->label ? key->label : calc_decimal_point (), key->theme);
    }
    g_object_set_data (G_OBJECT (button), "calc-id", GINT_TO_POINTER (key->id));
    g_signal_connect (button, "clicked", G_CALLBACK (on_key_button_clicked), self);
    calc_pad_attach (CALC_PAD (pad), button, land ? key->land_column : key->column, land ? key->land_row : key->row,
                     margin);
    if (key->invertible >= 0)
      self->invertible[key->invertible] = button;
    if (key->inverse >= 0) {
      self->inverse[key->inverse] = button;
      gtk_widget_set_visible (button, FALSE);
    }
    if (key->id == BTN_TOGGLE_INV)
      self->inv_toggle = button;
    if (key->id == BTN_TOGGLE_MODE)
      self->mode_toggle = button;
  }
  return pad;
}

static void
build (CalcMain *self, gboolean one_line, gboolean tablet)
{
  self->one_line = one_line;
  self->tablet = tablet;
  self->generation = g_object_new (G_TYPE_OBJECT, NULL);
  self->listener = (EvaluationListener) { &listener_funcs, self, self->generation };

  self->display = calc_display_new_full (one_line, tablet);
  self->formula = calc_display_get_formula (CALC_DISPLAY (self->display));
  self->result = calc_display_get_result (CALC_DISPLAY (self->display));
  calc_formula_set_scroll_to_end (self->formula, TRUE);
  calc_result_set_evaluator (self->result, self->evaluator, EVALUATOR_MAIN_INDEX);
  g_signal_connect (self->formula, "text-size-changed", G_CALLBACK (on_text_size_changed), self);
  g_signal_connect (self->formula, "long-pressed", G_CALLBACK (on_formula_long_pressed), self);
  g_signal_connect (self->result, "long-pressed", G_CALLBACK (on_result_long_pressed), self);
  g_signal_connect (calc_display_get_overflow (CALC_DISPLAY (self->display)), "clicked",
                    G_CALLBACK (on_overflow_clicked), self);
  self->advanced = make_pad (self, advanced_keys, G_N_ELEMENTS (advanced_keys), TRUE);
  self->input = make_pad (self, input_keys, G_N_ELEMENTS (input_keys), FALSE);
  gtk_widget_set_parent (self->display, GTK_WIDGET (self));
  gtk_widget_set_parent (self->advanced, GTK_WIDGET (self));
  gtk_widget_set_parent (self->input, GTK_WIDGET (self));
  self->state = STATE_NONE;
  self->progress = 0;
}

static void
teardown (CalcMain *self)
{
  stop_motion (self);
  stop_action_mode (self);
  remove_history (self);
  self->paste_serial++;
  self->formula = NULL;
  self->result = NULL;
  g_clear_pointer (&self->display, gtk_widget_unparent);
  g_clear_pointer (&self->advanced, gtk_widget_unparent);
  g_clear_pointer (&self->input, gtk_widget_unparent);
  /* what the old layout asked for no longer calls back */
  g_clear_object (&self->generation);
  self->dragging = FALSE;
}

static void
save_instance_state (CalcMain *self, SavedState *saved)
{
  evaluator_cancel_all (self->evaluator, TRUE);
  saved->state = self->state;
  saved->unprocessed = g_strdup (self->unprocessed);
  saved->eval_state = evaluator_save_state (self->evaluator);
  saved->inverse = self->inverse_mode;
  saved->history = self->history && self->progress >= 0.5;
}

static void
restore_instance_state (CalcMain *self, SavedState *saved)
{
  set_state (self, saved->state);
  g_free (self->unprocessed);
  self->unprocessed = g_steal_pointer (&saved->unprocessed);
  if (saved->eval_state)
    evaluator_restore_state (self->evaluator, saved->eval_state);
  g_clear_pointer (&saved->eval_state, g_bytes_unref);
  on_inverse_toggled (self, saved->inverse);
}

/* a result or an error comes back by being computed again */
static void
restore_display (CalcMain *self)
{
  on_mode_changed (self, evaluator_get_degree_mode (self->evaluator, EVALUATOR_MAIN_INDEX));
  if (self->state != STATE_RESULT && self->state != STATE_INIT_FOR_RESULT)
    redisplay_formula (self);
  if (self->state == STATE_INPUT) {
    calc_result_set_should_evaluate (self->result, CALC_RESULT_SHOULD_EVALUATE, &self->listener);
  } else {
    set_state (self, map_from_saved (self->state));
    calc_result_set_should_evaluate (self->result, CALC_RESULT_SHOULD_REQUIRE, &self->listener);
  }
}

/* a tablet: the window's layout says so */
static gboolean
on_tablet (CalcMain *self)
{
  VibeosLayout *layout = vibeos_layout_get (GTK_WIDGET (self));

  return layout && vibeos_layout_is_tablet (layout);
}

/* a rotation, or a display size that makes the screen a tablet or no longer one: the layout is made again (the
 * one-line display and pads in landscape, a tablet's sizes) from its saved state */
static gboolean
rebuild_idle (gpointer data)
{
  CalcMain *self = data;
  int width = gtk_widget_get_width (GTK_WIDGET (self)), height = gtk_widget_get_height (GTK_WIDGET (self));
  gboolean tablet = on_tablet (self);
  SavedState saved = { 0 };

  self->rebuild_idle = 0;
  if ((width > height) == self->one_line && tablet == self->tablet)
    return G_SOURCE_REMOVE;
  save_instance_state (self, &saved);
  teardown (self);
  build (self, width > height, tablet);
  restore_instance_state (self, &saved);
  restore_display (self);
  /* turned with the history open: it is open again, at once */
  if (saved.history)
    set_progress (self, 1);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the layout */

typedef struct {
  double x, y, w, h;      /* device px */
} Box;

/* a percent dimension or guideline, rounded to the device pixel */
static int
percent (double fraction, int length)
{
  return (int) (fraction * length + 0.5);
}

static double
lerp (double a, double b, double t)
{
  return a + (b - a) * t;
}

static double
round_px (double v)
{
  return floor (v + 0.5);
}

/* every view between its start and end constraints, at progress p */
static double
motion_layout (CalcMain *self, int W, int H, double p, Box *display, Box *advanced, Box *input, Box *history)
{
  if (!self->one_line) {
    int display_h = percent (0.3, H), advanced_h = percent (0.2, H), input_h = percent (0.5, H);
    int helper = percent (0.7, H);
    double input_end = H + helper - input_h, advanced_end = input_end - advanced_h;
    double display_end = advanced_end - display_h;
    double display_y = round_px (lerp (0, display_end, p));

    *display = (Box) { 0, display_y, W, display_h };
    *advanced = (Box) { 0, round_px (lerp (display_h, advanced_end, p)), W, advanced_h };
    *input = (Box) { 0, round_px (lerp (display_h + advanced_h, input_end, p)), W, input_h };
    *history = (Box) { 0, 0, W, display_y };
    return display_end;
  } else {
    int display_h = percent (0.4, H), pads_h = percent (0.6, H), helper = percent (0.6, H);
    int guide = percent (0.33, W);
    double pads_end = H + helper - pads_h, display_end = pads_end - display_h;
    double display_y = round_px (lerp (0, display_end, p)), pads_y = round_px (lerp (display_h, pads_end, p));

    *display = (Box) { 0, display_y, W, display_h };
    *advanced = (Box) { 0, pads_y, guide, pads_h };
    *input = (Box) { guide, pads_y, W - guide, pads_h };
    *history = (Box) { 0, 0, W, display_y };
    return display_end;
  }
}

static void
place (GtkWidget *child, const Box *box, int scale)
{
  double x = box->x / scale, y = box->y / scale, w = box->w / scale, h = box->h / scale;

  gtk_widget_allocate (child, (int) ceil (w - 0.001), (int) ceil (h - 0.001), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
}

static void
calc_main_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                   int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void
calc_main_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcMain *self = CALC_MAIN (widget);
  int scale = vibeos_device_scale (widget);
  int W = width * scale, H = height * scale;
  Box display, advanced, input, history;
  double travel;

  if (((width > height) != self->one_line || on_tablet (self) != self->tablet) && !self->rebuild_idle && width > 0
      && height > 0)
    self->rebuild_idle = g_idle_add (rebuild_idle, self);
  if (!self->display)
    return;
  travel = motion_layout (self, W, H, self->progress, &display, &advanced, &input, &history);
  self->travel = travel / scale;
  graphene_rect_init (&self->display_box, display.x / scale, display.y / scale, display.w / scale,
                      display.h / scale);
  self->history_height = history.h / scale;

  calc_display_set_exact_size (CALC_DISPLAY (self->display), display.w / scale, display.h / scale);
  place (self->display, &display, scale);
  calc_pad_set_exact_size (CALC_PAD (self->advanced), advanced.w / scale, advanced.h / scale);
  place (self->advanced, &advanced, scale);
  calc_pad_set_exact_size (CALC_PAD (self->input), input.w / scale, input.h / scale);
  place (self->input, &input, scale);
  if (self->history) {
    calc_history_set_exact_size (CALC_HISTORY (self->history), history.w / scale, history.h / scale);
    place (self->history, &history, scale);
  }
}

static void
calc_main_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcMain *self = CALC_MAIN (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  /* one flat colour: the original's gradient runs between two colours that are the same in the dark theme */
  GdkRGBA background = vibeos_color (VIBEOS_SURFACE);
  GdkRGBA history = vibeos_color (VIBEOS_SURFACE_CONTAINER);   /* behind the history */

  gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, w, h));
  if (self->history_height > 0)
    gtk_snapshot_append_color (snapshot, &history, &GRAPHENE_RECT_INIT (0, 0, w, self->history_height));
  if (self->history)
    gtk_widget_snapshot_child (widget, self->history, snapshot);
  if (self->display) {
    gtk_widget_snapshot_child (widget, self->display, snapshot);
    gtk_widget_snapshot_child (widget, self->advanced, snapshot);
    gtk_widget_snapshot_child (widget, self->input, snapshot);
  }
}

/* the window found to be a tablet (or no longer one) after it was laid out: made again, as for a rotation */
static void
on_layout_changed (VibeosLayout *layout, CalcMain *self)
{
  if (vibeos_layout_is_tablet (layout) != self->tablet && !self->rebuild_idle)
    self->rebuild_idle = g_idle_add (rebuild_idle, self);
}

static void
calc_main_root (GtkWidget *widget)
{
  CalcMain *self = CALC_MAIN (widget);

  GTK_WIDGET_CLASS (calc_main_parent_class)->root (widget);
  self->layout = vibeos_layout_get (widget);
  if (self->layout) {
    g_object_ref (self->layout);
    g_signal_connect (self->layout, "changed", G_CALLBACK (on_layout_changed), self);
    on_layout_changed (self->layout, self);
  }
}

static void
calc_main_unroot (GtkWidget *widget)
{
  CalcMain *self = CALC_MAIN (widget);

  if (self->layout) {
    g_signal_handlers_disconnect_by_data (self->layout, self);
    g_clear_object (&self->layout);
  }
  GTK_WIDGET_CLASS (calc_main_parent_class)->unroot (widget);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* keys */

static gboolean
on_key_pressed (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CalcMain *self)
{
  /* only a key pressed here is acted on when it is released: one that closed a dialog or a menu is not */
  self->key_down = keyval;
  return keyval != GDK_KEY_Tab && keyval != GDK_KEY_ISO_Left_Tab;
}

static void
on_key_released (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CalcMain *self)
{
  gunichar c;
  char text[8];

  if (keyval != self->key_down || !self->display)
    return;
  self->key_down = 0;
  switch (keyval) {
  case GDK_KEY_Escape:
  case GDK_KEY_Back:
    on_back_pressed (self);
    return;
  case GDK_KEY_Up:
  case GDK_KEY_Down:
  case GDK_KEY_Left:
  case GDK_KEY_Right:
    return;
  default:
    break;
  }
  stop_action_mode (self);
  cancel_unrequested (self);
  switch (keyval) {
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
  case GDK_KEY_ISO_Enter:
    on_equals (self);
    return;
  case GDK_KEY_BackSpace:
    on_delete (self);
    return;
  case GDK_KEY_Clear:
    on_clear (self);
    return;
  default:
    cancel_if_evaluating (self, FALSE);
    c = gdk_keyval_to_unicode (keyval);
    /* no dead keys, no control or format characters, no white space */
    if (c == 0 || g_unichar_iscntrl (c) || g_unichar_type (c) == G_UNICODE_FORMAT || g_unichar_isspace (c)
        || g_unichar_ismark (c))
      return;
    if (c == '=') {
      on_equals (self);
    } else {
      text[g_unichar_to_utf8 (c, text)] = '\0';
      add_chars (self, text, TRUE);
      redisplay_after_formula_change (self);
    }
  }
}

/* ------------------------------------------------------------------------------------------------------------ */

static void
calc_main_dispose (GObject *object)
{
  CalcMain *self = CALC_MAIN (object);

  if (self->rebuild_idle)
    g_source_remove (self->rebuild_idle);
  self->rebuild_idle = 0;
  if (self->display)
    teardown (self);
  if (self->licenses) {
    g_signal_handlers_disconnect_by_data (self->licenses, self);
    self->licenses = NULL;
  }
  if (self->evaluator) {
    evaluator_cancel_all (self->evaluator, TRUE);
    evaluator_free (self->evaluator);
    self->evaluator = NULL;
  }
  G_OBJECT_CLASS (calc_main_parent_class)->dispose (object);
}

static void
calc_main_finalize (GObject *object)
{
  CalcMain *self = CALC_MAIN (object);

  g_free (self->unprocessed);
  g_free (self->paste_text);
  g_free (self->paste_tag);
  G_OBJECT_CLASS (calc_main_parent_class)->finalize (object);
}

static void
calc_main_class_init (CalcMainClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_main_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_main_finalize;
  widget_class->measure = calc_main_measure;
  widget_class->size_allocate = calc_main_size_allocate;
  widget_class->snapshot = calc_main_snapshot;
  widget_class->root = calc_main_root;
  widget_class->unroot = calc_main_unroot;
  gtk_widget_class_set_css_name (widget_class, "calc-main");
}

static void
calc_main_init (CalcMain *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->state = STATE_NONE;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

static CalcMain *
calc_main_new (GSettings *settings)
{
  CalcMain *self = g_object_new (CALC_TYPE_MAIN, NULL);
  g_autofree char *db = g_build_filename (g_get_user_data_dir (), "vibeos.Calculator", "history.db", NULL);

  self->evaluator = evaluator_new (db, settings);
  evaluator_set_callback (self->evaluator, &evaluator_callback, self);
  build (self, FALSE, FALSE);
  /* a new start: an empty display */
  set_state (self, STATE_INPUT);
  evaluator_clear_main (self->evaluator);
  on_inverse_toggled (self, FALSE);
  restore_display (self);
  return self;
}

GtkWidget *
calc_window_new (GtkApplication *app, GSettings *settings)
{
  GtkWidget *window = vibeos_window_new (app);
  CalcMain *content = calc_main_new (settings);
  GtkEventController *keys = gtk_event_controller_key_new ();

  gtk_window_set_title (GTK_WINDOW (window), _("Calculator"));
  gtk_widget_add_css_class (window, "calculator");
  vibeos_window_set_content (GTK_WINDOW (window), GTK_WIDGET (content));
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), content);
  g_signal_connect (keys, "key-released", G_CALLBACK (on_key_released), content);
  gtk_widget_add_controller (window, keys);
  return window;
}
