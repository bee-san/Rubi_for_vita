// Headless Yomitan's {sentence-furigana} for the sentence-readings measurement (compare.py): a pinned checkout's
// own importer, database and translator (as tools/yomitan_check/yomitan_lookup.mjs sets them up), and its
// scanning parser as Backend._textParseScanning runs it for an Anki note's sentence (anki-note-builder.js
// _getTextFurigana: parseText with the internal parser, scanning.length 16): at each position the first entry of
// Translator.findTerms('simple', the next 16 characters) with the profile's defaults and the Jiten frequency list
// as the sort dictionary, its reading placed by distributeFuriganaInflected, else one character on.
//
//   node yomitan_furigana.mjs --yomitan DIR --dicts DIR --lines lines.jsonl --out readings.tsv [--shard I/N]
//
// --dicts: the stored ZIPs of tools/yomitan_check/prepare_yomitan_dicts.py, imported in Rubi's order. Each line of
// lines.jsonl is {"id", "text"}; each output line `id<TAB>readings`, readings `start:end:reading` (code points of
// the text, separated by spaces), as host/sentence_readings.c writes Rubi's.
import {readFileSync, createWriteStream, createReadStream} from 'node:fs';
import {createInterface} from 'node:readline';
import {join, resolve} from 'node:path';
import {pathToFileURL, fileURLToPath} from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) { acc.push([a.slice(2), all[i + 1]]); }
    return acc;
}, []));
const YOMITAN = resolve(args.yomitan);
const DICTS = resolve(args.dicts);
const [SHARD, SHARDS] = (args.shard ?? '0/1').split('/').map(Number);
const SCAN_LENGTH = 16; // options.scanning.length's default (ext/data/schemas/options-schema.json)
const ORDER = ['Daijirin4-plain.zip', 'Sankoku8-plain.zip', 'JMdict-plain.zip', 'VNDB-Names-PSP-PSVita.zip',
    'jiten_freq_global.zip', 'kanjium-pitch.zip'];

const {IDBKeyRange, indexedDB} = await import(pathToFileURL(join(YOMITAN, 'node_modules/fake-indexeddb/build/esm/index.js')).href);
const extDir = join(YOMITAN, 'ext');
globalThis.indexedDB = indexedDB;
globalThis.IDBKeyRange = IDBKeyRange;
globalThis.self = {constructor: {name: 'Window'}};
globalThis.Worker = function Worker() { return {addEventListener: () => {}, terminate: () => {}}; };
globalThis.chrome = {runtime: {getURL: (path) => pathToFileURL(join(extDir, path.replace(/^\//, ''))).href}};
globalThis.fetch = async (url) => {
    let filePath;
    try { filePath = fileURLToPath(url); } catch { filePath = resolve(extDir, String(url).replace(/^[/\\]/, '')); }
    const content = readFileSync(filePath);
    return {ok: true, status: 200, statusText: 'OK', text: async () => content.toString('utf8'),
        json: async () => JSON.parse(content.toString('utf8'))};
};

const imp = (p) => import(pathToFileURL(join(extDir, p)).href);
const {DictionaryDatabase} = await imp('js/dictionary/dictionary-database.js');
const {DictionaryImporter} = await imp('js/dictionary/dictionary-importer.js');
const {Translator} = await imp('js/language/translator.js');
const {distributeFuriganaInflected, isCodePointJapanese} = await imp('js/language/ja/japanese.js');

const t0 = Date.now();
const log = (msg) => process.stderr.write(`[yomitan ${((Date.now() - t0) / 1000).toFixed(1)}s] ${msg}\n`);
const database = new DictionaryDatabase();
await database.prepare();
delete globalThis.Worker;
const {configure: configureZip} = await imp('lib/zip.js');
configureZip({useWebWorkers: false});
const importer = new DictionaryImporter({getImageDetails: async (content) => ({content, width: 100, height: 100})});
const titles = [];
for (const file of ORDER) {
    const data = readFileSync(join(DICTS, file));
    const {errors, result} = await importer.importDictionary(database, data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
        {prefixWildcardsSupported: false, yomitanVersion: '26.10.12.0'});
    if (errors.length || !result) { throw new Error(`${file}: ${errors.map(String).join('; ')}`); }
    titles.push(result.title);
    log(`imported ${file} as "${result.title}"`);
}
const translator = new Translator(database);
translator.prepare();
const enabledDictionaryMap = new Map(titles.map((name, index) => [name, {
    index, alias: name, allowSecondarySearches: false, partsOfSpeechFilter: true, useDeinflections: true,
}]));
// Backend._getTranslatorFindTermsOptions('simple', {matchType: 'exact', deinflect: true}, the profile's defaults)
const options = {
    matchType: 'exact', deinflect: true, primaryReading: '', mainDictionary: titles[0],
    sortFrequencyDictionary: 'Jiten', sortFrequencyDictionaryOrder: 'ascending',
    removeNonJapaneseCharacters: false, searchResolution: 'letter', textReplacements: [null],
    enabledDictionaryMap, excludeDictionaryDefinitions: null, language: 'ja', useAllFrequencyDictionaries: false,
};

// Backend._textParseScanning: the segments [{text, reading}] of the text, in UTF-16 offsets
async function parse(text) {
    const segments = [];
    let i = 0;
    while (i < text.length) {
        const codePoint = text.codePointAt(i);
        const character = String.fromCodePoint(codePoint);
        const substring = text.substring(i, i + SCAN_LENGTH);
        const {dictionaryEntries, originalTextLength} = await translator.findTerms('simple', substring, options);
        if (dictionaryEntries.length > 0 && originalTextLength > 0 &&
            (originalTextLength !== character.length || isCodePointJapanese(codePoint))) {
            const {headwords: [{term, reading}]} = dictionaryEntries[0];
            const source = substring.substring(0, originalTextLength);
            let at = i;
            for (const {text: t, reading: r} of distributeFuriganaInflected(term, reading, source)) {
                segments.push({start: at, end: at + t.length, reading: r});
                at += t.length;
            }
            i += originalTextLength;
        } else {
            i += character.length;
        }
    }
    return segments;
}

const cp = (text, utf16) => Array.from(text.substring(0, utf16)).length;
const out = createWriteStream(args.out);
const rl = createInterface({input: createReadStream(args.lines), crlfDelay: Infinity});
let lineNo = 0, done = 0;
for await (const line of rl) {
    if (!line.trim()) { continue; }
    if ((lineNo++ % SHARDS) !== SHARD) { continue; }
    const {id, text} = JSON.parse(line);
    const readings = (await parse(text)).filter((s) => s.reading.length > 0)
        .map((s) => `${cp(text, s.start)}:${cp(text, s.end)}:${s.reading}`);
    out.write(`${id}\t${readings.join(' ')}\n`);
    if (++done % 500 === 0) { log(`${done} lines`); }
}
await new Promise((r) => out.end(r));
log(`done: ${done} lines`);
