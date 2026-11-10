# MISRA C:2012 – Konformität der Pi-2-Software

Geprüft gegen die neun verpflichtenden Regeln aus `MISRA_C_verpflichtende_Regeln.pdf`. Diese neun Regeln sind
für das Projekt maßgeblich (Festlegung der Gruppe), nicht die Kategorie „Mandatory“ von MISRA C:2012.
Geltungsbereich: `src/common/`, `src/pi2_safety/`, `tests/`. Stand: 10.11.2026.

## Übersicht

| Regel | Inhalt | Status | Nachweis |
|---|---|---|---|
| Dir 2.1 | Übersetzung ohne Fehler | erfüllt (Simulation), offen für `hal_pigpio.c` | GCC mit `-Wall -Wextra -Wpedantic -Wshadow -Werror` ohne Fehler und Warnungen, im normalen Build und im Testmodus. `hal_pigpio.c` ist nur auf Syntax geprüft und muss am Raspberry Pi gebaut werden. |
| Dir 3.1 | Rückverfolgbarkeit zu Anforderungen | teilweise | Kommentare verweisen auf SAF-xx, Hx, Px, TC-xx. Offene Punkte siehe unten. |
| Dir 4.1 | Laufzeitfehler minimieren | erfüllt | Zeigerprüfung in allen öffentlichen Funktionen, Bereichsprüfung vor jedem Array-Zugriff und jeder einengenden Umwandlung, keine dynamische Speicherverwaltung, 216 Unit-Test-Prüfungen. |
| Dir 4.6 | Typen mit fester Breite | erfüllt mit Ausnahmen | Durchgehend `<stdint.h>`-Typen, `bool`, `size_t`, `char_t`. Ausnahmen siehe unten. |
| Dir 4.7 | Fehlerinformationen auswerten | erfüllt gemäß Projektliste | Projektliste siehe unten. |
| Dir 4.10 | Include-Guards | erfüllt | Alle Header mit `#ifndef NAME_H` / `#define NAME_H`, Namen projektweit eindeutig. |
| Dir 4.11 | Argumente für Bibliotheksfunktionen prüfen | erfüllt | Längen vor `memcpy`/`memmove`/`recv`, Zeiger vor Verwendung, `strtol` mit `errno` und Endzeiger, Tabellenindizes mit Bereichsprüfung, PWM-Tastverhältnis 1..1000. |
| Rule 14.2 | Wohlgeformte `for`-Schleifen | erfüllt | cppcheck-MISRA-Addon ohne Befund, zusätzlich Durchsicht aller `for`-Schleifen. |
| Rule 15.1 | Kein `goto` | erfüllt | Kein `goto` im Code, cppcheck-MISRA-Addon ohne Befund. |

## Dir 4.6 – verbleibende Basistypen

| Stelle | Begründung |
|---|---|
| `typedef char char_t;` in `types.h` | Ausnahme 1 der Direktive |
| `int main(int argc, char **argv)` und `int main(void)` | Ausnahmen 2 bis 4 der Direktive |
| `on_terminate(int)`, `on_fatal(int)` in `main_pi2.c` | Signatur durch `<signal.h>` vorgegeben |
| `echo_callback(int, int, ...)` in `hal_pigpio.c` | Signatur durch pigpio vorgegeben |

## Dir 4.7 – Projektliste

Als Funktionen mit Fehlerinformation gelten alle eigenen Funktionen mit Rückgabewert `int32_t` (0 = OK) sowie alle
POSIX-, pthread- und pigpio-Funktionen. Ihr Rückgabewert wird ausgewertet. Davon ausgenommen:

| Funktion | Begründung |
|---|---|
| `printf`, `fprintf`, `fflush` | Reine Diagnoseausgabe außerhalb der Safety-Logik; ein Fehler beeinflusst die Safety-Funktion nicht. |
| `close` | Aufräumpfad; es ist keine sinnvolle Reaktion möglich. |
| `memset`, `memcpy`, `memmove` | Liefern keinen Fehler, sondern den Zielzeiger. |
| `pthread_mutex_unlock`, `pthread_cond_broadcast` | Aufruf nur nach erfolgreichem `pthread_mutex_lock` desselben Threads auf einem initialisierten Objekt; ein Fehler ist ausgeschlossen (Ausnahme der Direktive). |
| `signal`, `raise`, `gpioWrite` in `on_fatal` bzw. `hal_motor_emergency_off` | Fehlerpfad im Signal-Handler; keine weitere Reaktion möglich. |
| `shared_state_wait`, `atomic_fetch_add` | Rückgabewert ist keine Fehlerinformation. |

## Dir 3.1 – Code ohne Anforderung im Designdokument

Diese Teile sind derzeit nicht auf eine dokumentierte Anforderung zurückführbar. Sie müssen entweder im
Designdokument ergänzt oder aus dem Code entfernt werden.

| Code | Zweck |
|---|---|
| TCP-Transport (`--tcp`) in `bt_server.c` | Entwicklung ohne Bluetooth |
| Simulations-HAL `hal_sim.c` und Konsolenbefehl `d` | Entwicklung ohne Hardware |
| Überwachung der Safety-Logik in `motor.c` (`SAFETY_ALIVE_TIMEOUT_MS`) | Motor AUS, wenn die Safety-Logik ihren Zustand nicht mehr aktualisiert |
| Hardware-Watchdog `watchdog.c` und Rückmeldung `motor_status` | Reset des Pi, wenn Safety-Logik oder Motorsteuerung hängen oder das Programm abstürzt; im README als Ergänzung zum Designdokument beschrieben |
| Lokaler Stop über SIGINT/SIGTERM und Handler für Programmfehler in `main_pi2.c` | Motor AUS beim Beenden oder Absturz des Programms |
| Zähler `motor_off_failures` | Meldung, wenn Motor AUS nicht gesetzt werden konnte |
| `mlockall` und feste Stackgröße der Threads | Zeitverhalten |
| Befehl `run` und Zähler für Zyklusüberschreitungen im Testmodus | Nachlaufmessung für SAF-10 und Nachweis für SAF-06; nur in `pi2_app_test` enthalten |

## Weitere Befunde des cppcheck-MISRA-Addons

Das Addon meldet Befunde zu Regeln, die nicht in der verpflichtenden Liste stehen, vor allem Rule 21.6
(`<stdio.h>`) durch die Diagnoseausgaben. Diese Regeln wurden nicht bearbeitet.
