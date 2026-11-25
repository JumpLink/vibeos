// back-probe.sh's shell side: a11y.js's BackProbe asked about the app with pid ARGV[0], in this process's language;
// ARGV[1] the kind it must answer, ARGV[2] "stale" to start it off with a name the app no longer has (an app started
// over in place), ARGV[3] the case's name. Prints PASS or FAIL.
import GLib from 'gi://GLib';
import System from 'system';

import {BackProbe} from './a11y.js';

const [pid, want, stale, name] = [parseInt(ARGV[0]), ARGV[1], ARGV[2], ARGV[3]];
const logged = [];
globalThis.log = m => logged.push(m);
const probe = new BackProbe();
const loop = new GLib.MainLoop(null, false);
let ok = false;
(async () => {
    if (stale === 'stale') {
        await probe._connection();
        probe._pids.set(':1.999999', pid);
    }
    const result = await probe.probe(pid);
    ok = result.kind === want;
    print(ok ? 'PASS' : 'FAIL', name, JSON.stringify(result), logged.join(' / '));
})().catch(e => print('FAIL', name, `${e}`)).finally(() => loop.quit());
loop.run();
probe.destroy();
System.exit(ok ? 0 : 1);
