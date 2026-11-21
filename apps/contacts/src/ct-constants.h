/* The app's numbers: sizes in dp (a GTK px here, whatever the display's scale), text sizes in sp, times in ms. The
 * colours are the theme's roles (vibeos_color), read when drawn, so none is here. */
#pragma once

#include <glib.h>

/* ======== text ======== */
#define CT_WEIGHT_REGULAR               700          /* the text renderer's weights: regular, and */
#define CT_WEIGHT_PAINT                 400          /* the avatar's letter, which Bold text leaves alone */

/* ======== the home page ======== */
#define CT_RAIL_W_DP                     80.0f       /* the tablet's navigation rail */
#define CT_TAB_FADE_MS                  150          /* one tab giving way to another */
#define CT_TAB_PAD_BOTTOM_DP             24.0f       /* under the last card of Highlights and Organize */
#define CT_LIST_PANE_RATIO                0.36f      /* on a tablet, the list's share of the width */
#define CT_LIST_PANE_MIN_DP             360.0f       /* and its bounds */
#define CT_LIST_PANE_MAX_DP             420.0f
#define CT_DETAIL_EMPTY_ICON_DP          96.0f       /* the picture on the detail side when nothing is open */

/* ======== the list's head ======== */
#define CT_HEAD_GAP_DP                    8.0f       /* above the pill, the filter row and "Your info" */
#define CT_PILL_H_DP                     48.0f
#define CT_PILL_ICON_X_DP                16.0f       /* the magnifier's start, from the pill's */
#define CT_PILL_TEXT_X_DP                52.0f       /* the hint's */
#define CT_PILL_DISC_DP                  42.0f       /* the account picture at its end */
#define CT_SHEET_AVATAR_DP               40.0f       /* the account sheet's profile picture */
#define CT_ACCOUNT_BUTTON_H_DP           40.0f       /* "All contacts ▾" */
#define CT_ACCOUNT_ICON_DP               18.0f
#define CT_CHIP_ICON_DP                  18.0f
#define CT_CHIP_RADIUS_DP                 8.0f
#define CT_CHIP_GAP_DP                    8.0f

/* ======== the rows ======== */
#define CT_LIST_SIDE_DP                  16.0f       /* the cards' margin from the list's sides */
#define CT_LIST_PAD_TOP_DP                8.0f       /* above the first card with no head */
#define CT_LIST_PAD_BOTTOM_DP            24.0f       /* under the last */
#define CT_LIST_PAD_BOTTOM_FAB_DP        88.0f       /* the same, clear of the FAB */
#define CT_CARD_OUTER_RADIUS_DP          20.0f       /* a card's ends */
#define CT_CARD_INNER_RADIUS_DP           4.0f       /* between its rows */
#define CT_ROW_H_DP                      56.0f
#define CT_ROW_SNIPPET_H_DP              72.0f       /* a search result with the line that matched */
#define CT_ROW_GAP_DP                     2.0f       /* between the rows of a card */
#define CT_ROW_PHOTO_DP                  40.0f
#define CT_ROW_PHOTO_X_DP                16.0f       /* from the card's start */
#define CT_ROW_NAME_X_DP                 72.0f
#define CT_ROW_NAME_BOX_DP               24.0f
#define CT_ROW_SNIPPET_BOX_DP            20.0f
#define CT_ROW_END_DP                    16.0f       /* the text's (or the checkbox's) end, from the card's */
#define CT_ROW_CHECK_GAP_DP               8.0f
#define CT_SECTION_H_DP                  32.0f       /* a section's header */
#define CT_SECTION_TEXT_X_DP             16.0f       /* from the card's start */
#define CT_SECTION_TEXT_BOX_DP           16.0f
#define CT_SECTION_STAR_DP               16.0f
#define CT_SECTION_STAR_TEXT_X_DP        40.0f       /* "Favorites", past the star */

/* ======== the FAB ======== */
#define CT_FAB_DP                        56.0f
#define CT_FAB_RADIUS_DP                 16.0f
#define CT_FAB_MARGIN_END_DP             24.0f
#define CT_FAB_MARGIN_BOTTOM_DP          16.0f
#define CT_FAB_SCALE_IN_MS              186          /* linear-out-slow-in */
#define CT_FAB_SCALE_OUT_MS             175          /* fast-out-linear-in */

/* ======== search, Highlights ======== */
#define CT_RECENT_CELLS                   8          /* search's "Recent contacts" */
#define CT_RECENT_CELL_W_DP              76.0f
#define CT_RECENT_AVATAR_DP              48.0f
#define CT_FAVOURITE_CELL_W_DP           90.0f       /* a favourite's cell, edge to edge across the width */
#define CT_FAVOURITE_AVATAR_DP           58.0f
#define CT_FAVOURITE_COLUMNS              4          /* at least, and at most on a wide window */
#define CT_FAVOURITE_COLUMNS_MAX         12

/* ======== pages with a toolbar (Settings, the picker) and selection ======== */
#define CT_TOOLBAR_H_DP                  56.0f
#define CT_TOOLBAR_NAV_W_DP              56.0f
#define CT_TOOLBAR_TITLE_SP              20.0f
#define CT_SEARCH_ICON_DP                56.0f
#define CT_SELECTION_CLOSE_DP            56.0f

