# Pi 1 – Inbetriebnahme Schritt für Schritt

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Opus 5.5), geprüft von: <Name>

Diese Anleitung führt vom leeren Raspberry Pi 1 bis zum Test mit Pi 2. Jeder Schritt sagt, **was** du tust,
**warum**, und **woran du erkennst, dass es geklappt hat**. Arbeite die Schritte der Reihe nach ab. Wenn eine
Ausgabe anders aussieht, hör auf und schau unter „Fehlersuche“ nach.

Befehle mit `pi1$` werden auf Pi 1 ausgeführt, mit `pi2$` auf Pi 2.

## Überblick: Was läuft wo?

```
 Pi 1 (pi1_app)                                    Pi 2 (pi2_app)
 ┌─────────────────────────────┐   RFCOMM Kanal 1   ┌───────────────────────┐
 │ input   (Tasten 1/2/3/q)    │   AES-256-GCM      │ Sensoren, Safety-Logik│
 │ comm    (HEARTBEAT, STOP) ──┼───────────────────►│ Motor                 │
 │         (STATUS, ACK)     ◄─┼────────────────────┤                       │
 │ display (LCD 20x4 über I2C) │                    └───────────────────────┘
 │ logging (CSV in ./logs)     │
 └─────────────────────────────┘
```

Zum Testen ohne Pi 2 gibt es `mock_pi2`, einen simulierten Pi 2. Er spricht dasselbe Protokoll über TCP.

## Schritt 1: Pakete installieren

```bash
pi1$ sudo apt-get update
pi1$ sudo apt-get install -y git build-essential cmake pkg-config libssl-dev libbluetooth-dev cppcheck i2c-tools
```

**Warum:**
- `libssl-dev` liefert OpenSSL für AES-256-GCM.
- `libbluetooth-dev` liefert die BlueZ-Header für RFCOMM.
- `i2c-tools` enthält `i2cdetect`, damit wir das LCD finden.

**Geklappt, wenn:** `cmake --version` eine Version ausgibt und `ls /usr/include/openssl/evp.h` die Datei zeigt.

## Schritt 2: Code holen und bauen

```bash
pi1$ cd ~
pi1$ git clone https://github.com/freudehcw/Safety_Strategies_Gruppe3.git
pi1$ cd Safety_Strategies_Gruppe3
pi1$ git checkout pi1
pi1$ cmake -S . -B build
pi1$ cmake --build build -j4
pi1$ ctest --test-dir build --output-on-failure
```

**Warum:** CMake baut alle Programme: `pi1_app`, `mock_pi2`, die Pi-2-Programme und die Unit-Tests. Dass dabei
eine Warnung über die „SIMULIERTE Hardware“ von Pi 2 erscheint, ist auf Pi 1 normal.

**Geklappt, wenn:** Der Build ohne `error` endet und `ctest` meldet: `100% tests passed`.

Bei späteren Änderungen reicht im Projektordner:
```bash
pi1$ git pull
pi1$ cmake --build build -j4
```

## Schritt 3: Schlüssel erzeugen und verteilen

Beide Pis brauchen **denselben** 32-Byte-Schlüssel unter `/etc/ssm/psk.bin`. Der Schlüssel kommt **nie** ins Git.

**Auf Pi 1 erzeugen.** Pi 1 läuft als normaler Benutzer, deshalb muss die Datei diesem Benutzer gehören:
```bash
pi1$ sudo tools/gen_psk.sh /etc/ssm/psk.bin
pi1$ sudo chown $USER:$USER /etc/ssm/psk.bin
pi1$ ls -l /etc/ssm/psk.bin
```

**Geklappt, wenn:** `-rw------- 1 <dein Benutzer> ... 32 ... /etc/ssm/psk.bin` erscheint. Wichtig sind die
Rechte `rw-------` (0600) und die Größe 32.

**Auf Pi 2 kopieren.** Pi 2 läuft mit `sudo`, deshalb gehört die Datei dort `root`:
```bash
pi1$ scp /etc/ssm/psk.bin <benutzer>@<pi2-adresse>:/tmp/psk.bin
pi2$ sudo mkdir -p /etc/ssm
pi2$ sudo install -m 600 -o root -g root /tmp/psk.bin /etc/ssm/psk.bin
pi2$ rm /tmp/psk.bin
```

**Prüfen, ob beide gleich sind.** Verglichen wird nur der Hashwert; der Schlüssel selbst wird dabei nicht
angezeigt:
```bash
pi1$ sha256sum /etc/ssm/psk.bin
pi2$ sudo sha256sum /etc/ssm/psk.bin
```

