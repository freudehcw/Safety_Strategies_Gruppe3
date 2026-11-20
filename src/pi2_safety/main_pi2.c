/*
 * main_pi2.c - Raspberry Pi 2: Sensorerfassung, Safety-Logik, Motorsteuerung.
 *
 * Aufruf:
 *   pi2_app [--tcp PORT] [--key PFAD] [--no-watchdog]
 *     --tcp PORT     TCP auf 127.0.0.1 statt Bluetooth-RFCOMM (Entwicklung)
 *     --key PFAD     Schluesseldatei (Standard /etc/ssm/psk.bin)
 *     --no-watchdog  Hardware-Watchdog nicht verwenden (nur Entwicklung)
 *
 * Testmodus: Wird mit SSM_TESTMODE gebaut (Ziel pi2_app_test), nimmt das
 * Programm Befehle zur Fehlereinspeisung auf der Standardeingabe an und
 * schreibt ein Test-Log. Im normalen Build ist dieser Code nicht enthalten.
 *
 * Ablauf (SAF-02, SAF-09): Motor AUS -> Initialisierung -> Threads starten ->
 * Zustandswechsel protokollieren -> bei Stop/Shutdown zuerst Motor AUS, dann
 * Threads und Programm beenden.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2 und Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bt_server.h"
#include "config.h"
#include "crypto_utils.h"
#include "hal.h"
#include "motor.h"
#include "names.h"
#include "pi2_config.h"
#include "protocol.h"
#include "safety_logic.h"
#include "sensors.h"
#include "shared.h"
#include "time_utils.h"
#include "types.h"
#include "watchdog.h"

#define MAIN_POLL_MS         10
/* laenger als SHUTDOWN_MOTOR_WAIT_MS, damit die Kommunikation fertig wird */
#define SHUTDOWN_ACK_WAIT_MS 700U
#define SIGNAL_GRACE_MS      300U
#define THREAD_STACK_BYTES   (256U * 1024U)
#define CONSOLE_VALUE_MAX    100000
#define TCP_PORT_MAX         65535

typedef struct {
    pthread_t handle;
    void *(*entry)(void *);
    void *arg;
    int32_t priority;
    const char_t *name;
    bool started;
    bool realtime; /* SCHED_FIFO gesetzt */
} thread_desc_t;

typedef struct {
    bt_server_cfg_t comm;
    const char_t *key_path;
    bool use_watchdog;
} options_t;

static volatile sig_atomic_t g_signal_received = 0;
static const uint32_t g_sensor_index[SENSOR_COUNT] = { 0U, 1U };

#ifdef SSM_TESTMODE
static FILE *g_testlog = NULL; /* nur vom Hauptprogramm verwendet */
#endif

/* Signatur durch <signal.h> vorgegeben (Dir 4.6: Bibliotheksschnittstelle). */
static void on_terminate(int signum)
{
    (void)signum;
    g_signal_received = 1;
}

/* Programmfehler (z. B. SIGSEGV): Enable sofort wegnehmen, dann abbrechen. */
static void on_fatal(int signum)
{
    hal_motor_emergency_off();
    /* Dir 4.7: bewusst ungeprueft - im Fehlerpfad eines Signal-Handlers ist
     * keine weitere Reaktion moeglich; der Motor ist bereits abgeschaltet. */
    (void)signal(signum, SIG_DFL);
    (void)raise(signum);
}

static int32_t install_signal_handlers(void)
{
    static const int32_t fatal_signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    struct sigaction sa;
    int32_t result = 0;
    size_t i;

    (void)memset(&sa, 0, sizeof(sa));
    if (sigemptyset(&sa.sa_mask) != 0) {
        result = -1;
    }
    sa.sa_handler = on_terminate;
    if ((sigaction(SIGINT, &sa, NULL) != 0) || (sigaction(SIGTERM, &sa, NULL) != 0)) {
        result = -1;
    }

    sa.sa_handler = on_fatal;
    for (i = 0U; i < (sizeof(fatal_signals) / sizeof(fatal_signals[0])); i++) {
        if (sigaction(fatal_signals[i], &sa, NULL) != 0) {
            result = -1;
        }
    }
    return result;
}

