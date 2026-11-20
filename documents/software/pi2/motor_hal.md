# Pi 2 – Motorsteuerung und Hardware-Schicht (`motor.c`, `hal_*.c`)

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Fable 5.2), geprüft von: <Name>

## Aufgabe

Der Motorsteuerungs-Thread setzt ausschließlich die Stufe um, die die Safety-Logik in `safety_state`
freigegeben hat. Alles andere führt zu Motor AUS (SAF-01). Die Hardware-Schicht (HAL) kapselt alle
Zugriffe auf GPIO und PWM hinter `hal.h`.

## Ablauf der Motorsteuerung

1. Der Thread wartet an der Bedingungsvariable von `safety_state`, höchstens 50 ms.
2. Er prüft zwei Bedingungen: Zustand ist BETRIEB, und `safety_state` wurde vor höchstens 150 ms aktualisiert.
3. Sind beide erfüllt, bestimmt `motor_duty_permille()` das Tastverhältnis zur Stufe.
4. Ist das Tastverhältnis 0, ruft er `hal_motor_off()` auf, und zwar in jedem Durchlauf erneut.
5. Sonst ruft er `hal_motor_run()` auf, aber nur wenn sich die Stufe geändert hat.
6. Er schreibt die umgesetzte Stufe und die Generation von `safety_state` nach `motor_status`. Konnte der
   Motor nicht abgeschaltet werden, meldet er „unbekannt“.

| Stufe | Tastverhältnis (vorläufig) |
|---|---|
| LOW | 40 % |
| MEDIUM | 65 % |
| HIGH | 90 % |
| alles andere | 0 %, Motor AUS |

## Schaltreihenfolge am TB6612FNG

| Vorgang | Reihenfolge |
|---|---|
| Abschalten | STBY = LOW, dann PWM = 0, dann AIN1 = AIN2 = LOW |
| Einschalten | AIN1 = HIGH, AIN2 = LOW, PWM setzen, zuletzt STBY = HIGH |

STBY ist das „Enable“ aus dem Designdokument. Es wird beim Abschalten zuerst und beim Einschalten zuletzt
geschaltet, damit der Treiber nie mit einem halb eingestellten Zustand aktiv ist.

## Die beiden Hardware-Varianten

| Datei | Verwendung | Auswahl |
|---|---|---|
| `hal_pigpio.c` | Raspberry Pi: Hardware-PWM mit 20 kHz auf GPIO 18, Echo-Messung über Flanken-Callbacks | CMake, wenn pigpio gefunden wird |
| `hal_sim.c` | PC: Abstand wird vorgegeben, Motorbefehle erscheinen auf der Konsole | sonst, mit Warnung beim Konfigurieren |

Die Safety-Logik, die Sensorprüfungen und die Motorsteuerung sind in beiden Varianten derselbe Code.

## Wichtige Entscheidungen

- **Überwachung der Safety-Logik:** Aktualisiert die Safety-Logik ihren Zustand 150 ms lang nicht, schaltet
  die Motorsteuerung von sich aus ab. Das steht nicht im Designdokument.
- **Fehler beim Abschalten werden gezählt.** `hal_motor_off()` meldet, wenn ein Ausgang nicht gesetzt werden
  konnte. Die Motorsteuerung versucht es im nächsten Durchlauf erneut, das Hauptprogramm gibt den Fehler aus.
  Solange die Rückmeldung „unbekannt“ lautet, wird der Watchdog nicht bedient und setzt den Pi nach 1 s zurück.
- **Rückmeldung statt Annahme.** ACK und Watchdog stützen sich auf `motor_status`, also auf das, was die
  Motorsteuerung tatsächlich ausgeführt hat, und nicht auf die Entscheidung der Safety-Logik.
- **`hal_motor_emergency_off()`** setzt nur STBY = LOW. Die Funktion wird im Handler für Programmfehler
  (z. B. SIGSEGV) aufgerufen, weil dort nur ein einfacher Registerzugriff vertretbar ist.
- **Eigene Signal-Handler:** pigpio installiert normalerweise eigene Handler. Das wird mit
  `PI_CFG_NOSIGHANDLER` abgeschaltet, damit SIGINT und SIGTERM zum kontrollierten Beenden führen.

## Stellen, an denen man Fehler leicht übersieht

- `hal_pigpio.c` wurde bisher nur gegen einen nachgebauten Header auf Syntax geprüft. Gebaut und getestet
  werden muss die Datei am Raspberry Pi.
- Motor AUS heißt nicht Stillstand. Der Rotor läuft nach (H4). Die Nachlaufzeit muss am Aufbau gemessen
  werden und bestimmt die Sicherheitsabstände (SAF-10). Dafür gibt es in `pi2_app_test` den Befehl
  `run <1|2|3> <s>`: Der Motor läuft für die gewählte Dauer mit der Stufe und wird dann abgeschaltet, der
  Zeitpunkt steht im Test-Log.
- Die Tastverhältnisse und die Sicherheitsabstände in `pi2_config.h` sind vorläufig.
- Hardware-PWM läuft unabhängig vom Prozess weiter. Endet der Prozess, ohne die Ausgänge zurückzusetzen,
  bliebe der Motor an. Dagegen wirkt nur der Hardware-Watchdog (siehe unten), und der ist mit
  `--no-watchdog` abgeschaltet.
- Der Watchdog wurde am Raspberry Pi noch nicht getestet. Am Aufbau ist zu prüfen, dass der Pi nach
  `kill -9` innerhalb von etwa einer Sekunde neu startet und der Motor stehen bleibt.
- Der Auftrag `pi2_agent_prompt.md` verlangt libgpiod und PWM über sysfs statt pigpio. Betroffen wäre nur
  eine neue Datei neben `hal_pigpio.c`.

## Hardware-Watchdog (`watchdog.c`)

Der Watchdog ist eine Ergänzung zum Designdokument. Er verwendet `/dev/watchdog` (Treiber `bcm2835_wdt`)
mit einem Timeout von 1 Sekunde und ist unabhängig von pigpio.

| Schritt | Verhalten |
|---|---|
| Start | `watchdog_open()` direkt vor dem Start der Threads. Fehlt das Gerät, fehlen Root-Rechte oder belegt systemd den Watchdog, bricht das Programm mit Motor AUS ab. |
| Betrieb | Die Safety-Logik bedient ihn in jedem Zyklus, wenn die Motorsteuerung aktuell ist. |
| Kontrolliertes Ende | `watchdog_close()` schreibt das Zeichen `V` („Magic Close“) und schließt. Der Watchdog löst danach nicht mehr aus. |
| Ende mit Fehler beim Abschalten | Der Watchdog bleibt offen und setzt den Pi zurück. |
| Simulation | Der Watchdog wird nicht geöffnet. |

Voraussetzung am Pi: `/dev/watchdog` muss vorhanden sein (sonst `dtparam=watchdog=on` in
`/boot/firmware/config.txt`), und systemd darf ihn nicht selbst verwenden (`RuntimeWatchdogSec`).
