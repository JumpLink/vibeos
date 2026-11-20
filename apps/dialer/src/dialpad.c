/* The dialpad: see dialpad.h. It is both the Phone tab's dialpad and the in-call keypad. */
#include "dialpad.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>

#define DIGITS_HEIGHT   60      /* the field with the typed number, at least */
#define DIGITS_SIZE     34      /* sp: the typed number, shrunk as far as DIGITS_MIN_SIZE to fit */
#define DIGITS_MIN_SIZE 24
#define GSF_LINE        (2504 / 2000.0)     /* Google Sans Flex's line height, of its size */
#define SPACE_ABOVE     (1 + 14)        /* a one-pixel spacer, then the gap over the keys */
#define KEY_HEIGHT      64      /* the rows' minimum; they share what is left over */
#define SPACE_BELOW     8
#define END_SPACE       80      /* under the keys, for the dial button */
#define INCALL_END_SPACE 116    /* the same in a call, for the end-call button */
#define CORNER          28      /* the sheet's top corners */
#define BUTTON_MARGIN   8
#define MENU_PADDING    10      /* round ✕ and ⋮ */
#define DELETE_PADDING  16      /* round ⌫ */
#define NUMBER_SIZE     36      /* a key's digit: dp, it keeps its size whatever the font size */
#define NUMBER_GAP      3       /* between a key's digit and its letters */
#define LETTERS_SIZE    12      /* sp, as * and # are */
#define STAR_SIZE       36
#define POUND_SIZE      23
#define SYMBOL_GAP      13      /* room left under * and #, which lifts them */
#define VOICEMAIL_SIZE  18      /* the voicemail glyph under the 1 */
#define LONG_PRESS_MS   400

/* colours, read as the pad draws: the theme's roles in the Dialer; in a call, whose screen is dark in light mode too,
 * the dark tones of the theme's palettes */
typedef enum
{
  BACKGROUND,                   /* the sheet */
  PRIMARY,                      /* the digits and the icons */
  SECONDARY,                    /* the letters, * and # */
} PadColor;

static const VibeosColor pad_roles[] = {
  [BACKGROUND] = VIBEOS_SURFACE_CONTAINER_HIGHEST,
  [PRIMARY] = VIBEOS_ON_PRIMARY_CONTAINER,
  [SECONDARY] = VIBEOS_ON_SURFACE_VARIANT,
};
static const struct { VibeosPalette palette; int stop; } pad_in_call[] = {
  [BACKGROUND] = { VIBEOS_PALETTE_NEUTRAL1, 800 },
  [PRIMARY] = { VIBEOS_PALETTE_ACCENT1, 100 },      /* the theme's primary_fixed, as the CSS gives its icons */
  [SECONDARY] = { VIBEOS_PALETTE_NEUTRAL2, 300 },
};

static gboolean pad_is_in_call (DialerDialpad *self);

static GdkRGBA
rgb (DialerDialpad *pad, PadColor color, double alpha)
{
  GdkRGBA rgba;

  if (!pad_is_in_call (pad))
    return vibeos_color_alpha (pad_roles[color], alpha);
  rgba = vibeos_palette_stop (pad_in_call[color].palette, pad_in_call[color].stop);
  rgba.alpha = alpha;
  return rgba;
}

/* the layout's text at `size` px, at 400 as Bold text draws it; unchanged when it is that already */
static void
gsf_style (PangoLayout *layout, double size)
{
  g_autoptr(PangoFontDescription) font = pango_font_description_from_string ("Google Sans Flex, VibeOS Roboto");

  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_font_description_set_weight (font, vibeos_font_weight (400));
  pango_layout_set_font_description (layout, font);
}

static PangoLayout *
gsf_layout (GtkWidget *widget, const char *text, double size)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (widget, text);

  gsf_style (layout, size);
  return layout;
}

/* the field with the typed number: as tall as a line of it at the font size in effect, DIGITS_HEIGHT at least */
static double
digits_height (void)
{
  return MAX (DIGITS_HEIGHT, ceil (vibeos_sp (DIGITS_SIZE) * GSF_LINE));
}

/* ---- a key ---------------------------------------------------------------------------------------------------- */

