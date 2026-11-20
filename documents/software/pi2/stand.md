# Pi 2 – Stand und offene Punkte

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Opus 5.5), geprüft von: <Name>

Stand: 20.11.2026. Diese Datei sammelt alles, was noch zu tun ist, und wo der Code vom Auftrag
`pi2_agent_prompt.md` abweicht. Die übrigen Dokumente in diesem Ordner beschreiben nur, wie der Code
funktioniert.

## Offene Punkte

### Am Raspberry Pi zu prüfen

Bisher lief der Code nur in der Simulation auf einem Linux-PC (Verbindung über TCP statt Bluetooth).

| Punkt | Bezug | Was zu tun ist |
|---|---|---|
| `hal_pigpio.c` bauen | Dir 2.1 | Die Datei ist nur gegen einen nachgebauten Header auf Syntax geprüft. Am Pi bauen; wegen `-Werror` bricht jede Warnung den Build ab. |
| Sensoren und Motor | TC-01, TC-05 | Echte HC-SR04 und Motortreiber in Betrieb nehmen. |
| Bluetooth (RFCOMM) | SAF-05 | Verbindung Pi 1 zu Pi 2 über RFCOMM Kanal 1 testen. |
| Hardware-Watchdog | Ergänzung | Nach `kill -9` muss der Pi innerhalb von etwa einer Sekunde neu starten und der Motor stehen bleiben. |
| Reaktionszeit | TC-03, SAF-05 | In der Simulation 347,4 ms nach der letzten gültigen Nachricht (Grenze 350 ms). Die Reserve ist klein; am Pi mit dem Test-Log nachmessen. |
| Zykluszeit | SAF-06 | Abweichung des 50-ms-Zyklus über 60 Sekunden im Testmodus messen (Auftrag: höchstens ± 5 ms). |
| Nachlaufzeit, Sicherheitsabstände, PWM-Stufen | SAF-10, H4, H5 | Nachlauf mit `run` in `pi2_app_test` messen, danach S_LOW, S_MEDIUM, S_HIGH (derzeit 30 / 50 / 80 cm) und die Tastverhältnisse in `pi2_config.h` festlegen. |

### In der Gruppe zu klären

| Punkt | Stand |
|---|---|
| Projektstruktur | Der Auftrag sieht `common/`, `pi1/`, `pi2/` im Wurzelverzeichnis und ein Makefile vor. Im Repository: `src/…` und CMake. |
| GPIO-Bibliothek | Der Auftrag verlangt libgpiod und PWM über sysfs. Verwendet wird pigpio (so in README und Installationsskripten). Ein Wechsel beträfe nur eine neue Datei neben `hal_pigpio.c`. |
| Abweichungsliste MISRA | Der Auftrag verlangt sie im README; sie steht in [misra_konformitaet.md](misra_konformitaet.md). |
| Prüfung durch ein Gruppenmitglied | In jeder Datei steht noch der Platzhalter `geprueft von: <Name>`. |

## Abgleich mit dem Auftrag

Der Code entstand vor dem Auftrag und wurde danach angepasst. Bereits von Anfang an erfüllt waren unter
anderem: Safety-Entscheidung als reine Funktion mit Unit-Tests, fünf Threads mit `SCHED_FIFO`,
Prioritätsvererbung, `CLOCK_MONOTONIC`, Bedingungsvariable zur Motorsteuerung, Session-ID zuerst,
Prüfreihenfolge der Nachrichten, Schaltreihenfolge am Motortreiber, kontrolliertes Beenden bei Signalen und
keine dynamische Speicherverwaltung.

Die Nummern in der zweiten Spalte beziehen sich auf die „Ergänzenden Festlegungen“ im Auftrag.

| Punkt im Auftrag | Nr. | Umsetzung |
|---|---|---|
| `common/` wird von Pi 1 geschrieben | – | `bt_server.c` verwendet `transport`, `session`, `proto_stream` und `crypto_utils` aus `src/common/`. Die Klartextnamen liegen als `names` ebenfalls dort. |
| AES-256-GCM über OpenSSL | – | von Pi 1 in `crypto_utils.c`; Pi 2 lädt den Schlüssel beim Start |
| Echo-Messung, Messfenster 25 ms | 2 | pigpio-Flanken-Callbacks mit Mikrosekunden-Ticks |
| Gemeinsamer Startzeitpunkt der Sensor-Threads | 3 | `sensors_set_start_time()` |
| P2: erste Messung ohne Prüfung, Bezugswert nach P1 | 4 | `sensor_check_sample()` |
| P3: Bewertung bei jedem neuen Messwert | 5 | `update_cross_check()` |
| Echte Nachricht mit ungültigem Inhalt führt sofort zu FEHLER / KOMMUNIKATION | 6 | `handle_frame()` |
| Sicherheitsabstände 30 / 50 / 80 cm, Prüfung beim Start | 9 | `pi2_config.h`, `safety_config_valid()` |
| Rückmeldung der Motorsteuerung | 10 | `motor_status` |
| Hardware-Watchdog | 11 | `watchdog.c` |
| Abbruch bei fehlenden Rechten, Zählen von Zyklusüberschreitungen | 14 | in der Simulation nur Warnung |
| ACK erst nach Motor AUS | 13 | `handle_shutdown()`; ohne Bestätigung innerhalb von 500 ms kein ACK, ohne STOP von Pi 1 ebenfalls nicht |
| Testmodus nur per Compile-Flag, Test-Log, Nachlaufmessung | 17 | `pi2_app_test`, `pi2_testlog.csv`, Befehl `run`. Bei `run` bleiben Sensorprüfungen und Sicherheitsabstand aktiv. |
| Sequenznummer läuft nie über | – | `session_next_tx_seq()` in `common/`, getestet in `tests/test_common.c` |
| `-Werror` | – | gesetzt |
| Simulierter Pi 1 in C mit `common/` | – | stattdessen `tools/pi1_sim.py` (AES-256-GCM über das Python-Paket `cryptography`); im Normalbetrieb dient der echte `pi1_app` als Gegenstelle |
| MISRA | – | Maßgeblich sind die neun Regeln aus `documents/vorgaben/MISRA_C_verpflichtende_Regeln.pdf` (Festlegung der Gruppe), nicht die Kategorie „Mandatory“. |