**Geklappt, wenn:** Beide Hashwerte gleich sind.

**Warum so streng:** Das Programm startet nicht, wenn die Datei fehlt, nicht genau 32 Byte hat oder Gruppe bzw.
andere Benutzer Rechte darauf haben (Festlegung 8). Sonst könnte jeder Benutzer am Pi den Schlüssel lesen.

## Schritt 4: Erster Test auf Pi 1 allein (mit simuliertem Pi 2)

Damit prüfst du Software und Schlüssel, bevor Bluetooth und LCD dazukommen. Du brauchst zwei Terminals, z. B.
zwei SSH-Fenster.

```bash
# Terminal 1: simulierter Pi 2
pi1$ ./build/mock_pi2
# Terminal 2: Pi 1 über TCP, Anzeige auf der Konsole
pi1$ ./build/pi1_app --tcp --lcd-console
```

**Das solltest du in Terminal 2 sehen:**
1. `Log-Datei: ./logs/pi1_....csv`, danach die Tastenhilfe.
2. `VERBUNDEN warte auf STATUS`, danach `SESSION xxxxxxxx aufgebaut`.
3. In der LCD-Zeile steht `Stufe waehlen 1/2/3`. Terminal 1 zeigt noch **keinen** Heartbeat, denn ohne Stufe wird
   keiner gesendet.

Drück jetzt `2`. Terminal 1 zeigt `erster Heartbeat` und jede Sekunde eine Statistik mit `mittel=100.0 ms`. Die
LCD-Zeile zeigt `Status: BETRIEB | Soll:MEDIUM Ist:MEDIUM`.

Mit `q` beendest du Pi 1. Er meldet nacheinander `STOP gesendet`, `ACK empfangen` und `Pi 1 beendet.`

## Schritt 5: LCD anschließen und I2C einschalten

**Verkabelung laut Pinout im Designdokument (Abbildung „Pinbelegung von Raspberry Pi 1“):**

| LCD-Modul HW-061 | Raspberry Pi 1 |
|---|---|
| VCC | Pin 2 (5 V) |
| GND | Pin 6 (GND) |
| SDA | Pin 3 (GPIO 2) |
| SCL | Pin 5 (GPIO 3) |

Der I2C-Pegelwandler (3,3 V ↔ 5 V) ist laut Pinout optional, aber empfohlen. Schließ ihn so an, wie es in der
Abbildung steht. Verkabeln nur bei ausgeschaltetem Pi.

**I2C einschalten und das LCD suchen:**
```bash
pi1$ sudo raspi-config nonint do_i2c 0     # entspricht: raspi-config -> Interface Options -> I2C -> Yes
pi1$ sudo usermod -aG i2c $USER            # damit pi1_app ohne sudo auf /dev/i2c-1 darf
pi1$ sudo reboot
pi1$ ls -l /dev/i2c-1
pi1$ i2cdetect -y 1
```

**Geklappt, wenn:** `i2cdetect` in der Tabelle `27` anzeigt (manche Module melden `3f`). Steht dort `3f`, startest
du `pi1_app` später mit `--lcd-addr 0x3f`.

**Wenn das LCD leuchtet, aber nichts zu sehen ist:** Dreh am blauen Potentiometer hinten auf dem Modul. Das ist
der Kontrast.

> Der LCD-Treiber (`lcd_i2c.c`) wird geschrieben, sobald der Pi verbunden ist. Bis dahin meldet `pi1_app` ohne
> `--lcd-console` das Ereignis `LCD_AUSFALL` und läuft ohne LCD weiter.

## Schritt 6: Bluetooth koppeln

Pi 2 ist der Server auf RFCOMM-Kanal 1, Pi 1 der Client. Die beiden Geräte werden **einmal** gekoppelt.

**Adresse von Pi 2 herausfinden:**
```bash
pi2$ bluetoothctl show | head -1        # z. B. "Controller DC:A6:32:AA:BB:CC ..."
```

**Koppeln** (auf beiden Pis `bluetoothctl` starten; die Befehle stehen hinter dem Prompt `[bluetooth]#`):
```
pi2$ bluetoothctl
[bluetooth]# power on
[bluetooth]# agent on
[bluetooth]# default-agent
[bluetooth]# discoverable on
[bluetooth]# pairable on

pi1$ bluetoothctl
[bluetooth]# power on
[bluetooth]# agent on
[bluetooth]# default-agent
[bluetooth]# scan on                   # warten, bis die Adresse von Pi 2 erscheint
[bluetooth]# scan off
[bluetooth]# pair DC:A6:32:AA:BB:CC    # Nachfrage auf beiden Seiten mit "yes" bestätigen
[bluetooth]# trust DC:A6:32:AA:BB:CC
[bluetooth]# quit
```
Auf Pi 2 zusätzlich `trust <Adresse von Pi 1>` eingeben und danach `discoverable off`.