#define DIALPAD_TYPE_KEY (dialpad_key_get_type ())
G_DECLARE_FINAL_TYPE (DialpadKey, dialpad_key, DIALPAD, KEY, GtkWidget)

struct _DialpadKey
{
  GtkWidget parent;
  DialerDialpad *pad;           /* the dialpad it is on, which says what a key did */
  char c;
  PangoLayout *number, *letters;
  VibeosRipple ripple;
  gboolean down;
  guint long_press;
};

G_DEFINE_FINAL_TYPE (DialpadKey, dialpad_key, GTK_TYPE_WIDGET)

enum { KEY_DOWN, KEY_UP, KEY_LONG_PRESSED, DIGITS_CHANGED, CLOSE, OVERFLOW, N_SIGNALS };
static guint signals[N_SIGNALS];

static void pad_key_down (DialerDialpad *self, char c);
static void pad_key_up (DialerDialpad *self, char c);
static void pad_key_long_pressed (DialerDialpad *self, char c);

/* the keys' letters, Latin */
static const char *
key_letters (char c)
{
  static const char *letters[] = { "+", "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" };

  return c >= '0' && c <= '9' ? letters[c - '0'] : NULL;
}

/* a key's text: exactly as big as its glyphs' ink, so the key centres what is drawn, not the line box */
typedef struct
{
  double x, y, width, height;
} Ink;

static Ink
ink_of (PangoLayout *layout)
{
  PangoRectangle ink;

  pango_layout_get_extents (layout, &ink, NULL);
  return (Ink) { ink.x / (double) PANGO_SCALE, ink.y / (double) PANGO_SCALE, ink.width / (double) PANGO_SCALE,
                 ink.height / (double) PANGO_SCALE };
}

static void
draw_ink (GtkSnapshot *snapshot, PangoLayout *layout, const Ink *ink, double cx, double top, const GdkRGBA *color)
{
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx - ink->width / 2 - ink->x, top - ink->y));
  gtk_snapshot_append_layout (snapshot, layout, color);
  gtk_snapshot_restore (snapshot);
}

static void
dialpad_key_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DialpadKey *self = DIALPAD_KEY (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget), cx = width / 2;
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, width, height);
  GdkRGBA primary = rgb (self->pad, PRIMARY, 1), secondary = rgb (self->pad, SECONDARY, 1);
  Ink number, letters = { 0 };
  double group, top;

  /* the sizes and weight at the font size and Bold text in effect (the digit's size is fixed) */
  gsf_style (self->number, self->c == '*' ? vibeos_sp (STAR_SIZE)
                           : self->c == '#' ? vibeos_sp (POUND_SIZE) : NUMBER_SIZE);
  if (self->letters)
    gsf_style (self->letters, vibeos_sp (LETTERS_SIZE));
  number = ink_of (self->number);

  /* a key's ripple, the theme's */
  if (vibeos_ripple_is_active (&self->ripple))
    {
      GdkRGBA ripple = vibeos_ripple_default_color ();

      gtk_snapshot_push_clip (snapshot, &box);
      vibeos_ripple_snapshot (&self->ripple, snapshot, &box, &ripple);
      gtk_snapshot_pop (snapshot);
    }

  if (self->c == '*' || self->c == '#')
    {
      /* * and #: the secondary colour, a little faded, lifted by SYMBOL_GAP */
      GdkRGBA symbol = rgb (self->pad, SECONDARY, 0.8);

      top = round ((height - (number.height + SYMBOL_GAP)) / 2);
      draw_ink (snapshot, self->number, &number, cx, top, &symbol);
      return;
    }
  if (self->c == '1')
    group = number.height + NUMBER_GAP + 1 + VOICEMAIL_SIZE;
  else
    {
      letters = ink_of (self->letters);
      group = number.height + NUMBER_GAP + letters.height;
    }
  top = round ((height - group) / 2);
  draw_ink (snapshot, self->number, &number, cx, top, &primary);
  top += number.height + NUMBER_GAP;
  if (self->c == '1')
    /* the voicemail glyph, under a 1 px spacer that lines it up with the letters of the keys beside it */
    vibeos_snapshot_icon (snapshot, widget, "dialer-voicemail-symbolic", cx - VOICEMAIL_SIZE / 2.0, top + 1,
                       VOICEMAIL_SIZE, &secondary);
  else
    draw_ink (snapshot, self->letters, &letters, cx, top, &secondary);
}

