# Pi 2 – Sensoren (`sensors.c`)

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Fable 5.2), geprüft von: <Name>

## Aufgabe

Zwei Sensor-Threads lesen je einen HC-SR04 aus, prüfen den Messwert nach P1 und P2 und legen Abstand,
Gültigkeit, Zeitstempel und Messzähler in `sensor_data[i]` ab (SAF-03, SAF-04).

## Ablauf einer Messung

1. Der Thread schläft bis zu seinem nächsten Messzeitpunkt. Die Periode ist 60 ms, Sensor 2 startet 30 ms
   später als Sensor 1, damit sich die Schallimpulse nicht stören.
2. Er merkt sich die monotone Zeit `t0` und löst mit `hal_sonar_trigger()` den Trigger-Impuls von 10 µs aus.
3. Er wartet ein festes Messfenster von 30 ms. Ein Echo aus 400 cm braucht etwa 23,3 ms.
4. `hal_sonar_result()` liefert die Echo-Dauer oder „kein Echo“.
5. Umrechnung ohne Gleitkomma: Abstand in mm = Echo-Dauer in µs × 343 / 2000.
6. `sensor_check_sample()` prüft P1 und P2.
7. Der Datensatz wird mit Zeitstempel `t0` und erhöhtem Zähler `seq` geschrieben.

## Die Prüfungen

| Prüfung | Bedingung für „ungültig“ |
|---|---|
| P1 | kein Echo im Messfenster, Abstand unter 20 mm oder über 4000 mm |
| P2 | Änderung gegenüber der vorherigen Messung schneller als 2 mm/ms |

P2 wird ohne Division gerechnet: `|Δd| × 1000 > 2 × Δt` mit Δt in µs.

## Wichtige Entscheidungen

- **Zeitstempel ist der Beginn der Messung.** Das Datenalter (P4) wird damit eher zu groß als zu klein gerechnet.
- **Bezugswert für P2 ist die letzte Messung im Messbereich,** auch wenn sie selbst P2 verletzt hat. Springt
  der Abstand wirklich (ein Objekt taucht auf), ist genau eine Messung ungültig und die nächste wieder gültig.
- **Ohne Bezugswert gilt die Messung als ungültig.** Das betrifft die erste Messung nach dem Start und nach
  einem P1-Fehler. Ein Sensor braucht also zwei plausible Messungen bis zur Freigabe. Der Auftrag
  `pi2_agent_prompt.md` (Ergänzung 4) legt das anders fest: Dort gilt die erste Messung als gültig.
- **Der Zähler `seq` beginnt bei 1.** Der Wert 0 bedeutet „noch nie gemessen“.
- **Verpasster Takt:** Liegt der nächste Messzeitpunkt schon in der Vergangenheit, setzt der Thread neu auf,
  statt Messungen nachzuholen.

## Stellen, an denen man Fehler leicht übersieht

- Steht kein Objekt innerhalb von 4 m, kommt kein Echo und das System bleibt in FEHLER / SENSOR. Für die
  Vorführung braucht es eine Rückwand im Messbereich.
- Bei 60 ms Messabstand erlaubt P2 höchstens 120 mm Änderung. Ein Objekt, das seitlich in den Strahl kommt,
  löst deshalb zuerst FEHLER / SENSOR und erst danach OBJEKT_STOPP aus. Der Motor ist in beiden Fällen aus.
- Die Sensoren messen um 30 ms versetzt. Bewegt sich ein Objekt schnell, können d1 und d2 allein dadurch
  um mehrere Zentimeter abweichen (bei 2 m/s um 6 cm) und P3 auslösen.
- Jeder Thread berechnet seinen Startzeitpunkt selbst. Der Versatz von 30 ms hängt deshalb davon ab, wie
  schnell nacheinander die Threads starten.

## Testmodus

Mit `--test` lassen sich Fehler einspeisen: `force <1|2> <mm>` ersetzt den Rohmesswert (TC-01a, TC-01b,
TC-02), `block <1|2> <ms>` hält den Thread einmalig an (TC-04). In der Simulation setzt `d <1|2> <mm>` den
gemessenen Abstand.