/* Protokolliert Zustandswechsel mit der monotonen Uhr (ausserhalb des
 * Safety-Logik-Threads), z. B. zur Messung von Reaktionszeiten. */
static void log_state(const safety_state_t *state)
{
    sensor_data_t s0;
    sensor_data_t s1;
    comm_data_t comm;

    shared_sensor_read(0U, &s0);
    shared_sensor_read(1U, &s1);
    shared_comm_read(&comm);

    (void)printf("[%7" PRIu64 ".%03" PRIu64 " ms] %-12s Fehler=%-13s Motor=%-6s "
                 "d1=%5" PRIu16 " mm d2=%5" PRIu16 " mm "
                 "letzte gueltige Nachricht=%" PRIu64 ".%03" PRIu64 " ms\n",
                 state->state_since_us / TIME_US_PER_MS, state->state_since_us % TIME_US_PER_MS,
                 name_state(state->state), name_error(state->error),
                 name_level(state->motor_level),
                 s0.valid ? s0.distance_mm : (uint16_t)DIST_INVALID_MM,
                 s1.valid ? s1.distance_mm : (uint16_t)DIST_INVALID_MM,
                 comm.last_valid_us / TIME_US_PER_MS, comm.last_valid_us % TIME_US_PER_MS);
    (void)fflush(stdout);

#ifdef SSM_TESTMODE
    /* Abschnitt 2.10: Test-Log mit der monotonen Uhr von Pi 2 (Zeiten in us). */
    if (g_testlog != NULL) {
        (void)fprintf(g_testlog,
                      "%" PRIu64 ",%s,%s,%s,%" PRIu16 ",%" PRIu16 ",%" PRIu64 ",%" PRIu32 ",%" PRIu64 "\n",
                      state->state_since_us, name_state(state->state), name_error(state->error),
                      name_level(state->motor_level),
                      s0.valid ? s0.distance_mm : (uint16_t)DIST_INVALID_MM,
                      s1.valid ? s1.distance_mm : (uint16_t)DIST_INVALID_MM,
                      comm.last_valid_us, state->overruns, state->max_late_us);
        (void)fflush(g_testlog);
    }
#endif
}

#ifdef SSM_TESTMODE
static void print_console_help(void)
{
    (void)printf("Testkonsole:\n"
                 "  d <1|2> <mm>      simulierten Abstand setzen (nur Simulation, 0 = kein Echo)\n"
                 "  force <1|2> <mm>  Rohmesswert des Sensors ersetzen (TC-01a/b, TC-02)\n"
                 "  unforce <1|2>     Vorgabe aufheben\n"
                 "  block <1|2> <ms>  Sensor-Thread einmalig blockieren (TC-04)\n"
                 "  run <1|2|3> <s>   Stufe LOW/MEDIUM/HIGH fuer s Sekunden ohne Pi 1 freigeben,\n"
                 "                    danach Motor AUS (Nachlaufmessung, SAF-10)\n"
                 "  stop              lokaler Stop/Shutdown\n");
    (void)fflush(stdout);
}