static void
dialpad_key_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? KEY_HEIGHT : 0;
}

static void
key_up (DialpadKey *self)
{
  g_clear_handle_id (&self->long_press, g_source_remove);
  if (!self->down)
    return;
  self->down = FALSE;
  vibeos_ripple_release (&self->ripple);
  pad_key_up (self->pad, self->c);
}

static gboolean
on_long_press (gpointer data)
{
  DialpadKey *self = data;

  self->long_press = 0;
  pad_key_long_pressed (self->pad, self->c);
  return G_SOURCE_REMOVE;
}

/* pressed: the tone starts as the key goes down and lasts until it comes up */
static void
on_key_pressed (GtkGestureClick *gesture, int n_press, double x, double y, DialpadKey *self)
{
  if (self->down)
    return;
  self->down = TRUE;
  vibeos_ripple_press (&self->ripple, x, y);
  g_clear_handle_id (&self->long_press, g_source_remove);
  self->long_press = g_timeout_add (LONG_PRESS_MS, on_long_press, self);
  pad_key_down (self->pad, self->c);
}

static void
on_key_released (GtkGestureClick *gesture, int n_press, double x, double y, DialpadKey *self)
{
  key_up (self);
}

static void
on_key_stopped (GtkGestureClick *gesture, DialpadKey *self)
{
  key_up (self);
}

static void
dialpad_key_dispose (GObject *object)
{
  DialpadKey *self = DIALPAD_KEY (object);

  g_clear_handle_id (&self->long_press, g_source_remove);
  vibeos_ripple_clear (&self->ripple);
  g_clear_object (&self->number);
  g_clear_object (&self->letters);
  G_OBJECT_CLASS (dialpad_key_parent_class)->dispose (object);
}

static void
dialpad_key_class_init (DialpadKeyClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialpad_key_dispose;
  widget_class->snapshot = dialpad_key_snapshot;
  widget_class->measure = dialpad_key_measure;
  gtk_widget_class_set_css_name (widget_class, "dialpad-key");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_BUTTON);
}

static void
dialpad_key_init (DialpadKey *self)
{
  vibeos_ripple_init (&self->ripple, GTK_WIDGET (self));
}

static GtkWidget *
dialpad_key_new (DialerDialpad *pad, char c)
{
  DialpadKey *self = g_object_new (DIALPAD_TYPE_KEY, NULL);
  GtkGesture *click = gtk_gesture_click_new ();
  char text[2] = { c, '\0' };
  const char *letters = key_letters (c);
  g_autofree char *description = g_strdup_printf ("%c,%s", c, letters ? letters : "");

  self->pad = pad;
  self->c = c;
  /* sized as the key draws */
  self->number = gsf_layout (GTK_WIDGET (self), text, NUMBER_SIZE);
  if (letters && *letters)
    self->letters = gsf_layout (GTK_WIDGET (self), letters, LETTERS_SIZE);
  /* the content descriptions are "1," "2,ABC" … "*" "#" */
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  c == '*' || c == '#' ? text : description, -1);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "pressed", G_CALLBACK (on_key_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_key_released), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_key_stopped), self);
  g_signal_connect_swapped (click, "cancel", G_CALLBACK (key_up), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  return GTK_WIDGET (self);
}

/* ---- the dialpad ---------------------------------------------------------------------------------------------- */

struct _DialerDialpad
{
  GtkWidget parent;
  DialpadMode mode;
  GtkWidget *keys[12];          /* 1 2 3 / 4 5 6 / 7 8 9 / * 0 # */
  GtkWidget *close, *overflow, *backspace;
  GString *digits;
  PangoLayout *digits_layout;
  VibeosAnim *overflow_fade;
  double overflow_alpha;
};

G_DEFINE_FINAL_TYPE (DialerDialpad, dialer_dialpad, GTK_TYPE_WIDGET)

static const char key_order[] = "123456789*0#";

static gboolean
pad_is_in_call (DialerDialpad *self)
{
  return self->mode == DIALPAD_INCALL;
}

