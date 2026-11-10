/*
 * main_pi2.c - Raspberry Pi 2: Sensorerfassung, Safety-Logik, Motorsteuerung.
 *
 * Aufruf:
 *   pi2_app [--tcp PORT] [--no-watchdog]
 *     --tcp PORT     TCP auf 127.0.0.1 statt Bluetooth-RFCOMM (Entwicklung)
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
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
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
#include "hal.h"
#include "motor.h"
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

static const char_t *state_name(uint8_t state)
{
    static const char_t *const names[] = { "INIT", "BETRIEB", "OBJEKT_STOPP", "FEHLER", "SHUTDOWN" };
    return (state <= STATE_SHUTDOWN) ? names[state] : "?";
}

static const char_t *error_name(uint8_t error)
{
    static const char_t *const names[] = { "-", "SENSOR", "KOMMUNIKATION", "DATENALTER" };
    return (error <= ERR_DATENALTER) ? names[error] : "?";
}

static const char_t *level_name(uint8_t level)
{
    static const char_t *const names[] = { "AUS", "LOW", "MEDIUM", "HIGH" };
    return (level <= LEVEL_HIGH) ? names[level] : "?";
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
                 state_name(state->state), error_name(state->error),
                 level_name(state->motor_level),
                 s0.valid ? s0.distance_mm : (uint16_t)DIST_INVALID_MM,
                 s1.valid ? s1.distance_mm : (uint16_t)DIST_INVALID_MM,
                 comm.last_valid_us / TIME_US_PER_MS, comm.last_valid_us % TIME_US_PER_MS);
    (void)fflush(stdout);

#ifdef SSM_TESTMODE
    /* Abschnitt 2.10: Test-Log mit der monotonen Uhr von Pi 2 (Zeiten in us). */
    if (g_testlog != NULL) {
        (void)fprintf(g_testlog,
                      "%" PRIu64 ",%s,%s,%s,%" PRIu16 ",%" PRIu16 ",%" PRIu64 ",%" PRIu32 ",%" PRIu64 "\n",
                      state->state_since_us, state_name(state->state), error_name(state->error),
                      level_name(state->motor_level),
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
    options->use_watchdog = true;

    while (ok && (a < argc)) {
        if ((strcmp(argv[a], "--tcp") == 0) && ((a + 1) < argc)) {
            ok = parse_port(argv[a + 1], &options->comm.tcp_port);
            options->comm.use_tcp = ok;
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
#ifdef SSM_TESTMODE
    bool stdin_open = false;
    uint32_t logged_overruns = 0U;
#endif
    bool all_started = true;
    bool all_realtime = true;
    bool running = true;
    uint32_t logged_transitions = 0U;
    uint32_t logged_off_failures = 0U;
    uint64_t signal_since_us = 0U;
    int32_t exit_code = EXIT_SUCCESS;
    size_t i;

    if (!parse_options(argc, argv, &options)) {
        (void)fprintf(stderr, "Aufruf: %s [--tcp PORT] [--no-watchdog]\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* SAF-07: Die Sicherheitsabstaende muessen mit der Stufe wachsen. */
    if (!safety_config_valid()) {
        (void)fprintf(stderr, "Fehler: Konfiguration ungueltig, verlangt ist "
                              "0 < S_LOW < S_MEDIUM < S_HIGH (pi2_config.h)\n");
        return EXIT_FAILURE;
    }
    /* Reale Hardware braucht Root-Rechte (GPIO, SCHED_FIFO, mlockall, Watchdog).
     * Ohne sie wird nicht gestartet; die Ausgaenge bleiben unberuehrt. */
    if (!hal_is_simulation() && (geteuid() != 0)) {
        (void)fprintf(stderr, "Fehler: Root-Rechte noetig (Start mit sudo). "
                              "Das Programm wurde nicht gestartet, der Motor bleibt AUS.\n");
        return EXIT_FAILURE;
    }

    /* 1. Hardware initialisieren; Motor bleibt AUS. */
    if (hal_init() != 0) {
        (void)fprintf(stderr, "Fehler: Hardware-Initialisierung (%s) fehlgeschlagen\n", hal_name());
        return EXIT_FAILURE;
    }
    if ((hal_motor_off() != 0) || (install_signal_handlers() != 0) || (shared_init() != 0)) {
        (void)fprintf(stderr, "Fehler: Initialisierung fehlgeschlagen (Motor AUS, "
                              "Signal-Handler oder gemeinsame Daten)\n");
        if (hal_shutdown() != 0) {
            (void)fprintf(stderr, "Fehler: Hardware konnte nicht freigegeben werden\n");
        }
        return EXIT_FAILURE;
    }
    if (bt_server_init(&options.comm) != 0) {
        (void)fprintf(stderr, "Fehler: Kommunikation (%s) konnte nicht gestartet werden\n",
                      options.comm.use_tcp ? "TCP" : "RFCOMM");
        if (hal_shutdown() != 0) {
            (void)fprintf(stderr, "Fehler: Hardware konnte nicht freigegeben werden\n");
        }
        return EXIT_FAILURE;
    }
    /* Speicher sperren (keine Verzoegerung durch Auslagern). Nur mit root:
     * ohne root wuerde das Limit RLIMIT_MEMLOCK spaetere Allokationen
     * (z. B. Thread-Stacks) fehlschlagen lassen. Auf realer Hardware ist ein
     * Fehlschlag ein Startfehler, in der Simulation nur eine Warnung. */
    if ((geteuid() != 0) || (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)) {
        if (hal_is_simulation()) {
            (void)fprintf(stderr, "Warnung: Speicher nicht gesperrt (mlockall, root noetig)\n");
        } else {
            (void)fprintf(stderr, "Fehler: Speicher konnte nicht gesperrt werden (mlockall)\n");
            bt_server_close();
            if (hal_shutdown() != 0) {
                (void)fprintf(stderr, "Fehler: Hardware konnte nicht freigegeben werden\n");
            }
            return EXIT_FAILURE;
        }
    }

    (void)printf("Pi 2 gestartet: HAL=%s, Transport=%s\n", hal_name(),
                 options.comm.use_tcp ? "TCP" : "RFCOMM");
#ifdef SSM_TESTMODE
    (void)printf("TESTMODUS: Fehlereinspeisung aktiv, nicht fuer den normalen Betrieb\n");
    print_console_help();
    open_testlog();
    /* Ungepuffert, damit poll() auf stdin keine bereits gelesenen Zeilen uebersieht. */
    if (setvbuf(stdin, NULL, _IONBF, 0) == 0) {
        stdin_open = true;
    } else {
        (void)fprintf(stderr, "Warnung: Testkonsole nicht verfuegbar\n");
    }
#endif

    /* Hardware-Watchdog: direkt vor dem Start der Threads oeffnen, damit die
     * Safety-Logik ihn sofort bedient. In der Simulation gibt es keinen. */
    if (hal_is_simulation()) {
        (void)printf("Hardware-Watchdog: nicht aktiv (Simulation)\n");
    } else if (!options.use_watchdog) {
        (void)fprintf(stderr, "WARNUNG: Hardware-Watchdog abgeschaltet (--no-watchdog); "
                              "bei einem Absturz kann der Motor weiterlaufen\n");
    } else if (watchdog_open() != 0) {
        (void)fprintf(stderr, "Fehler: Hardware-Watchdog (%s) nicht verfuegbar. Moegliche Ursachen: "
                              "keine Root-Rechte, dtparam=watchdog=on fehlt oder systemd "
                              "verwendet ihn bereits (RuntimeWatchdogSec)\n", WATCHDOG_DEVICE);
        bt_server_close();
        if (hal_shutdown() != 0) {
            (void)fprintf(stderr, "Fehler: Hardware konnte nicht freigegeben werden\n");
        }
        return EXIT_FAILURE;
    } else {
        (void)printf("Hardware-Watchdog: aktiv, Timeout %" PRId32 " s\n", (int32_t)WATCHDOG_TIMEOUT_S);
    }

    /* 2. Threads starten. Beide Sensor-Threads erhalten denselben Startzeitpunkt. */
    sensors_set_start_time(time_now_us() + ((uint64_t)SENSOR_START_DELAY_MS * TIME_US_PER_MS));
    for (i = 0U; i < thread_count; i++) {
        start_thread(&threads[i]);
        all_started = all_started && threads[i].started;
        all_realtime = all_realtime && threads[i].realtime;
    }
    if (!all_started) {
        shared_request_stop();
        exit_code = EXIT_FAILURE;
        running = false;
    } else if (!all_realtime) {
        if (hal_is_simulation()) {
            (void)fprintf(stderr, "Warnung: SCHED_FIFO nicht moeglich (root noetig); "
                                  "Zeitverhalten nicht garantiert\n");
        } else {
            /* Ohne Echtzeit-Prioritaet ist der 50-ms-Zyklus nicht gesichert. */
            (void)fprintf(stderr, "Fehler: SCHED_FIFO konnte nicht gesetzt werden; "
                                  "das Programm wird mit Motor AUS beendet\n");
            shared_request_stop();
            exit_code = EXIT_FAILURE;
            running = false;
        }
    } else {
        /* alle Threads laufen mit SCHED_FIFO */
    }

    /* 3. Hauptschleife: Zustandswechsel protokollieren, Testkonsole bedienen. */
    while (running) {
        safety_state_t state;
        uint32_t off_failures;

#ifdef SSM_TESTMODE
        if (stdin_open) {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };

            if (poll(&pfd, 1U, MAIN_POLL_MS) > 0) {
                char_t line[64];
                if (fgets(line, (int32_t)sizeof(line), stdin) != NULL) {
                    handle_console_line(line);
                } else {
                    stdin_open = false;
                }
            }
        } else {
            time_sleep_until_us(time_now_us() + ((uint64_t)MAIN_POLL_MS * TIME_US_PER_MS));
        }
#else
        time_sleep_until_us(time_now_us() + ((uint64_t)MAIN_POLL_MS * TIME_US_PER_MS));
#endif

        if ((g_signal_received != 0) && (signal_since_us == 0U)) {
            shared_request_stop();
            signal_since_us = time_now_us();
        }

        off_failures = motor_off_failures();
        if (off_failures != logged_off_failures) {
            logged_off_failures = off_failures;
            exit_code = EXIT_FAILURE;
            (void)fprintf(stderr, "FEHLER: Motor AUS konnte nicht gesetzt werden (%" PRIu32 "x)\n",
                          off_failures);
        }

        shared_state_read(&state);
        if (state.transitions != logged_transitions) {
            logged_transitions = state.transitions;
            log_state(&state);
        }
#ifdef SSM_TESTMODE
        /* Zyklusueberschreitungen der Safety-Logik melden (SAF-06) */
        if (state.overruns != logged_overruns) {
            logged_overruns = state.overruns;
            (void)printf("Safety-Logik: %" PRIu32 " Zyklusueberschreitung(en), groesste Verspaetung "
                         "%" PRIu64 " us\n", state.overruns, state.max_late_us);
            log_state(&state);
        }
#endif
        if (state.state == STATE_SHUTDOWN) {
            running = false;
        } else if ((signal_since_us != 0U) &&
                   (time_age_us(time_now_us(), signal_since_us) >
                    ((uint64_t)SIGNAL_GRACE_MS * TIME_US_PER_MS))) {
            /* Reagiert die Safety-Logik nicht auf die Stop-Anforderung, trotzdem beenden. */
            exit_code = EXIT_FAILURE;
            running = false;
        } else {
            /* weiterlaufen */
        }
    }

    /* 4. Zuerst Motor AUS (SAF-09), erst danach Threads beenden. */
    if (hal_motor_off() != 0) {
        (void)fprintf(stderr, "FEHLER: Motor AUS konnte nicht gesetzt werden\n");
        exit_code = EXIT_FAILURE;
    }
    {
        uint64_t deadline_us = time_now_us() + ((uint64_t)SHUTDOWN_ACK_WAIT_MS * TIME_US_PER_MS);
        while (!bt_server_shutdown_acked() && (time_now_us() < deadline_us)) {
            time_sleep_until_us(time_now_us() + ((uint64_t)MAIN_POLL_MS * TIME_US_PER_MS));
        }
    }
    shared_set_running(false);
    for (i = 0U; i < thread_count; i++) {
        if (threads[i].started && (pthread_join(threads[i].handle, NULL) != 0)) {
            (void)fprintf(stderr, "Fehler: Thread '%s' nicht sauber beendet\n", threads[i].name);
            exit_code = EXIT_FAILURE;
        }
    }
    bt_server_close();
#ifdef SSM_TESTMODE
    {
        safety_state_t final_state;

        shared_state_read(&final_state);
        (void)printf("Safety-Logik: %" PRIu32 " Zyklen, %" PRIu32 " Ueberschreitung(en), "
                     "groesste Verspaetung %" PRIu64 " us\n",
                     final_state.generation, final_state.overruns, final_state.max_late_us);
        close_testlog();
    }
#endif
    if ((hal_motor_off() != 0) || (hal_shutdown() != 0) || (motor_off_failures() != 0U)) {
        /* Der Watchdog bleibt absichtlich offen: Ohne "Magic Close" setzt er
         * den Pi nach dem Programmende zurueck und der Motor wird stromlos. */
        (void)fprintf(stderr, "FEHLER: Motor AUS bzw. Hardware-Freigabe fehlgeschlagen\n");
        exit_code = EXIT_FAILURE;
    } else if (watchdog_close() != 0) {
        (void)fprintf(stderr, "FEHLER: Watchdog nicht ordnungsgemaess geschlossen; "
                              "der Pi wird zurueckgesetzt\n");
        exit_code = EXIT_FAILURE;
    } else {
        (void)printf("Pi 2 beendet, Motor AUS\n");
    }
    return exit_code;
}
