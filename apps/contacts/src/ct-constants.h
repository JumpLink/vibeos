/* The numbers of LineageOS 23.2 Contacts (com.android.contacts, BP4A.251205.006), from spec/contacts.md §12:
 * sizes in dp (1 dp is 1 GTK px on the phone, which renders at scale 3), text in sp (= dp here), times in ms.
 * Colours are ARGB and are what the phone shows in dark mode after HWUI's force dark (spec §0.2), unless the name
 * ends in _SRC. Each comment names the section of the spec the value comes from. */
#pragma once

#include <glib.h>
#include <stdint.h>


/* ======== 0. rendering model ======== */
#define CT_PX_PER_DP                      3
#define CT_SCREEN_W_PX                 1080
#define CT_SCREEN_H_PX                 2280
#define CT_STATUSBAR_H_PX               116          /* portrait, §0.5 */
#define CT_STATUSBAR_H_LAND_PX           84          /* landscape; cutout inset on the left = 116 */
#define CT_CUTOUT_INSET_LAND_PX         116
#define CT_IME_CONTENT_BOTTOM_PX       1349          /* measured with the keyboard up */
#define CT_BOLD_TEXT_ADJUST             300          /* TextView weight + 300, clamp 1000 -> renders 900 */
#define CT_WEIGHT_REGULAR               700          /* sans-serif 400 + 300 */
#define CT_WEIGHT_MEDIUM                800          /* sans-serif-medium 500 + 300 */
#define CT_WEIGHT_BOLD                  900          /* any textStyle=bold */
#define CT_WEIGHT_PAINT                 400          /* LetterTile letter (Paint, not adjusted) */
#define CT_TEXT_BOX_EM                    1.355f     /* includeFontPadding box: 14sp 57px, 16sp 65, 20dp 81, 32dp 129 */
#define CT_FD_MAKE_L_PIVOT              110.0f       /* makeDark/makeLight: L' = min(110 - L*, 100) */
#define CT_FD_PALETTE_V_SPLIT             0.5f       /* HSV V >= 0.5 -> Light palette */

/* ---- palette (TONAL_SPOT dark, §0.3) ---- */
#define CT_ACCENT1_100               0xFFB8EBF3u
#define CT_ACCENT1_300               0xFF81B4BBu
#define CT_ACCENT1_400               0xFF6799A0u
#define CT_ACCENT1_500               0xFF4D7F86u
#define CT_ACCENT1_600               0xFF33666Du
#define CT_ACCENT1_700               0xFF174E55u
#define CT_ACCENT1_800               0xFF00363Cu
#define CT_ACCENT2_300               0xFF96B0B4u
#define CT_ACCENT2_400               0xFF7C9599u
#define CT_ACCENT2_500               0xFF627B7Fu
#define CT_ACCENT2_600               0xFF4A6366u
#define CT_ACCENT2_700               0xFF324B4Eu
#define CT_ACCENT2_800               0xFF1C3438u
#define CT_ACCENT3_300               0xFF8DAFD1u
#define CT_ACCENT3_400               0xFF7394B5u
#define CT_ACCENT3_500               0xFF597A9Au
#define CT_ACCENT3_600               0xFF406180u
#define CT_ACCENT3_700               0xFF274967u
#define CT_ACCENT3_800               0xFF0C334Fu
#define CT_NEUTRAL1_800              0xFF2C3132u
#define CT_NEUTRAL1_900              0xFF181C1Du

