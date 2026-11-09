/* calc-failure: how the calculator's arithmetic reports that it failed.
 *
 * ExactCalculator's arithmetic (Evaluator, CalculatorExpr, UnifiedReal, BoundedRational and the
 * com.hp.creals library) reports trouble with Java exceptions, and Evaluator.AsyncEvaluator turns
 * each kind into one message (Evaluator.java:603-618). C has no exceptions, so the arithmetic here
 * records the first failure in a per-thread "failure" and keeps going: every loop that could run long
 * checks calc_failed() and stops, and approximations made after a failure are thrown away (never
 * cached). Whoever started the computation looks at calc_failure() at the end, the way the Java code
 * catches the exception at the end. Only the first failure counts, like the first exception thrown.
 *
 * Cancelling works like Thread.interrupt() / CR.please_stop: the thread that computes is given a flag
 * (calc_set_cancel_flag) that another thread may set; calc_failed() notices it and records
 * CALC_ABORTED. */
#pragma once

#include <glib.h>

typedef enum {
  CALC_OK = 0,
  CALC_SYNTAX_ERROR,       /* CalculatorExpr.SyntaxException -> "Bad expression" */
  CALC_DIVIDE_BY_ZERO,     /* UnifiedReal/BoundedRational.ZeroDivisionException -> "Can't divide by 0" */
  CALC_DOMAIN_ERROR,       /* any other ArithmeticException, e.g. sqrt(-1), ln(0) -> "Not a number" */
  CALC_PRECISION_OVERFLOW, /* CR.PrecisionOverflowException -> "Infinite?" */
  CALC_ABORTED,            /* CR.AbortedException (cancelled or timed out) -> "Aborted" */
  CALC_TOO_BIG,            /* StackOverflowError or a result too big to print: treated as a timeout */
} CalcFailure;

/* records the failure, unless one is recorded already */
void        calc_fail (CalcFailure failure);
CalcFailure calc_failure (void);
/* forgets the failure: done before each new computation */
void        calc_clear_failure (void);
/* has the computation failed, or been cancelled? (checks the cancel flag) */
gboolean    calc_failed (void);
/* the flag that cancels the computations of this thread (NULL: nothing can cancel them) */
void        calc_set_cancel_flag (gint *flag);