static int
end_space (DialerDialpad *self)
{
  return self->mode == DIALPAD_INCALL ? INCALL_END_SPACE : END_SPACE;
}

static void
dialer_dialpad_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  DialerDialpad *self = DIALER_DIALPAD (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    *minimum = *natural = 0;
  else
    *minimum = *natural = digits_height () + SPACE_ABOVE + 4 * KEY_HEIGHT + end_space (self) + SPACE_BELOW;
}

/* the ✕, ⋮ and ⌫ buttons: an icon with padding beside it, the row's height less the margins */
static void
allocate_button (GtkWidget *button, double x, double width)
{
  if (!gtk_widget_get_visible (button))
    return;
  gtk_widget_size_allocate (button, &(GtkAllocation) { (int) x, BUTTON_MARGIN, (int) width,
                                                       (int) digits_height () - 2 * BUTTON_MARGIN }, -1);
}

/* the field between the buttons */
static void
digits_field (DialerDialpad *self, double width, double *x0, double *x1)
{
  *x0 = 0;
  *x1 = width;
  if (self->mode == DIALPAD_INCALL)
    *x0 = BUTTON_MARGIN + 2 * MENU_PADDING + 24 + BUTTON_MARGIN;
  else if (gtk_widget_get_visible (self->overflow))
    *x0 = BUTTON_MARGIN + 2 * MENU_PADDING + 24 + BUTTON_MARGIN;
  /* ⌫ takes its room even where it is hidden (INVISIBLE on the in-call keypad) */
  *x1 = width - (BUTTON_MARGIN + 2 * DELETE_PADDING + 24 + BUTTON_MARGIN);
}

static void
dialer_dialpad_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  DialerDialpad *self = DIALER_DIALPAD (widget);
  double field = digits_height (), fixed = field + SPACE_ABOVE + end_space (self) + SPACE_BELOW;
  double row = MAX (KEY_HEIGHT, (height - fixed) / 4);
  double key = width * 15.0 / 47, side = width / 47.0;

  allocate_button (self->close, BUTTON_MARGIN, 2 * MENU_PADDING + 24);
  allocate_button (self->overflow, BUTTON_MARGIN, 2 * MENU_PADDING + 24);
  allocate_button (self->backspace, width - BUTTON_MARGIN - (2 * DELETE_PADDING + 24), 2 * DELETE_PADDING + 24);
  /* each row: a space of weight 1, three keys of 15, a space of 1 */
  for (int i = 0; i < 12; i++)
    {
      int r = i / 3, c = i % 3;
      double x = side + c * key, y = field + SPACE_ABOVE + r * row;

      gtk_widget_size_allocate (self->keys[i], &(GtkAllocation) { (int) round (x), (int) round (y),
                                                                  (int) (round (x + key) - round (x)),
                                                                  (int) (round (y + row) - round (y)) }, -1);
    }
}

