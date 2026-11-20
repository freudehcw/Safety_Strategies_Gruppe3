/*
 * options.h - Kommandozeilenoptionen von Pi 1 (Festlegung 7).
 *
 *   --bt-addr AA:BB:CC:DD:EE:FF  Bluetooth-Adresse von Pi 2 (Pflicht ohne --tcp)
 *   --channel N                  RFCOMM-Kanal (Standard 1)
 *   --tcp                        TCP statt RFCOMM (Test am PC mit mock_pi2)
 *   --tcp-host ADRESSE           TCP-Ziel (Standard 127.0.0.1)
 *   --tcp-port PORT              TCP-Port (Standard 5555)
 *   --key PFAD                   Schluesseldatei (Standard /etc/ssm/psk.bin)
 *   --i2c-bus PFAD               I2C-Bus (Standard /dev/i2c-1)
 *   --lcd-addr ADRESSE           I2C-Adresse des LCD (Standard 0x27)
 *   --lcd-console                Anzeige auf der Konsole statt am LCD
 *   --log-dir PFAD               Log-Verzeichnis (Standard ./logs)
 *   --help                       Hilfe
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#ifndef PI1_OPTIONS_H
#define PI1_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>

#include "pi1_config.h"
#include "transport.h"
#include "types.h"

#define OPTIONS_PATH_LEN 256U

typedef struct {
    transport_cfg_t transport;
    char_t key_path[OPTIONS_PATH_LEN];
    char_t i2c_bus[OPTIONS_PATH_LEN];
    uint8_t lcd_addr;
    bool lcd_console;
    char_t log_dir[OPTIONS_PATH_LEN];
    bool help;
} options_t;

/* Setzt die Standardwerte. */
void options_defaults(options_t *opt);

/*
 * Wertet argv aus. 0 = OK, -1 = ungueltig (Meldung steht in err).
 * Ohne --tcp ist --bt-addr Pflicht.
 */
int32_t options_parse(int32_t argc, char_t *const argv[], options_t *opt,
                      char_t *err, size_t err_cap);

void options_print_usage(const char_t *prog);

#endif /* PI1_OPTIONS_H */
