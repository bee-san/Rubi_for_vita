// Ports suffix rules of Yomitan's Japanese transforms to the old deinflect.json format that
// tools/gen_deinflect.py reads (third_party/yomitan/deinflect-supplement.json):
//
//   node tools/yomitan_check/port_yomitan_rules.mjs --yomitan DIR > third_party/yomitan/deinflect-supplement.json
//
// DIR is a checkout of yomidevs/yomitan (its README pins the release). Only the transforms in
// FAMILIES are ported, each under the label Rubi shows; Yomitan's conditions become the old
// rule bits: v1, v5, vs, vk, vz and adj-i as they are, v as all five verb classes, -て as
// iru (the old -te rules' input), and the intermediate -ば, -ゃ, -ん and -ます, which the old
// rules do not have, as none (the old -ba and polite rules read the text itself, which has
// none). Rules the old table already has (with the same bits) are left out, and so is the old
// bare imperative negative (な) that Yomitan no longer has: tools/gen_deinflect.py removes it.
// Rubi's own additions (the long vowel mark forms of -え, which Yomitan reaches by turning ー
// into the vowel before its rules run) are in the same file under "rubi".
import {readFileSync} from 'node:fs';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) { acc.push([a.slice(2), all[i + 1]]); }
    return acc;
}, []));
const root = resolve(args.yomitan);
const {japaneseTransforms} = await import(pathToFileURL(join(root, 'ext/js/language/ja/japanese-transforms.js')).href);
const old = JSON.parse(readFileSync(new URL('../../third_party/yomichan/deinflect.json', import.meta.url)));

// Yomitan transform -> the label Rubi shows (an old label where the old table has the family)
// and which rules to take. Left out, measured on 5,000 hooked visual-novel lines
// (reports/yomitan-check.md): imperative negative slang (んな -> る: みんな read as 見る),
// -ん's さん, たん, はん and なん (the honorific さん, たん of りったん, ごはん, なん "what"),
// and -ゃ's や -> えば and にゃ -> ねば (the particle や, a cat's ニャ).
const SKIP = {'-ん': ['さん', 'たん', 'はん', 'なん'], '-ゃ': ['や', 'にゃ']};
const FAMILIES = [
    ['-おく', '-toku', (r) => !['とく', 'どく'].includes(r.kanaIn)], // the old -toku has とく/どく
    ['-て', '-te', (r) => r.kanaIn === 'まして'],
    ['-え', '-e', () => true],
    ['-ん', '-n', (r) => !SKIP['-ん'].includes(r.kanaIn)],
    ['-ちゃ', '-cha', () => true],
    ['-ゃ', '-ya', (r) => !SKIP['-ゃ'].includes(r.kanaIn)],
    ['-ざる', '-zaru', () => true],
    ['-ねば', '-neba', () => true],
    ['-やがる', '-yagaru', () => true],
    ['-やしない', '-yashinai', () => true],
];
const BITS = {'v1': ['v1'], 'v5': ['v5'], 'vs': ['vs'], 'vk': ['vk'], 'vz': ['vz'], 'adj-i': ['adj-i'],
    'v': ['v1', 'v5', 'vk', 'vs', 'vz'], '-て': ['iru'], '-ば': [], '-ゃ': [], '-ん': [], '-ます': []};
const bits = (conditions) => [...new Set(conditions.flatMap((c) => {
    if (!(c in BITS)) { throw new Error(`no old rule bits for condition ${c}`); }
    return BITS[c];
}))];
const have = new Set(Object.values(old).flat().map((r) => JSON.stringify([r.kanaIn, r.kanaOut, [...r.rulesIn].sort(), [...r.rulesOut].sort()])));

const add = {};
for (const [name, label, keep] of FAMILIES) {
    const transform = japaneseTransforms.transforms[name];
    if (!transform) { throw new Error(`Yomitan has no transform ${name}`); }
    for (const rule of transform.rules) {
        if (rule.type !== 'suffix') { continue; }
        const source = rule.isInflected.source;
        if (!source.endsWith('$') || /[\\^[\](){}.*+?|]/.test(source.slice(0, -1))) { throw new Error(`not a plain suffix: ${source}`); }
        let rules = [{kanaIn: source.slice(0, -1), kanaOut: rule.deinflected,
            rulesIn: bits(rule.conditionsIn), rulesOut: bits(rule.conditionsOut)}];
        if (!keep(rules[0])) { continue; }
        // -ゃ's outputs are -ば forms (なきゃ: なければ), which Yomitan only lets its -ば rules
        // read, never a dictionary entry: here each -ゃ rule is composed with those -ば rules
        // that end at a dictionary form (not れば -> ます's stem) into rules to it (なきゃ: ない).
        if (rule.conditionsOut.join() === '-ば') {
            rules = japaneseTransforms.transforms['-ば'].rules.filter((b) => b.type === 'suffix' &&
                bits(b.conditionsOut).length && rule.deinflected.endsWith(b.isInflected.source.slice(0, -1))).map((b) => ({
                kanaIn: rules[0].kanaIn,
                kanaOut: rule.deinflected.slice(0, rule.deinflected.length - (b.isInflected.source.length - 1)) + b.deinflected,
                rulesIn: rules[0].rulesIn, rulesOut: bits(b.conditionsOut)}));
        }
        for (const r of rules) {
            const id = JSON.stringify([r.kanaIn, r.kanaOut, [...r.rulesIn].sort(), [...r.rulesOut].sort()]);
            if (have.has(id) || (add[label] ?? []).some((x) => JSON.stringify([x.kanaIn, x.kanaOut, [...x.rulesIn].sort(), [...x.rulesOut].sort()]) === id)) { continue; }
            (add[label] ??= []).push(r);
        }
    }
}
// Rubi's own: each -え rule (old and ported) again with ー for its last え or い (知らねー,
// すげー, 寒みー), the way Yomitan's katakana-to-hiragana step turns ー into the vowel.
const prolonged = [];
for (const r of [...old['-e'], ...(add['-e'] ?? [])]) {
    if (!/[えい]$/.test(r.kanaIn)) { continue; }
    const kanaIn = `${r.kanaIn.slice(0, -1)}ー`;
    if (!prolonged.some((p) => p.kanaIn === kanaIn && p.kanaOut === r.kanaOut)) {
        prolonged.push({...r, kanaIn});
    }
}
const out = {
    source: 'yomidevs/yomitan ext/js/language/ja/japanese-transforms.js (GPL-3.0-or-later), ported by tools/yomitan_check/port_yomitan_rules.mjs',
    remove: [{reason: 'imperative negative', kanaIn: 'な', kanaOut: ''}],
    add,
    rubi: {'-e': prolonged},
};
process.stdout.write(`${JSON.stringify(out, null, 1)}\n`);