/* ---- app colours, effective (§0.2, §0.3) ---- */
#define CT_C_WINDOW_BG               0xFF181C1Du     /* background_primary night */
#define CT_C_TOOLBAR                 0xFF33666Du     /* primary_color, makeDark no-op */
#define CT_C_STATUSBAR               0xFF174E55u     /* primary_color_dark, painted by DrawerLayout */
#define CT_C_STATUSBAR_LABEL         0xFF181C1Du     /* group_primary_color_dark (§5.1.2) */
#define CT_C_TOOLBAR_LABEL           0xFF2C3132u     /* group_primary_color */
#define CT_C_STATUSBAR_QC            0xFF051112u     /* 0xC8 scrim over #174e55 */
#define CT_C_LIST_NAME               0xFFDADADAu     /* exempt list, night value */
#define CT_C_LIST_SECONDARY          0xFF737373u     /* exempt list: untransformed */
#define CT_C_SECTION_HEADER          0xFF33666Du     /* exempt list */
#define CT_C_TEXT_PRIMARY            0xFFDBDBDBu     /* #363636 makeLight */
#define CT_C_TEXT_SECONDARY          0xFF959595u     /* #737373 makeLight */
#define CT_C_ACCENT_TEXT             0xFF81B5BCu     /* #33666d makeLight */
#define CT_C_ICON_NORMAL             0xFFEBEBEBu     /* invL(#737373) */
#define CT_C_ICON_SELECTED           0xFFEDF9FBu     /* invL(#33666d) */
#define CT_C_ICON_SECONDARY_TINT     0xFFEAEAEAu     /* #89000000 SRC_ATOP -> #767676 -> invL */
#define CT_C_ICON_HINT_TINT          0xFF9E9E9Eu     /* #61000000 SRC_ATOP, Light palette */
#define CT_C_ICON_OVERFLOW_SELECT    0xFFE9E9E9u     /* invL(#777777) */
#define CT_C_WHITE                   0xFFFFFFFFu
#define CT_C_FAB                     0xFF00383Eu     /* makeDark(accent1_100) */
#define CT_C_SEARCH_BOX              0xFF1B1B1Bu
#define CT_C_SEARCH_HINT             0xFF767676u     /* #66ffffff on #1b1b1b */
#define CT_C_SELECTION_BAR           0xFF222222u     /* makeDark(#f6f6f6) */
#define CT_C_SELECTION_COUNT         0xDFFFFFFFu
#define CT_C_DRAWER_BG               0xFF1F1F1Fu
#define CT_C_DRAWER_SELECTED         0xFF2C2C2Cu
#define CT_C_DRAWER_DIVIDER          0xFF2E2E2Eu
#define CT_C_DRAWER_TEXT             0xDDFFFFFFu     /* = #e1e1e1 on #1f1f1f */
#define CT_C_DRAWER_HEADER_TEXT      0x89FFFFFFu     /* = #979797 */
#define CT_C_DRAWER_STATUS_SCRIM     0x33000000u
#define CT_C_DRAWER_SCRIM            0x99000000u
#define CT_C_ALERT_BG                0xFF323232u     /* makeDark(#e0e0e0) */
#define CT_C_EMPTY_BG                0xFF272727u     /* makeDark(#efefef) */
#define CT_C_CARD                    0xFF1B1B1Bu
#define CT_C_CARD_GUTTER             0xFF232323u
#define CT_C_QC_HEADER_TEXT          0xFFF8F8F8u
#define CT_C_QC_SUB_TEXT             0xFF959595u
#define CT_C_QC_DEFAULT_TINT         0xFF607D8Bu
#define CT_C_QC_DEFAULT_TINT_DARK    0xFF455A64u
#define CT_C_QC_DEFAULT_TITLE        0xFF7794A2u     /* makeLight(#607d8b) */
#define CT_C_QC_SCRIM                0xC8000000u
#define CT_C_POPUP_BG                0xFF1F1F1Fu
#define CT_C_POPUP_TEXT              0xDEFFFFFFu     /* = #e2e2e2 */
#define CT_C_CHECK_ON                0xFFEDF9FBu     /* a force-darked check box: #33666d inverted (§5.5 SIM rows) */
#define CT_C_CHECK_OFF               0xFFEBEBEBu     /* #737373 inverted */
#define CT_C_DIALOG_BG               0xFF1B1B1Bu
#define CT_C_DIALOG_SCRIM            0x99000000u     /* 0.6 black */
#define CT_C_DIALOG_TEXT             0xFFE1E1E1u     /* #de000000 made light (meas. on the import dialog) */
#define CT_C_SNACKBAR_BG             0xFF323232u
#define CT_C_SCROLLBAR               0x87737373u     /* #737373 at 0.53: measured #484a4a over #181c1d, §7.4 */
#define CT_C_FASTSCROLL_THUMB        0xFF737373u
#define CT_C_FASTSCROLL_THUMB_PRESSED 0xFF33666Du
#define CT_C_FASTSCROLL_TRACK        0x39737373u
#define CT_C_TOOLBAR_SHADOW_TOP      0xFF15191Au     /* measured y 284 */

