/* Call statistics: see call-stats.h. */
#include "call-stats.h"
#include "avatar.h"
#include "call-log.h"
#include "calls-client.h"
#include "contacts-app.h"
#include "empty-view.h"
#include "phone.h"
#include "phone-number.h"
#include "popup-menu.h"

#include <glib/gi18n.h>
#include <math.h>
#include <spongy-gtk.h>
#include <string.h>

/* the list */
#define BAND_HEIGHT     48      /* the band for the two filters, empty in this build */
#define OUTER_MARGIN    8
#define LIST_PAD_X      8
/* a row */
#define ROW_PAD         12
#define PHOTO           48
#define PHOTO_PAD_TOP   2       /* the picture is PHOTO less this, this far down the badge, centred across */
#define INFO_START      16
#define LABEL_END       8
#define BAR             4       /* the row's bar of call types */
#define BAR_MARGIN      6
/* the detail page: the caller, as in Call details' header, then the cards */
#define BAR_HEIGHT      56      /* the toolbar */
#define NAV_SIZE        56
#define TITLE_START     72
#define CALLER_TOP      6
#define CALLER_PAD_TOP_START 12
#define CALLER_PAD_BOTTOM_END 8
#define CALLER_TEXT     12      /* between the photo and the name */
#define BUTTON          48      /* the call button */
#define INFO_PAD_X      16
#define INFO_PAD_Y      8
#define CARD_PAD        16
#define CARD_GAP        16      /* between the cards */
#define CARD_TITLE_GAP  16
#define DETAIL_BAR      6       /* a card's bar of call types */
#define DETAIL_BAR_MARGIN 8
#define LINES_TOP       8
#define ICON_MARGIN     2       /* under each line, after the icon, before the percent */
#define TYPE_ICON       12
#define DIVIDER_MARGIN  8       /* above and below the separator over the actions */
#define ACTION_PAD      28      /* an action row's icon, in from the start */
#define ACTION_PAD_V    16
#define ACTION_ICON     24

/* the bars' colours: incoming, outgoing, missed and blocked calls, then the rest of the bar */
#define BAR_INCOMING    0xff00a8ff
#define BAR_OUTGOING    0xff00c853
#define BAR_MISSED      0xfffb6a51
#define BAR_BLOCKED     0xffa3a7ac
#define BAR_REST        0x88888888

static const char *
country (void)
{
  const char *country = calls_client_get_country (calls_client_get ());

  return country && *country ? country : phone_number_locale_country ();
}

static GtkWidget *
text_label (const char *text, const char *css_class, double size)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, css_class);
  vibeos_label_set_font_padding (label, size);
  return label;
}

/* ---- a number's calls --------------------------------------------------------------------------------------- */

typedef struct
{
  char *number;                 /* as the log has it, from the newest call; "" for a withheld one */
  Presentation presentation;
  gint64 in_duration, out_duration;
  guint in_count, out_count, missed_count, blocked_count;
} Stat;

static Stat *
stat_copy (const Stat *stat)
{
  Stat *copy = g_memdup2 (stat, sizeof *stat);

  copy->number = g_strdup (stat->number);
  return copy;
}

static void
stat_free (Stat *stat)
{
  if (!stat)
    return;
  g_free (stat->number);
  g_free (stat);
}

static gint64
stat_duration (const Stat *stat)
{
  return stat->in_duration + stat->out_duration;
}

static guint
stat_count (const Stat *stat)
{
  return stat->in_count + stat->out_count + stat->missed_count + stat->blocked_count;
}

/* what is counted is an incoming, an outgoing, a missed or a blocked call; a declined call, a
 * voicemail or one answered on another device counts for nothing */
static void
stat_add (Stat *stat, CallType type, gint64 duration)
{
  switch (type)
    {
    case CALL_TYPE_INCOMING:
      stat->in_count++;
      stat->in_duration += duration;
      break;
    case CALL_TYPE_OUTGOING:
      stat->out_count++;
      stat->out_duration += duration;
      break;
    case CALL_TYPE_MISSED:
      stat->missed_count++;
      break;
    case CALL_TYPE_BLOCKED:
      stat->blocked_count++;
      break;
    default:
      break;
    }
}

static void
stat_merge (Stat *into, const Stat *other)
{
  into->in_duration += other->in_duration;
  into->out_duration += other->out_duration;
  into->in_count += other->in_count;
  into->out_count += other->out_count;
  into->missed_count += other->missed_count;
  into->blocked_count += other->blocked_count;
}

/* the number is shown as a name and can be called */
static gboolean
stat_callable (const Stat *stat)
{
  return *stat->number && !phone_number_presentation_name (phone_number_presentation (stat->number,
                                                                                      stat->presentation));
}

/* the duration or the count, with no call-type filter */
static double
stat_value (const Stat *stat, gboolean by_duration)
{
  return by_duration ? (double) stat_duration (stat) : (double) stat_count (stat);
}

/* ---- the words: counts and durations ------------------------------------------------------------------------ */

static char *
count_string (guint count)
{
  return g_strdup_printf (ngettext ("%d call", "%d calls", count), (int) count);
}

/* "1 hr 5 mins 3 secs", the parts that are not 0; without the seconds, rounded to the minute. So
 * 59 min 30 s comes out as "1 hr 60 mins": the hour goes up and the minutes stay */
