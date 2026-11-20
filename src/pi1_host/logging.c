/*
 * logging.c - CSV-Logging von Pi 1 mit Ringpuffern und eigenem Thread
 * (Designdokument Abschnitt 2.10, Festlegung 11). Beschreibung siehe logging.h.
 *
 * Ausnahme zur Regel "ein schreibender Thread je Datensatz": Ein Ringpuffer
 * ist eine Warteschlange. Der erzeugende Thread haengt an, der Logging-Thread
 * entnimmt; beides nur unter dem Mutex dieses Puffers und nur als Kopie.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "logging.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "names.h"
#include "shared.h"
#include "time_utils.h"

#define LOG_LINE_LEN   256U
#define LOG_BATCH_LEN  32U          /* Eintraege je Puffer und Durchgang */
#define LOG_STDIO_BUF  (32U * 1024U) /* statischer Puffer fuer stdio */

static log_ring_t g_rings[LOG_SRC_COUNT];
static pthread_mutex_t g_ring_mutex[LOG_SRC_COUNT];
static bool g_rings_ready = false;

/* Nur vom Logging-Thread (bzw. vor/nach ihm von main) verwendet. */
static FILE *g_file = NULL;
static char_t g_path[PI1_LOG_PATH_LEN] = "";
static char g_stdio_buf[LOG_STDIO_BUF];

/* ===== Reine Hilfsfunktionen ================================================ */

void log_ring_reset(log_ring_t *ring)
{
    if (ring != NULL) {
        ring->head = 0U;
        ring->count = 0U;
        ring->dropped_meas = 0U;
        ring->dropped_events = 0U;
    }
}

bool log_ring_push(log_ring_t *ring, const log_record_t *rec)
{
    bool stored = false;

    if ((ring != NULL) && (rec != NULL)) {
        /* Messwerte nur, solange mehr als die Reserve frei ist; Ereignisse
         * duerfen auch die Reserve belegen (Festlegung 11: Vorrang). */
        uint32_t limit = (rec->type == LOG_TYPE_MEAS)
                             ? (PI1_LOG_RING_LEN - PI1_LOG_EVENT_RESERVE)
                             : PI1_LOG_RING_LEN;

        if (ring->count < limit) {
            uint32_t tail = (ring->head + ring->count) % PI1_LOG_RING_LEN;
            ring->items[tail] = *rec;
            ring->count++;
            stored = true;
        } else if (rec->type == LOG_TYPE_MEAS) {
            ring->dropped_meas++;
        } else {
            ring->dropped_events++;
        }
    }
    return stored;
}

bool log_ring_pop(log_ring_t *ring, log_record_t *rec)
{
    bool taken = false;

    if ((ring != NULL) && (rec != NULL) && (ring->count > 0U)) {
        *rec = ring->items[ring->head];
        ring->head = (ring->head + 1U) % PI1_LOG_RING_LEN;
        ring->count--;
        taken = true;
    }
    return taken;
}

int32_t log_format_header(char_t *buf, size_t cap)
{
    int32_t n = -1;

    if ((buf != NULL) && (cap > 0U)) {
        n = (int32_t)snprintf(buf, cap, "wall_time,mono_ms,type,event,state,error,"
                                        "level_requested,level_active,d1_mm,d2_mm\n");
        if ((n < 0) || ((size_t)n >= cap)) {
            n = -1;
        }
    }
    return n;
}

/* Abstand als Text; 0xFFFF (ungueltig) bleibt leer. */
static void format_distance(uint16_t d_mm, char_t *out, size_t cap)
{
    if (d_mm == (uint16_t)DIST_INVALID_MM) {
        out[0] = '\0';
    } else {
        (void)snprintf(out, cap, "%u", (unsigned)d_mm);
    }
}

