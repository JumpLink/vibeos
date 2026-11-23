# modules.sh DIR: the stand-ins power.js, chord.js and keylaunch.js import, written into DIR beside copies of them
# (power-keys.sh), each a small module re-exporting mock/shell.js or saying what the real one would
d=$1
for m in Clutter Meta Shell St; do
	printf "import {%s as m} from './shell.js';\nexport default m;\n" "$m" > "$d/gi-$m.js"
done
printf "import {Main} from './shell.js';\nexport const {layoutManager} = Main;\n" > "$d/main.js"
printf "export class EventEmitter {\n    emit() {}\n}\n" > "$d/signals.js"
printf "export const captureScreenshot = async () => {};\n" > "$d/screenshot.js"
printf "export class Aod {}\nexport class Panel {}\n" > "$d/aod.js"
printf "export class HardwareKeys {}\n" > "$d/keys.js"
printf "export const animate = () => ({stop() {}});\nexport const bezier = () => t => t;\nexport const LINEAR = t => t;\nexport const FAST_OUT_SLOW_IN = t => t;\n" > "$d/motion.js"
printf "export const displayConfig = async () => null;\nexport const STATE_TYPE = null;\n" > "$d/rotation.js"
printf "export const geometry = {connectObject() {}, disconnectObject() {}};\nexport const TABLET = false;\n" > "$d/units.js"
printf "export const Color = {white: 0};\nexport const cogl = () => null;\n" > "$d/palette.js"
printf "export const overWindows = () => {};\n" > "$d/scanout.js"
