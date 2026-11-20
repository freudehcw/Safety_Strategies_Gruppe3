/*
 * main_pi1.c - Raspberry Pi 1: Host- und Bedieneinheit des SSM-Demonstrators
 * (Designdokument Abschnitt 2.1).
 *
 * Ablauf:
 *   1. Optionen auswerten, Schluessel laden, AES-256-GCM initialisieren.
 *      Ohne gueltigen Schluessel startet das Programm nicht (Festlegung 8).
 *   2. Gemeinsame Daten und Logging anlegen, Ereignis "PROGRAMMSTART".
 *   3. Threads starten: logging, display, input, comm. SIGINT/SIGTERM sind
 *      in diesen Threads blockiert und kommen nur bei main an.
 *   4. Terminal in den nicht-kanonischen Modus, Tastenhilfe ausgeben.
 *   5. Warten, bis q gedrueckt wird oder SIGINT/SIGTERM eintrifft. Dann
 *      app_ctrl.shutdown setzen: comm sendet STOP und wartet hoechstens 2 s
 *      auf ACK (Abschnitt 2.11, Festlegungen 3 und 4).
 *   6. Nach dem Ende von comm die uebrigen Threads beenden, Log schreiben,
 *      Terminal wiederherstellen, Ressourcen freigeben.
 *
 * Kryptografie: OpenSSL EVP (https://www.openssl.org), AES-256-GCM nach
 * NIST SP 800-38D, siehe src/common/crypto_utils.c.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_client.h"
#include "crypto_utils.h"
#include "input.h"
#include "logging.h"
#include "options.h"
#include "pi1_config.h"
#include "shared.h"
#include "time_utils.h"
#include "types.h"
#include "ui_lcd.h"

#define THREAD_COUNT 4U

typedef struct {
    pthread_t handle;
    void *(*entry)(void *);
    void *arg;
    const char_t *name;
    bool started;
} thread_desc_t;

/* Festlegung 4: Im Signal-Handler nur ein Flag setzen. */
static volatile sig_atomic_t g_signal_received = 0;

/* Dir 4.6: Signatur durch <signal.h> vorgegeben. */
static void on_terminate(int signum)
{
    g_signal_received = (sig_atomic_t)signum;
}

/* Absturz: Terminal wiederherstellen (async-signal-sicher), dann Standardreaktion. */
static void on_fatal(int signum)
{
    input_terminal_restore_from_signal();
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
    sa.sa_handler = on_terminate;
    if ((sigemptyset(&sa.sa_mask) != 0) ||
        (sigaction(SIGINT, &sa, NULL) != 0) || (sigaction(SIGTERM, &sa, NULL) != 0)) {
        result = -1;
    }
    sa.sa_handler = on_fatal;
    for (i = 0U; (i < (sizeof(fatal_signals) / sizeof(fatal_signals[0]))) && (result == 0); i++) {
        if (sigaction(fatal_signals[i], &sa, NULL) != 0) {
            result = -1;
        }
    }
    return result;
}

/* Blockiert (block = true) bzw. erlaubt SIGINT und SIGTERM im aufrufenden Thread. */
static int32_t mask_terminate_signals(bool block)
{
    sigset_t set;
    int32_t result = -1;

    if ((sigemptyset(&set) == 0) && (sigaddset(&set, SIGINT) == 0) &&
        (sigaddset(&set, SIGTERM) == 0) &&
        (pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL) == 0)) {
        result = 0;
    }
    return result;
}

/* Laedt den Schluessel und initialisiert OpenSSL. Der Schluessel wird nie ausgegeben. */
static bool init_crypto(const char_t *path)
{
    uint8_t key[CRYPTO_KEY_LEN];
    int32_t rc = crypto_load_key_file(path, key);
    bool ok = false;

    if (rc != CRYPTO_KEY_OK) {
        (void)fprintf(stderr, "Fehler: %s: %s\n", path, crypto_key_error_text(rc));
    } else if (crypto_init(key) != 0) {
        (void)fprintf(stderr, "Fehler: OpenSSL-Initialisierung fehlgeschlagen\n");
    } else {
        ok = true;
    }
    crypto_wipe(key, sizeof(key)); /* Kopie loeschen; Schluessel liegt nur noch in OpenSSL */
    return ok;
}

