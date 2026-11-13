/* expression-db: the saved expressions, as ExactCalculator's ExpressionDB keeps them: one SQLite
 * table with the same columns.
 *
 * Positive indices (1, 2, ...) are the history, oldest first. Negative indices from -11 down are
 * expressions kept for the memory and the clipboard, never shown. Index 0 is the main expression,
 * which is never stored here.
 *
 * Java wrote on a background thread; these writes are a few hundred bytes each and done right away. */
#pragma once

#include <glib.h>

/* the largest index a hidden expression may have */
#define EXPRESSION_DB_MAXIMUM_MIN_INDEX -10

typedef struct _ExpressionDB ExpressionDB;

typedef struct {
  GBytes  *expression;    /* calc_expr_to_bytes */
  gboolean degree_mode;
  gboolean long_timeout;
  gint64   time_stamp;    /* ms since 1970 */
} ExpressionRow;

/* opens or creates the database at path; never fails: without a usable file it keeps the rows in
 * memory */
ExpressionDB *expression_db_open (const char *path);
void          expression_db_close (ExpressionDB *db);

/* stores a row (its time stamp is now) as the next history entry, or as the next hidden one */
long          expression_db_add_row (ExpressionDB *db, gboolean hidden, GBytes *expression,
                                     gboolean degree_mode, gboolean long_timeout, gint64 *time_stamp);
/* the row at index; FALSE if there is none */
gboolean      expression_db_get_row (ExpressionDB *db, long index, ExpressionRow *row);
void          expression_row_clear (ExpressionRow *row);
void          expression_db_erase_all (ExpressionDB *db);
long          expression_db_get_min_index (ExpressionDB *db);
long          expression_db_get_max_index (ExpressionDB *db);
