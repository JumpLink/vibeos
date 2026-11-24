// A modal grab left behind, made visible: once the session has been idle for a while with the shell at rest on the
// home screen (awake, unlocked, nothing of ours open) and a modal grab is still up, the grab and its action mode go to
// the journal. Under such a grab every shell key can be filtered out (the power key, the volume keys). Only logged,
// once for each stack as it is; nothing is popped.
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const IDLE_MS = 10000;

export class ModalWatch {
    // atRest(): the shell at rest on the home screen, where no modal grab of ours belongs
    constructor(atRest) {
        this._atRest = atRest;
        this._logged = '';
        this._monitor = global.backend.get_core_idle_monitor();
        this._watch = this._monitor.add_idle_watch(IDLE_MS, () => this._check());
    }

    _check() {
        if (Main.modalCount === 0 || !this._atRest()) {
            this._logged = '';
            return;
        }
        const stack = Main.modalActorFocusStack.map(r =>
            `${r.actor.name || r.actor.constructor.name} over mode ${r.actionMode}`).join(', ');
        const line = `modal stack ${Main.modalCount}, mode ${Main.actionMode} at rest (${stack})`;
        if (line === this._logged)
            return;
        this._logged = line;
        console.warn(`vibeos: ${line}`);
    }

    destroy() {
        if (this._watch)
            this._monitor.remove_watch(this._watch);
        this._watch = 0;
    }
}
