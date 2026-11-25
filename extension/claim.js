// A system swipe's touch is the system's once it is recognised. On Android the swipe up for home or recents, the
// quick switch along the gesture bar and the back swipe cancel the touch the app under them was given
// (ACTION_CANCEL), and the app sees nothing more of it. Mutter hands every touch to the Wayland client under it before
// the stage's gestures see it, and goes on doing so while one of them has taken the touch, until the stage is grabbed:
// the grab cancels the clients' touches as it starts (wl_touch.cancel), and the rest of the sequence, its lift
// included, reaches no client after it, not even once the grab is gone. Without it the quick switch along the bar
// paged Calendar back a week under the finger, and a swipe's lift went on to whatever window was under it.
//
// The grab is on the stage itself, which holds the stage's own gestures, so the swipe that took it goes on; a grab on
// any other actor would cancel it. The clients' keyboard focus is away while it lasts, back when it goes.
export class TouchClaim {
    constructor() {
        this._grab = null;
    }

    get held() {
        return !!this._grab;
    }

    // the touch is the swipe's: the apps' sequences cancelled, nothing more of them for any client
    take() {
        this._grab ??= global.stage.grab(global.stage);
    }

    release() {
        const grab = this._grab;
        this._grab = null;
        grab?.dismiss();
    }
}
