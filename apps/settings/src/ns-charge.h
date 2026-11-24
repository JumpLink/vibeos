/* the battery's charge since it was last full, drawn from UPower's history (see ns-charge.c) */
#pragma once

#include <gtk/gtk.h>

/* the chart, its times and a line under it, for a card; it reads the history at once and every minute after */
GtkWidget *ns_charge_history_new (void);
