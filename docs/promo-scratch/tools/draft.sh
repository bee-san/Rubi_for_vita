#!/usr/bin/env bash
# tools/draft.sh N DEST: copies out/media and out/stills into DEST/draft-N (never overwrites) with a
# timestamped list of the captions and the mock-up shots, for the critic.
set -euo pipefail
cd "$(dirname "$0")/.."
n=$1; dest="$2/draft-$n"; O=${O:-out}
[ -e "$dest" ] && { echo "$dest exists: drafts are never overwritten" >&2; exit 1; }
mkdir -p "$dest/stills"
cp "$O/media/promo-scratch-16x9.mp4" "$O/media/promo-scratch-9x16.mp4" "$O/media/promo-scratch.gif" "$dest/"
cp "$O"/stills/*.jpg "$dest/stills/"
node --input-type=module -e "
import { CAPTIONS, MOCKUPS, STEPS } from './src/script.js';
const f = (t) => t.toFixed(2).padStart(5, ' ');
let s = '# promo-scratch draft $n (' + new Date().toISOString().slice(0, 16) + 'Z), source: $(git rev-parse --short HEAD)' + (process.argv[1] ? '' : '') + '\n\n## Captions (seconds)\n';
for (const c of CAPTIONS) s += f(c.t0) + '-' + f(c.t1) + '  line ' + c.line + ': ' + c.text + '\n';
s += '\n## Install steps (line 6, on screen with the caption)\n';
for (const st of STEPS) s += f(st.t) + '  ' + st.text + '\n';
s += f(STEPS.note.t) + '  (note) ' + STEPS.note.text + '\n';
s += '\n## Mock-up shots (features landing for test.4)\n';
for (const m of MOCKUPS) s += f(m.t0) + '-' + f(m.t1) + '  ' + m.what + '\n';
process.stdout.write(s);
" > "$dest/captions.txt"
(cd "$dest" && sha256sum *.mp4 *.gif > SHA256SUMS)
ls -la "$dest"