/* ======== QuickContact ======== */
#define CT_QC_INTERMEDIATE_RATIO          0.6f
#define CT_QC_SCRIM_FADE_MS             200
#define CT_QC_ENTRANCE_MS               300          /* linear-out-slow-in */
#define CT_QC_EXIT_MS                   250          /* accelerating */
#define CT_QC_EXPAND_HEADER_MS          300
#define CT_QC_TITLE_MAX_DP               32.0f
#define CT_QC_PHONETIC_MAX_DP            28.0f
#define CT_QC_TITLE_PLACEHOLDER_DP       20.0f
#define CT_QC_TITLE_MARGIN_H_DP          16.0f
#define CT_QC_TITLE_MAX_MARGIN_DP        16.0f
#define CT_QC_TITLE_CURVE_X1              0.16f      /* the title's curve: cubic Bézier (0.16, 0.4, 0.2, 1) */
#define CT_QC_TITLE_CURVE_Y1              0.4f
#define CT_QC_TITLE_CURVE_X2              0.2f
#define CT_QC_TITLE_CURVE_Y2              1.0f
#define CT_QC_GRADIENT_ALPHA              0.533f     /* the scrim's colour at the header's bottom, under the title */
#define CT_QC_GRADIENT_TITLE_FACTOR       1.25f
#define CT_QC_TOOLBAR_ELEVATION_DP        4.5f
#define CT_QC_COLOR_BLEND_START           0.5f
#define CT_QC_TILE_INTERMEDIATE_ALPHA     0.8f
#define CT_QC_TILE_EXPONENT               3.0f
#define CT_QC_CARD_TITLE_SP              16.0f
#define CT_QC_ENTRY_PAD_START_DP         20.0f
#define CT_QC_ENTRY_PAD_END_DP            8.0f
#define CT_QC_ENTRY_PAD_TOP_DP           13.0f
#define CT_QC_ENTRY_PAD_BOTTOM_DP        16.0f
#define CT_QC_NULL_TITLE_EXTRA_DP         2.0f
#define CT_QC_NO_ICON_MARGIN_TOP_DP       6.0f
#define CT_QC_ICON_DP                    24.0f
#define CT_QC_ICON_MARGIN_TOP_DP         12.0f
#define CT_QC_IMAGE_SPACING_DP           27.0f
#define CT_QC_HEADER_SP                  16.0f
#define CT_QC_SUB_SP                     14.0f
#define CT_QC_CLICKABLE_ICON_DP          48.0f

/* ======== the editor ======== */
#define CT_ED_MIN_LINE_DP                54.0f
#define CT_ED_KIND_ICON_TOP_DP           14.0f
#define CT_ED_KIND_ICON_COL_DP           68.0f       /* the kind's icon with its margins */
#define CT_ED_KIND_ICON_START_DP         16.0f
#define CT_ED_COLUMN_MAX_DP               600        /* on a tablet the form keeps to a column this wide */
#define CT_ED_PHOTO_DP                    160        /* the photo: a square this big, in the middle (not the
                                                       * screen-wide width / 1.96 band: "nobody has landscape
                                                       * avatars", owner 11-21) */
#define CT_ED_PHOTO_TOP_DP                 16        /* above it */
#define CT_ED_TYPE_LABEL_W_DP           150.0f
#define CT_ED_END_COLUMN_DP              56.0f       /* the ✕ / chevron column */
#define CT_ED_SPINNER_EXTRA_DP           56.0f       /* spinner = widest label + this (its padding and caret) */
#define CT_ED_PHOTO_BADGE_MARGIN_DP      16.0f       /* the camera badge, bottom and end */
#define CT_ED_ANIM_MS                   200          /* a row's fade in (after 200) and out */

/* ======== avatars ======== */
#define CT_TILE_LETTER_RATIO              0.67f

/* ======== menus, dialogs, toasts ======== */
#define CT_POPUP_H_OFFSET_DP             -4.0f
#define CT_POPUP_MIN_W_DP               196.0f
#define CT_POPUP_ITEM_H_DP               48.0f
#define CT_POPUP_ITEM_PAD_DP             16.0f
#define CT_POPUP_SPINNER_PAD_DP           8.0f
#define CT_POPUP_TEXT_SP                 16.0f
#define CT_POPUP_ENTER_FADE_MS          100          /* from alpha 0.1 */
#define CT_POPUP_EXIT_MS                300
#define CT_DIALOG_W_DP                  320.0f       /* CT_DIALOG_FRAMEWORK dialogs */
#define CT_DIALOG_TITLE_SP               20.0f
#define CT_TOAST_SHORT_MS              2000

/* ======== motion ======== */
#define CT_TOUCH_SLOP_DP                  8.0f
#define CT_ACTIVITY_SLIDE_DP             96.0f       /* the emphasized curve (fast-out-extra-slow-in) */
#define CT_ACTIVITY_ANIM_MS             450
#define CT_ACTIVITY_OPEN_FADE_DELAY_MS   50
#define CT_ACTIVITY_CLOSE_FADE_DELAY_MS  35
#define CT_ACTIVITY_FADE_MS              83
#define CT_LIST_LOAD_MS                 190          /* linear-out-slow-in */
#define CT_LIST_LOAD_DELAY_FRAC           0.10f
#define CT_LIST_LOAD_TRANSLATE_FRAC       0.05f
