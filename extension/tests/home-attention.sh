#!/bin/sh
# home-attention.sh SOURCE_DIR: Home's rule (home.js: every app window on the phone's screen to recents, one on a TV
# left where it is) and a window asking for attention (attention.js: no "is ready" banner of GNOME's, the app just
# launched or a tap's intent brought to the front, nothing by itself after Home), driven by home-attention.js on copies
# of home.js and attention.js whose GNOME Shell imports are its own stand-ins. GLib is real; nothing is drawn, no
# window is real and no bus is used.
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
cp "$here/home-attention.js" "$t/"
for f in home.js attention.js; do
	sed -E -e "s#'gi://(Meta|Shell)'#'./gi-\1.js'#" \
	    -e "s#'resource:///org/gnome/shell/ui/main.js'#'./main.js'#" "$src/$f" > "$t/$f"
done
# the stand-ins: the test fills them in (home-attention.js)
cat > "$t/stand-ins.js" <<'EOF'
export const Meta = {WindowType: {NORMAL: 0, DIALOG: 3}};
export const Shell = {WindowTracker: {get_default: () => ({get_window_app: win => win?.app ?? null})}};
export const Main = {
    layoutManager: {primaryIndex: 0},
    wm: {skipped: [], skipNextEffect(actor) {
        this.skipped.push(actor);
    }},
    windowAttentionHandler: null,
    activated: [],
};
EOF
printf "import {Meta as m} from './stand-ins.js';\nexport default m;\n" > "$t/gi-Meta.js"
printf "import {Shell as m} from './stand-ins.js';\nexport default m;\n" > "$t/gi-Shell.js"
cat > "$t/main.js" <<'EOF'
import {Main} from './stand-ins.js';
export const {layoutManager, wm} = Main;
export const windowAttentionHandler = Main.windowAttentionHandler;
export const activateWindow = win => Main.activated.push(win);
EOF
printf "export class SingleStack {\n    destroy() {}\n}\n" > "$t/stack.js"
gjs -m "$t/home-attention.js"