static void handle_console_line(const char_t *line)
{
    char_t cmd[16];
    uint32_t sensor = 0U;
    int32_t value = 0;
    int32_t fields = sscanf(line, "%15s %" SCNu32 " %" SCNd32, cmd, &sensor, &value);

    if (fields >= 1) {
        bool sensor_ok = (fields >= 2) && (sensor >= 1U) && (sensor <= SENSOR_COUNT);
        bool value_ok = (fields >= 3) && (value >= 0) && (value <= CONSOLE_VALUE_MAX);

        if (strcmp(cmd, "stop") == 0) {
            shared_request_stop();
        } else if ((strcmp(cmd, "d") == 0) && sensor_ok && value_ok) {
            if (hal_test_set_distance_mm(sensor - 1U, (uint32_t)value) != 0) {
                (void)printf("'d' wird nur von der Simulation unterstuetzt\n");
            }
        } else if ((strcmp(cmd, "force") == 0) && sensor_ok && value_ok) {
            sensors_test_force(sensor - 1U, value);
        } else if ((strcmp(cmd, "unforce") == 0) && sensor_ok) {
            sensors_test_force(sensor - 1U, -1);
        } else if ((strcmp(cmd, "block") == 0) && sensor_ok && value_ok) {
            sensors_test_block(sensor - 1U, (uint32_t)value);
        } else if ((strcmp(cmd, "run") == 0) && (fields >= 3) && (sensor >= LEVEL_LOW) &&
                   (sensor <= LEVEL_HIGH) && (value > 0) && (value <= TEST_RUN_MAX_SECONDS)) {
            /* zweites Feld ist hier die Stufe, drittes die Dauer in Sekunden */
            safety_test_run((uint8_t)sensor, (uint32_t)value * 1000U);
        } else {
            print_console_help();
        }
    }
}

/* Oeffnet das Test-Log; ohne Datei laeuft der Test mit Konsolenausgabe weiter. */
static void open_testlog(void)
{
    g_testlog = fopen(TESTLOG_FILE, "w");
    if (g_testlog == NULL) {
        (void)fprintf(stderr, "Warnung: Test-Log %s konnte nicht angelegt werden\n", TESTLOG_FILE);
    } else {
        (void)fprintf(g_testlog, "state_since_us,state,error,motor,d1_mm,d2_mm,"
                                 "last_valid_msg_us,overruns,max_late_us\n");
    }
}

static void close_testlog(void)
{
    if ((g_testlog != NULL) && (fclose(g_testlog) != 0)) {
        (void)fprintf(stderr, "Warnung: Test-Log nicht vollstaendig geschrieben\n");
    }
    g_testlog = NULL;
}
#endif /* SSM_TESTMODE */

/* Wandelt eine Portnummer; false bei ungueltiger Eingabe. */
static bool parse_port(const char_t *text, uint16_t *port)
{
    bool ok = false;

    if ((text != NULL) && (port != NULL) && (text[0] != '\0')) {
        char_t *end = NULL;
        int64_t value;

        errno = 0;
        value = strtol(text, &end, 10);
        if ((errno == 0) && (end != NULL) && (*end == '\0') &&
            (value > 0) && (value <= TCP_PORT_MAX)) {
            *port = (uint16_t)value;
            ok = true;
        }
    }
    return ok;
}

/* Wertet die Kommandozeile aus; false bei unbekanntem oder ungueltigem Argument. */
static bool parse_options(int32_t argc, char_t *const argv[], options_t *options)
{
    bool ok = true;
    int32_t a = 1;

    options->comm.use_tcp = false;
    options->comm.tcp_port = 0U;
    options->key_path = CFG_PSK_DEFAULT_PATH;
    options->use_watchdog = true;

    while (ok && (a < argc)) {
        if ((strcmp(argv[a], "--tcp") == 0) && ((a + 1) < argc)) {
            ok = parse_port(argv[a + 1], &options->comm.tcp_port);
            options->comm.use_tcp = ok;
            a += 2;
        } else if ((strcmp(argv[a], "--key") == 0) && ((a + 1) < argc) &&
                   (argv[a + 1][0] != '\0')) {
            options->key_path = argv[a + 1];
            a += 2;
        } else if (strcmp(argv[a], "--no-watchdog") == 0) {
            options->use_watchdog = false;
            a += 1;
        } else {
            ok = false;
        }
    }
    return ok;
}

