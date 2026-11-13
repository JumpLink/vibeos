/* The Camera's measures. Sizes are GTK px unless a comment says device px. */
#pragma once

/* bands, chained bottom-up from the window's bottom edge */
#define CAM_MODE_SELECTOR_H        80       /* padding, a mode button, padding */
#define CAM_MODE_PAD               16
#define CAM_MODE_BUTTON_W          88
#define CAM_MODE_BUTTON_H          48
#define CAM_MODE_COLUMN_W          104      /* landscape: the buttons with a margin each side */
#define CAM_MODE_PILL_H            40       /* the highlight pill, inset in its button */
#define CAM_PRIMARY_BAR_H          91       /* padding and the shutter */
#define CAM_PRIMARY_BAR_PAD        16
#define CAM_SHUTTER_VIEW           75
#define CAM_SHUTTER_DRAW           72       /* the vector, drawn unscaled                           */
#define CAM_GALLERY_SIZE           60
#define CAM_GALLERY_MARGIN         32
#define CAM_GALLERY_RADIUS         8
#define CAM_GALLERY_ICON           30       /* half the gallery button */
#define CAM_FLIP_SIZE              60
#define CAM_FLIP_ICON              32
#define CAM_PAUSE_SIZE             32
#define CAM_SECONDARY_BAR_H        72
#define CAM_BAR_BUTTON             48
#define CAM_BAR_BUTTON_PAD         4
#define CAM_BAR_BUTTON_MARGIN      40
#define CAM_BAR_ICON               24
#define CAM_LENS_BUTTON            32
#define CAM_LENS_MARGIN            (5.0 / 3)   /* 5 device px                                       */
#define CAM_LENS_RADIUS            8
#define CAM_DIVIDER_H              1
#define CAM_PRO_ROW_H              (253.0 / 3) /* the pro row: 253 device px */
#define CAM_PRO_ROW_PAD_V          8
#define CAM_PRO_ROW_SPACER         16
#define CAM_PRO_BUTTON_W           96
#define CAM_PRO_ICON               24
#define CAM_PRO_ICON_TOP           9        /* the icon from the button's top */
#define CAM_PRO_LABEL_TOP          46       /* the label's cap top from the button's top */
#define CAM_ISLAND_H               36
#define CAM_ISLAND_RADIUS          18
#define CAM_ISLAND_MARGIN          12
#define CAM_FOCUS_RING             75
#define CAM_FOCUS_RING_INSET       8        /* the ring's padding grows from 0 to this while focusing, drawing it in */
#define CAM_ZOOM_SLIDER_W          250
#define CAM_ZOOM_SLIDER_H          32
#define CAM_FLING_MIN              75       /* the least sideways drag that flings to another mode */
#define CAM_FLING_MIN_VELOCITY     50       /* the slowest fling that counts, px/s                  */
#define CAM_RIPPLE_RADIUS          34       /* half a bar button's diagonal: the unbounded ripple's reach */

/* text */
#define CAM_SP_MODE_LABEL          14       /* the mode buttons' labels */
#define CAM_SP_PRO_LABEL           12       /* the pro row's labels, up to 3 lines */
#define CAM_SP_DURATION            14       /* the recording's duration */
#define CAM_DP_LENS_TEXT           10       /* a lens button's ratio */
#define CAM_SP_COUNTDOWN           125      /* the self-timer's countdown */
#define CAM_DP_SLIDER_TEXT         10

/* colours (standard contrast) */
#define CAM_C_REC_RED              0xE95950
#define CAM_C_GRAY_10              0xF6FAFA
#define CAM_C_GRAY_60              0x2A3232
#define CAM_C_DARK_GREY            0x444444
#define CAM_C_YELLOW               0xE9B650
#define CAM_C_PRO_ROW_BG           0x99000000 /* ARGB                                               */
#define CAM_C_GRID                 0x7FFFFFFF /* ARGB, the grid's and the leveler's lines */
#define CAM_C_ZOOM_TRACK           0x7FCCCCCC /* ARGB                                               */

/* timing (ms) */
#define CAM_MS_DEFAULT             300      /* the usual animation, accelerate-decelerate          */
#define CAM_MS_PHOTO_PULSE         100      /* there and back, fast-out-slow-in, scale 0.625       */
#define CAM_MS_SHUTTER_MORPH       300      /* fast-out-slow-in; the colour accelerate-decelerate  */
#define CAM_MS_FLIP                500      /* accelerate-decelerate, 0 → −180°                    */
#define CAM_MS_PAUSE_MORPH         200      /* + 100 delay                                         */
#define CAM_MS_SLIDE_UP            250      /* the pro row                                         */
#define CAM_MS_SLIDE_DOWN          200
#define CAM_MS_SLIDER_HIDE         2000
#define CAM_MS_FOCUS_HIDE          500
#define CAM_MS_PINCH_GUARD         500      /* no mode fling after a pinch                         */
#define CAM_MS_COUNTDOWN_FADE      800      /* scale 1 → 2.5, alpha 1 → 0, accelerate-decelerate   */
#define CAM_MS_COUNTDOWN_TICK      1000
#define CAM_MS_START_SOUND_GAP     500      /* before a recording starts                           */
#define CAM_MS_BLINK               300      /* black 0 → 255 → 0, accelerate-decelerate            */
#define CAM_MS_ISLAND_FADE         200
#define CAM_MS_THUMB_CROSSFADE     200
#define CAM_MS_TOAST               2000
#define CAM_MS_ROTATE              300      /* the shortest way round, accelerate-decelerate       */

#define CAM_PHOTO_PULSE_SCALE      0.625
#define CAM_VIDEO_DOT_SCALE        0.3125
#define CAM_VIDEO_DOT_END_SCALE    0.0625
#define CAM_COUNTDOWN_END_SCALE    2.5
#define CAM_LEVEL_ZONE_DEG         2
#define CAM_LEVEL_LINE_FRAC        0.15
#define CAM_LEVEL_BASE_X_FRAC      0.15
#define CAM_LEVEL_BASE_Y_FRAC      0.01
#define CAM_GOLDEN_RATIO           0.618
#define CAM_JPEG_QUALITY           90
#define CAM_BLUR_RADIUS            (25.0 / 3) /* the preview's blur while the camera is reconfigured: 25 device px */

/* the stream: about 2 MP for the aspect asked for (owner, 11-10), from the sizes libcamera lists */
#define CAM_STREAM_TARGET_PIXELS   2000000
#define CAM_THUMB_PX               225      /* device px: the gallery thumbnail is decoded at its button's size */

#define CAM_C(rgb)    ((GdkRGBA) { (((rgb) >> 16) & 0xff) / 255.f, (((rgb) >> 8) & 0xff) / 255.f, \
                                   ((rgb) & 0xff) / 255.f, 1 })
#define CAM_CA(argb)  ((GdkRGBA) { (((argb) >> 16) & 0xff) / 255.f, (((argb) >> 8) & 0xff) / 255.f, \
                                   ((argb) & 0xff) / 255.f, (((guint32) (argb) >> 24) & 0xff) / 255.f })
