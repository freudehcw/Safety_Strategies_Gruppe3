/*
 * input.c - Tastaturbedienung von Pi 1 (Festlegungen 1 und 2).
 * Beschreibung siehe input.h.
 *
 * Der Thread wartet mit poll() und 50 ms Timeout auf eine Taste, damit er das
 * Programmende rechtzeitig bemerkt. Es gibt keine Standardstufe: Solange keine
 * Taste 1-3 gedrueckt wurde, bleibt level_chosen = false und der
 * Kommunikations-Thread sendet keine Heartbeats (Pi 2 bleibt sicher in FEHLER).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "input.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "logging.h"
#include "pi1_config.h"
#include "names.h"
#include "protocol.h"
#include "shared.h"

static struct termios g_saved_termios;
static volatile bool g_termios_saved = false;

int32_t input_terminal_setup(void)
{
    int32_t result = 0;

    if (isatty(STDIN_FILENO) == 1) {
        struct termios raw;

        if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) {
            result = -1;
        } else {
            raw = g_saved_termios;
            /* ICANON aus: Taste ohne Enter; ECHO aus: Taste nicht anzeigen.
             * ISIG bleibt an, damit Strg+C weiterhin SIGINT ausloest. */
            raw.c_lflag &= ~((tcflag_t)ICANON | (tcflag_t)ECHO);
            raw.c_cc[VMIN] = 1U;
            raw.c_cc[VTIME] = 0U;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
                result = -1;
            } else {
                g_termios_saved = true;
            }
        }
    }
    return result;
}

void input_terminal_restore(void)
{
    if (g_termios_saved) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios) != 0) {
            (void)fprintf(stderr, "Warnung: Terminal konnte nicht wiederhergestellt werden "
                                  "(Befehl 'reset' hilft)\n");
        }
        g_termios_saved = false;
    }
}

void input_terminal_restore_from_signal(void)
{
    if (g_termios_saved) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios); /* async-signal-sicher */
    }
}

uint8_t input_decode_key(char_t key)
{
    uint8_t result;

    switch (key) {
    case '1':
        result = LEVEL_LOW;
        break;
    case '2':
        result = LEVEL_MEDIUM;
        break;
    case '3':
        result = LEVEL_HIGH;
        break;
    case 'q':
    case 'Q':
        result = INPUT_KEY_QUIT;
        break;
    default:
        result = 0U; /* unbekannte Taste: ignorieren */
        break;
    }
    return result;
}

void input_print_help(void)
{
    (void)printf("Tasten: 1 = LOW, 2 = MEDIUM, 3 = HIGH, q = Programm beenden (STOP an Pi 2)\n"
                 "Hinweis: Ohne Stufenwahl werden keine Heartbeats gesendet.\n");
    (void)fflush(stdout);
}

/* Wartet hoechstens PI1_INPUT_POLL_MS auf ein Zeichen. 1 = gelesen, 0 = nichts, -1 = Ende. */
static int32_t read_key(char_t *key)
{
    struct pollfd pfd;
    int32_t result = 0;

    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1U, PI1_INPUT_POLL_MS) > 0) {
        ssize_t n = read(STDIN_FILENO, key, 1U);
        if (n == 1) {
            result = 1;
        } else if ((n < 0) && (errno == EINTR)) {
            result = 0;
        } else {
            result = -1; /* Dateiende (Pipe geschlossen) oder Fehler */
        }
    }
    return result;
}

void *input_thread(void *arg)
{
    operator_cmd_t cmd;
    bool running = true;
    bool stdin_open = true;

    (void)arg;
    (void)memset(&cmd, 0, sizeof(cmd)); /* keine Stufe gewaehlt */
    shared_operator_write(&cmd);

    while (running) {
        app_ctrl_t ctrl;
        char_t key = '\0';
        int32_t rc = 0;

        (void)memset(&ctrl, 0, sizeof(ctrl));
        shared_ctrl_read(&ctrl);
        running = !ctrl.stop_threads;

        if (running && stdin_open) {
            rc = read_key(&key);
        } else if (running) {
            /* stdin geschlossen: nur noch auf das Programmende warten */
            (void)poll(NULL, 0U, PI1_INPUT_POLL_MS);
        } else {
            /* Thread endet */
        }

        if (rc < 0) {
            stdin_open = false;
            log_event(LOG_SRC_INPUT, "EINGABE_GESCHLOSSEN", false,
                      cmd.level_chosen ? cmd.level : LOG_LEVEL_NONE, NULL);
        } else if (rc > 0) {
            uint8_t decoded = input_decode_key(key);

            if (decoded == INPUT_KEY_QUIT) {
                if (!cmd.quit) {
                    cmd.quit = true;
                    shared_operator_write(&cmd);
                    log_event(LOG_SRC_INPUT, "BEDIENER Programmende (q)", true,
                              cmd.level_chosen ? cmd.level : LOG_LEVEL_NONE, NULL);
                }
            } else if (proto_level_valid(decoded) &&
                       (!cmd.level_chosen || (cmd.level != decoded))) {
                char_t text[PI1_LOG_EVENT_TEXT_LEN];

                cmd.level_chosen = true;
                cmd.level = decoded;
                shared_operator_write(&cmd);
                /* Abschnitt 2.10: Aenderung der Sollstufe ist ein Ereignis. */
                (void)snprintf(text, sizeof(text), "SOLLSTUFE %s", name_level(decoded));
                log_event(LOG_SRC_INPUT, text, false, decoded, NULL);
            } else {
                /* unbekannte Taste oder gleiche Stufe */
            }
        } else {
            /* keine Taste */
        }
    }
    return NULL;
}