**Geklappt, wenn:** `bluetoothctl info DC:A6:32:AA:BB:CC` auf Pi 1 die Zeilen `Paired: yes` und `Trusted: yes`
zeigt.

**Warum:** Die Kopplung ist nur die Bluetooth-Ebene. Die eigentliche Absicherung macht das Programm selbst mit
AES-256-GCM (SEC-01), weil Bluetooth-Verschlüsselung allein laut Aufgabenstellung nicht reicht.

> Die RFCOMM-Anbindung in `transport.c` wird umgesetzt, sobald der Pi verbunden ist (Meilenstein 8).

## Schritt 7: Gesamttest mit Pi 2

```bash
pi2$ sudo ./build/pi2_app
pi1$ ./build/pi1_app --bt-addr DC:A6:32:AA:BB:CC
```

Erwartet wird dasselbe Verhalten wie in Schritt 4, nur über Bluetooth und am LCD.

## Tests mit dem simulierten Pi 2 (Designdokument Abschnitt 6.4)

Pi 1 läuft dabei wie in Schritt 4 mit `--tcp --lcd-console`, mit gewählter Stufe. Die Befehle tippst du in das
Terminal des simulierten Pi 2 und bestätigst sie mit Enter.

| Test | Befehl in mock_pi2 | Erwartung bei Pi 1 |
|---|---|---|
| Takt 100 ms ± 10 ms | (60 s laufen lassen), dann `stats` | `ausserhalb 100+-10 ms: 0` |
| TC-03 Verbindung getrennt | `pause`, später `resume` | spätestens 300 ms plus ein Anzeigezyklus: „Verbindung getrennt“, Ereignis `STATUS_TIMEOUT`; nach `resume`: `STATUS_WIEDER` |
| TC-07 Wiederaufbau | `drop` (oder mock beenden und neu starten) | `VERBINDUNG_VERLOREN`, nach etwa 1 s `SESSION ... aufgebaut` mit neuer ID |
| TC-08 Spoofing | `badtag`, `badpayload` | `VERWORFEN Tag ungueltig`, Anzeige unverändert |
| TC-09 Replay | `replay` | `VERWORFEN alte Sequenznummer` |
| TC-09 alte Session | `drop`, nach dem Wiederaufbau `oldsession` | `VERWORFEN fremde Session-ID` |
| Wertebereich | `badvalue` | `VERWORFEN Wertebereich` |
| Unbekannter Typ / falsche Länge | `badtype` bzw. `badlen` | `VERBINDUNG_VERLOREN Typ oder Laenge ungueltig`, danach Wiederaufbau |
| TC-06 Programmende | bei Pi 1 `q` drücken (oder `kill -INT`/`kill -TERM <pid>`) | `STOP gesendet`, `ACK empfangen`, Terminal wieder normal |
| Kein ACK | `noack`, dann bei Pi 1 `q` | nach 2 s `KEIN_ACK innerhalb 2 s`, Pi 1 beendet sich trotzdem |
| Log-Ausfall | Pi 1 mit `--log-dir /nicht/vorhanden` starten | Warnung, Programm läuft ohne Datei |

Das Log liegt in `./logs/pi1_<Startzeit>.csv`. Anschauen kannst du es mit `column -s, -t < logs/pi1_*.csv | less -S`.

## Fehlersuche

| Meldung | Ursache und Lösung |
|---|---|
| `Rechte der Schluesseldatei weiter als 0600` | `chmod 600 /etc/ssm/psk.bin` |
| `Schluesseldatei fehlt oder ist nicht lesbar` | Pfad prüfen; gehört die Datei dem richtigen Benutzer? (`ls -l`) |
| `VERWORFEN Tag ungueltig` bei jeder Nachricht | Die Schlüssel sind verschieden; `sha256sum` auf beiden Pis vergleichen |
| `VERBINDUNGSAUFBAU fehlgeschlagen` | Läuft Pi 2 bzw. mock_pi2? Stimmen Adresse und Port? Ist Bluetooth gekoppelt? |
| `LCD_AUSFALL Initialisierung` | `i2cdetect -y 1` prüfen, Verkabelung, `--lcd-addr`, Benutzer in Gruppe `i2c` |
| Terminal zeigt keine Eingaben mehr | Das Programm wurde hart abgebrochen: blind `reset` tippen und Enter drücken |
