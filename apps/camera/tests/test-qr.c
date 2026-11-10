/* What the QR sheet makes of a code (cam_qr_classify): zxing's ResultParsers, as QrTextClassifier uses them. */
#include "cam-qr-sheet.h"

typedef struct {
  const char *text;
  CamQrKind   kind;
  const char *data, *uri;
} Case;

static const Case cases[] = {
  { "https://lineageos.org/", CAM_QR_URL, "https://lineageos.org/", "https://lineageos.org/" },
  { "www.example.com/a?b=1", CAM_QR_URL, "www.example.com/a?b=1", "http://www.example.com/a?b=1" },
  { "tel:+49301234567", CAM_QR_TEL, "+49301234567", "tel:+49301234567" },
  { "SMSTO:+4930123:hello there", CAM_QR_SMS, "+4930123", "sms:+4930123?body=hello%20there" },
  { "sms:+4930123?body=hi", CAM_QR_SMS, "+4930123", "sms:+4930123?body=hi" },
  { "mailto:a@b.de?subject=x", CAM_QR_EMAIL, "a@b.de", "mailto:a@b.de?subject=x" },
  { "MATMSG:TO:a@b.de;SUB:Hi;BODY:Text;;", CAM_QR_EMAIL, "a@b.de", "mailto:a%40b.de?subject=Hi&body=Text" },
  { "someone@example.org", CAM_QR_EMAIL, "someone@example.org", "mailto:someone@example.org" },
  { "geo:52.52,13.405", CAM_QR_GEO, "52.52, 13.405", "geo:52.52,13.405" },
  { "9783161484100", CAM_QR_ISBN, "9783161484100", "https://isbnsearch.org/isbn/9783161484100" },
  { "4006381333931", CAM_QR_PRODUCT, "4006381333931", "https://www.barcodelookup.com/4006381333931" },
  { "1M8GDM9AXKP042788", CAM_QR_VIN, "1M8GDM9AXKP042788",
    "https://www.vindecoderz.com/EN/check-lookup/1M8GDM9AXKP042788" },
  { "WIFI:T:WPA;S:Home;P:secret;;", CAM_QR_WIFI, "Home", NULL },
  { "MECARD:N:Doe,John;TEL:+4930;EMAIL:j@d.de;;", CAM_QR_CONTACT, "John Doe", NULL },
  { "BEGIN:VCARD\nVERSION:3.0\nFN:Jane Roe\nEND:VCARD", CAM_QR_CONTACT, "Jane Roe", NULL },
  { "BEGIN:VEVENT\nSUMMARY:Party\nDTSTART:20261110T200000Z\nEND:VEVENT", CAM_QR_EVENT, "Party", NULL },
  { "just some words", CAM_QR_TEXT, "just some words", NULL },
  { "4006381333932", CAM_QR_TEXT, "4006381333932", NULL },
};

int
main (void)
{
  int failed = 0;

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      CamQrResult *result = cam_qr_classify (cases[i].text);

      if (result->kind != cases[i].kind || g_strcmp0 (result->data, cases[i].data)
          || g_strcmp0 (result->uri, cases[i].uri))
        {
          g_printerr ("FAIL %s: kind %d data \"%s\" uri \"%s\"\n", cases[i].text, result->kind, result->data,
                      result->uri ? result->uri : "(null)");
          failed++;
        }
      cam_qr_result_free (result);
    }
  g_print ("%d of %d cases right\n", (int) G_N_ELEMENTS (cases) - failed, (int) G_N_ELEMENTS (cases));
  return failed ? 1 : 0;
}
