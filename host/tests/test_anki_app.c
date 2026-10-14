/* The Overlay app's Anki page logic (core/anki_app.c): finding AnkiConnect
 * (the saved address, else a search of the /24 that saves what it finds),
 * the connection test (version, the Kiku note type's fields, the Kiku deck)
 * and sending the queue, run here against the real queue storage
 * (shell/anki_queue.c and core/anki_queue.c on real files in a temporary
 * directory, through the Vita file API mapped onto POSIX) and a fake
 * AnkiConnect that keeps notes by their vita_queue_ tag. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main acutest_main
#include "acutest.h"
#undef main
#include "anki_app.h"
#include "../../shell/anki_queue.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include "../../shell/shell.h"

/* ---------------- the memory card: real files ---------------- */

static DIR *directory;
static char directory_path[256];
static char logs[8192];
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs)) {
        logs[n] = '\n';
        logs[n + 1] = '\0';
    }
}
int sceIoOpen(const char *p, int flags, unsigned mode) { return open(p, flags, mode); }
int sceIoClose(int fd) { return close(fd); }
int sceIoRead(int fd, void *p, size_t n) { return (int)read(fd, p, n); }
int sceIoWrite(int fd, const void *p, size_t n) { return (int)write(fd, p, n); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return mkdir(p, mode); }
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) st->st_mode = s.st_mode;
    return rc;
}
int sceIoRename(const char *from, const char *to) { return rename(from, to); }
int sceIoRemove(const char *p) { return unlink(p); }
int sceIoSyncByFd(int fd, int flag) { (void)flag; return fsync(fd); }
int sceIoSync(const char *device, unsigned flags) { (void)device; (void)flags; return 0; }
int sceIoDopen(const char *p)
{
    snprintf(directory_path, sizeof(directory_path), "%s", p);
    directory = opendir(p);
    return directory ? 10000 : -1;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e = readdir(directory);
    char path[512];
    (void)fd;
    if (!e) return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", directory_path, e->d_name);
    TEST_ASSERT(sceIoGetstat(path, &ent->d_stat) == 0);
    return 1;
}
int sceIoDclose(int fd) { (void)fd; int rc = closedir(directory); directory = NULL; return rc; }

#include "../../shell/anki_queue.c"
#include "../../core/anki_app.c"

/* ---------------- a fake AnkiConnect on the LAN ---------------- */

#define MAX_NOTES 64
static struct {
    uint32_t at;          /* where AnkiConnect listens (host byte order) */
    int port;
    int kiku;             /* the note type exists */
    const char *fields;   /* its fields' JSON array */
    int deck;             /* the Kiku deck exists */
    int version;
    char tag[MAX_NOTES][48];
    int n_notes, adds, finds, decks;
    const char *duplicate;
    int offline_after;    /* connections before it goes away (-1: never) */
    int connections;
    char actions[1024];
    char last_add[64 * 1024];
} anki;
static int cancel_after = -1; /* the user presses Circle after this many connections */

typedef struct {
    char out[64 * 1024];
    size_t out_len;
    char in[8192];
    size_t in_len, pos;
    int answered;
} Conn;
static Conn conn;

static void action(const char *what)
{
    if (strlen(anki.actions) + strlen(what) + 2 < sizeof(anki.actions)) {
        if (anki.actions[0]) strcat(anki.actions, " ");
        strcat(anki.actions, what);
    }
}

static void respond(const char *json)
{
    conn.in_len = (size_t)snprintf(conn.in, sizeof(conn.in),
                                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
                                   (unsigned long)strlen(json), json);
}