static char *
duration_string (gint64 duration, gboolean with_seconds)
{
  int hours = (int) (duration / 3600), minutes, seconds;
  gboolean show_hours, show_minutes, show_seconds;
  GString *text = g_string_new (NULL);

  duration -= (gint64) hours * 3600;
  minutes = (int) (duration / 60);
  seconds = (int) (duration - (gint64) minutes * 60);
  if (!with_seconds)
    {
      if (seconds >= 30)
        minutes++;
      if (minutes >= 60)
        hours++;
    }
  show_hours = hours > 0;
  show_minutes = minutes > 0 || (!with_seconds && hours == 0);
  show_seconds = with_seconds && (seconds > 0 || (hours == 0 && minutes == 0));
  /* a duration: the parts in that order, a space between */
  if (show_hours)
    g_string_append_printf (text, ngettext ("%d hr", "%d hrs", hours), hours);
  if (show_minutes)
    {
      if (text->len)
        g_string_append_c (text, ' ');
      g_string_append_printf (text, ngettext ("%d min", "%d mins", minutes), minutes);
    }
  if (show_seconds)
    {
      if (text->len)
        g_string_append_c (text, ' ');
      g_string_append_printf (text, ngettext ("%d sec", "%d secs", seconds), seconds);
    }
  return g_string_free (text, FALSE);
}

/* rounded as a float: halves go up */
static int
round_half_up (double value)
{
  return (int) floor (value + 0.5);
}

/* ---- the colour bar ------------------------------------------------------------------------------------------ */

#define STATS_TYPE_BAR (stats_bar_get_type ())
G_DECLARE_FINAL_TYPE (StatsBar, stats_bar, STATS, BAR, GtkWidget)

struct _StatsBar
{
  GtkWidget parent;
  double ratios[4];             /* incoming, outgoing, missed, blocked: shares of the width */
};

G_DEFINE_FINAL_TYPE (StatsBar, stats_bar, GTK_TYPE_WIDGET)

static void
rgba_from (guint32 argb, GdkRGBA *rgba)
{
  rgba->red = ((argb >> 16) & 0xff) / 255.0;
  rgba->green = ((argb >> 8) & 0xff) / 255.0;
  rgba->blue = (argb & 0xff) / 255.0;
  rgba->alpha = ((argb >> 24) & 0xff) / 255.0;
}

/* the bar: the four parts from the start, each end cut to the whole pixel, then the rest of the
 * width in the background colour, all inside a pill (the clip path's radius is half the height) */
static void
stats_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  StatsBar *self = STATS_BAR (widget);
  static const guint32 colors[5] = { BAR_INCOMING, BAR_OUTGOING, BAR_MISSED, BAR_BLOCKED, BAR_REST };
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  int scale = vibeos_device_scale (widget), pixels = (int) round (width * scale), left = 0, right = 0;
  GskRoundedRect clip;

  if (width <= 0 || height <= 0)
    return;
  gsk_rounded_rect_init_from_rect (&clip, &GRAPHENE_RECT_INIT (0, 0, width, height), height / 2);
  gtk_snapshot_push_rounded_clip (snapshot, &clip);
  for (int i = 0; i < 5; i++)
    {
      GdkRGBA color;

      right = i < 4 ? right + (int) (pixels * self->ratios[i]) : pixels;
      if (left >= right)
        continue;
      rgba_from (colors[i], &color);
      gtk_snapshot_append_color (snapshot, &color,
                                 &GRAPHENE_RECT_INIT ((double) left / scale, 0, (double) (right - left) / scale,
                                                      height));
      left = right;
    }
  gtk_snapshot_pop (snapshot);
}

static void
stats_bar_class_init (StatsBarClass *klass)
{
  GTK_WIDGET_CLASS (klass)->snapshot = stats_bar_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "stats-bar");
}

static void
stats_bar_init (StatsBar *self)
{
}

static GtkWidget *
stats_bar_new (int height, int margin, double incoming, double outgoing, double missed, double blocked)
{
  StatsBar *self = g_object_new (STATS_TYPE_BAR, NULL);

  self->ratios[0] = isfinite (incoming) ? incoming : 0;
  self->ratios[1] = isfinite (outgoing) ? outgoing : 0;
  self->ratios[2] = isfinite (missed) ? missed : 0;
  self->ratios[3] = isfinite (blocked) ? blocked : 0;
  gtk_widget_set_size_request (GTK_WIDGET (self), -1, height);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_margin_top (GTK_WIDGET (self), margin);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), margin);
  return GTK_WIDGET (self);
}

/* ---- a number's statistics ---------------------------------------------------------------------------------- */

#define STATS_TYPE_DETAIL (stats_detail_get_type ())
G_DECLARE_FINAL_TYPE (StatsDetail, stats_detail, STATS, DETAIL, GtkWidget)

struct _StatsDetail
{
  GtkWidget parent;
  GtkWidget *box;               /* the toolbar, the caller, the cards */
  GtkWidget *caller;
  Contacts *contacts;
  VibeosSnackbar *snackbar;
  Stat *stat, *total;           /* the number's and every number's, as they were when it opened */
  char *uid;                    /* the contact's, or NULL */
};

G_DEFINE_FINAL_TYPE (StatsDetail, stats_detail, GTK_TYPE_WIDGET)

enum { DETAIL_CLOSED, DETAIL_EDIT_NUMBER, N_DETAIL_SIGNALS };
static guint detail_signals[N_DETAIL_SIGNALS];

static void
detail_close (StatsDetail *self)
{
  g_signal_emit (self, detail_signals[DETAIL_CLOSED], 0);
}

static void
on_detail_dialed (guint id, const GError *error, gpointer data)
{
  StatsDetail *self = data;

  if (error)
    vibeos_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
  else
    phone_start_app (INCALL_APP_ID);
  g_object_unref (self);
}

/* the call button: a voice call at once */
static void
on_detail_call (StatsDetail *self)
{
  calls_client_dial (calls_client_get (), self->stat->number, on_detail_dialed, g_object_ref (self));
}

/* the picture: a contact's card */
static void
on_detail_photo (StatsDetail *self)
{
  if (self->uid)
    contacts_app_view (GTK_WIDGET (self), self->uid, self->snackbar);
}

