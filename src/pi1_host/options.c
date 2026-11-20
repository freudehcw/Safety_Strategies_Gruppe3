/*
 * options.c - Kommandozeilenoptionen von Pi 1. Beschreibung siehe options.h.
 *
 * Bewusst ohne getopt: eine einfache Schleife ueber argv, jede Option mit
 * Pruefung des Wertebereichs (Dir 4.11: Argumente pruefen).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "options.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define PORT_MAX     65535L
#define CHANNEL_MAX  30L   /* RFCOMM-Kanaele 1-30 */
#define I2C_ADDR_MAX 0x77L /* 7-Bit-Adressen 0x03-0x77 */
#define I2C_ADDR_MIN 0x03L

/* Wandelt text in eine Zahl in [min, max]; base 0 erlaubt "0x27". */
static bool parse_number(const char_t *text, int64_t min, int64_t max, int64_t *value)
{
    bool ok = false;

    if ((text != NULL) && (text[0] != '\0')) {
        char_t *end = NULL;
        long long v;

        errno = 0;
        v = strtoll(text, &end, 0);
        if ((errno == 0) && (end != NULL) && (*end == '\0') && (v >= min) && (v <= max)) {
            *value = (int64_t)v;
            ok = true;
        }
    }
    return ok;
}

/* Kopiert text nach dst; false, wenn er nicht passt oder leer ist. */
static bool copy_text(char_t *dst, size_t cap, const char_t *text)
{
    bool ok = false;

    if ((text != NULL) && (text[0] != '\0') && (strlen(text) < cap)) {
        (void)snprintf(dst, cap, "%s", text);
        ok = true;
    }
    return ok;
}

/* Bluetooth-Adresse im Format AA:BB:CC:DD:EE:FF (nur Form pruefen). */
static bool bt_addr_valid(const char_t *text)
{
    bool ok = (text != NULL) && (strlen(text) == 17U);
    size_t i;

    for (i = 0U; ok && (i < 17U); i++) {
        char_t c = text[i];
        if ((i % 3U) == 2U) {
            ok = (c == ':');
        } else {
            ok = ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'F')) ||
                 ((c >= 'a') && (c <= 'f'));
        }
    }
    return ok;
}

void options_defaults(options_t *opt)
{
    if (opt != NULL) {
        (void)memset(opt, 0, sizeof(*opt));
        opt->transport.kind = TRANSPORT_KIND_RFCOMM;
        (void)snprintf(opt->transport.host, sizeof(opt->transport.host), "%s", PI1_DEFAULT_TCP_HOST);
        opt->transport.tcp_port = (uint16_t)PI1_DEFAULT_TCP_PORT;
        opt->transport.rfcomm_channel = (uint8_t)CFG_RFCOMM_CHANNEL;
        (void)snprintf(opt->key_path, sizeof(opt->key_path), "%s", CFG_PSK_DEFAULT_PATH);
        (void)snprintf(opt->i2c_bus, sizeof(opt->i2c_bus), "%s", PI1_DEFAULT_I2C_BUS);
        opt->lcd_addr = (uint8_t)PI1_DEFAULT_LCD_ADDR;
        (void)snprintf(opt->log_dir, sizeof(opt->log_dir), "%s", PI1_DEFAULT_LOG_DIR);
    }
}

int32_t options_parse(int32_t argc, char_t *const argv[], options_t *opt,
                      char_t *err, size_t err_cap)
{
    int32_t a = 1;
    bool ok = true;
    int64_t v = 0;

    if ((argv == NULL) || (opt == NULL) || (err == NULL) || (err_cap == 0U)) {
        return -1;
    }
    err[0] = '\0';
    options_defaults(opt);

    while (ok && (a < argc)) {
        const char_t *name = argv[a];
        const char_t *value = ((a + 1) < argc) ? argv[a + 1] : NULL;
        int32_t used = 2; /* Option mit Wert */

        if (strcmp(name, "--tcp") == 0) {
            opt->transport.kind = TRANSPORT_KIND_TCP;
            used = 1;
        } else if (strcmp(name, "--lcd-console") == 0) {
            opt->lcd_console = true;
            used = 1;
        } else if ((strcmp(name, "--help") == 0) || (strcmp(name, "-h") == 0)) {
            opt->help = true;
            used = 1;
        } else if (strcmp(name, "--bt-addr") == 0) {
            ok = bt_addr_valid(value) &&
                 copy_text(opt->transport.bt_addr, sizeof(opt->transport.bt_addr), value);
        } else if (strcmp(name, "--channel") == 0) {
            ok = parse_number(value, 1, CHANNEL_MAX, &v);
            opt->transport.rfcomm_channel = ok ? (uint8_t)v : opt->transport.rfcomm_channel;
        } else if (strcmp(name, "--tcp-host") == 0) {
            ok = copy_text(opt->transport.host, sizeof(opt->transport.host), value);
        } else if (strcmp(name, "--tcp-port") == 0) {
            ok = parse_number(value, 1, PORT_MAX, &v);
            opt->transport.tcp_port = ok ? (uint16_t)v : opt->transport.tcp_port;
        } else if (strcmp(name, "--key") == 0) {
            ok = copy_text(opt->key_path, sizeof(opt->key_path), value);
        } else if (strcmp(name, "--i2c-bus") == 0) {
            ok = copy_text(opt->i2c_bus, sizeof(opt->i2c_bus), value);
        } else if (strcmp(name, "--lcd-addr") == 0) {
            ok = parse_number(value, I2C_ADDR_MIN, I2C_ADDR_MAX, &v);
            opt->lcd_addr = ok ? (uint8_t)v : opt->lcd_addr;
        } else if (strcmp(name, "--log-dir") == 0) {
            ok = copy_text(opt->log_dir, sizeof(opt->log_dir), value);
        } else {
            ok = false;
            (void)snprintf(err, err_cap, "unbekannte Option: %s", name);
        }

        if (!ok && (err[0] == '\0')) {
            (void)snprintf(err, err_cap, "ungueltiger oder fehlender Wert fuer %s", name);
        }
        a += used;
    }

    if (ok && !opt->help && (opt->transport.kind == TRANSPORT_KIND_RFCOMM) &&
        (opt->transport.bt_addr[0] == '\0')) {
        ok = false;
        (void)snprintf(err, err_cap, "--bt-addr ist im RFCOMM-Betrieb Pflicht (oder --tcp)");
    }
    return ok ? 0 : -1;
}

void options_print_usage(const char_t *prog)
{
    (void)printf("Aufruf: %s --bt-addr AA:BB:CC:DD:EE:FF [Optionen]\n"
                 "        %s --tcp [--tcp-host 127.0.0.1] [--tcp-port 5555] [Optionen]\n"
                 "Optionen:\n"
                 "  --channel N          RFCOMM-Kanal (Standard 1)\n"
                 "  --key PFAD           Schluesseldatei (Standard %s)\n"
                 "  --i2c-bus PFAD       I2C-Bus (Standard %s)\n"
                 "  --lcd-addr ADRESSE   LCD-Adresse (Standard 0x%02X)\n"
                 "  --lcd-console        Anzeige auf der Konsole statt am LCD\n"
                 "  --log-dir PFAD       Log-Verzeichnis (Standard %s)\n",
                 (prog != NULL) ? prog : "pi1_app", (prog != NULL) ? prog : "pi1_app",
                 CFG_PSK_DEFAULT_PATH, PI1_DEFAULT_I2C_BUS, (unsigned)PI1_DEFAULT_LCD_ADDR,
                 PI1_DEFAULT_LOG_DIR);
}