static void serve(void)
{
    const char *body = strstr(conn.out, "\r\n\r\n"), *t;
    char json[2048];
    body = body ? body + 4 : "";
    if (strstr(body, "\"action\":\"version\"")) {
        action("version");
        snprintf(json, sizeof(json), "{\"result\": %d, \"error\": null}", anki.version);
        respond(json);
    } else if (strstr(body, "\"action\":\"modelFieldNames\"")) {
        action("fields");
        TEST_CHECK(strstr(body, "\"modelName\":\"Kiku\"") != NULL);
        if (anki.kiku)
            snprintf(json, sizeof(json), "{\"result\": %s, \"error\": null}", anki.fields);
        else
            snprintf(json, sizeof(json), "{\"result\": null, \"error\": \"model was not found: Kiku\"}");
        respond(json);
    } else if (strstr(body, "\"action\":\"deckNames\"")) {
        action("decks");
        respond(anki.deck ? "{\"result\": [\"Default\", \"Kiku\"], \"error\": null}"
                          : "{\"result\": [\"Default\"], \"error\": null}");
    } else if (strstr(body, "\"action\":\"findNotes\"")) {
        int k = 0;
        char tag[48];
        anki.finds++;
        action("find");
        t = strstr(body, "tag:vita_queue_");
        snprintf(tag, sizeof(tag), "%.43s", t ? t + 4 : "");
        k = snprintf(json, sizeof(json), "{\"result\": [");
        for (int i = 0, n = 0; i < anki.n_notes; i++)
            if (!strcmp(anki.tag[i], tag))
                k += snprintf(json + k, sizeof(json) - (size_t)k, "%s%lld", n++ ? ", " : "", 1791436200000ll + i);
        snprintf(json + k, sizeof(json) - (size_t)k, "], \"error\": null}");
        respond(json);
    } else if (strstr(body, "\"action\":\"addNote\"")) {
        anki.adds++;
        action("add");
        snprintf(anki.last_add, sizeof(anki.last_add), "%s", body);
        if (!anki.kiku) {
            respond("{\"result\": null, \"error\": \"model was not found: Kiku\"}");
            return;
        }
        if (anki.duplicate && strstr(body, anki.duplicate)) {
            respond("{\"result\": null, \"error\": \"cannot create note because it is a duplicate\"}");
            return;
        }
        t = strstr(body, "\"vita_queue_");
        TEST_ASSERT(t && anki.n_notes < MAX_NOTES);
        snprintf(anki.tag[anki.n_notes], sizeof(anki.tag[0]), "%.43s", t + 1);
        snprintf(json, sizeof(json), "{\"result\": %lld, \"error\": null}", 1791436200000ll + anki.n_notes++);
        respond(json);
    } else {
        action("?");
        respond("{\"result\": null, \"error\": \"unsupported action\"}");
    }
}

static int conn_send(void *ctx, const void *p, size_t n)
{
    size_t k = sizeof(conn.out) - 1 - conn.out_len < n ? sizeof(conn.out) - 1 - conn.out_len : n;
    (void)ctx;
    memcpy(conn.out + conn.out_len, p, k);
    conn.out_len += k;
    conn.out[conn.out_len] = '\0';
    return (int)n;
}

static int conn_recv(void *ctx, void *p, size_t n)
{
    size_t k;
    (void)ctx;
    if (!conn.answered) {
        conn.answered = 1;
        serve();
    }
    k = conn.in_len - conn.pos < n ? conn.in_len - conn.pos : n;
    memcpy(p, conn.in + conn.pos, k);
    conn.pos += k;
    return (int)k;
}

static uint32_t parse_ip(const char *host)
{
    unsigned a, b, c, d;
    return sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? a << 24 | b << 16 | c << 8 | d : 0;
}

static int user_cancelled;
static int fake_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    (void)ud;
    (void)timeout_us;
    (void)io_timeout_us;
    if (user_cancelled)
        return VJO_E_CANCELLED;
    if (cancel_after >= 0 && anki.connections >= cancel_after) {
        user_cancelled = 1;
        return VJO_E_CANCELLED;
    }
    anki.connections++;
    if (parse_ip(host) != anki.at || port != anki.port ||
        (anki.offline_after >= 0 && anki.connections > anki.offline_after))
        return VJO_E_NET;
    memset(&conn, 0, sizeof(conn));
    out->ctx = &conn;
    out->send = conn_send;
    out->recv = conn_recv;
    return VJO_OK;
}
static void fake_disconnect(void *ud, VjoConn *c) { (void)ud; (void)c; }
static uint64_t fake_time(void *ud) { (void)ud; return 1791158400u; }

/* ---------------- the app's services ---------------- */