/* ======== 1. PeopleActivity ======== */
/* ---- toolbar (§1.2) ---- */
#define CT_TOOLBAR_H_DP                  56.0f
#define CT_TOOLBAR_H_LAND_DP             48.0f
#define CT_TOOLBAR_ELEVATION_DP           2.0f       /* tab_elevation */
#define CT_TOOLBAR_NAV_W_DP              56.0f
#define CT_TOOLBAR_TITLE_X_DP            72.0f       /* contentInsetStartWithNavigation */
#define CT_TOOLBAR_TITLE_SP              20.0f       /* dp in AppCompat; medium -> 800 */
#define CT_TOOLBAR_ACTION_PAD_DP         16.0f       /* ContactsActionButtonStyle */
#define CT_TOOLBAR_ACTION_W_PX          168          /* measured [912,128][1080,272] */
#define CT_DRAWER_ARROW_BAR_LEN_DP       18.0f
#define CT_DRAWER_ARROW_BAR_THICK_DP      2.0f
#define CT_DRAWER_ARROW_GAP_DP            3.0f
#define CT_DRAWER_ARROW_HEAD_DP           8.0f
#define CT_DRAWER_ARROW_SHAFT_DP         16.0f
#define CT_DRAWER_ARROW_SIZE_DP          24.0f

/* ---- rows (§1.3) ---- */
#define CT_LIST_PAD_TOP_DP                7.0f
#define CT_ROW_H_DP                      56.0f
#define CT_ROW_PAD_TOP_DP                 7.0f
#define CT_ROW_PAD_BOTTOM_DP              7.0f
#define CT_ROW_PAD_RIGHT_DP              32.0f
#define CT_ROW_HEADER_COL_DP             56.0f
#define CT_ROW_GAP_HEADER_PHOTO_DP       16.0f
#define CT_ROW_PHOTO_DP                  40.0f
#define CT_ROW_PHOTO_OFFSET_TOP_DP       -1.0f
#define CT_ROW_GAP_PHOTO_TEXT_DP         15.0f
#define CT_ROW_NAME_SP                   16.0f       /* -> 700 */
#define CT_ROW_TEXT_OFFSET_TOP_DP        -2.0f
#define CT_ROW_SECONDARY_SP              14.0f
#define CT_ROW_CHECKBOX_GAP_PX           20          /* raw px, not dp (ContactListItemView.java:105) */
#define CT_ROW_PHOTO_X_PX               216          /* measured */
#define CT_ROW_NAME_X_PX                381
#define CT_ROW_NAME_X_SEARCH_PX         213

/* ---- section headers (§1.4) ---- */
#define CT_SECTION_SP                    16.0f       /* allCaps, bold -> 900 */
#define CT_SECTION_BLANK                 "…"
#define CT_PINNED_FADE_MS                20

/* ---- fast scroller (§1.5) ---- */
#define CT_FS_THUMB_W_DP                  8.0f
#define CT_FS_THUMB_H_DP                 48.0f
#define CT_FS_TRACK_W_DP                  8.0f
#define CT_FS_MIN_PAGES                   4
#define CT_FS_FADE_IN_MS                150
#define CT_FS_FADE_OUT_MS               300
#define CT_FS_HIDE_DELAY_MS            1500
#define CT_FS_SLIDE_MS                  150
#define CT_FS_PREVIEW_FADE_MS            50
#define CT_FS_PREVIEW_RESIZE_MS         100
#define CT_FS_PREVIEW_TEXT_SP            45.0f
#define CT_FS_PREVIEW_MIN_W_DP          104.0f
#define CT_FS_PREVIEW_MIN_H_DP           88.0f
#define CT_FS_PREVIEW_CORNER_DP          44.0f       /* 44/44/0/44 */
#define CT_FS_PREVIEW_PAD_H_DP           22.0f
#define CT_FS_PREVIEW_INSET_DP           16.0f