static void
dialer_dialpad_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DialerDialpad *self = DIALER_DIALPAD (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget), x0, x1;
  double full = vibeos_sp (DIGITS_SIZE), size = full, field = digits_height ();
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, width, height);
  GskRoundedRect shape;
  GdkRGBA background = rgb (self, BACKGROUND, 1), primary = rgb (self, PRIMARY, 1), shadow;
  int text_width, text_height;

  /* the sheet: round top corners and a shadow over the page above it */
  gsk_rounded_rect_init (&shape, &box, &GRAPHENE_SIZE_INIT (CORNER, CORNER), &GRAPHENE_SIZE_INIT (CORNER, CORNER),
                         &GRAPHENE_SIZE_INIT (0, 0), &GRAPHENE_SIZE_INIT (0, 0));
  shadow = vibeos_color_alpha (VIBEOS_SHADOW, 0.25);
  gtk_snapshot_append_outset_shadow (snapshot, &shape, &shadow, 0, -2, 0, 10);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &background, &box);
  gtk_snapshot_pop (snapshot);

  /* the digits: centred in their field; too wide, the size shrinks with the width down to DIGITS_MIN_SIZE, and
   * past that the field shows the end of them */
  digits_field (self, width, &x0, &x1);
  pango_layout_set_text (self->digits_layout, self->digits->str, -1);
  {
    g_autoptr(PangoFontDescription) font = pango_font_description_copy (pango_layout_get_font_description (self->digits_layout));

    pango_font_description_set_absolute_size (font, full * PANGO_SCALE);
    pango_font_description_set_weight (font, vibeos_font_weight (400));
    pango_layout_set_font_description (self->digits_layout, font);
    pango_layout_get_pixel_size (self->digits_layout, &text_width, &text_height);
    if (text_width > x1 - x0)
      {
        size = MAX (vibeos_sp (DIGITS_MIN_SIZE), full * (x1 - x0) / text_width);
        pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
        pango_layout_set_font_description (self->digits_layout, font);
        pango_layout_get_pixel_size (self->digits_layout, &text_width, &text_height);
      }
  }
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (x0, 0, x1 - x0, field));
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (text_width > x1 - x0 ? x1 - text_width
                                                                               : round ((x0 + x1 - text_width) / 2),
                                                          round ((field - text_height) / 2.0)));
  gtk_snapshot_append_layout (snapshot, self->digits_layout, &primary);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);

  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    if (child == self->overflow)
      {
        gtk_snapshot_push_opacity (snapshot, self->overflow_alpha);
        gtk_widget_snapshot_child (widget, child, snapshot);
        gtk_snapshot_pop (snapshot);
      }
    else
      gtk_widget_snapshot_child (widget, child, snapshot);
}

/* ---- digits --------------------------------------------------------------------------------------------------- */

static void
on_overflow_fade (VibeosAnim *anim, double value, gpointer data)
{
  DialerDialpad *self = data;

  self->overflow_alpha = value;
  /* faded out: gone at the end */
  if (!vibeos_anim_is_running (anim) && value == 0)
    gtk_widget_set_visible (self->overflow, FALSE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ⋮ fades in with the first digit and out with the last, over 300 ms */
static void
update_overflow (DialerDialpad *self)
{
  gboolean show = self->mode == DIALPAD_DIALER && self->digits->len > 0;
  double to = show ? 1 : 0;

  if (vibeos_anim_is_running (self->overflow_fade) ? vibeos_anim_get_to (self->overflow_fade) == to
                                                : self->overflow_alpha == to)
    return;
  if (show)
    gtk_widget_set_visible (self->overflow, TRUE);
  vibeos_anim_start (self->overflow_fade, self->overflow_alpha, to, 300, 0, SPONGY_ACCELERATE_DECELERATE);
}

static void
digits_changed (DialerDialpad *self)
{
  update_overflow (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[DIGITS_CHANGED], 0);
}

const char *
dialer_dialpad_get_digits (DialerDialpad *self)
{
  return self->digits->str;
}

void
dialer_dialpad_set_digits (DialerDialpad *self, const char *digits)
{
  g_string_assign (self->digits, digits ? digits : "");
  digits_changed (self);
}

void
dialer_dialpad_append (DialerDialpad *self, char c)
{
  g_string_append_c (self->digits, c);
  digits_changed (self);
}

static void
pad_key_down (DialerDialpad *self, char c)
{
  /* the main dialpad types the key; the in-call one shows the tone once it is sent (dialer_dialpad_append) */
  if (self->mode == DIALPAD_DIALER)
    dialer_dialpad_append (self, c);
  g_signal_emit (self, signals[KEY_DOWN], 0, (int) c);
}

static void
pad_key_up (DialerDialpad *self, char c)
{
  g_signal_emit (self, signals[KEY_UP], 0, (int) c);
}

static void
pad_key_long_pressed (DialerDialpad *self, char c)
{
  /* a long press on 0 makes the 0 just typed a + */
  if (self->mode == DIALPAD_DIALER && c == '0' && self->digits->len > 0
      && self->digits->str[self->digits->len - 1] == '0')
    {
      self->digits->str[self->digits->len - 1] = '+';
      digits_changed (self);
    }
  g_signal_emit (self, signals[KEY_LONG_PRESSED], 0, (int) c);
}

static void
on_backspace (VibeosPressable *button, DialerDialpad *self)
{
  if (self->digits->len == 0)
    return;
  g_string_truncate (self->digits, self->digits->len - 1);
  digits_changed (self);
}

static void
on_backspace_long (VibeosPressable *button, DialerDialpad *self)
{
  dialer_dialpad_set_digits (self, "");
}

static void
on_close (VibeosPressable *button, DialerDialpad *self)
{
  g_signal_emit (self, signals[CLOSE], 0);
}

static void
on_overflow (VibeosPressable *button, DialerDialpad *self)
{
  g_signal_emit (self, signals[OVERFLOW], 0, button);
}

/* ✕, ⋮, ⌫: an icon with the keys' ripple */
static GtkWidget *
row_button (DialerDialpad *self, const char *icon_name, const char *description)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *button = vibeos_pressable_new (image);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  gtk_widget_add_css_class (image, "dialpad-icon");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, description, -1);
  gtk_widget_set_parent (button, GTK_WIDGET (self));
  return button;
}

