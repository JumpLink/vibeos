/* How many text messages a text goes out as, and how much room the last one has left: in the GSM 7-bit alphabet 160
 * characters, or 153 each when it takes several, the extension table's characters ({ } [ ] ~ \ | ^ €) counting two;
 * anything outside it makes the whole text UCS-2, 70 UTF-16 units, or 67 each. The compose bar's counter shows it. */
#pragma once

#include <glib.h>

typedef struct {
  guint segments;               /* at least 1 */
  guint remaining;              /* what the last one still takes, in its own units */
  gboolean unicode;             /* UCS-2 */
} SmsLength;

SmsLength sms_segments_count (const char *text);