static void start_thread(thread_desc_t *desc)
{
    pthread_attr_t attr;
    bool have_attr = (pthread_attr_init(&attr) == 0);
    int32_t rc;

    /* Feste Stackgroesse: begrenzt den mit mlockall gesperrten Speicher. */
    if (have_attr && (pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES) != 0)) {
        (void)fprintf(stderr, "Warnung: Stackgroesse fuer Thread '%s' nicht gesetzt\n", desc->name);
    }
    rc = pthread_create(&desc->handle, have_attr ? &attr : NULL, desc->entry, desc->arg);
    if (have_attr && (pthread_attr_destroy(&attr) != 0)) {
        (void)fprintf(stderr, "Warnung: Thread-Attribute nicht freigegeben\n");
    }

    if (rc == 0) {
        struct sched_param param;

        desc->started = true;
        (void)memset(&param, 0, sizeof(param));
        param.sched_priority = desc->priority;
        desc->realtime = (pthread_setschedparam(desc->handle, SCHED_FIFO, &param) == 0);
    } else {
        (void)fprintf(stderr, "Fehler: Thread '%s' konnte nicht gestartet werden\n", desc->name);
    }
}

/* --- Start ---------------------------------------------------------------- */

/* Prueft alles, was vor dem ersten Hardwarezugriff feststehen muss. Bei einem
 * Fehler wird nicht gestartet; die Ausgaenge bleiben unberuehrt. */
static bool check_preconditions(const options_t *options)
{
    bool ok = false;

    if (!safety_config_valid()) {
        /* SAF-07: Die Sicherheitsabstaende muessen mit der Stufe wachsen. */
        (void)fprintf(stderr, "Fehler: Konfiguration ungueltig, verlangt ist "
                              "0 < S_LOW < S_MEDIUM < S_HIGH (pi2_config.h)\n");
    } else if (!hal_is_simulation() && (geteuid() != 0)) {
        /* Reale Hardware braucht Root-Rechte (GPIO, SCHED_FIFO, mlockall, Watchdog). */
        (void)fprintf(stderr, "Fehler: Root-Rechte noetig (Start mit sudo). "
                              "Das Programm wurde nicht gestartet, der Motor bleibt AUS.\n");
    } else {
        /* SEC-01: Pre-Shared-Key laden und AES-256-GCM initialisieren (OpenSSL EVP,
         * https://www.openssl.org, siehe src/common/crypto_utils.c). */
        uint8_t key[CRYPTO_KEY_LEN];
        int32_t key_rc = crypto_load_key_file(options->key_path, key);

        if ((key_rc != CRYPTO_KEY_OK) || (crypto_init(key) != 0)) {
            (void)fprintf(stderr, "Fehler: %s (%s)\n", options->key_path,
                          (key_rc != CRYPTO_KEY_OK) ? crypto_key_error_text(key_rc)
                                                    : "OpenSSL-Initialisierung fehlgeschlagen");
        } else {
            ok = true;
        }
        crypto_wipe(key, sizeof(key)); /* Schluessel liegt danach nur noch in OpenSSL */
    }
    return ok;
}

/* Gibt bei einem Startfehler alles frei, was bis dahin angelegt wurde. */
static void release_after_init_failure(bool comm_open)
{
    if (comm_open) {
        bt_server_close();
    }
    if (hal_shutdown() != 0) {
        (void)fprintf(stderr, "Fehler: Hardware konnte nicht freigegeben werden\n");
    }
    crypto_cleanup();
}

/* Sperrt den Speicher (keine Verzoegerung durch Auslagern). Nur mit root:
 * ohne root wuerde das Limit RLIMIT_MEMLOCK spaetere Allokationen (z. B.
 * Thread-Stacks) fehlschlagen lassen. Auf realer Hardware ist ein
 * Fehlschlag ein Startfehler, in der Simulation nur eine Warnung. */
static bool lock_memory(void)
{
    bool ok = true;

    if ((geteuid() != 0) || (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)) {
        if (hal_is_simulation()) {
            (void)fprintf(stderr, "Warnung: Speicher nicht gesperrt (mlockall, root noetig)\n");
        } else {
            (void)fprintf(stderr, "Fehler: Speicher konnte nicht gesperrt werden (mlockall)\n");
            ok = false;
        }
    }
    return ok;
}

/* Oeffnet den Hardware-Watchdog direkt vor dem Start der Threads, damit die
 * Safety-Logik ihn sofort bedient. In der Simulation gibt es keinen. */