static char host_file[80];
static int have_ip = 1, probes, statuses;
static char last_status[160];
static int local_ipv4(void *ud, uint32_t *ip, uint32_t *mask)
{
    (void)ud;
    *ip = 0xC0A8011Eu; /* 192.168.1.30/24 */
    *mask = 0xFFFFFF00u;
    return have_ip ? 0 : -1;
}
static int probe(void *ud, const uint32_t *ips, int n, int port, int window_us, uint8_t *open)
{
    (void)ud;
    TEST_CHECK(n <= VJO_ANKI_PROBE_MAX && window_us == VJO_ANKI_SCAN_WINDOW_US);
    probes++;
    for (int i = 0; i < n; i++)
        open[i] = ips[i] == anki.at && port == anki.port;
    return n;
}
static int load_host(void *ud, char *line, size_t cap)
{
    (void)ud;
    if (!host_file[0]) return -1;
    snprintf(line, cap, "%s", host_file);
    return 0;
}
static void save_host(void *ud, const char *line) { (void)ud; snprintf(host_file, sizeof(host_file), "%s", line); }
static int app_stopped(void *ud) { (void)ud; return user_cancelled; }
static void app_status(void *ud, const char *msg)
{
    (void)ud;
    statuses++;
    snprintf(last_status, sizeof(last_status), "%s", msg);
}
static const VjoAnkiAppOps ops = {NULL, local_ipv4, probe, load_host, save_host, app_stopped, app_status};

/* ---------------- fixtures ---------------- */

#define KIKU_FIELDS                                                                                           \
    "[\"Expression\", \"ExpressionFurigana\", \"ExpressionReading\", \"ExpressionAudio\", \"RelatedExpression\", " \
    "\"SelectionText\", \"MainDefinition\", \"DefinitionPicture\", \"Sentence\", \"SentenceFurigana\", "          \
    "\"SentenceTranslation\", \"SentenceAudio\", \"Picture\", \"Glossary\", \"Hint\", \"IsWordAndSentenceCard\", "  \
    "\"IsClickCard\", \"IsSentenceCard\", \"IsAudioCard\", \"PitchPosition\", \"PitchCategories\", \"Frequency\", " \
    "\"FreqSort\", \"MiscInfo\"]"

static uint8_t mem[1024 * 1024];
static VjoArena A;
static VjoPlatform P;
static VjoConfig cfg;

static void clear_queue(void)
{
    char p[512];
    DIR *d = opendir(VJO_ANKI_QUEUE_DIR);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof(p), VJO_ANKI_QUEUE_DIR "/%s", e->d_name);
            unlink(p);
        }
    closedir(d);
}

static void reset(void)
{
    memset(&anki, 0, sizeof(anki));
    anki.at = 0xC0A80117u; /* 192.168.1.23 */
    anki.port = VJO_ANKI_PORT;
    anki.kiku = anki.deck = 1;
    anki.version = 6;
    anki.fields = KIKU_FIELDS;
    anki.offline_after = -1;
    cancel_after = -1;
    user_cancelled = 0;
    host_file[0] = last_status[0] = logs[0] = '\0';
    have_ip = 1;
    probes = statuses = 0;
    clear_queue();
    memset(mem, 0xA5, sizeof(mem));
    vjo_arena_init(&A, mem, sizeof(mem));
    memset(&P, 0, sizeof(P));
    P.connect = fake_connect;
    P.disconnect = fake_disconnect;
    P.unix_time = fake_time;
    vjo_config_defaults(&cfg);
    cfg.anki_mining = 1;
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "192.168.1.23");
}

/* A queued card, as × wrote it (or an older build did). */
static void queue_record(const char *json)
{
    char id[VJO_QUEUE_ID_SIZE], path[256];
    FILE *f;
    TEST_ASSERT(vjo_queue_count() >= 0); /* the directory exists */
    vjo_queue_id(json, strlen(json), id);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    fputs(json, f);
    fclose(f);
}

static void queue_card(const char *spelling, const char *reading, const char *meaning)
{
    const char *m[1] = {meaning};
    VjoAnkiNote n = {.spelling = spelling, .reading = reading, .rank = VJO_NO_RANK, .meanings = m, .n_meanings = 1,
                     .sentence = spelling, .hl_start = 0, .hl_end = (int)strlen(spelling), .sf_hl_start = -1,
                     .sf_hl_end = -1, .misc = "SOME GAME (2026-10-14 17:12)", .game = "SOME GAME",
                     .title_id = "PCSG00000", .platform = "vita", .unix_time = 1791727902, .ocr_engine = "vocr",
                     .ocr_model = "VOCR3_H15_w8.vocr"};
    size_t mark = vjo_arena_mark(&A);
    char *json = vjo_queue_encode(&A, &n, 0);
    TEST_ASSERT(json != NULL);
    queue_record(json);
    vjo_arena_release(&A, mark);
}