/* ---- FAB (§1.6) ---- */
#define CT_FAB_DP                        56.0f
#define CT_FAB_MARGIN_DP                 16.0f       /* end and bottom, no nav inset */
#define CT_FAB_ICON_DP                   24.0f
#define CT_FAB_ELEVATION_DP               6.0f
#define CT_FAB_TRANSLATION_Z_DP           8.0f
#define CT_FAB_SCALE_IN_MS              186          /* EASE_IN (0,0,0.2,1) */
#define CT_FAB_ICON_FADE_IN_MS          116
#define CT_FAB_ICON_FADE_IN_DELAY_MS     70
#define CT_FAB_SCALE_OUT_MS             175          /* EASE_OUT (0.4,0,1,1) */
#define CT_FAB_ICON_FADE_OUT_MS          46

/* ---- drawer (§1.7, §7.6) ---- */
#define CT_DRAWER_W_DP                  320.0f
#define CT_DRAWER_ELEVATION_DP           10.0f
#define CT_DRAWER_SPACER_DP               8.0f
#define CT_DRAWER_ITEM_H_DP              48.0f
#define CT_DRAWER_ITEM_PAD_START_DP      16.0f
#define CT_DRAWER_PRIMARY_PAD_END_DP     32.0f
#define CT_DRAWER_ICON_BOX_DP            32.0f
#define CT_DRAWER_TEXT_MARGIN_DP         32.0f       /* title x = 240 px */
#define CT_DRAWER_TEXT_SP                14.0f       /* medium + bold -> 900 */
#define CT_DRAWER_HEADER_SP              14.0f       /* medium -> 800 */
#define CT_DRAWER_HEADER_PAD_V_DP        12.0f
#define CT_DRAWER_HEADER_PAD_END_DP      24.0f
#define CT_DRAWER_DIVIDER_FRAME_DP       16.0f
#define CT_DRAWER_DIVIDER_DP              1.0f
#define CT_DRAWER_EDGE_DP                20.0f       /* [AndroidX-K] */
#define CT_DRAWER_SETTLE_BASE_MS        256          /* [AndroidX-K] */
#define CT_DRAWER_SETTLE_MAX_MS         600          /* [AndroidX-K] */
#define CT_DRAWER_PEEK_DELAY_MS         160          /* [AndroidX-K] */
#define CT_DRAWER_MIN_FLING_DP_S        400.0f       /* [AndroidX-K] */

/* ---- account header, alert, refresh, empty (§1.8, §1.9) ---- */
#define CT_ACCOUNT_HEADER_H_DP           48.0f
#define CT_ACCOUNT_HEADER_SP             14.0f
#define CT_HEADER_ICON_X_DP              16.0f       /* the account icon, 24 dp ([48,320]) */
#define CT_HEADER_TEXT_X_DP              64.0f       /* the count's start ([192,308]) */
#define CT_ALERT_PAD_START_DP            20.0f
#define CT_ALERT_TEXT_SP                 16.0f
#define CT_ALERT_TEXT_PAD_V_DP           16.0f
#define CT_ALERT_DISMISS_W_DP            56.0f
#define CT_PULL_REFRESH_DISTANCE_DP      40.0f
#define CT_EMPTY_IMAGE_H_DP             150.0f
#define CT_EMPTY_IMAGE_OFFSET_DP        210.0f       /* top = heightPixels/2 - 210dp */
#define CT_EMPTY_TEXT_SP                 18.0f
#define CT_EMPTY_TEXT_ALPHA               0.54f
#define CT_EMPTY_TEXT_PAD_TOP_DP         32.0f
#define CT_EMPTY_ACCOUNT_IMAGE_DP        96.0f       /* top = heightPixels/4 + 30dp */
#define CT_EMPTY_ACCOUNT_OFFSET_DP       30.0f
#define CT_NO_ACCOUNT_IMAGE_W_DP        180.0f       /* top = screenH/2 - 230dp */
#define CT_NO_ACCOUNT_OFFSET_DP         230.0f