static bool open_watchdog(const options_t *options)
{
    bool ok = true;

    if (hal_is_simulation()) {
        (void)printf("Hardware-Watchdog: nicht aktiv (Simulation)\n");
    } else if (!options->use_watchdog) {
        (void)fprintf(stderr, "WARNUNG: Hardware-Watchdog abgeschaltet (--no-watchdog); "
                              "bei einem Absturz kann der Motor weiterlaufen\n");
    } else if (watchdog_open() != 0) {
        (void)fprintf(stderr, "Fehler: Hardware-Watchdog (%s) nicht verfuegbar. Moegliche Ursachen: "
                              "keine Root-Rechte, dtparam=watchdog=on fehlt oder systemd "
                              "verwendet ihn bereits (RuntimeWatchdogSec)\n", WATCHDOG_DEVICE);
        ok = false;
    } else {
        (void)printf("Hardware-Watchdog: aktiv, Timeout %" PRId32 " s\n", (int32_t)WATCHDOG_TIMEOUT_S);
    }
    return ok;
}

/* Initialisierung in der Reihenfolge von SAF-02: zuerst Hardware mit Motor
 * AUS, dann Signale, gemeinsame Daten, Kommunikation, Speicher, Watchdog.
 * Bei einem Fehler ist danach alles wieder freigegeben. */
static bool init_system(const options_t *options)
{
    bool ok = false;

    if (hal_init() != 0) {
        (void)fprintf(stderr, "Fehler: Hardware-Initialisierung (%s) fehlgeschlagen\n", hal_name());
        crypto_cleanup();
    } else if ((hal_motor_off() != 0) || (install_signal_handlers() != 0) || (shared_init() != 0)) {
        (void)fprintf(stderr, "Fehler: Initialisierung fehlgeschlagen (Motor AUS, "
                              "Signal-Handler oder gemeinsame Daten)\n");
        release_after_init_failure(false);
    } else if (bt_server_init(&options->comm) != 0) {
        (void)fprintf(stderr, "Fehler: Kommunikation (%s) konnte nicht gestartet werden\n",
                      options->comm.use_tcp ? "TCP" : "RFCOMM");
        release_after_init_failure(false);
    } else if (!lock_memory() || !open_watchdog(options)) {
        release_after_init_failure(true);
    } else {
        ok = true;
    }
    return ok;
}

/* Startet alle Threads. false, wenn einer nicht startet oder auf realer
 * Hardware SCHED_FIFO fehlt (dann ist der 50-ms-Zyklus nicht gesichert). */
static bool start_all_threads(thread_desc_t threads[], size_t count)
{
    bool all_started = true;
    bool all_realtime = true;
    bool ok = true;
    size_t i;

    /* Beide Sensor-Threads erhalten denselben Startzeitpunkt. */
    sensors_set_start_time(time_now_us() + ((uint64_t)SENSOR_START_DELAY_MS * TIME_US_PER_MS));
    for (i = 0U; i < count; i++) {
        start_thread(&threads[i]);
        all_started = all_started && threads[i].started;
        all_realtime = all_realtime && threads[i].realtime;
    }
    if (!all_started) {
        ok = false;
    } else if (!all_realtime) {
        if (hal_is_simulation()) {
            (void)fprintf(stderr, "Warnung: SCHED_FIFO nicht moeglich (root noetig); "
                                  "Zeitverhalten nicht garantiert\n");
        } else {
            (void)fprintf(stderr, "Fehler: SCHED_FIFO konnte nicht gesetzt werden; "
                                  "das Programm wird mit Motor AUS beendet\n");
            ok = false;
        }
    } else {
        /* alle Threads laufen mit SCHED_FIFO */
    }
    if (!ok) {
        shared_request_stop();
    }
    return ok;
}

/* --- Betrieb --------------------------------------------------------------- */

/* Zustand der Hauptschleife zwischen zwei Durchlaeufen. */
typedef struct {
    uint32_t logged_transitions;
    uint32_t logged_off_failures;
    uint64_t signal_since_us;
#ifdef SSM_TESTMODE
    bool stdin_open;
    uint32_t logged_overruns;
#endif
} main_loop_t;