/* the picture, the name and the number, the call button */
static GtkWidget *
make_caller (StatsDetail *self)
{
  const Stat *stat = self->stat;
  const char *withheld = phone_number_presentation_name (phone_number_presentation (stat->number,
                                                                                    stat->presentation));
  const Contact *contact = withheld ? NULL : contacts_lookup (self->contacts, stat->number, country ());
  g_autofree char *formatted = withheld ? NULL : phone_number_format (stat->number, country ());
  gboolean is_contact = contact && contact->name && *contact->name;
  const char *shown = withheld ? withheld : formatted;
  g_autofree char *secondary = NULL;
  GtkWidget *caller = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = dialer_avatar_new (PHOTO);
  GtkWidget *badge, *texts, *label;

  g_clear_pointer (&self->uid, g_free);
  if (is_contact)
    {
      const char *kind = contact_number_label (contact, stat->number, country ());

      self->uid = g_strdup (contact->uid);
      /* the number's kind, a space, the number ("Mobile +49 176 …") */
      secondary = kind && *kind ? g_strdup_printf ("%s %s", kind, shown) : g_strdup (shown);
    }

  gtk_widget_set_margin_top (caller, CALLER_TOP + CALLER_PAD_TOP_START);
  gtk_widget_set_margin_start (caller, CALLER_PAD_TOP_START);
  gtk_widget_set_margin_bottom (caller, CALLER_PAD_BOTTOM_END);
  gtk_widget_set_margin_end (caller, CALLER_PAD_BOTTOM_END);

  /* the badge: the photo, no padding; only a contact's opens anything */
  dialer_avatar_set (DIALER_AVATAR (avatar), is_contact ? contact->uid : shown, is_contact ? contact->name : NULL,
                     is_contact ? contact->photo : NULL);
  badge = vibeos_pressable_new (avatar);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (badge), VIBEOS_RIPPLE_CIRCLE, PHOTO / 2.0);
  vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (badge), &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (badge, is_contact);
  if (is_contact)
    gtk_accessible_update_property (GTK_ACCESSIBLE (badge), GTK_ACCESSIBLE_PROPERTY_LABEL, contact->name, -1);
  g_signal_connect_swapped (badge, "clicked", G_CALLBACK (on_detail_photo), self);
  gtk_box_append (GTK_BOX (caller), badge);

  texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (texts, CALLER_TEXT);
  label = text_label (is_contact ? contact->name : shown, "dialer-primary-text", 16);
  gtk_box_append (GTK_BOX (texts), label);
  /* under a number that is no contact's, its place; there is none here (no geocoder): nothing */
  if (secondary)
    gtk_box_append (GTK_BOX (texts), text_label (secondary, "dialer-secondary-text", 14));
  gtk_box_append (GTK_BOX (caller), texts);

  /* a call button, for a number that can be called */
  if (stat_callable (stat))
    {
      GtkWidget *call = vibeos_icon_button_new ("dialer-call-symbolic");

      gtk_widget_set_size_request (call, BUTTON, BUTTON);
      gtk_widget_set_valign (call, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (call, "dialer-icon");
      vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (call), &(GdkRGBA) { 1, 1, 1, 0.06 });
      gtk_accessible_update_property (GTK_ACCESSIBLE (call), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Call"), -1);
      g_signal_connect_swapped (call, "clicked", G_CALLBACK (on_detail_call), self);
      gtk_box_append (GTK_BOX (caller), call);
    }
  return caller;
}

/* a line of a card: the call type's small icon, "Incoming: 1 min", the percent */
static GtkWidget *
detail_line (CallType type, const char *value, int percent)
{
  static const struct { CallType type; const char *icon, *css_class, *format; } kinds[] = {
    { CALL_TYPE_INCOMING, "dialer-call-received-symbolic", "stats-incoming", N_("Incoming: %s") },
    { CALL_TYPE_OUTGOING, "dialer-call-made-symbolic", "stats-outgoing", N_("Outgoing: %s") },
    { CALL_TYPE_MISSED, "dialer-call-missed-symbolic", "stats-missed", N_("Missed: %s") },
    { CALL_TYPE_BLOCKED, "dialer-block-symbolic", "stats-blocked", N_("Blocked: %s") },
  };
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon, *label;
  g_autofree char *text = NULL;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (kinds) - 1 && kinds[i].type != type; i++)
    ;
  text = g_strdup_printf (_(kinds[i].format), value);
  gtk_widget_set_margin_bottom (line, ICON_MARGIN);

  /* the call-type icon, centred on the line */
  icon = gtk_image_new_from_icon_name (kinds[i].icon);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), TYPE_ICON);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (icon, ICON_MARGIN);
  gtk_widget_add_css_class (icon, kinds[i].css_class);
  gtk_box_append (GTK_BOX (line), icon);

  label = text_label (text, "dialer-primary-text", 16);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_BASELINE_FILL);
  gtk_widget_set_margin_start (label, ICON_MARGIN);
  gtk_box_append (GTK_BOX (line), label);
  if (percent >= 0)
    {
      g_autofree char *shown = g_strdup_printf (_("%d%%"), percent);
      GtkWidget *right = text_label (shown, "dialer-secondary-text", 14);

      gtk_label_set_ellipsize (GTK_LABEL (right), PANGO_ELLIPSIZE_NONE);
      gtk_widget_set_valign (right, GTK_ALIGN_BASELINE_FILL);
      gtk_widget_set_margin_start (right, ICON_MARGIN);
      gtk_box_append (GTK_BOX (line), right);
    }
  return line;
}