/* ---- selection mode, search (§1.10, §2) ---- */
#define CT_ACTIONBAR_ANIM_MS            195          /* search box and selection bar alpha */
#define CT_SELECTION_CLOSE_DP            56.0f
#define CT_SELECTION_COUNT_MARGIN_DP     72.0f
#define CT_SEARCH_ICON_DP                56.0f
#define CT_SEARCH_NAV_MARGIN_DP           8.0f
#define CT_SEARCH_TEXT_MARGIN_DP          8.0f
#define CT_SEARCH_TEXT_SP                16.0f
#define CT_SEARCH_CLOSE_PAD_DP           16.0f
#define CT_SEARCH_CURSOR_W_DP             2.0f
#define CT_SEARCH_CURSOR_H_DP            22.0f
#define CT_SEARCH_NO_RESULTS_SP          22.0f
#define CT_SEARCH_NO_RESULTS_PAD_DP      24.0f

/* ======== 3. QuickContact ======== */
#define CT_QC_STARTING_EMPTY_H_DP       150.0f
#define CT_QC_INTERMEDIATE_RATIO          0.6f
#define CT_QC_SCRIM_FADE_MS             200
#define CT_QC_ENTRANCE_MS               300          /* LOSI */
#define CT_QC_EXIT_MS                   250          /* AcceleratingFlingInterpolator */
#define CT_QC_EXPAND_HEADER_MS          300
#define CT_QC_TITLE_MAX_DP               32.0f       /* -> 700 */
#define CT_QC_PHONETIC_MAX_DP            28.0f
#define CT_QC_TITLE_PLACEHOLDER_DP       20.0f
#define CT_QC_TITLE_MARGIN_H_DP          16.0f
#define CT_QC_TITLE_MARGIN_BOTTOM_DP      8.0f
#define CT_QC_TITLE_MAX_MARGIN_DP        16.0f
#define CT_QC_TITLE_CURVE_X1              0.16f      /* PathInterpolator(0.16, 0.4, 0.2, 1) */
#define CT_QC_TITLE_CURVE_Y1              0.4f
#define CT_QC_TITLE_CURVE_X2              0.2f
#define CT_QC_TITLE_CURVE_Y2              1.0f
#define CT_QC_GRADIENT_END           0x88000000u
#define CT_QC_GRADIENT_TITLE_FACTOR       1.25f
#define CT_QC_TOOLBAR_ELEVATION_DP        4.5f
#define CT_QC_COLOR_BLEND_START           0.5f
#define CT_QC_TILE_INTERMEDIATE_ALPHA     0.8f
#define CT_QC_TILE_EXPONENT               3.0f
#define CT_QC_SPRING_DAMPENING            0.01f
#define CT_QC_SNAP_SLOP_DP               33.0f
#define CT_QC_DISMISS_RELEASE_DP         40.0f
#define CT_QC_DISMISS_SCROLL_DP         100.0f
#define CT_QC_SCROLLER_DEFAULT_MS       250
#define CT_QC_LAND_PHOTO_RATIO            0.7f
#define CT_QC_CARD_CORNER_DP             28.0f
#define CT_QC_CARD_MARGIN_H_DP            8.0f
#define CT_QC_CARD_MARGIN_BOTTOM_DP      12.0f
#define CT_QC_CARDS_PAD_TOP_DP           12.0f
#define CT_QC_CARD_TITLE_SP              16.0f
#define CT_QC_CARD_TITLE_PAD_DP          16.0f
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
#define CT_QC_HEADER_PAD_TOP_DP           5.0f
#define CT_QC_HEADER_MARGIN_BOTTOM_DP     2.0f
#define CT_QC_SUB_SP                     14.0f
#define CT_QC_CLICKABLE_ICON_DP          48.0f
#define CT_QC_CLICKABLE_ICON_PAD_DP      12.0f
#define CT_QC_STATUS_ANIM_MS            150          /* no effect at targetSdk 36 */