/* ---------------- tests ---------------- */

static void test_address(void)
{
    char out[64];
    static const struct {
        const char *typed, *want;
    } ok[] = {
        {"192.168.1.23", "192.168.1.23"}, {" 192.168.1.23:8766 ", "192.168.1.23:8766"},
        {"http://192.168.1.23:8765/", "192.168.1.23:8765"}, {"HTTP://my-pc.local", "my-pc.local"},
        {"AUTO", "auto"}, {"Auto", "auto"}, {"", ""}, {"   ", ""},
    };
    static const char *const bad[] = {"192.168.1.23:", "192.168.1.23:70000", "https://192.168.1.23", "a b",
                                      "http://", ":8765", "1.2.3.4/x"};
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        TEST_CHECK(vjo_anki_app_address(ok[i].typed, out, sizeof(out)) == 0 && !strcmp(out, ok[i].want));
        TEST_MSG("'%s' -> '%s'", ok[i].typed, out);
    }
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK(vjo_anki_app_address(bad[i], out, sizeof(out)) == -1);
        TEST_MSG("'%s' -> '%s'", bad[i], out);
    }
}

static void test_connection_test(void)
{
    VjoAnkiTest t;
    char msg[240];
    /* all there */
    reset();
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK(!strcmp(anki.actions, "version fields decks") && t.kiku && !t.missing && t.deck && !t.searched);
    TEST_CHECK(!strcmp(msg, "Connected to AnkiConnect at 192.168.1.23:8765. Kiku note type OK; deck Kiku exists."));
    TEST_MSG("%s", msg);
    /* no deck yet: the first card creates it */
    reset();
    anki.deck = 0;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK(strstr(msg, "deck Kiku will be created with the first card") != NULL);
    /* no Kiku */
    reset();
    anki.kiku = 0;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_ANKI && !t.kiku);
    TEST_CHECK(strstr(msg, "but Anki has no Kiku note type: import Kiku_v2.1.0.apkg") != NULL);
    TEST_MSG("%s", msg);
    /* an older Kiku without two of the fields a card fills */
    reset();
    anki.fields = "[\"Expression\", \"ExpressionFurigana\", \"ExpressionReading\", \"ExpressionAudio\", "
                  "\"MainDefinition\", \"Sentence\", \"SentenceFurigana\", \"Picture\", \"PitchPosition\", "
                  "\"PitchCategories\", \"Frequency\", \"FreqSort\"]";
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_OK && t.missing == 2);
    TEST_CHECK(!strcmp(t.missing_names, "Glossary, MiscInfo") && strstr(msg, "lacks 2 fields (Glossary, MiscInfo)"));
    TEST_MSG("%s", msg);
    /* nothing listening, on a port of its own */
    reset();
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "192.168.1.23:9000");
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_NET);
    TEST_CHECK(strstr(msg, "No AnkiConnect at 192.168.1.23:9000. Is Anki open, AnkiConnect's webBindAddress "
                           "0.0.0.0, and the port allowed") == msg);
    TEST_MSG("%s", msg);
    /* something else answers there */
    reset();
    anki.version = 5;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_PARSE);
    TEST_CHECK(strstr(msg, "answers, but not as AnkiConnect") != NULL);
    /* no address */
    reset();
    cfg.anki_host[0] = '\0';
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_NET && !anki.connections);
    TEST_CHECK(!strcmp(msg, "Set AnkiConnect's address first: your computer's IP, or auto."));
}