/* a duration line: none for a type never talked on */
static void
add_duration_line (GtkWidget *lines, CallType type, gint64 duration, gint64 total)
{
  g_autofree char *value = NULL;

  if (duration == 0)
    return;
  value = duration_string (duration, TRUE);
  gtk_box_append (GTK_BOX (lines), detail_line (type, value, round_half_up (100.0 * duration / total)));
}

/* a count line */
static void
add_count_line (GtkWidget *lines, CallType type, guint count, guint total)
{
  g_autofree char *value = NULL;

  if (count == 0 && total > 0)
    return;
  value = count_string (count);
  gtk_box_append (GTK_BOX (lines), detail_line (type, value, total ? round_half_up (100.0 * count / total) : 0));
}

/* the average line: no percent */
static void
add_average_line (GtkWidget *lines, CallType type, gint64 duration, guint count)
{
  g_autofree char *value = NULL;

  if (count == 0)
    return;
  value = duration_string (round_half_up ((double) duration / count), TRUE);
  gtk_box_append (GTK_BOX (lines), detail_line (type, value, -1));
}

/* "Of total" or "This number" at the start of a line, the value at its end, on one baseline */
static GtkWidget *
total_line (const char *title, const char *value)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *start = text_label (title, "dialer-primary-text", 16);
  GtkWidget *end = text_label (value, "dialer-primary-text", 16);

  gtk_widget_set_hexpand (start, TRUE);
  gtk_widget_set_valign (start, GTK_ALIGN_BASELINE_FILL);
  gtk_label_set_ellipsize (GTK_LABEL (end), PANGO_ELLIPSIZE_NONE);
  gtk_widget_set_valign (end, GTK_ALIGN_BASELINE_FILL);
  gtk_box_append (GTK_BOX (line), start);
  gtk_box_append (GTK_BOX (line), end);
  return line;
}

/* a card: its padding and its title */
static GtkWidget *
make_card (const char *title, GtkWidget **content)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label;

  gtk_widget_add_css_class (card, "stats-card");
  gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
  *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (*content, CARD_PAD);
  gtk_widget_set_margin_end (*content, CARD_PAD);
  gtk_widget_set_margin_top (*content, CARD_PAD);
  gtk_widget_set_margin_bottom (*content, CARD_PAD);
  label = text_label (title, "stats-card-title", 16);
  gtk_widget_set_margin_bottom (label, gtk_widget_get_margin_bottom (label) + CARD_TITLE_GAP);
  gtk_box_append (GTK_BOX (*content), label);
  gtk_box_append (GTK_BOX (card), *content);
  return card;
}

static GtkWidget *
card_gap (void)
{
  GtkWidget *gap = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_set_size_request (gap, -1, CARD_GAP);
  return gap;
}

/* then the three cards */
static GtkWidget *
make_cards (StatsDetail *self)
{
  const Stat *stat = self->stat, *total = self->total;
  gint64 duration = stat_duration (stat), all_duration = stat_duration (total);
  guint count = stat_count (stat), all_count = stat_count (total);
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *card, *content, *lines;

  gtk_widget_set_margin_start (info, INFO_PAD_X);
  gtk_widget_set_margin_end (info, INFO_PAD_X);
  gtk_widget_set_margin_top (info, INFO_PAD_Y);
  gtk_widget_set_margin_bottom (info, INFO_PAD_Y);

  /* Call durations: the card stays when the number was never talked to, with nothing in it (its container goes) */
  card = make_card (_("Call durations"), &content);
  if (duration != 0)
    {
      g_autofree char *of_total = duration_string (all_duration, TRUE);
      g_autofree char *this_number = duration_string (duration, TRUE);

      gtk_box_append (GTK_BOX (content), total_line (_("Of total"), of_total));
      gtk_box_append (GTK_BOX (content),
                      stats_bar_new (DETAIL_BAR, DETAIL_BAR_MARGIN, (double) stat->in_duration / all_duration,
                                     (double) stat->out_duration / all_duration, 0, 0));
      gtk_box_append (GTK_BOX (content), total_line (_("This number"), this_number));
      gtk_box_append (GTK_BOX (content),
                      stats_bar_new (DETAIL_BAR, DETAIL_BAR_MARGIN, (double) stat->in_duration / duration,
                                     (double) stat->out_duration / duration, 0, 0));
      lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_margin_top (lines, LINES_TOP);
      add_duration_line (lines, CALL_TYPE_INCOMING, stat->in_duration, duration);
      add_duration_line (lines, CALL_TYPE_OUTGOING, stat->out_duration, duration);
      gtk_box_append (GTK_BOX (content), lines);
    }
  else
    gtk_widget_set_visible (content, FALSE);
  gtk_box_append (GTK_BOX (info), card);
  gtk_box_append (GTK_BOX (info), card_gap ());

  /* Call count */
  card = make_card (_("Call count"), &content);
  {
    g_autofree char *of_total = count_string (all_count);
    g_autofree char *this_number = count_string (count);

    gtk_box_append (GTK_BOX (content), total_line (_("Of total"), of_total));
    gtk_box_append (GTK_BOX (content),
                    stats_bar_new (DETAIL_BAR, DETAIL_BAR_MARGIN, (double) stat->in_count / all_count,
                                   (double) stat->out_count / all_count, (double) stat->missed_count / all_count,
                                   (double) stat->blocked_count / all_count));
    gtk_box_append (GTK_BOX (content), total_line (_("This number"), this_number));
    gtk_box_append (GTK_BOX (content),
                    stats_bar_new (DETAIL_BAR, DETAIL_BAR_MARGIN, (double) stat->in_count / count,
                                   (double) stat->out_count / count, (double) stat->missed_count / count,
                                   (double) stat->blocked_count / count));
    lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top (lines, LINES_TOP);
    add_count_line (lines, CALL_TYPE_INCOMING, stat->in_count, count);
    add_count_line (lines, CALL_TYPE_OUTGOING, stat->out_count, count);
    add_count_line (lines, CALL_TYPE_MISSED, stat->missed_count, count);
    add_count_line (lines, CALL_TYPE_BLOCKED, stat->blocked_count, count);
    gtk_box_append (GTK_BOX (content), lines);
  }
  gtk_box_append (GTK_BOX (info), card);
  gtk_box_append (GTK_BOX (info), card_gap ());

  /* Average call duration: just its title when nothing was talked on */
  card = make_card (_("Average call duration"), &content);
  add_average_line (content, CALL_TYPE_INCOMING, stat->in_duration, stat->in_count);
  add_average_line (content, CALL_TYPE_OUTGOING, stat->out_duration, stat->out_count);
  gtk_box_append (GTK_BOX (info), card);
  return info;
}