static const char_t *stop_result_text(uint8_t result)
{
    const char_t *text;

    switch (result) {
    case STOP_RESULT_ACKED:
        text = "PROGRAMMENDE STOP bestaetigt (ACK)";
        break;
    case STOP_RESULT_NO_ACK:
        text = "PROGRAMMENDE ohne ACK";
        break;
    case STOP_RESULT_NOT_POSSIBLE:
        text = "PROGRAMMENDE ohne STOP (keine Verbindung)";
        break;
    default:
        text = "PROGRAMMENDE";
        break;
    }
    return text;
}

/*
 * Wartet, bis das Programmende angefordert wird, und danach, bis der
 * Kommunikations-Thread STOP/ACK abgeschlossen hat.
 */
static void main_loop(void)
{
    const uint64_t period_us = (uint64_t)PI1_MAIN_PERIOD_MS * TIME_US_PER_MS;
    app_ctrl_t ctrl;
    bool done = false;

    (void)memset(&ctrl, 0, sizeof(ctrl));
    while (!done) {
        operator_cmd_t op;
        link_status_t link;
        sig_atomic_t sig = g_signal_received;

        (void)memset(&op, 0, sizeof(op));
        (void)memset(&link, 0, sizeof(link));
        shared_operator_read(&op);
        shared_link_read(&link);

        if (!ctrl.shutdown && (op.quit || (sig != 0))) {
            /* Festlegung 4: SIGINT/SIGTERM wie q, inklusive STOP. */
            ctrl.shutdown = true;
            shared_ctrl_write(&ctrl);
            if (sig == SIGINT) {
                log_event(LOG_SRC_MAIN, "PROGRAMMENDE angefordert (SIGINT)", true, LOG_LEVEL_NONE, NULL);
            } else if (sig == SIGTERM) {
                log_event(LOG_SRC_MAIN, "PROGRAMMENDE angefordert (SIGTERM)", true, LOG_LEVEL_NONE, NULL);
            } else {
                log_event(LOG_SRC_MAIN, "PROGRAMMENDE angefordert (q)", true, LOG_LEVEL_NONE, NULL);
            }
        }
        if (link.comm_finished) {
            done = true;
        } else {
            time_sleep_until_us(time_now_us() + period_us);
        }
    }
}

static void start_threads(thread_desc_t threads[THREAD_COUNT], bool *all_ok)
{
    uint32_t i;

    *all_ok = true;
    for (i = 0U; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i].handle, NULL, threads[i].entry, threads[i].arg) == 0) {
            threads[i].started = true;
        } else {
            (void)fprintf(stderr, "Fehler: Thread '%s' konnte nicht gestartet werden\n", threads[i].name);
            *all_ok = false;
        }
    }
}

static void join_thread(thread_desc_t *t)
{
    if (t->started) {
        if (pthread_join(t->handle, NULL) != 0) {
            (void)fprintf(stderr, "Fehler: Thread '%s' nicht sauber beendet\n", t->name);
        }
        t->started = false;
    }
}