static void test_auto_search(void)
{
    VjoAnkiTest t;
    char msg[240];
    /* a search finds it and saves it for △ */
    reset();
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "auto");
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_OK && t.searched);
    TEST_CHECK(!strcmp(host_file, "192.168.1.23:8765\n") && probes == 1 && statuses >= 1);
    TEST_CHECK(strstr(msg, "at 192.168.1.23:8765 (found and saved)") != NULL);
    TEST_CHECK(strstr(last_status, "Searching 192.168.1.0 for AnkiConnect") == last_status);
    TEST_MSG("%s / %s", msg, last_status);
    /* the saved address answers: no search */
    probes = 0;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_OK && !t.searched && !probes);
    /* it moved: the saved one fails, a new search finds it */
    anki.at = 0xC0A80180u; /* 192.168.1.128: in the fourth batch of 32 */
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_OK && t.searched);
    TEST_CHECK(!strcmp(host_file, "192.168.1.128:8765\n") && probes == 4);
    TEST_MSG("%s probes %d", host_file, probes);
    /* nothing on the network */
    anki.at = 0x0A000001u;
    host_file[0] = '\0';
    probes = 0;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_NOT_FOUND && probes == 8);
    TEST_CHECK(strstr(msg, "No AnkiConnect found on this network") == msg && !host_file[0]);
    /* no network address (Wi-Fi off) */
    have_ip = 0;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_NET);
    TEST_CHECK(!strcmp(msg, "No network address: is Wi-Fi on and connected?"));
    TEST_MSG("%s", msg);
    /* Circle during the search */
    have_ip = 1;
    user_cancelled = 1;
    TEST_CHECK(vjo_anki_app_test(&A, &P, &ops, &cfg, &t, msg, sizeof(msg)) == VJO_E_CANCELLED);
    TEST_CHECK(!strcmp(msg, "Cancelled."));
}

static void test_send(void)
{
    VjoAnkiSend s;
    char msg[240];
    /* empty */
    reset();
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK && !anki.connections);
    TEST_CHECK(!strcmp(msg, "No cards queued. × in the overlay saves the word shown."));
    /* three cards: a v3 one, the test build's v2 one, #5's v1 one; Kiku notes, oldest ID first */
    reset();
    queue_card("猫", "ねこ", "[JMdict Plain]\ncat");
    queue_record("{\"version\":2,\"spelling\":\"部屋\",\"reading\":\"へや\",\"sentence\":\"隣の部屋で\","
                 "\"rank\":99999,\"hl_start\":6,\"hl_end\":12,\"picture\":false,\"meanings\":[\"[Daijirin4 Plain]"
                 "\\nA room.\"],\"sentence_furigana\":\"隣の部屋で\",\"sf_hl_start\":6,\"sf_hl_end\":12,"
                 "\"source\":null,\"pitch\":[],\"pos\":[],\"misc\":\"SOME GAME (2026-10-11 17:11)\"}\n");
    queue_record("{\"version\":1,\"spelling\":\"犬\",\"reading\":\"いぬ\",\"sentence\":\"犬が\",\"rank\":1234,"
                 "\"hl_start\":0,\"hl_end\":3,\"picture\":false,\"meanings\":[\"dog\"]}\n");
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK(s.queued == 3 && s.sent == 3 && !s.duplicates && !s.unreadable && vjo_queue_count() == 0);
    TEST_CHECK(!strcmp(anki.actions, "find add find add find add") && anki.n_notes == 3);
    TEST_CHECK(!strcmp(msg, "Sent 3 to Anki (deck Kiku)."));
    TEST_CHECK(strstr(last_status, "Sending to Anki… 2 sent, 1 left") == last_status);
    TEST_MSG("%s / %s / %s", msg, last_status, anki.actions);
    TEST_CHECK(strstr(anki.last_add, "\"deckName\":\"Kiku\",\"modelName\":\"Kiku\"") != NULL);
    /* again: nothing to do, and Anki has each card once */
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK && anki.n_notes == 3);
    /* a duplicate is archived and the rest goes on; a damaged card is kept */
    reset();
    queue_card("猫", "ねこ", "cat");
    queue_card("好き", "すき", "liked");
    queue_record("{\"version\":2,\"spelling\":\"x\"}\n");
    anki.duplicate = "\"Expression\":\"猫\"";
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK);
    TEST_CHECK(s.sent == 1 && s.duplicates == 1 && s.unreadable == 1 && vjo_queue_count() == 1);
    TEST_CHECK(!strcmp(msg, "Sent 1 to Anki (deck Kiku), some already there, some kept: unreadable (see log.txt)."));
    TEST_MSG("%s", msg);
    TEST_CHECK(strstr(logs, "checksum or format mismatch") != NULL);
    /* Anki goes away after the first card: the rest stay, a retry sends them once */
    reset();
    queue_card("猫", "ねこ", "cat");
    anki.offline_after = 2; /* find, add answered; then gone */
    queue_card("好き", "すき", "liked");
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_E_NET);
    TEST_CHECK(s.sent == 1 && vjo_queue_count() == 1 && !strcmp(msg, "1 sent; the rest stay queued: Anki offline"));
    TEST_MSG("%s", msg);
    anki.offline_after = -1;
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK && s.sent == 1 &&
               anki.n_notes == 2 && vjo_queue_count() == 0);
    /* no Kiku: the card stays, and the page says why */
    reset();
    queue_card("猫", "ねこ", "cat");
    anki.kiku = 0;
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_E_ANKI && vjo_queue_count() == 1);
    TEST_CHECK(!strcmp(msg, "0 sent; the rest stay queued: Kiku note type not found in Anki: import Kiku (docs/anki.md)"));
    TEST_MSG("%s", msg);
    /* Circle between cards: the rest stay */
    reset();
    queue_card("猫", "ねこ", "cat");
    queue_card("好き", "すき", "liked");
    cancel_after = 2;
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_E_CANCELLED);
    TEST_CHECK(s.sent == 1 && vjo_queue_count() == 1 && !strcmp(msg, "Stopped: 1 sent, the rest stay queued."));
    TEST_MSG("%s", msg);
    /* auto, not found: nothing sent, the cards wait */
    reset();
    queue_card("猫", "ねこ", "cat");
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "auto");
    anki.at = 0x0A000001u;
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_E_NOT_FOUND);
    TEST_CHECK(vjo_queue_count() == 1 && strstr(msg, "No AnkiConnect found") == msg);
}

