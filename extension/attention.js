// A window asking for attention. GNOME puts up "“Window” is ready" (WindowAttentionHandler), a banner and a row in
// the tray, for a window that maps without the focus (mutter's focus-stealing prevention) or asks to be shown with no
// activation token of the user's. Android has nothing of the kind, so here no such window gets anything to see:
// GNOME's handler is off while the extension is on, and back as it was after. Apps started with no token (from a
// shell over ssh, or a service) put up "Messages is ready" and "Contacts is ready" in the status bar, and the owner
// found them pointless (11-24).
//
// A window the user did ask for comes to the front instead:
//   - one of the app just launched from the launcher (launch.js, whose launch gives it the activation token), for
//     ATTENTION_MS: a cold start's window maps without the focus and asks for it a moment later;
//   - one asking within INTENT_MS of a tap or key: one app opening another's screen (the call log's contact,
//     Contacts' Send SMS) is Android's intent, and GTK asks for its token as the finger lifts, when mutter no longer
//     counts the touch (owner, 11-10). Not when the phone went home after that tap or key: after Home nothing comes
//     up over the home screen by itself, only what the user opens next.
// Any other window gets nothing and stays where it is, in recents or behind the app on the screen.
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

export const ATTENTION_MS = 10000;      // how long a launched app's windows are its launch's
export const INTENT_MS = 3000;          // a window asking within this of a tap or key was asked for by the user

export class Attention {
    // launched(): the launch the launcher made last, {app, at (monotonic µs), splash (the app its splash is up for,
    // or null)}; home: home.js's Home, whose homeAt is the user time as the phone last went home
    constructor(launched, home) {
        this._launched = launched;
        this._home = home;
        this._gnome = Main.windowAttentionHandler;
        if (this._gnome)
            global.display.disconnectObject(this._gnome);
        global.display.connectObject(
            'window-demands-attention', (display, win) => this._onAttention(display, win),
            'window-marked-urgent', (display, win) => this._onAttention(display, win),
            this);
    }

    _onAttention(display, win) {
        if (!win || win.has_focus())
            return;
        const app = Shell.WindowTracker.get_default().get_window_app(win);
        const {app: launched, at, splash} = this._launched();
        if (app && app === launched && GLib.get_monotonic_time() - at < ATTENTION_MS * 1000) {
            // under the splash it is focused as the splash goes; after it, now
            if (splash !== app)
                Main.activateWindow(win);
            return;
        }
        const userTime = display.get_last_user_time();
        if (display.get_current_time_roundtrip() - userTime < INTENT_MS && userTime !== (this._home?.homeAt ?? 0))
            Main.activateWindow(win);
    }

    destroy() {
        global.display.disconnectObject(this);
        const handler = this._gnome;
        if (!handler)
            return;
        global.display.connectObject(
            'window-demands-attention', handler._onWindowDemandsAttention.bind(handler),
            'window-marked-urgent', handler._onWindowDemandsAttention.bind(handler),
            handler);
    }
}