/* Wartet einen Durchlauf (10 ms); im Testmodus werden dabei Konsolenbefehle gelesen. */
static void wait_one_round(main_loop_t *loop)
{
#ifdef SSM_TESTMODE
    if (loop->stdin_open) {
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };

        if (poll(&pfd, 1U, MAIN_POLL_MS) > 0) {
            char_t line[64];
            if (fgets(line, (int32_t)sizeof(line), stdin) != NULL) {
                handle_console_line(line);
            } else {
                loop->stdin_open = false;
            }
        }
    } else {
        time_sleep_until_us(time_now_us() + ((uint64_t)MAIN_POLL_MS * TIME_US_PER_MS));
    }
#else
    (void)loop;
    time_sleep_until_us(time_now_us() + ((uint64_t)MAIN_POLL_MS * TIME_US_PER_MS));
#endif
}

/* Ein Durchlauf der Hauptschleife: Signale, Fehler der Motorsteuerung und
 * Zustandswechsel auswerten. Liefert false, sobald das Programm enden soll;
 * *failed wird bei einem Fehler gesetzt. */
static bool main_loop_round(main_loop_t *loop, bool *failed)
{
    safety_state_t state;
    uint32_t off_failures;
    bool keep_running = true;

    wait_one_round(loop);

    if ((g_signal_received != 0) && (loop->signal_since_us == 0U)) {
        shared_request_stop();
        loop->signal_since_us = time_now_us();
    }

    off_failures = motor_off_failures();
    if (off_failures != loop->logged_off_failures) {
        loop->logged_off_failures = off_failures;
        *failed = true;
        (void)fprintf(stderr, "FEHLER: Motor AUS konnte nicht gesetzt werden (%" PRIu32 "x)\n",
                      off_failures);
    }

    shared_state_read(&state);
    if (state.transitions != loop->logged_transitions) {
        loop->logged_transitions = state.transitions;
        log_state(&state);
    }
#ifdef SSM_TESTMODE
    /* Zyklusueberschreitungen der Safety-Logik melden (SAF-06) */
    if (state.overruns != loop->logged_overruns) {
        loop->logged_overruns = state.overruns;
        (void)printf("Safety-Logik: %" PRIu32 " Zyklusueberschreitung(en), groesste Verspaetung "
                     "%" PRIu64 " us\n", state.overruns, state.max_late_us);
        log_state(&state);
    }
#endif
    if (state.state == STATE_SHUTDOWN) {
        keep_running = false;
    } else if ((loop->signal_since_us != 0U) &&
               (time_age_us(time_now_us(), loop->signal_since_us) >
                ((uint64_t)SIGNAL_GRACE_MS * TIME_US_PER_MS))) {
        /* Reagiert die Safety-Logik nicht auf die Stop-Anforderung, trotzdem beenden. */
        *failed = true;
        keep_running = false;
    } else {
        /* weiterlaufen */
    }
    return keep_running;
}

#ifdef SSM_TESTMODE
static void testmode_begin(main_loop_t *loop)
{
    (void)printf("TESTMODUS: Fehlereinspeisung aktiv, nicht fuer den normalen Betrieb\n");
    print_console_help();
    open_testlog();
    /* Ungepuffert, damit poll() auf stdin keine bereits gelesenen Zeilen uebersieht. */
    if (setvbuf(stdin, NULL, _IONBF, 0) == 0) {
        loop->stdin_open = true;
    } else {
        (void)fprintf(stderr, "Warnung: Testkonsole nicht verfuegbar\n");
    }
}

static void testmode_end(void)
{
    safety_state_t final_state;

    shared_state_read(&final_state);
    (void)printf("Safety-Logik: %" PRIu32 " Zyklen, %" PRIu32 " Ueberschreitung(en), "
                 "groesste Verspaetung %" PRIu64 " us\n",
                 final_state.generation, final_state.overruns, final_state.max_late_us);
    close_testlog();
}
#endif

/* --- Beenden --------------------------------------------------------------- */