static GtkWidget *
action_row (StatsDetail *self, const char *icon_name, const char *text, GCallback callback)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = text_label (text, "dialer-secondary-text", 14);
  GtkWidget *row;

  gtk_widget_set_size_request (box, -1, ACTION_PAD_V + ACTION_ICON + ACTION_PAD_V);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), ACTION_ICON);
  gtk_widget_set_margin_start (icon, ACTION_PAD);
  gtk_widget_add_css_class (icon, "dialer-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, ACTION_PAD);
  gtk_widget_set_margin_end (label, ACTION_PAD);
  gtk_box_append (GTK_BOX (box), label);
  row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  g_signal_connect_swapped (row, "clicked", callback, self);
  return row;
}

/* the number to the clipboard, with a toast */
static void
on_detail_copy (StatsDetail *self)
{
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), self->stat->number);
  vibeos_snackbar_show (self->snackbar, _("Text copied"), NULL, NULL, NULL, VIBEOS_SNACKBAR_SHORT);
}

/* the dialpad with the number, in the main screen */
static void
on_detail_edit (StatsDetail *self)
{
  g_signal_emit (self, detail_signals[DETAIL_EDIT_NUMBER], 0, self->stat->number);
}

/* the separator (#12000000: black, so on this dark theme all but invisible) and the two rows, for a number that can
 * be called; a SIP address has no Edit number before call */
static void
add_actions (StatsDetail *self, GtkWidget *list)
{
  GtkWidget *divider;

  if (!stat_callable (self->stat))
    return;
  divider = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (divider, "stats-separator");
  gtk_widget_set_size_request (divider, -1, 1);
  gtk_widget_set_margin_top (divider, DIVIDER_MARGIN);
  gtk_widget_set_margin_bottom (divider, DIVIDER_MARGIN);
  gtk_box_append (GTK_BOX (list), divider);
  gtk_box_append (GTK_BOX (list), action_row (self, "dialer-copy-symbolic", _("Copy number"),
                                              G_CALLBACK (on_detail_copy)));
  if (!strchr (self->stat->number, '@'))
    gtk_box_append (GTK_BOX (list), action_row (self, "dialer-edit-symbolic", _("Edit number before call"),
                                                G_CALLBACK (on_detail_edit)));
}

/* a contact added or changed: the caller again */
static void
on_detail_contacts_changed (StatsDetail *self)
{
  GtkWidget *caller = make_caller (self);

  gtk_box_insert_child_after (GTK_BOX (self->box), caller, self->caller);
  gtk_box_remove (GTK_BOX (self->box), self->caller);
  self->caller = caller;
}

static gboolean
on_detail_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, StatsDetail *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  detail_close (self);
  return TRUE;
}

/* the toolbar: ✕ (close) and "Contact statistics details" */
static GtkWidget *
make_detail_bar (StatsDetail *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *close = vibeos_icon_button_new ("dialer-close-symbolic");
  GtkWidget *title = gtk_label_new (_("Contact statistics details"));

  gtk_widget_add_css_class (bar, "dialer-history-bar");
  gtk_widget_set_size_request (bar, -1, BAR_HEIGHT);
  gtk_widget_set_size_request (close, NAV_SIZE, NAV_SIZE);
  gtk_widget_set_valign (close, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (close, "dialer-icon");
  /* named "Back", the button the shell's back gesture clicks; "Close" is its description */
  gtk_accessible_update_property (GTK_ACCESSIBLE (close), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Close"), -1);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (detail_close), self);
  gtk_box_append (GTK_BOX (bar), close);

  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (title), TRUE);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_set_margin_start (title, TITLE_START - NAV_SIZE);
  gtk_widget_add_css_class (title, "dialer-toolbar-title");
  gtk_box_append (GTK_BOX (bar), title);
  return bar;
}

static void
stats_detail_dispose (GObject *object)
{
  StatsDetail *self = STATS_DETAIL (object);

  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->stat, stat_free);
  g_clear_pointer (&self->total, stat_free);
  g_clear_pointer (&self->uid, g_free);
  G_OBJECT_CLASS (stats_detail_parent_class)->dispose (object);
}

static void
stats_detail_class_init (StatsDetailClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = stats_detail_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "call-stats-detail");
  detail_signals[DETAIL_CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                                NULL, NULL, G_TYPE_NONE, 0);
  detail_signals[DETAIL_EDIT_NUMBER] = g_signal_new ("edit-number", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
stats_detail_init (StatsDetail *self)
{
}

/* the caller stays put; the cards and the rows under them scroll */
static GtkWidget *
stats_detail_new (Contacts *contacts, VibeosSnackbar *snackbar, const Stat *stat, const Stat *total)
{
  StatsDetail *self = g_object_new (STATS_TYPE_DETAIL, NULL);
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkWidget *scroller, *list;

  self->contacts = g_object_ref (contacts);
  self->snackbar = snackbar;
  self->stat = stat_copy (stat);
  self->total = stat_copy (total);

  gtk_widget_add_css_class (GTK_WIDGET (self), "dialer-history");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_box_append (GTK_BOX (self->box), make_detail_bar (self));
  self->caller = make_caller (self);
  gtk_box_append (GTK_BOX (self->box), self->caller);

  list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (list, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (list), make_cards (self));
  add_actions (self, list);
  scroller = spongy_scroll_view_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroller), list);
  gtk_box_append (GTK_BOX (self->box), scroller);

  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_detail_contacts_changed), self);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_detail_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  return GTK_WIDGET (self);
}