/* Ersetzt Kommas und Zeilenumbrueche, damit der Text eine CSV-Zelle bleibt. */
static void sanitize(const char_t *in, char_t *out, size_t cap)
{
    size_t i = 0U;

    while ((i + 1U) < cap) {
        char_t c = in[i];
        if (c == '\0') {
            break;
        }
        out[i] = ((c == ',') || (c == '\n') || (c == '\r')) ? ';' : c;
        i++;
    }
    out[i] = '\0';
}

int32_t log_format_row(const log_record_t *rec, char_t *buf, size_t cap)
{
    int32_t n = -1;

    if ((rec != NULL) && (buf != NULL) && (cap > 0U)) {
        char_t wall[40];
        char_t event[PI1_LOG_EVENT_TEXT_LEN];
        char_t d1[8];
        char_t d2[8];
        struct tm tm_local;
        const char_t *state = "";
        const char_t *error = "";
        const char_t *level_act = "";
        const char_t *level_req = (rec->level_req == LOG_LEVEL_NONE) ? "-" : name_level(rec->level_req);

        /* ISO 8601 mit Millisekunden; ohne gueltige Uhrzeit bleibt das Feld leer. */
        wall[0] = '\0';
        if (localtime_r(&rec->wall.tv_sec, &tm_local) != NULL) {
            char_t date[24];
            if (strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm_local) > 0U) {
                (void)snprintf(wall, sizeof(wall), "%s.%03ld", date,
                               (long)(rec->wall.tv_nsec / 1000000L));
            }
        }
        d1[0] = '\0';
        d2[0] = '\0';
        if (rec->has_status) {
            state = name_state(rec->status.state);
            error = name_error(rec->status.error);
            level_act = name_level(rec->status.level);
            format_distance(rec->status.d1_mm, d1, sizeof(d1));
            format_distance(rec->status.d2_mm, d2, sizeof(d2));
        }
        sanitize((rec->type == LOG_TYPE_EVENT) ? rec->event : "", event, sizeof(event));

        n = (int32_t)snprintf(buf, cap, "%s,%llu,%s,%s,%s,%s,%s,%s,%s,%s\n",
                              wall, (unsigned long long)rec->mono_ms,
                              (rec->type == LOG_TYPE_EVENT) ? "EVENT" : "MEAS",
                              event, state, error, level_req, level_act, d1, d2);
        if ((n < 0) || ((size_t)n >= cap)) {
            n = -1;
        }
    }
    return n;
}

/* ===== Erzeugen von Eintraegen (beliebiger Thread, eigener Puffer) ========== */

static void fill_times(log_record_t *rec)
{
    if (clock_gettime(CLOCK_REALTIME, &rec->wall) != 0) {
        rec->wall.tv_sec = 0;
        rec->wall.tv_nsec = 0;
    }
    rec->mono_ms = time_now_us() / TIME_US_PER_MS;
}

static void push_record(uint8_t src, const log_record_t *rec)
{
    if (g_rings_ready && (src < LOG_SRC_COUNT) &&
        (pthread_mutex_lock(&g_ring_mutex[src]) == 0)) {
        (void)log_ring_push(&g_rings[src], rec); /* Verwerfen wird gezaehlt */
        (void)pthread_mutex_unlock(&g_ring_mutex[src]);
    }
}

void log_event(uint8_t src, const char_t *text, bool urgent,
               uint8_t level_req, const proto_status_t *status)
{
    log_record_t rec;

    (void)memset(&rec, 0, sizeof(rec));
    rec.type = LOG_TYPE_EVENT;
    rec.urgent = urgent;
    rec.level_req = level_req;
    fill_times(&rec);
    if (text != NULL) {
        (void)snprintf(rec.event, sizeof(rec.event), "%s", text);
    }
    if (status != NULL) {
        rec.has_status = true;
        rec.status = *status;
    }
    push_record(src, &rec);
}

void log_measurement(uint8_t src, uint8_t level_req, const proto_status_t *status)
{
    if (status != NULL) {
        log_record_t rec;

        (void)memset(&rec, 0, sizeof(rec));
        rec.type = LOG_TYPE_MEAS;
        rec.level_req = level_req;
        rec.has_status = true;
        rec.status = *status;
        fill_times(&rec);
        push_record(src, &rec);
    }
}