/* SAF-09: zuerst Motor AUS, dann auf das ACK der Kommunikation warten, erst
 * danach Threads beenden und Ressourcen freigeben. Liefert false bei Fehler. */
static bool shutdown_system(thread_desc_t threads[], size_t count)
{
    bool ok = true;
    uint64_t deadline_us;
    size_t i;

    if (hal_motor_off() != 0) {
        (void)fprintf(stderr, "FEHLER: Motor AUS konnte nicht gesetzt werden\n");
        ok = false;
    }
    deadline_us = time_now_us() + ((uint64_t)SHUTDOWN_ACK_WAIT_MS * TIME_US_PER_MS);
    while (!bt_server_shutdown_acked() && (time_now_us() < deadline_us)) {
        time_sleep_until_us(time_now_us() + ((uint64_t)MAIN_POLL_MS * TIME_US_PER_MS));
    }
    shared_set_running(false);
    for (i = 0U; i < count; i++) {
        if (threads[i].started && (pthread_join(threads[i].handle, NULL) != 0)) {
            (void)fprintf(stderr, "Fehler: Thread '%s' nicht sauber beendet\n", threads[i].name);
            ok = false;
        }
    }
    bt_server_close();
    crypto_cleanup();
#ifdef SSM_TESTMODE
    testmode_end();
#endif
    if ((hal_motor_off() != 0) || (hal_shutdown() != 0) || (motor_off_failures() != 0U)) {
        /* Der Watchdog bleibt absichtlich offen: Ohne "Magic Close" setzt er
         * den Pi nach dem Programmende zurueck und der Motor wird stromlos. */
        (void)fprintf(stderr, "FEHLER: Motor AUS bzw. Hardware-Freigabe fehlgeschlagen\n");
        ok = false;
    } else if (watchdog_close() != 0) {
        (void)fprintf(stderr, "FEHLER: Watchdog nicht ordnungsgemaess geschlossen; "
                              "der Pi wird zurueckgesetzt\n");
        ok = false;
    } else {
        (void)printf("Pi 2 beendet, Motor AUS\n");
    }
    return ok;
}

/* Dir 4.6 Ausnahmen 2-4: Signatur von main. */
int main(int argc, char **argv)
{
    thread_desc_t threads[] = {
        { 0, safety_thread, NULL, PRIO_SAFETY, "safety", false, false },
        { 0, motor_thread, NULL, PRIO_MOTOR, "motor", false, false },
        { 0, sensor_thread, (void *)&g_sensor_index[0], PRIO_SENSOR, "sensor1", false, false },
        { 0, sensor_thread, (void *)&g_sensor_index[1], PRIO_SENSOR, "sensor2", false, false },
        { 0, bt_server_thread, NULL, PRIO_COMM, "comm", false, false },
    };
    const size_t thread_count = sizeof(threads) / sizeof(threads[0]);
    options_t options;
    main_loop_t loop;
    bool failed = false;
    bool running;

    if (!parse_options(argc, argv, &options)) {
        (void)fprintf(stderr, "Aufruf: %s [--tcp PORT] [--key PFAD] [--no-watchdog]\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* 1. Voraussetzungen und Initialisierung; der Motor bleibt AUS (SAF-02). */
    if (!check_preconditions(&options) || !init_system(&options)) {
        return EXIT_FAILURE;
    }
    (void)printf("Pi 2 gestartet: HAL=%s, Transport=%s\n", hal_name(),
                 options.comm.use_tcp ? "TCP" : "RFCOMM");

    (void)memset(&loop, 0, sizeof(loop));
#ifdef SSM_TESTMODE
    testmode_begin(&loop);
#endif

    /* 2. Threads starten. */
    running = start_all_threads(threads, thread_count);
    failed = !running;

    /* 3. Hauptschleife: Zustandswechsel protokollieren, Testkonsole bedienen. */
    while (running) {
        running = main_loop_round(&loop, &failed);
    }

    /* 4. Kontrolliert beenden (SAF-09). */
    if (!shutdown_system(threads, thread_count)) {
        failed = true;
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