/* Dir 4.6 Ausnahme: Signatur von main. */
int main(int argc, char **argv)
{
    static options_t opt;
    static comm_cfg_t comm_cfg;
    static display_cfg_t display_cfg;
    char_t err[128];
    thread_desc_t threads[THREAD_COUNT];
    app_ctrl_t ctrl;
    link_status_t link;
    bool all_ok = false;
    int32_t log_rc;
    int32_t exit_code = EXIT_SUCCESS;

    /* 1. Optionen und Schluessel */
    if (options_parse((int32_t)argc, argv, &opt, err, sizeof(err)) != 0) {
        (void)fprintf(stderr, "Fehler: %s\n", err);
        options_print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (opt.help) {
        options_print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (!init_crypto(opt.key_path)) {
        return EXIT_FAILURE;
    }

    /* 2. Gemeinsame Daten, Logging, Signale */
    if ((shared_init() != 0) || (install_signal_handlers() != 0)) {
        (void)fprintf(stderr, "Fehler: Initialisierung (Mutexe/Signale) fehlgeschlagen\n");
        crypto_cleanup();
        return EXIT_FAILURE;
    }
    log_rc = log_init(opt.log_dir);
    if (log_rc == -2) {
        (void)fprintf(stderr, "Fehler: Logging konnte nicht angelegt werden\n");
        crypto_cleanup();
        return EXIT_FAILURE;
    } else if (log_rc != 0) {
        /* Abschnitt 2.10: Ausfall des Logging haelt das Programm nicht an. */
        (void)fprintf(stderr, "Warnung: Log-Datei in '%s' nicht moeglich, weiter ohne Datei\n",
                      opt.log_dir);
    } else {
        (void)printf("Log-Datei: %s\n", log_file_path());
    }
    log_event(LOG_SRC_MAIN,
              (opt.transport.kind == TRANSPORT_KIND_TCP) ? "PROGRAMMSTART (TCP)" : "PROGRAMMSTART (RFCOMM)",
              false, LOG_LEVEL_NONE, NULL);

    /* 3. Threads; SIGINT/SIGTERM nur bei main */
    comm_cfg.transport = opt.transport;
    display_cfg.use_console = opt.lcd_console;
    display_cfg.i2c_bus = opt.i2c_bus;
    display_cfg.lcd_addr = opt.lcd_addr;
    threads[0] = (thread_desc_t){ 0, log_thread, NULL, "logging", false };
    threads[1] = (thread_desc_t){ 0, display_thread, &display_cfg, "display", false };
    threads[2] = (thread_desc_t){ 0, input_thread, NULL, "input", false };
    threads[3] = (thread_desc_t){ 0, comm_thread, &comm_cfg, "comm", false };

    if (mask_terminate_signals(true) != 0) {
        (void)fprintf(stderr, "Warnung: Signalmaske nicht gesetzt\n");
    }
    start_threads(threads, &all_ok);
    if (mask_terminate_signals(false) != 0) {
        (void)fprintf(stderr, "Warnung: Signalmaske nicht zurueckgesetzt\n");
    }

    /* 4. Terminal und Tastenhilfe (Festlegung 1) */
    if (input_terminal_setup() != 0) {
        (void)fprintf(stderr, "Warnung: Terminal nicht umgeschaltet, Tasten mit Enter bestaetigen\n");
    }
    input_print_help();

    /* 5. Betrieb bis zum Programmende */
    (void)memset(&ctrl, 0, sizeof(ctrl));
    if (all_ok) {
        main_loop();
    } else {
        exit_code = EXIT_FAILURE;
        ctrl.shutdown = true; /* comm (falls gestartet) beendet sich */
        shared_ctrl_write(&ctrl);
    }

    /* 6. Kontrolliertes Beenden: zuerst comm (STOP/ACK), dann die anderen. */
    join_thread(&threads[3]);
    (void)memset(&link, 0, sizeof(link));
    shared_link_read(&link);
    log_event(LOG_SRC_MAIN, stop_result_text(link.stop_result), true, LOG_LEVEL_NONE, NULL);

    ctrl.shutdown = true;
    ctrl.stop_threads = true;
    shared_ctrl_write(&ctrl);
    join_thread(&threads[2]);
    join_thread(&threads[1]);
    join_thread(&threads[0]); /* zuletzt: leert alle Puffer und schreibt die Datei */

    input_terminal_restore(); /* Festlegung 1: in jedem Fall */
    log_close();
    crypto_cleanup();
    shared_destroy();
    (void)printf("Pi 1 beendet.\n");
    return exit_code;
}
