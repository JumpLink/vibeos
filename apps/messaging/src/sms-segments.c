/* How many text messages a text goes out as (sms-segments.h). */
#include "sms-segments.h"

#include <string.h>

/* 3GPP TS 23.038: the default alphabet, and the extension table reached by an escape (two septets each) */
static const char *gsm_basic =
  "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?"
  "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà";
static const char *gsm_extension = "\f^{}\\[~]|€";

static gboolean
in_set (const char *set, gunichar c)
{
  for (const char *p = set; *p; p = g_utf8_next_char (p))
    if (g_utf8_get_char (p) == c)
      return TRUE;
  return FALSE;
}

SmsLength
sms_segments_count (const char *text)
{
  SmsLength length = { 1, 160, FALSE };
  guint septets = 0, units = 0;

  for (const char *p = text ? text : ""; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (in_set (gsm_basic, c))
        septets += 1;
      else if (in_set (gsm_extension, c))
        septets += 2;
      else
        length.unicode = TRUE;
      /* UTF-16: one unit, two beyond the BMP */
      units += c > 0xffff ? 2 : 1;
    }
  if (!length.unicode)
    {
      length.segments = septets <= 160 ? 1 : (septets + 152) / 153;
      length.remaining = septets <= 160 ? 160 - septets : length.segments * 153 - septets;
    }
  else
    {
      length.segments = units <= 70 ? 1 : (units + 66) / 67;
      length.remaining = units <= 70 ? 70 - units : length.segments * 67 - units;
    }
  return length;
}