/* ---- the tab ------------------------------------------------------------------------------------------------- */

struct _CallStats
{
  GtkWidget parent;
  GtkWidget *stack, *scroller, *header, *rows, *empty;
  Contacts *contacts;
  VibeosSnackbar *snackbar;
  GPtrArray *stats;             /* Stat: every number in the log, the newest first */
  Stat total;                   /* all of them together */
  GCancellable *loading;
  gboolean loaded, by_duration;
};

G_DEFINE_FINAL_TYPE (CallStats, call_stats, GTK_TYPE_WIDGET)

enum { SHOW_SCREEN, EDIT_NUMBER, N_SIGNALS };
static guint signals[N_SIGNALS];

/* what a row and its context menu act on */
typedef struct
{
  CallStats *page;              /* not a reference: the row is the page's */
  Stat *stat;
  double press_x, press_y;
} RowData;

static void
row_data_free (RowData *data)
{
  stat_free (data->stat);
  g_free (data);
}

static void
on_detail_edit_number (CallStats *self, const char *number)
{
  g_signal_emit (self, signals[EDIT_NUMBER], 0, number);
}

/* a tap: the number's statistics over Call history */
static void
on_row_clicked (GtkWidget *row, RowData *data)
{
  CallStats *self = data->page;
  GtkWidget *screen = stats_detail_new (self->contacts, self->snackbar, data->stat, &self->total);

  g_signal_connect_swapped (screen, "edit-number", G_CALLBACK (on_detail_edit_number), self);
  g_signal_emit (self, signals[SHOW_SCREEN], 0, screen);
}

/* the context menu's own copy of the number */
typedef struct
{
  CallStats *page;
  char *number;
} MenuTarget;

static void
menu_target_free (gpointer data)
{
  MenuTarget *target = data;

  g_object_unref (target->page);
  g_free (target->number);
  g_free (target);
}

static void
on_menu_copy (gpointer data)
{
  MenuTarget *target = data;

  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (target->page)), target->number);
  vibeos_snackbar_show (target->page->snackbar, _("Text copied"), NULL, NULL, NULL, VIBEOS_SNACKBAR_SHORT);
}

static void
on_menu_edit (gpointer data)
{
  MenuTarget *target = data;

  g_signal_emit (target->page, signals[EDIT_NUMBER], 0, target->number);
}

/* a row's context menu: the number as it is, Copy number and, for a number that can be called and is no
 * SIP address, Edit number before call; a number the network did not send has none */
static void
on_row_long_pressed (GtkWidget *row, RowData *data)
{
  DialerPopupMenu *menu;
  MenuTarget *target;

  if (!*data->stat->number)
    return;
  menu = dialer_popup_menu_new ();
  target = g_new0 (MenuTarget, 1);
  target->page = g_object_ref (data->page);
  target->number = g_strdup (data->stat->number);
  g_object_set_data_full (G_OBJECT (menu), "menu-target", target, menu_target_free);
  dialer_popup_menu_set_header (menu, target->number);
  dialer_popup_menu_append (menu, _("Copy number"), on_menu_copy, target);
  if (stat_callable (data->stat) && !strchr (data->stat->number, '@'))
    dialer_popup_menu_append (menu, _("Edit number before call"), on_menu_edit, target);
  dialer_popup_menu_present_at (menu, row, data->press_x, data->press_y);
}

static void
on_row_pressed (GtkGestureClick *click, int n_press, double x, double y, RowData *data)
{
  data->press_x = x;
  data->press_y = y;
}

static void
on_row_photo (GtkWidget *badge, const char *uid)
{
  CallStats *self = CALL_STATS (gtk_widget_get_ancestor (badge, CALL_TYPE_STATS));

  if (uid && self)
    contacts_app_view (badge, uid, self->snackbar);
}

