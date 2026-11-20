# Pi 2 – Safety-Logik (`safety_logic.c`)

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Fable 5.2), geprüft von: <Name>

## Aufgabe

Der Safety-Logik-Thread bestimmt alle 50 ms den Zustand von Pi 2 und die freigegebene Motorstufe
(SAF-06). Die Entscheidung selbst steckt in `safety_evaluate()`. Diese Funktion greift weder auf Hardware
noch auf die Uhr oder auf gemeinsame Daten zu und lässt sich deshalb vollständig per Unit-Test prüfen.

## Ablauf eines Zyklus

1. Der Thread bedient den Hardware-Watchdog, wenn die Motorsteuerung aktuell ist (siehe unten).
2. Er kopiert `sensor_data[0]`, `sensor_data[1]` und `comm_data`.
3. Er liest die monotone Zeit. Das geschieht nach dem Kopieren, damit kein Zeitstempel in der Zukunft liegt.
4. `safety_evaluate()` entscheidet in der Reihenfolge des Designdokuments (Abschnitt 2.3):

| Schritt | Prüfung | Ergebnis bei Fehlschlag |
|---|---|---|
| 1 | Stop/Shutdown von Pi 1 oder lokales Signal | SHUTDOWN (Endzustand) |
| 2 | beide Sensorwerte gültig (P1, P2) und P3 nicht verletzt | FEHLER / SENSOR |
| 3 | Nachricht vorhanden, höchstens 300 ms alt, Stufe gültig | FEHLER / KOMMUNIKATION |
| 4 | beide Messwerte höchstens 150 ms alt (P4) | FEHLER / DATENALTER |
| 5 | Sicherheitsabstand S zur Stufe wählen | FEHLER / KOMMUNIKATION bei unbekannter Stufe |
| 6 | d_min < S | OBJEKT_STOPP |
| – | alles bestanden | BETRIEB mit der Sollstufe |

5. Der Thread schreibt `safety_state` und weckt damit die Motorsteuerung.
6. Er schläft bis zum nächsten absoluten Zeitpunkt (`clock_nanosleep` mit `TIMER_ABSTIME`).

## Wichtige Entscheidungen

- **Grundstellung ist FEHLER mit Motor AUS.** `safety_evaluate()` setzt das Ergebnis zuerst auf
  FEHLER / SENSOR / AUS. Eine Stufe wird nur im letzten Zweig eingetragen (SAF-01).
- **Kein Speichern von Fehlern.** Der Zustand wird in jedem Zyklus neu bestimmt. Sind alle Bedingungen
  erfüllt, läuft der Motor ohne Quittierung wieder an (SAF-11, Abschnitt 2.6).
- **SHUTDOWN bleibt.** `ctx->shutdown` wird nie zurückgesetzt.
- **P3 zählt neue Messwerte, nicht Safety-Zyklen** (Klarstellung 5 im Auftrag). Jeder Messwert trägt einen
  Zähler `seq`. P3 wird nur bewertet, wenn beide Werte gültig sind und seit der letzten Bewertung mindestens
  ein Sensor einen neuen Wert geliefert hat. Zwei Bewertungen mit mehr als 5 cm Abweichung hintereinander
  verletzen P3, eine Bewertung ohne Abweichung setzt den Zähler zurück.
- **Die Sicherheitsabstände stehen in `pi2_config.h`** und sind vorläufig: 30, 50 und 80 cm. Beim Start prüft
  `safety_config_valid()`, dass sie mit der Stufe wachsen. Sonst startet das Programm nicht.
- **Das Alter der Nachricht berechnet die Safety-Logik selbst** aus `comm_data.last_valid_us`. Hängt der
  Kommunikations-Thread, altert der Zeitstempel und es folgt FEHLER / KOMMUNIKATION.
- **Ungültige Zeiten gelten als veraltet.** `time_age_us()` liefert den größtmöglichen Wert, wenn ein
  Zeitstempel 0 ist oder in der Zukunft liegt.

## Watchdog-Bedingung

`safety_motor_up_to_date()` vergleicht die zuletzt geschriebene Generation von `safety_state` mit der
Generation, die die Motorsteuerung in `motor_status` zurückgemeldet hat. Der Watchdog wird nur bedient, wenn
die Motorsteuerung höchstens einen Zyklus (50 ms) zurückliegt und der Motor nicht im Zustand „unbekannt“ ist.

| Fall | Folge |
|---|---|
| Safety-Logik hängt | Watchdog wird nicht mehr bedient, Reset nach 1 s |
| Motorsteuerung hängt | Rückmeldung veraltet, Watchdog wird nicht mehr bedient, Reset nach 1 s |
| Motor AUS konnte nicht gesetzt werden | Rückmeldung „unbekannt“, Watchdog wird nicht mehr bedient, Reset nach 1 s |
| Prozess stürzt ab oder wird mit `kill -9` beendet | Watchdog läuft weiter, Reset nach 1 s |
| Watchdog lässt sich nicht bedienen | Safety-Logik fordert Stop an, kontrolliertes Beenden |

Nach dem Reset sind die GPIOs Eingänge, und der Pull-down an STBY schaltet den Motortreiber ab.

## Stellen, an denen man Fehler leicht übersieht

- Die Grenzen sind einschließlich: genau 300 ms, genau 150 ms, genau 5 cm und d_min = S sind noch zulässig.
- Reaktionszeit bei Kommunikationsverlust: 300 ms Timeout plus höchstens ein Zyklus von 50 ms (SAF-05).
- Ein hängender Sensor-Thread lässt `valid` auf `true` stehen. Erkannt wird er über das Datenalter (P4).
- Weil abwechselnd alle 30 ms ein neuer Messwert eintrifft, bewertet praktisch jeder Zyklus der Safety-Logik
  P3. Eine dauerhafte Abweichung führt deshalb nach zwei Zyklen zum Fehler, also nach rund 50 bis 100 ms.
- Zählt eine Nachricht von Pi 1 als ungültiger Befehl, entwertet die Kommunikation `comm_data` sofort. Die
  Safety-Logik sieht dann „keine Nachricht“ und setzt FEHLER / KOMMUNIKATION, ohne den Timeout abzuwarten.

## Überwachung der Zykluszeit

Der Thread misst in jedem Zyklus, wie viel später als geplant er begonnen hat, und merkt sich den größten
Wert (`max_late_us`). Dauert ein Zyklus länger als die Periode, erhöht er `overruns`. Beide Werte stehen in
`safety_state`. Im Testmodus gibt das Hauptprogramm sie aus und schreibt sie ins Test-Log (SAF-06).

## Nachlaufmessung im Testmodus

Mit dem Befehl `run <1|2|3> <s>` ersetzt `pi2_app_test` für die gewählte Dauer den Befehl von Pi 1 durch die
angegebene Stufe. Alle Sensorprüfungen und der Sicherheitsabstand bleiben aktiv. Nach Ablauf folgt
FEHLER / KOMMUNIKATION und Motor AUS. Der Zeitpunkt steht im Test-Log, sodass sich die Nachlaufzeit des
Rotors dagegen messen lässt (SAF-10). `safety_evaluate()` ist im Testmodus unverändert.

## Tests

`tests/test_pi2.c` prüft jede Zeile der Tabelle oben, die Grenzwerte, die Reihenfolge der Fehlercodes, den
Wiederanlauf, alle drei Stufen, P3 mit einzeln eintreffenden Messwerten und die Watchdog-Bedingung. Aufruf: `ctest --output-on-failure` im Build-Verzeichnis.
