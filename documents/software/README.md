# Software-Dokumentation

Dokumentation zur Software des Demonstrators. Das verbindliche Design steht in den Designdokumenten
(`documents/designdoc-safety/`, `documents/designdoc-security/`), die Vorgaben der Lehrveranstaltung in
`documents/vorgaben/`. Bauen, Starten und Testen sind im `README.md` im Wurzelverzeichnis beschrieben.

## Aufbau

| Ordner | Inhalt |
|---|---|
| `pi1/` | Raspberry Pi 1 (Bedienung, LCD, Logging, Kommunikation als Client) |
| `pi2/` | Raspberry Pi 2 (Sensoren, Safety-Logik, Motor, Kommunikation als Server) |
| `common/` | gemeinsamer Code beider Pis (Protokoll, Kryptografie, Session, Transport); Ordner wird mit der ersten Erklärung angelegt |

Neue Modulerklärungen kommen als `<modul>.md` in den Ordner des jeweiligen Pi.

## Pi 1

| Datei | Inhalt |
|---|---|
| [plan.md](pi1/plan.md) | Plan, Module und Entscheidungen |
| [inbetriebnahme.md](pi1/inbetriebnahme.md) | Inbetriebnahme am Raspberry Pi |

## Pi 2

Zum Einlesen in dieser Reihenfolge:

| Datei | Inhalt |
|---|---|
| [ueberblick.md](pi2/ueberblick.md) | Module, Begriffe, Threads, gemeinsame Daten, Ablauf |
| [safety_logik.md](pi2/safety_logik.md) | Safety-Entscheidung, P3, P4, Zustände, Watchdog-Bedingung |
| [sensoren.md](pi2/sensoren.md) | Sensor-Threads, P1, P2, Testmodus |
| [motor_hal.md](pi2/motor_hal.md) | Motorsteuerung, Hardware-Schicht, Hardware-Watchdog |
| [kommunikation.md](pi2/kommunikation.md) | Kommunikations-Thread, Nachrichtenprüfung, STOP und ACK |
| [misra_konformitaet.md](pi2/misra_konformitaet.md) | Nachweis der neun verpflichtenden MISRA-Regeln |
| [stand.md](pi2/stand.md) | offene Punkte, Abgleich mit dem Auftrag |
