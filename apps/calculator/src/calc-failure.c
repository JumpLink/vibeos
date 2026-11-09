/* calc-failure: see calc-failure.h */
#include "calc-failure.h"

/* per thread: the evaluator's worker and the tests each have their own */
static _Thread_local CalcFailure current_failure;
static _Thread_local gint *cancel_flag;

void
calc_fail (CalcFailure failure)
{
  if (current_failure == CALC_OK)
    current_failure = failure;
}

CalcFailure
calc_failure (void)
{
  calc_failed ();
  return current_failure;
}

void
calc_clear_failure (void)
{
  current_failure = CALC_OK;
}

gboolean
calc_failed (void)
{
  if (current_failure == CALC_OK && cancel_flag && g_atomic_int_get (cancel_flag))
    current_failure = CALC_ABORTED;
  return current_failure != CALC_OK;
}

void
calc_set_cancel_flag (gint *flag)
{
  cancel_flag = flag;
}
