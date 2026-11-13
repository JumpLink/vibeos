/* cam-qr-sheet: what a scanned code says, and what can be done with it.
 *
 * A modal bottom sheet over a dark scrim: the drag handle, then a card with the action's icon, its title
 * ("Open URL", "Add contact", … or "Text") and the code's data, and a Copy button. A tap on the card runs the
 * action. An action is offered only when an app on the phone can do it (a browser for links and lookups, the
 * Contacts app for a vCard, the Calendar for an event, a handler for tel:, sms:, mailto:, geo:); otherwise the code
 * shows as Text. Left out, because nothing here can back them: Share (no share chooser on the phone), Wi-Fi networks
 * (adding one needs NetworkManager and a confirmation; shown as Text), finding addresses or dates in plain text. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CamQrDismissed) (gpointer data);

/* shows the sheet over the window `parent` is in; `dismissed` runs once it is gone (scanning goes on then) */
void cam_qr_sheet_show (GtkWidget *parent, const char *text, CamQrDismissed dismissed, gpointer data);

/* the classification alone, for tests: the action's title (NULL for plain text), the line shown, and the URI or
 * file content it hands over */
typedef enum {
  CAM_QR_TEXT,
  CAM_QR_URL,
  CAM_QR_CONTACT,
  CAM_QR_EVENT,
  CAM_QR_EMAIL,
  CAM_QR_GEO,
  CAM_QR_TEL,
  CAM_QR_SMS,
  CAM_QR_ISBN,
  CAM_QR_PRODUCT,
  CAM_QR_VIN,
  CAM_QR_WIFI,
} CamQrKind;

typedef struct {
  CamQrKind kind;
  char     *data;           /* the line under the title */
  char     *uri;            /* what is opened (a URL, tel:…), or NULL */
  char     *content;        /* a vCard or iCalendar to hand over as a file, or NULL */
  const char *mime;         /* its type */
} CamQrResult;

CamQrResult *cam_qr_classify     (const char *text);
void         cam_qr_result_free  (CamQrResult *result);

G_END_DECLS