/* a row: @first is the top row's, for the bar's length */
static GtkWidget *
make_row (CallStats *self, const Stat *stat, const Stat *first)
{
  const char *withheld = phone_number_presentation_name (phone_number_presentation (stat->number,
                                                                                    stat->presentation));
  const Contact *contact = withheld ? NULL : contacts_lookup (self->contacts, stat->number, country ());
  g_autofree char *formatted = withheld ? NULL : phone_number_format (stat->number, country ());
  gboolean is_contact = contact && contact->name && *contact->name;
  const char *shown = withheld ? withheld : formatted;
  const char *kind = is_contact ? contact_number_label (contact, stat->number, country ()) : NULL;
  gboolean by = self->by_duration;
  double value = stat_value (stat, by), ratio = value / stat_value (first, by);
  double in = (by ? stat->in_duration : stat->in_count) * ratio / value;
  double out = (by ? stat->out_duration : stat->out_count) * ratio / value;
  double missed = by ? 0 : stat->missed_count * ratio / value;
  double blocked = by ? 0 : stat->blocked_count * ratio / value;
  g_autofree char *percent = g_strdup_printf ("%.1f%%", (float) (value * 100.0 / stat_value (&self->total, by)));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = dialer_avatar_new (PHOTO - PHOTO_PAD_TOP);
  GtkWidget *badge, *texts, *line, *label, *row;
  GtkGesture *press;
  RowData *data;

  gtk_widget_set_margin_start (box, ROW_PAD);
  gtk_widget_set_margin_end (box, ROW_PAD);
  gtk_widget_set_margin_top (box, ROW_PAD);
  gtk_widget_set_margin_bottom (box, ROW_PAD);

  /* the badge's picture, PHOTO_PAD_TOP smaller: that far down, centred across */
  dialer_avatar_set (DIALER_AVATAR (avatar), is_contact ? contact->uid : shown, is_contact ? contact->name : NULL,
                     is_contact ? contact->photo : NULL);
  gtk_widget_set_margin_top (avatar, PHOTO_PAD_TOP);
  gtk_widget_set_margin_start (avatar, PHOTO_PAD_TOP / 2);
  gtk_widget_set_margin_end (avatar, PHOTO_PAD_TOP / 2);
  badge = vibeos_pressable_new (avatar);
  gtk_widget_set_valign (badge, GTK_ALIGN_START);
  gtk_widget_set_margin_end (badge, INFO_START);
  /* a contact's opens its card; anyone else's (no contact URI: a disabled badge) takes the tap and does nothing */
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (badge), VIBEOS_RIPPLE_CIRCLE, PHOTO / 2.0);
  vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (badge), &(GdkRGBA) { 1, 1, 1, is_contact ? 0.06 : 0 });
  if (is_contact)
    {
      gtk_accessible_update_property (GTK_ACCESSIBLE (badge), GTK_ACCESSIBLE_PROPERTY_LABEL, contact->name, -1);
      g_object_set_data_full (G_OBJECT (badge), "uid", g_strdup (contact->uid), g_free);
    }
  g_signal_connect (badge, "clicked", G_CALLBACK (on_row_photo), g_object_get_data (G_OBJECT (badge), "uid"));
  gtk_box_append (GTK_BOX (box), badge);

  texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (texts, TRUE);

  /* the name (or the number), and its share at the end on the same baseline */
  line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  label = text_label (is_contact ? contact->name : shown, "dialer-primary-text", 16);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_BASELINE_FILL);
  gtk_box_append (GTK_BOX (line), label);
  label = text_label (percent, "dialer-secondary-text", 14);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_NONE);
  gtk_label_set_xalign (GTK_LABEL (label), 1);
  gtk_widget_set_valign (label, GTK_ALIGN_BASELINE_FILL);
  gtk_box_append (GTK_BOX (line), label);
  gtk_box_append (GTK_BOX (texts), line);

  /* by duration incoming and outgoing; by count missed and blocked too */
  gtk_box_append (GTK_BOX (texts), stats_bar_new (BAR, BAR_MARGIN, in, out, missed, blocked));

  /* a contact's number: its kind and the number. Anyone else's: the place, which there is none of (no geocoder),
   * so an empty line that keeps its height */
  line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  if (is_contact && kind && *kind)
    {
      label = text_label (kind, "dialer-secondary-text", 14);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_NONE);
      gtk_widget_set_valign (label, GTK_ALIGN_BASELINE_FILL);
      gtk_widget_set_margin_end (label, LABEL_END);
      gtk_box_append (GTK_BOX (line), label);
    }
  label = text_label (is_contact ? shown : "", "dialer-secondary-text", 12);
  gtk_widget_add_css_class (label, "stats-number");
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_BASELINE_FILL);
  gtk_box_append (GTK_BOX (line), label);
  gtk_box_append (GTK_BOX (texts), line);
  gtk_box_append (GTK_BOX (box), texts);

  /* a bounded ripple; a tap the number's statistics, a long press the context menu */
  row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  vibeos_pressable_set_ripple_color (VIBEOS_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  data = g_new0 (RowData, 1);
  data->page = self;
  data->stat = stat_copy (stat);
  g_object_set_data_full (G_OBJECT (row), "row-data", data, (GDestroyNotify) row_data_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row_clicked), data);
  g_signal_connect (row, "long-pressed", G_CALLBACK (on_row_long_pressed), data);
  /* only watches: the pressable takes the touch */
  press = gtk_gesture_click_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (press), GTK_PHASE_CAPTURE);
  g_signal_connect (press, "pressed", G_CALLBACK (on_row_pressed), data);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (press));
  return row;
}

static int
compare_stats (gconstpointer a, gconstpointer b, gpointer by_duration)
{
  double va = stat_value (*(Stat **) a, GPOINTER_TO_INT (by_duration));
  double vb = stat_value (*(Stat **) b, GPOINTER_TO_INT (by_duration));

  return va < vb ? 1 : va > vb ? -1 : 0;
}

/* the rows with anything to show (by duration a number talked to, by count any), most first, under
 * "Total: 12 calls, 1 hr 5 mins" */
static void
show (CallStats *self)
{
  g_autoptr(GPtrArray) shown = g_ptr_array_new ();
  g_autofree char *calls = count_string (stat_count (&self->total));
  g_autofree char *duration = duration_string (stat_duration (&self->total), FALSE);
  g_autofree char *header = g_strdup_printf (_("Total: %1$s, %2$s"), calls, duration);
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->rows)))
    gtk_box_remove (GTK_BOX (self->rows), child);
  for (guint i = 0; self->stats && i < self->stats->len; i++)
    if (stat_value (self->stats->pdata[i], self->by_duration) > 0)
      g_ptr_array_add (shown, self->stats->pdata[i]);
  /* stable: equal ones stay newest first */
  g_ptr_array_sort_with_data (shown, compare_stats, GINT_TO_POINTER (self->by_duration));
  for (guint i = 0; i < shown->len; i++)
    gtk_box_append (GTK_BOX (self->rows), make_row (self, shown->pdata[i], shown->pdata[0]));
  gtk_label_set_text (GTK_LABEL (self->header), header);
  gtk_widget_set_visible (self->header, shown->len > 0);
  /* the empty view only once the log was read */
  gtk_stack_set_visible_child (GTK_STACK (self->stack), self->loaded && !shown->len ? self->empty : self->scroller);
}

