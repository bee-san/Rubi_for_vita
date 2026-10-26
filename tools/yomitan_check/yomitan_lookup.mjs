// Headless Yomitan: its own dictionary importer, database and translator (a pinned checkout's
// ext/js, as its test suite runs them in Node with fake-indexeddb), looking up taps in text the
// way a popup does: the text from the tapped character, 16 characters (scanning.length's
// default), through Translator.findTerms in the default 'group' result mode with the profile's
// default options, and the Jiten frequency list as the sort dictionary (ascending: its ranks).
//
//   node yomitan_lookup.mjs --yomitan DIR --dicts DIR --taps taps.jsonl --out out.jsonl
//        [--top N] [--shard I/N]
//
// --dicts holds the stored ZIPs of prepare_yomitan_dicts.py; they are imported in Rubi's
// order (its bundle's Daijirin4 > Sankoku8 > JMdict, then the names), then the frequency and
// pitch dictionaries. Each line of taps.jsonl is {"id", "text", "taps": [code point offsets]};
// each output line is one tap: {"id", "tap", "len" (code points matched), "entries": the first
// N dictionary entries, as the popup lists them}.
import {readFileSync, createWriteStream} from 'node:fs';
import {createInterface} from 'node:readline';
import {createReadStream} from 'node:fs';
import {join, resolve} from 'node:path';
import {pathToFileURL, fileURLToPath} from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) { acc.push([a.slice(2), all[i + 1]]); }
    return acc;
}, []));
const YOMITAN = resolve(args.yomitan);
const DICTS = resolve(args.dicts);
const TOP = Number(args.top ?? 5);
const [SHARD, SHARDS] = (args.shard ?? '0/1').split('/').map(Number);
const SCAN_LENGTH = 16; // options.scanning.length's default (ext/data/schemas/options-schema.json)
const ORDER = ['Daijirin4-plain.zip', 'Sankoku8-plain.zip', 'JMdict-plain.zip', 'VNDB-Names-PSP-PSVita.zip',
    'jiten_freq_global.zip', 'kanjium-pitch.zip'];

const {IDBKeyRange, indexedDB} = await import(pathToFileURL(join(YOMITAN, 'node_modules/fake-indexeddb/build/esm/index.js')).href);
const extDir = join(YOMITAN, 'ext');
globalThis.indexedDB = indexedDB;
globalThis.IDBKeyRange = IDBKeyRange;
globalThis.self = {constructor: {name: 'Window'}}; // the database runs in this thread, as in a page
// test/utilities/database.js: the media worker a page would start (no dictionary here has images)
globalThis.Worker = function Worker() { return {addEventListener: () => {}, terminate: () => {}}; };
globalThis.chrome = {runtime: {getURL: (path) => pathToFileURL(join(extDir, path.replace(/^\//, ''))).href}};
globalThis.fetch = async (url) => { // test/mocks/common.js: the extension's own files
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

const t0 = Date.now();
const log = (msg) => process.stderr.write(`[yomitan ${((Date.now() - t0) / 1000).toFixed(1)}s] ${msg}\n`);

const database = new DictionaryDatabase();
await database.prepare();
delete globalThis.Worker; // only the database's media worker: zip.js then decompresses in this thread
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
    log(`imported ${file} as "${result.title}" (${JSON.stringify(result.counts?.terms ?? result.counts)})`);
}
const translator = new Translator(database);
translator.prepare();

// Backend._getTranslatorEnabledDictionaryMap with every dictionary enabled at its defaults
// (allowSecondarySearches false, partsOfSpeechFilter true, useDeinflections true), in the order above.
const enabledDictionaryMap = new Map(titles.map((name, index) => [name, {
    index, alias: name, allowSecondarySearches: false, partsOfSpeechFilter: true, useDeinflections: true,
}]));
const options = { // Backend._getTranslatorFindTermsOptions('group', {}, defaults)
    matchType: 'exact', deinflect: true, primaryReading: '', mainDictionary: titles[0],
    sortFrequencyDictionary: 'Jiten', sortFrequencyDictionaryOrder: 'ascending',
    removeNonJapaneseCharacters: false, searchResolution: 'letter', textReplacements: [null],
    enabledDictionaryMap, excludeDictionaryDefinitions: null, language: 'ja', useAllFrequencyDictionaries: false,
};

function entryOut(e) {
    const dictionaries = [...new Set(e.definitions.map((d) => d.dictionary))];
    return {
        headwords: e.headwords.map((h) => ({term: h.term, reading: h.reading,
            sources: h.sources.map((s) => [s.originalText, s.transformedText, s.deinflectedText, s.matchType, s.matchSource, s.isPrimary])})),
        dictionaries,
        dictionaryIndex: e.dictionaryIndex,
        inflections: e.inflectionRuleChainCandidates.map((c) => ({source: c.source,
            rules: c.inflectionRules.map((r) => (typeof r === 'string' ? r : r.name))})),
        processors: e.textProcessorRuleChainCandidates,
        frequencies: e.frequencies.map((f) => ({dictionary: f.dictionary, headwordIndex: f.headwordIndex,
            hasReading: f.hasReading, frequency: f.frequency, displayValue: f.displayValue})),
        pitches: e.pronunciations.map((p) => ({dictionary: p.dictionary, headwordIndex: p.headwordIndex,
            positions: p.pronunciations.filter((x) => x.type === 'pitch-accent').map((x) => x.positions)})),
        definitionTags: [...new Set(e.definitions.flatMap((d) => d.tags.map((t) => t.name)))].slice(0, 12),
        wordClasses: [...new Set(e.headwords.flatMap((h) => h.wordClasses))],
    };
}

const out = createWriteStream(args.out);
const rl = createInterface({input: createReadStream(args.taps), crlfDelay: Infinity});
let lineNo = 0, taps = 0;
const tLookups = Date.now();
for await (const line of rl) {
    if (!line.trim()) { continue; }
    if ((lineNo++ % SHARDS) !== SHARD) { continue; }
    const {id, text, taps: positions} = JSON.parse(line);
    const chars = Array.from(text);
    for (const tap of positions) {
        const scan = chars.slice(tap, tap + SCAN_LENGTH).join('');
        const {dictionaryEntries, originalTextLength} = await translator.findTerms('group', scan, options);
        const entries = dictionaryEntries.slice(0, TOP).map(entryOut);
        const len = Array.from(scan.slice(0, originalTextLength)).length;
        // Over the whole list: the names of the first entry's span (Rubi stacks those below its
        // word) and the first entry from a dictionary of words (Rubi's page 0 when a name leads).
        const isName = (e) => e.definitions.some((d) => d.dictionary.startsWith('VNDB Names'));
        const span = (e) => e.headwords[0].sources[0].originalText.length;
        const names = dictionaryEntries.filter((e) => isName(e) && dictionaryEntries[0] && span(e) === span(dictionaryEntries[0]))
            .map((e) => ({term: e.headwords[0].term, reading: e.headwords[0].reading}));
        const firstWord = dictionaryEntries.find((e) => !isName(e));
        const word = firstWord ? {term: firstWord.headwords[0].term, reading: firstWord.headwords[0].reading,
            len: Array.from(firstWord.headwords[0].sources[0].originalText).length} : null;
        out.write(JSON.stringify({id, tap, len, n: dictionaryEntries.length, entries, names, word}) + '\n');
        if (++taps % 2000 === 0) { log(`${taps} taps, ${((Date.now() - tLookups) / taps).toFixed(2)} ms each`); }
    }
}
await new Promise((r) => out.end(r));
log(`done: ${taps} taps in ${((Date.now() - tLookups) / 1000).toFixed(1)} s`);