/* ======== 6. avatars ======== */
#define CT_TILE_LETTER_RATIO              0.67f
#define CT_TILE_SILHOUETTE_ALPHA        138
#define CT_TILE_DEFAULT              0xFFCCCCCCu
#define CT_TILE_LETTER               0xFFFFFFFFu
#define CT_TILE_PLACEHOLDER_SILHOUETTE 0xFFE8E8E8u
#define CT_TILE_COUNT                    12
static const uint32_t G_GNUC_UNUSED CT_TILE_COLORS[CT_TILE_COUNT] = {  /* index = abs(java_hash(lookup)) % 12 */
    0xFF81B4BBu, 0xFF96B0B4u, 0xFF8DAFD1u,   /* accent1/2/3_300 */
    0xFF6799A0u, 0xFF7C9599u, 0xFF7394B5u,   /* _400 */
    0xFF4D7F86u, 0xFF627B7Fu, 0xFF597A9Au,   /* _500 */
    0xFF33666Du, 0xFF4A6366u, 0xFF406180u }; /* _600 */
static const uint32_t G_GNUC_UNUSED CT_TILE_COLORS_DARK[CT_TILE_COUNT] = {  /* QC secondary */
    0xFF4D7F86u, 0xFF627B7Fu, 0xFF597A9Au,
    0xFF33666Du, 0xFF4A6366u, 0xFF406180u,
    0xFF174E55u, 0xFF324B4Eu, 0xFF274967u,
    0xFF00363Cu, 0xFF1C3438u, 0xFF0C334Fu };
static const uint32_t G_GNUC_UNUSED CT_QC_HEADER_BG[CT_TILE_COUNT] = {     /* makeDark(hue-mapped primary), §6.2 */
    0xFF34666Du, 0xFF4B6367u, 0xFF406381u,
    0xFF4E8086u, 0xFF647C80u, 0xFF406381u,
    0xFF4E8086u, 0xFF647C80u, 0xFF597A9Au,
    0xFF34666Du, 0xFF4A6366u, 0xFF406180u };

/* QuickContact's theme colour for a tile (§3.4's table): the letter_tile_colors entry nearest the tile's hue. The
 * kind icons take it as it is (SRC_ATOP over a Light palette: force dark leaves it); the card titles are text, so
 * force dark makes it light. A contact with a photo, and the default grey tile, take #607d8b / #455a64. */
static const uint32_t G_GNUC_UNUSED CT_QC_PRIMARY[CT_TILE_COUNT] = {
    0xFF81B4BBu, 0xFF96B0B4u, 0xFF8DAFD1u,
    0xFF6799A0u, 0xFF7C9599u, 0xFF8DAFD1u,
    0xFF6799A0u, 0xFF7C9599u, 0xFF597A9Au,
    0xFF81B4BBu, 0xFF4A6366u, 0xFF406180u };
static const uint32_t G_GNUC_UNUSED CT_QC_TITLE[CT_TILE_COUNT] = {        /* makeLight(CT_QC_PRIMARY) */
    0xFF81B4BBu, 0xFF96B0B4u, 0xFF8DAFD1u,
    0xFF6799A0u, 0xFF7C9599u, 0xFF8DAFD1u,
    0xFF6799A0u, 0xFF7C9599u, 0xFF7495B6u,
    0xFF81B4BBu, 0xFF96B1B4u, 0xFF90B0D2u };
#define CT_C_QC_CARD                 0xFF1B1B1Bu     /* #ffffff -> makeDark */
#define CT_C_QC_GUTTER               0xFF232323u     /* card_margin_color #f4f4f4 -> makeDark */
#define CT_C_QC_NO_DATA              0xFF737373u     /* "Add phone number": grey text and icon */

