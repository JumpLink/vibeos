/* navi-back: see navi-back.h */
#include "navi-back.h"

NaviBack
navi_back_decide (const NaviBackState *s)
{
  /* what is on top goes first: a dialog, then the search over the map */
  if (s->dialog)
    return NAVI_BACK_CANCEL_DIALOG;
  if (s->search)
    return NAVI_BACK_CLOSE_SEARCH;
  switch (s->mode) {
  case NAVI_MODE_PLACE:
    return NAVI_BACK_CLOSE_PLACE;
  case NAVI_MODE_ROUTES:
    return NAVI_BACK_TO_PLACE;
  case NAVI_MODE_DRIVE:
    /* the drive is asked about, never dropped by a stray swipe */
    return NAVI_BACK_ASK_END;
  case NAVI_MODE_ARRIVED:
    return NAVI_BACK_END_DRIVE;
  case NAVI_MODE_BROWSE:
    break;
  }
  return s->off_car ? NAVI_BACK_RECENTER : NAVI_BACK_NOTHING;
}

NaviBack
navi_back_end_answered (NaviMode mode, gboolean end)
{
  /* the drive may have arrived while the question showed: ending it then is what was asked too */
  if (end && (mode == NAVI_MODE_DRIVE || mode == NAVI_MODE_ARRIVED))
    return NAVI_BACK_END_DRIVE;
  return NAVI_BACK_NOTHING;
}

gboolean
navi_back_button_shown (NaviMode mode)
{
  return mode != NAVI_MODE_BROWSE;
}

const char *
navi_back_name (NaviBack back)
{
  switch (back) {
  case NAVI_BACK_NOTHING:
    return "nothing";
  case NAVI_BACK_RECENTER:
    return "recenter";
  case NAVI_BACK_CANCEL_DIALOG:
    return "cancel-dialog";
  case NAVI_BACK_CLOSE_SEARCH:
    return "close-search";
  case NAVI_BACK_CLOSE_PLACE:
    return "close-place";
  case NAVI_BACK_TO_PLACE:
    return "to-place";
  case NAVI_BACK_ASK_END:
    return "ask-end";
  case NAVI_BACK_END_DRIVE:
    return "end-drive";
  }
  return "?";
}