/* A card with the most a record holds: 8 dictionaries' long entries (the
 * record under 32 KiB). The overlay's 128 KiB arena cannot build its note
 * (shell_anki keeps it "too large"); Send now's 512 KiB can. */
static void test_send_the_largest_card(void)
{
    static char text[8][3700];
    static const char *m[8][1];
    VjoAnkiGloss more[7];
    VjoAnkiNote n = {.spelling = "好き", .reading = "すき", .rank = VJO_NO_RANK, .sentence = "好き",
                     .hl_start = 0, .hl_end = 6, .sf_hl_start = -1, .sf_hl_end = -1, .misc = "G (2026-10-14 17:12)"};
    VjoAnkiSend s;
    char msg[240], *json;
    size_t mark;
    reset();
    for (int k = 0; k < 8; k++) {
        int o = snprintf(text[k], sizeof(text[k]), "[Dictionary %d]\n", k);
        while (o + 3 < (int)sizeof(text[k]) - 1) {
            memcpy(text[k] + o, "好", 3);
            o += 3;
        }
        text[k][o] = '\0';
        m[k][0] = text[k];
        if (k)
            more[k - 1] = (VjoAnkiGloss){NULL, "好き", "すき", m[k], 1};
    }
    n.meanings = m[0];
    n.n_meanings = 1;
    n.more = more;
    n.n_more = 7;
    mark = vjo_arena_mark(&A);
    json = vjo_queue_encode(&A, &n, 0);
    TEST_ASSERT(json != NULL);
    printf("largest card: record %u B\n", (unsigned)strlen(json));
    TEST_CHECK(strlen(json) > 28000 && strlen(json) <= VJO_QUEUE_TEXT_MAX);
    queue_record(json);
    vjo_arena_release(&A, mark);
    TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK && s.sent == 1 &&
               !s.too_large && vjo_queue_count() == 0);
    TEST_MSG("%s", msg);
    TEST_CHECK(strstr(anki.last_add, "\"Glossary\":") && strstr(anki.last_add, "Dictionary 7"));
    printf("largest card: sent, addNote %u B, arena peak %u of %u B\n", (unsigned)strlen(anki.last_add),
           (unsigned)A.peak, (unsigned)A.size);
}

/* The real v2 records of a console's queue, when a directory of them is
 * given (VJO_ANKI_REAL_QUEUE, private: never committed): each decodes and
 * becomes a Kiku note, without printing its text. */