/* ======== 7. ripples, scrolling, motion ======== */
#define CT_RIPPLE_ENTER_MS              450          /* progress 0 -> 0.5, FOSI */
#define CT_RIPPLE_EXIT_MS               375          /* progress -> 1, linear, after enter */
#define CT_RIPPLE_NOISE_MS             7000
#define CT_RIPPLE_SPARKLE            0x8DFFFFFFu
#define CT_RIPPLE_FOCUS_BG_MS            80
#define CT_RIPPLE_LIST_SRC           0x1F000000u     /* exempt list: untransformed */
#define CT_RIPPLE_APPCOMPAT          0x1FFFFFFFu     /* #1f000000 -> makeLight */
#define CT_RIPPLE_DRAWER             0x1AFFFFFFu
#define CT_RIPPLE_FRAMEWORK          0x33FFFFFFu     /* QC entries, filter screen */
#define CT_RIPPLE_QC_TOOLBAR         0xA0FFFFFFu
#define CT_RIPPLE_FAB                0xFFFFFFFFu
#define CT_TAP_TIMEOUT_MS               100
#define CT_PRESSED_STATE_MS              64
#define CT_LONG_PRESS_MS                400
#define CT_TOUCH_SLOP_DP                  8.0f
#define CT_FLING_MIN_DP_S                50.0f
#define CT_FLING_MAX_DP_S              8000.0f
#define CT_FLING_FRICTION                 0.015f
#define CT_FLING_INFLEXION                0.35f
#define CT_FLING_PHYS_TUNING              0.84f      /* x 9.80665 x 39.37 x dpi */
#define CT_STRETCH_LINEAR                 0.016f
#define CT_STRETCH_EXP                    0.016f
#define CT_STRETCH_EXP_DIST               0.33f
#define CT_STRETCH_SPRING_FREQ           24.657f
#define CT_STRETCH_SPRING_DAMPING         0.98f
#define CT_STRETCH_ABSORB_MULT           13.0f
#define CT_STRETCH_LINEAR_VEL           200.0f
#define CT_STRETCH_LINEAR_DIST_PX         8.0f
#define CT_SCROLLBAR_W_DP                 4.0f
#define CT_SCROLLBAR_FADE_MS            250
#define CT_SCROLLBAR_DELAY_MS           400
#define CT_ACTIVITY_SLIDE_DP             96.0f       /* FOESI */
#define CT_ACTIVITY_ANIM_MS             450
#define CT_ACTIVITY_OPEN_FADE_DELAY_MS   50
#define CT_ACTIVITY_CLOSE_FADE_DELAY_MS  35
#define CT_ACTIVITY_FADE_MS              83
#define CT_LIST_LOAD_MS                 190          /* LOSI */
#define CT_LIST_LOAD_DELAY_FRAC           0.10f
#define CT_LIST_LOAD_TRANSLATE_FRAC       0.05f
#define CT_POPUP_ELEVATION_DP            16.0f
#define CT_POPUP_CORNER_DP                2.0f
#define CT_POPUP_H_OFFSET_DP             -4.0f
#define CT_POPUP_MIN_W_DP               196.0f
#define CT_POPUP_ITEM_H_DP               48.0f
#define CT_POPUP_ITEM_PAD_DP             16.0f
#define CT_POPUP_SPINNER_PAD_DP           8.0f       /* Widget.Material.DropDownItem.Spinner */
#define CT_POPUP_TEXT_SP                 16.0f
#define CT_POPUP_ENTER_MS               250          /* EpicenterTranslateClipReveal */
#define CT_POPUP_ENTER_FADE_MS          100          /* from alpha 0.1 */
#define CT_POPUP_EXIT_MS                300
#define CT_DIALOG_W_DP                  320.0f       /* framework dialogs */
#define CT_DIALOG_MIN_W_FRAC              0.95f      /* AppCompat MinWidth */
#define CT_DIALOG_CORNER_DP              16.0f       /* Contacts dialog_background */
#define CT_DIALOG_CORNER_FW_DP            2.0f       /* framework light dialogs */
#define CT_DIALOG_TITLE_PAD_TOP_DP       18.0f
#define CT_DIALOG_PAD_H_DP               24.0f
#define CT_DIALOG_TITLE_SP               20.0f
#define CT_DIALOG_BUTTON_H_DP            54.0f       /* h >= 720dp */
#define CT_DIALOG_ENTER_MS              220          /* STD, translateY 20dp -> 0 */
#define CT_DIALOG_ENTER_DY_DP            20.0f
#define CT_DIALOG_EXIT_MS               150          /* STD_ACC, 0 -> -10dp */
#define CT_DIALOG_EXIT_DY_DP            -10.0f
#define CT_DIALOG_APPCOMPAT_MS          150          /* alpha only */
#define CT_SNACKBAR_ELEVATION_DP          6.0f
#define CT_SNACKBAR_TEXT_SP              14.0f
#define CT_SNACKBAR_PAD_H_DP             12.0f
#define CT_SNACKBAR_PAD_V_DP             14.0f
#define CT_SNACKBAR_PAD_V_2LINE_DP       16.0f
#define CT_SNACKBAR_SLIDE_MS            250          /* [Material-K] */
#define CT_SNACKBAR_TEXT_FADE_MS        180          /* [Material-K], after 70 */
#define CT_SNACKBAR_SHORT_MS           1500          /* [Material-K] */
#define CT_SNACKBAR_LONG_MS            2750          /* [Material-K] */
#define CT_TOAST_Y_OFFSET_DP             48.0f
#define CT_TOAST_MAX_W_DP               300.0f
#define CT_TOAST_TEXT_SP                 14.0f
#define CT_TOAST_SHORT_MS              2000
#define CT_TOAST_LONG_MS               3500