/* ===== Datei ===================================================================== */

int32_t log_init(const char_t *dir)
{
    int32_t result = 0;
    uint32_t i;
    uint32_t ready = 0U;

    for (i = 0U; i < LOG_SRC_COUNT; i++) {
        log_ring_reset(&g_rings[i]);
        if (pthread_mutex_init(&g_ring_mutex[i], NULL) == 0) {
            ready++;
        }
    }
    if (ready != LOG_SRC_COUNT) {
        for (i = 0U; i < ready; i++) {
            (void)pthread_mutex_destroy(&g_ring_mutex[i]);
        }
        result = -2;
    } else {
        g_rings_ready = true;
    }

    if ((result == 0) && (dir != NULL)) {
        struct timespec now;
        struct tm tm_local;
        char_t stamp[20] = "unbekannt";

        /* Verzeichnis anlegen; existiert es schon, ist das kein Fehler. */
        if ((mkdir(dir, (mode_t)0755) != 0) && (errno != EEXIST)) {
            result = -1;
        }
        if ((clock_gettime(CLOCK_REALTIME, &now) == 0) &&
            (localtime_r(&now.tv_sec, &tm_local) != NULL)) {
            (void)strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_local);
        }
        if (result == 0) {
            int32_t n = (int32_t)snprintf(g_path, sizeof(g_path), "%s/pi1_%s.csv", dir, stamp);
            if ((n < 0) || ((size_t)n >= sizeof(g_path))) {
                result = -1;
            }
        }
        if (result == 0) {
            /* fopen reserviert einmalig bei der Initialisierung (Festlegung 12). */
            g_file = fopen(g_path, "w");
            if (g_file == NULL) {
                result = -1;
            } else if (setvbuf(g_file, g_stdio_buf, _IOFBF, sizeof(g_stdio_buf)) != 0) {
                /* ohne eigenen Puffer funktioniert es trotzdem */
            } else {
                char_t header[LOG_LINE_LEN];
                if ((log_format_header(header, sizeof(header)) < 0) ||
                    (fputs(header, g_file) < 0)) {
                    result = -1;
                }
            }
        }
        if ((result != 0) && (g_file != NULL)) {
            (void)fclose(g_file);
            g_file = NULL;
        }
        if (g_file == NULL) {
            g_path[0] = '\0';
        }
    }
    return result;
}

const char_t *log_file_path(void)
{
    return g_path;
}

/* Bei einem Schreibfehler wird das Logging abgeschaltet; das Programm laeuft weiter. */
static void file_failed(const char_t *what)
{
    (void)fprintf(stderr, "\r\nLOG: %s fehlgeschlagen (%s); Logging in Datei abgeschaltet\n",
                  what, strerror(errno));
    if (g_file != NULL) {
        (void)fclose(g_file);
        g_file = NULL;
    }
}

static void write_record(const log_record_t *rec)
{
    char_t line[LOG_LINE_LEN];

    /* Ereignisse zusaetzlich auf der Konsole, damit der Bediener sie sieht. */
    if (rec->type == LOG_TYPE_EVENT) {
        (void)printf("\r\033[K[%llu ms] %s\n", (unsigned long long)rec->mono_ms, rec->event);
    }
    if ((g_file != NULL) && (log_format_row(rec, line, sizeof(line)) > 0)) {
        if (fputs(line, g_file) < 0) {
            file_failed("Schreiben");
        }
    }
}

/* Schreibt den stdio-Puffer auf die Karte (fflush + fsync). */
static void flush_to_card(void)
{
    if (g_file != NULL) {
        if (fflush(g_file) != 0) {
            file_failed("fflush");
        } else if (fsync(fileno(g_file)) != 0) {
            file_failed("fsync");
        } else {
            /* geschrieben */
        }
    }
}

/*
 * Leert alle Puffer einmal (je Puffer hoechstens LOG_BATCH_LEN Eintraege).
 * Liefert true, wenn ein Fehlerereignis dabei war oder noch Eintraege
 * uebrig sind (urgent bzw. more).
 */