static void test_real_queue(void)
{
    const char *dir = getenv("VJO_ANKI_REAL_QUEUE");
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (!dir) {
        printf("(VJO_ANKI_REAL_QUEUE not set: skipped)\n");
        return;
    }
    reset();
    TEST_ASSERT(vjo_queue_count() == 0); /* (makes the queue directory) */
    TEST_ASSERT((d = opendir(dir)) != NULL);
    while ((e = readdir(d))) {
        char path[512], id[VJO_QUEUE_ID_SIZE], *json;
        size_t len;
        FILE *f = NULL;
        VjoAnkiNote note;
        VjoAnkiMedia none = {0};
        VjoAnkiBody body;
        int picture, version;
        if (strlen(e->d_name) != 37 || strcmp(e->d_name + 32, ".json"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        vjo_arena_reset(&A);
        json = vjo_arena_alloc(&A, VJO_QUEUE_TEXT_MAX + 1);
        TEST_ASSERT(json && (f = fopen(path, "rb")) != NULL);
        len = fread(json, 1, VJO_QUEUE_TEXT_MAX, f);
        fclose(f);
        vjo_queue_id(json, len, id);
        TEST_CHECK(!strncmp(id, e->d_name, 32)); /* the name is its content's ID */
        version = vjo_queue_decode(&A, json, len, &note, &picture);
        TEST_CHECK(version >= 1 && version <= 3);
        note.queue_tag = "vita_queue_x";
        TEST_CHECK(vjo_anki_add_request(&A, &note, &none, &body) == 0);
        TEST_CHECK(strstr(body.prefix, "\"deckName\":\"Kiku\",\"modelName\":\"Kiku\",\"fields\":{\"Expression\":") &&
                   strstr(body.prefix, "\"MainDefinition\":\"<div style=\\\"text-align: left;\\\" "
                                       "class=\\\"yomitan-glossary\\\"><ol><li data-dictionary=\\\"") &&
                   strstr(body.prefix, "\"Sentence\":\"") && strstr(body.prefix, "<b>"));
        printf("real record %.8s…: version %d, %u B, addNote %u B, dictionary named: %s\n", e->d_name, version,
               (unsigned)len, (unsigned)body.prefix_len, strstr(body.prefix, "data-dictionary") ? "yes" : "no");
        /* into this test's own queue directory (a temporary one), for the send below */
        snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s", e->d_name);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL && fwrite(json, 1, len, f) == len);
        fclose(f);
        n++;
    }
    closedir(d);
    TEST_CHECK(n > 0);
    printf("real records: %d decoded and made into Kiku notes\n", n);
    {
        /* "Send queued cards now" with them, to the fake AnkiConnect */
        VjoAnkiSend s;
        char msg[240];
        vjo_arena_reset(&A);
        TEST_CHECK(vjo_anki_app_send(&A, &P, &ops, &cfg, &s, msg, sizeof(msg)) == VJO_OK && s.sent == n &&
                   anki.n_notes == n && vjo_queue_count() == 0);
        printf("real records sent: %d of %d, %d duplicates, %d kept; arena peak %u B\n", s.sent, s.queued,
               s.duplicates, s.unreadable + s.too_large, (unsigned)A.peak);
    }
}

TEST_LIST = {
    {"typed_address_is_normalized_and_checked", test_address},
    {"connection_test_reports_version_kiku_fields_and_deck", test_connection_test},
    {"auto_searches_the_lan_saves_the_address_and_searches_again_when_it_moved", test_auto_search},
    {"send_now_sends_every_record_version_as_kiku_once", test_send},
    {"send_now_sends_the_largest_card_the_overlay_keeps", test_send_the_largest_card},
    {"real_console_records_become_kiku_notes", test_real_queue},
    {NULL, NULL},
};

static char test_dir[128];
static pid_t test_dir_owner;
static void remove_test_dir(void)
{
    char cmd[200];
    if (getpid() != test_dir_owner) /* (acutest runs each test in a child) */
        return;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", test_dir);
}

int main(int argc, char **argv)
{
    /* One storage directory for every test (reset empties the queue). */
    snprintf(test_dir, sizeof(test_dir), "/tmp/vjo-anki-app-tests-XXXXXX");
    if (!mkdtemp(test_dir))
        return 2;
    test_dir_owner = getpid();
    atexit(remove_test_dir);
    if (chdir(test_dir) < 0 || mkdir("ux0:data", 0777) < 0 || mkdir("ux0:data/VitaJPOverlay", 0777) < 0)
        return 2;
    return acutest_main(argc, argv);
}