/* ---- GObject -------------------------------------------------------------------------------------------------- */

static void
dialer_dialpad_dispose (GObject *object)
{
  DialerDialpad *self = DIALER_DIALPAD (object);
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  g_clear_pointer (&self->overflow_fade, vibeos_anim_free);
  g_clear_object (&self->digits_layout);
  if (self->digits)
    g_string_free (self->digits, TRUE);
  self->digits = NULL;
  G_OBJECT_CLASS (dialer_dialpad_parent_class)->dispose (object);
}

static void
dialer_dialpad_class_init (DialerDialpadClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = dialer_dialpad_dispose;
  widget_class->measure = dialer_dialpad_measure;
  widget_class->size_allocate = dialer_dialpad_size_allocate;
  widget_class->snapshot = dialer_dialpad_snapshot;
  gtk_widget_class_set_css_name (widget_class, "dialpad");
  signals[KEY_DOWN] = g_signal_new ("key-down", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 1, G_TYPE_INT);
  signals[KEY_UP] = g_signal_new ("key-up", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_INT);
  signals[KEY_LONG_PRESSED] = g_signal_new ("key-long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[DIGITS_CHANGED] = g_signal_new ("digits-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                          NULL, NULL, G_TYPE_NONE, 0);
  signals[CLOSE] = g_signal_new ("close", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
  signals[OVERFLOW] = g_signal_new ("overflow", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 1, GTK_TYPE_WIDGET);
}

static void
dialer_dialpad_init (DialerDialpad *self)
{
  self->digits = g_string_new (NULL);
  self->digits_layout = gsf_layout (GTK_WIDGET (self), "", DIGITS_SIZE);
  self->overflow_fade = vibeos_anim_new (GTK_WIDGET (self), on_overflow_fade, self);
}

GtkWidget *
dialer_dialpad_new (DialpadMode mode)
{
  DialerDialpad *self = g_object_new (DIALER_TYPE_DIALPAD, NULL);

  self->mode = mode;
  for (int i = 0; i < 12; i++)
    {
      self->keys[i] = dialpad_key_new (self, key_order[i]);
      gtk_widget_set_parent (self->keys[i], GTK_WIDGET (self));
    }
  self->close = row_button (self, "dialer-close-symbolic", _("Close keypad"));
  self->overflow = row_button (self, "dialer-more-symbolic", _("More options"));
  self->backspace = row_button (self, "dialer-backspace-symbolic", _("backspace"));
  g_signal_connect (self->close, "clicked", G_CALLBACK (on_close), self);
  g_signal_connect (self->overflow, "clicked", G_CALLBACK (on_overflow), self);
  g_signal_connect (self->backspace, "clicked", G_CALLBACK (on_backspace), self);
  g_signal_connect (self->backspace, "long-pressed", G_CALLBACK (on_backspace_long), self);
  /* the in-call keypad: dark like the call's screen; ✕ shown, ⋮ gone, ⌫ invisible but keeping its room */
  if (mode == DIALPAD_INCALL)
    gtk_widget_add_css_class (GTK_WIDGET (self), "dialpad-in-call");
  gtk_widget_set_visible (self->close, mode == DIALPAD_INCALL);
  gtk_widget_set_visible (self->overflow, FALSE);
  gtk_widget_set_visible (self->backspace, mode == DIALPAD_DIALER);
  return GTK_WIDGET (self);
}
