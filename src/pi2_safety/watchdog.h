/*
 * watchdog.h - Hardware-Watchdog des Raspberry Pi (/dev/watchdog).
 *
 * Ergaenzung zum Designdokument: Stuerzt das Programm ab oder haengt es,
 * laeuft die Hardware-PWM unabhaengig vom Prozess weiter und der Motor bliebe
 * an. Der Watchdog setzt den Pi dann nach WATCHDOG_TIMEOUT_S zurueck; nach dem
 * Reset sind die GPIOs Eingaenge und STBY liegt ueber den Pull-down auf LOW.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* Oeffnet den Watchdog und setzt den Timeout. 0 = OK, -1 = Fehler (Geraet
 * fehlt, keine Rechte oder bereits von systemd belegt). */
int32_t watchdog_open(void);

/* true, wenn der Watchdog geoeffnet ist und bedient werden muss. */
bool watchdog_is_open(void);

/* Bedient den Watchdog. 0 = OK, -1 = Fehler oder nicht geoeffnet. */
int32_t watchdog_kick(void);

/* Schliesst den Watchdog ordnungsgemaess ("Magic Close"); er loest danach
 * nicht mehr aus. 0 = OK, -1 = Fehler. */
int32_t watchdog_close(void);

#endif /* WATCHDOG_H */
