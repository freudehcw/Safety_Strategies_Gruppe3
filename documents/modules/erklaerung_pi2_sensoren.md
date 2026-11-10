# Pi 2 – Sensoren (`sensors.c`)

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Fable 5.2), geprüft von: <Name>

## Aufgabe

Zwei Sensor-Threads lesen je einen HC-SR04 aus, prüfen den Messwert nach P1 und P2 und legen Abstand,
Gültigkeit, Zeitstempel und Messzähler in `sensor_data[i]` ab (SAF-03, SAF-04).

## Ablauf einer Messung

1. Der Thread schläft bis zu seinem nächsten Messzeitpunkt. Die Periode ist 60 ms. Beide Threads rechnen
   vom selben Startzeitpunkt aus, Sensor 2 liegt 30 ms hinter Sensor 1. So stören sich die Schallimpulse nicht.
2. Er merkt sich die monotone Zeit `t0` und löst mit `hal_sonar_trigger()` den Trigger-Impuls von 10 µs aus.
3. Er wartet ein festes Messfenster von 25 ms. Ein Echo aus 400 cm braucht etwa 23,3 ms.
4. `hal_sonar_result()` liefert die Echo-Dauer oder „kein Echo“.
5. Umrechnung ohne Gleitkomma: Abstand in mm = Echo-Dauer in µs × 343 / 2000.
6. `sensor_check_sample()` prüft P1 und P2.
7. Der Datensatz wird mit Zeitstempel `t0` und erhöhtem Zähler `seq` geschrieben.

## Die Prüfungen

| Prüfung | Bedingung für „ungültig“ |
|---|---|
| P1 | kein Echo im Messfenster, Abstand unter 20 mm oder über 4000 mm |
| P2 | Änderung gegenüber dem Bezugswert schneller als 2 mm/ms |

P2 wird ohne Division gerechnet: `|Δd| × 1000 > 2 × Δt` mit Δt in µs.

## Wichtige Entscheidungen

- **Zeitstempel ist der Beginn der Messung.** Das Datenalter (P4) wird damit eher zu groß als zu klein gerechnet.
- **Bezugswert für P2 ist die letzte Messung, die P1 bestanden hat,** auch wenn sie selbst P2 verletzt hat
  (Klarstellung 4 im Auftrag). Springt der Abstand wirklich, weil ein Objekt auftaucht, ist genau eine
  Messung ungültig und die nächste wieder gültig.
- **Ohne Bezugswert wird P2 nicht geprüft und die Messung gilt.** Das betrifft die erste Messung nach dem
  Start und nach einem P1-Fehler.
- **Gemeinsamer Startzeitpunkt:** Das Hauptprogramm legt ihn vor dem Start der Threads fest
  (`sensors_set_start_time()`). Der Versatz von 30 ms hängt dadurch nicht davon ab, wann die Threads anlaufen.
- **Der Zähler `seq` beginnt bei 1.** Der Wert 0 bedeutet „noch nie gemessen“. Die Safety-Logik erkennt
  daran neue Messwerte für P3.
- **Verpasster Takt:** Liegt der nächste Messzeitpunkt schon in der Vergangenheit, setzt der Thread neu auf,
  statt Messungen nachzuholen.

## Stellen, an denen man Fehler leicht übersieht

- Steht kein Objekt innerhalb von 4 m, kommt kein Echo und das System bleibt in FEHLER / SENSOR. Für die
  Vorführung braucht es eine Rückwand im Messbereich.
- Bei 60 ms Messabstand erlaubt P2 höchstens 120 mm Änderung. Ein Objekt, das seitlich in den Strahl kommt,
  löst deshalb zuerst FEHLER / SENSOR und erst danach OBJEKT_STOPP aus. Der Motor ist in beiden Fällen aus.
- Die Sensoren messen um 30 ms versetzt. Bewegt sich ein Objekt schnell, können d1 und d2 allein dadurch
  um mehrere Zentimeter abweichen (bei 2 m/s um 6 cm) und P3 auslösen.
- Nach einem verpassten Takt stimmt der Versatz von 30 ms nicht mehr genau, weil der Thread von der
  aktuellen Zeit aus neu aufsetzt.

## Testmodus

Die Fehlereinspeisung ist nur im Programm `pi2_app_test` enthalten (Build mit `SSM_TESTMODE`). Im normalen
`pi2_app` fehlt dieser Code. Befehle auf der Standardeingabe:

| Befehl | Wirkung | Testfall |
|---|---|---|
| `force <1\|2> <mm>` | ersetzt den Rohmesswert des Sensors | TC-01a, TC-01b, TC-02 |
| `unforce <1\|2>` | hebt die Vorgabe auf | – |
| `block <1\|2> <ms>` | hält den Sensor-Thread einmalig an | TC-04 |
| `d <1\|2> <mm>` | setzt den simulierten Abstand (nur Simulation, 0 = kein Echo) | TC-05 |