/* one item per number, the same line in another format
 * being the same number; every withheld call is one item ("" is "") */
static void
on_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  CallStats *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) entries = calls_client_get_call_log_finish (CALLS_CLIENT (source), result, &error);
  g_autoptr(GHashTable) by_number = NULL;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->loading);
  if (!entries)
    {
      g_warning ("call statistics: %s", error->message);
      return;
    }
  by_number = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_clear_pointer (&self->stats, g_ptr_array_unref);
  self->stats = g_ptr_array_new_with_free_func ((GDestroyNotify) stat_free);
  memset (&self->total, 0, sizeof self->total);
  for (guint i = 0; i < entries->len; i++)
    {
      const CallLogEntry *entry = entries->pdata[i];
      const char *number = entry->number ? entry->number : "";
      gboolean withheld = !*number
                          || phone_number_presentation_name (phone_number_presentation (number, entry->presentation));
      g_autofree char *key = withheld ? g_strdup ("") : phone_number_canonical (number, country ());
      Stat *stat = g_hash_table_lookup (by_number, key);

      if (!stat)
        {
          stat = g_new0 (Stat, 1);
          stat->number = g_strdup (withheld ? "" : number);
          stat->presentation = entry->presentation;
          g_ptr_array_add (self->stats, stat);
          g_hash_table_insert (by_number, g_steal_pointer (&key), stat);
        }
      stat_add (stat, entry->type, entry->duration);
    }
  for (guint i = 0; i < self->stats->len; i++)
    stat_merge (&self->total, self->stats->pdata[i]);
  self->loaded = TRUE;
  show (self);
}

static void
load (CallStats *self)
{
  if (self->loading)
    g_cancellable_cancel (self->loading);
  g_clear_object (&self->loading);
  self->loading = g_cancellable_new ();
  calls_client_get_call_log (calls_client_get (), self->loading, on_loaded, self);
}

/* the contacts changed: the names and pictures again */
static void
on_contacts_changed (CallStats *self)
{
  if (self->loaded)
    show (self);
}

gboolean
call_stats_get_sort_by_duration (CallStats *self)
{
  g_return_val_if_fail (CALL_IS_STATS (self), TRUE);
  return self->by_duration;
}

void
call_stats_set_sort_by_duration (CallStats *self, gboolean by_duration)
{
  g_return_if_fail (CALL_IS_STATS (self));
  if (self->by_duration == !!by_duration)
    return;
  self->by_duration = !!by_duration;
  show (self);
}

static void
call_stats_dispose (GObject *object)
{
  CallStats *self = CALL_STATS (object);

  if (self->loading)
    g_cancellable_cancel (self->loading);
  g_clear_object (&self->loading);
  g_signal_handlers_disconnect_by_data (calls_client_get (), self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->stack, gtk_widget_unparent);
  g_clear_pointer (&self->stats, g_ptr_array_unref);
  G_OBJECT_CLASS (call_stats_parent_class)->dispose (object);
}

static void
call_stats_class_init (CallStatsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = call_stats_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "call-stats");
  signals[SHOW_SCREEN] = g_signal_new ("show-screen", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 1, GTK_TYPE_WIDGET);
  signals[EDIT_NUMBER] = g_signal_new ("edit-number", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
call_stats_init (CallStats *self)
{
  self->by_duration = TRUE;
}

/* the band over the list (the filter band, the total) scrolls away with the list, and comes back only at the top,
 * not on any scroll up */
GtkWidget *
call_stats_new (Contacts *contacts, VibeosSnackbar *snackbar)
{
  CallStats *self = g_object_new (CALL_TYPE_STATS, NULL);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *band = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  self->contacts = g_object_ref (contacts);
  self->snackbar = snackbar;
  self->stack = gtk_stack_new ();
  gtk_widget_set_parent (self->stack, GTK_WIDGET (self));

  gtk_widget_set_valign (content, GTK_ALIGN_START);
  gtk_widget_set_size_request (band, -1, BAND_HEIGHT);
  gtk_box_append (GTK_BOX (content), band);
  /* the total over the list */
  self->header = text_label ("", "stats-header", 14);
  gtk_widget_set_margin_start (self->header, OUTER_MARGIN + OUTER_MARGIN);
  gtk_widget_set_margin_end (self->header, OUTER_MARGIN);
  gtk_widget_set_margin_top (self->header, gtk_widget_get_margin_top (self->header) + OUTER_MARGIN);
  gtk_widget_set_margin_bottom (self->header, gtk_widget_get_margin_bottom (self->header) + OUTER_MARGIN);
  gtk_widget_set_visible (self->header, FALSE);
  gtk_box_append (GTK_BOX (content), self->header);
  self->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->rows, LIST_PAD_X);
  gtk_widget_set_margin_end (self->rows, LIST_PAD_X);
  gtk_box_append (GTK_BOX (content), self->rows);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), content);
  gtk_stack_add_child (GTK_STACK (self->stack), self->scroller);

  /* the empty view covers the band too; no "time range" here, so always this line */
  self->empty = empty_view_new ("dialer-empty-call-log-symbolic", _("Your call history is empty"), NULL, NULL, NULL);
  gtk_stack_add_child (GTK_STACK (self->stack), self->empty);

  g_signal_connect_swapped (calls_client_get (), "call-log-changed", G_CALLBACK (load), self);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  load (self);
  return GTK_WIDGET (self);
}