static void drain_once(bool *urgent, bool *more)
{
    static uint32_t reported_meas[LOG_SRC_COUNT];
    static uint32_t reported_events[LOG_SRC_COUNT];
    uint32_t src;

    for (src = 0U; src < LOG_SRC_COUNT; src++) {
        log_record_t batch[LOG_BATCH_LEN];
        uint32_t n = 0U;
        uint32_t dropped_meas = 0U;
        uint32_t dropped_events = 0U;
        uint32_t left = 0U;
        uint32_t i;

        /* Unter dem Mutex nur kopieren. */
        if (pthread_mutex_lock(&g_ring_mutex[src]) == 0) {
            while ((n < LOG_BATCH_LEN) && log_ring_pop(&g_rings[src], &batch[n])) {
                n++;
            }
            dropped_meas = g_rings[src].dropped_meas;
            dropped_events = g_rings[src].dropped_events;
            left = g_rings[src].count;
            (void)pthread_mutex_unlock(&g_ring_mutex[src]);
        }

        for (i = 0U; i < n; i++) {
            write_record(&batch[i]);
            if (batch[i].urgent) {
                *urgent = true;
            }
        }
        if (left > 0U) {
            *more = true;
        }
        /* Festlegung 11: Verworfene Eintraege als Zaehler protokollieren. */
        if ((dropped_meas != reported_meas[src]) || (dropped_events != reported_events[src])) {
            log_record_t rec;

            (void)memset(&rec, 0, sizeof(rec));
            rec.type = LOG_TYPE_EVENT;
            rec.level_req = LOG_LEVEL_NONE;
            fill_times(&rec);
            (void)snprintf(rec.event, sizeof(rec.event), "LOG_VERWORFEN src=%u meas=%u events=%u",
                           (unsigned)src, (unsigned)dropped_meas, (unsigned)dropped_events);
            write_record(&rec);
            reported_meas[src] = dropped_meas;
            reported_events[src] = dropped_events;
        }
    }
}

void *log_thread(void *arg)
{
    const uint64_t period_us = (uint64_t)PI1_LOG_PERIOD_MS * TIME_US_PER_MS;
    const uint64_t flush_us = (uint64_t)PI1_LOG_FLUSH_PERIOD_MS * TIME_US_PER_MS;
    uint64_t next_us = time_now_us();
    uint64_t last_flush_us = next_us;
    bool running = true;

    (void)arg;
    while (running) {
        app_ctrl_t ctrl;
        bool urgent = false;
        bool more = true;

        (void)memset(&ctrl, 0, sizeof(ctrl));
        shared_ctrl_read(&ctrl);
        /* Beim Beenden: alles leeren, was noch in den Puffern steht. */
        running = !ctrl.stop_threads;

        while (more) {
            more = false;
            drain_once(&urgent, &more);
            if (running) {
                more = false; /* im Betrieb hoechstens ein Durchgang pro Takt */
            }
        }
        if (urgent || !running || (time_age_us(time_now_us(), last_flush_us) >= flush_us)) {
            flush_to_card();
            last_flush_us = time_now_us();
        }
        (void)fflush(stdout);

        if (running) {
            next_us += period_us;
            if (next_us <= time_now_us()) {
                next_us = time_now_us() + period_us; /* nicht aufholen */
            }
            time_sleep_until_us(next_us);
        }
    }
    return NULL;
}

void log_close(void)
{
    uint32_t i;

    flush_to_card();
    if (g_file != NULL) {
        if (fclose(g_file) != 0) {
            (void)fprintf(stderr, "LOG: Schliessen der Datei fehlgeschlagen\n");
        }
        g_file = NULL;
    }
    if (g_rings_ready) {
        g_rings_ready = false;
        for (i = 0U; i < LOG_SRC_COUNT; i++) {
            (void)pthread_mutex_destroy(&g_ring_mutex[i]);
        }
    }
}