/* ======== 4. editor (subset; full table in §4.23) ======== */
#define CT_ED_MIN_LINE_DP                54.0f
#define CT_ED_ROUND_BUTTON_PAD_DP        16.0f
#define CT_ED_KIND_ICON_TOP_DP           14.0f
#define CT_ED_KIND_ICON_COL_DP           68.0f       /* 16 + 24 + 28 */
#define CT_ED_KIND_ICON_START_DP         16.0f
#define CT_ED_KIND_ICON_END_DP           28.0f
#define CT_ED_PHOTO_RATIO                 1.96f      /* height = width / 1.96 */
#define CT_ED_FORM_TEXT_SP               16.0f
#define CT_ED_ROW_BOTTOM_PAD_DP           2.0f
#define CT_ED_SPINNER_PAD_END_DP         24.0f
#define CT_ED_TYPE_LABEL_W_DP           150.0f
#define CT_ED_TOOLBAR_ELEVATION_DP        4.0f
#define CT_ED_ICON_COLOR             0xFFE5E5E5u     /* invL(#7f7f7f), §11 item 24 */
#define CT_ED_END_COLUMN_DP             56.0f       /* the ✕ / chevron column */
#define CT_ED_SPINNER_EXTRA_DP          56.0f       /* spinner = widest label + 4 + 4 + 48 */
#define CT_ED_PHOTO_BADGE_MARGIN_DP     16.0f       /* the camera badge, bottom and end */
#define CT_ED_ANIM_MS                   200          /* EditorAnimator: row fade in (after 200) and out */
#define CT_C_ED_UNDERLINE            0xFF737373u     /* EditText underline, spinner caret */
#define CT_C_ED_ACCOUNT_ICON         0xFFEBEBEBu     /* the Device icon, invL(#737373) */
#define CT_C_ED_CHEVRON              0xFFEAEAEAu     /* name chevron, account expander */

/* ======== curves ======== */
/* FOSI (0.4,0,0.2,1)  LOSI (0,0,0.2,1)  FOLI (0.4,0,1,1)  STD (0.2,0,0,1)  STD_ACC (0.3,0,1,1)
   FOESI: path M0,0 C0.05,0 0.133333,0.06 0.166666,0.4 C0.208333,0.82 0.25,1 1,1
   quintic out: t' = (t-1)^5 + 1 (QC fling, drawer settle) */
