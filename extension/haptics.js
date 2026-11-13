// Haptics as Android plays them on the S10e (LineageOS 23.2). Its vibrator has no composition primitives, so every
// effect there is a prebaked one or a waveform, read back with dumpsys vibrator_manager.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const APP = 'neo.GnomePhone';

// HapticFeedbackConstants.VIRTUAL_KEY / KEYBOARD_TAP (EFFECT_CLICK): feedbackd's shortest event, through its profile
// as the on-screen keyboard's and the back gesture's ticks
export function click() {
    Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback',
        'TriggerFeedback', new GLib.Variant('(ssa{sv}i)', [APP, 'button-released', {}, -1]),
        null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
}

// the HAL writes amplitude a as intensity a x 10000, which Samsung's driver puts on a PWM duty between 50 % (still)
// and max_duty 27551 of 37741 ns (sec_haptic.c:63); max77693-haptic spans 50..100 % for feedbackd's magnitude
const DUTY_MATCH = (27551 - 37741 / 2) / (37741 / 2);

// a waveform of [amplitude (Android's, 0..1), ms] steps, straight to the motor
export function vibrate(pattern) {
    const steps = pattern.map(([amplitude, ms]) => [amplitude * DUTY_MATCH, ms]);
    Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback.Haptic',
        'Vibrate', new GLib.Variant('(sa(du))', [APP, steps]),
        null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
}

// the prebaked EFFECT_TICK at its default strength (MEDIUM): 0.5 for 5 ms. EFFECT_TEXTURE_TICK is taken to be the
// same; it was not read back.
export const TICK = [[0.5, 5]];

// a waveform of [magnitude (feedbackd's own, 0..1: the max77693 haptic's PWM duty from 50 to 100 %), ms] steps
export function vibrateRaw(steps) {
    Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback.Haptic',
        'Vibrate', new GLib.Variant('(sa(du))', [APP, steps]),
        null, Gio.DBusCallFlags.NO_AUTO_START, -1, null, null);
}

// click()'s own rumble (feedbackd's VibraRumble: magnitude 0x8000, 75 % duty, for 12 ms) at twice its strength, 100 %
// duty: the owner found the click too subtle for the pattern's dots (2026-11-09)
export const DOT_TICK = [[1, 12]];
