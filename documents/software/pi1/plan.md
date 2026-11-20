# Pi 1 – Plan und Entscheidungen (Meilenstein 1)

Stand: 18.11.2026, Branch `pi1`. Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Opus 5.5),
geprüft von: <Name>

Dieses Dokument hält den freigegebenen Plan für die Software von Raspberry Pi 1 fest. Es enthält auch die
Antworten auf die offenen Fragen. Es dient als Übergabe an eine neue Sitzung (Umzug nach WSL). Die Grundlage ist
der schriftliche Auftrag `pi1_agent_prompt.md`. Er liegt nicht im Repository und muss der neuen Sitzung
zusätzlich gegeben werden. Verbindlich ist das Designdokument
`documents/designdoc-safety/Campus26_Gruppe3_Designdokument.pdf`.

## 1. Ausgangslage im Repository (Stand `main`, Commit 8cd4b32)

- Pi 2 ist bereits gemergt (`src/pi2_safety/`, `tests/test_pi2.c`, `tools/pi1_sim.py`).
- `src/common/` stammt von Pi 2 und enthält `config.h`, `types.h`, `protocol.*`, `crypto_utils.*` und
  `time_utils.*`. Es gibt kein `session` und kein `transport`. Die Sockets von Pi 2 liegen in `bt_server.c`.
- `src/common/crypto_utils.c` ist ein **Platzhalter**: Das Tag ist eine CRC-32, es wird nicht verschlüsselt.
  Die Signaturen von `crypto_random`, `crypto_seal` und `crypto_open` entsprechen bereits AES-256-GCM.
- `CMakeLists.txt` erwartet schon einen Block für Pi 1 (`src/pi1_host/main_pi1.c`, `ui_lcd.c`, `logging.c`,
  `bt_client.c`). Es linkt Pi 1 unnötig gegen pigpio. `common` linkt noch kein OpenSSL. Ein Target für
  cppcheck fehlt. Flags: `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror`, `-D_GNU_SOURCE`.
- CI: `.github/workflows/latex.yml` baut nur das PDF des Designdokuments.
- `core.autocrlf=true` unter Windows: Shell-Skripte haben im Arbeitsverzeichnis CRLF-Zeilenenden.
- Was von Pi 2 bereits festgelegt ist und übernommen wird:
  - Frame-Format nach `protocol.h`.
  - Die Sequenznummer wird nach bestandener Prüfung von Tag und Session übernommen, auch wenn der
    Wertebereich danach ungültig ist.
  - Vor dem ACK sendet Pi 2 einen letzten STATUS (SHUTDOWN).
  - Kommt keine Bestätigung von Motor AUS, sendet Pi 2 kein ACK.
  - TCP bindet nur auf `127.0.0.1`.

## 2. Umgebung

- Bauen und Testen unter **WSL2 Ubuntu 24.04** (gcc 13.3 ist vorhanden).
- Es fehlen noch: `cmake pkg-config libssl-dev libbluetooth-dev cppcheck`.
  `bootstrap.sh` ist ungeeignet (CRLF, installiert pigpio).
- Die Test-Schlüsseldatei liegt im Linux-Dateisystem (nicht unter `/mnt/c`), damit die Prüfung auf `0600`
  aussagekräftig ist.
- Der Raspberry Pi ist noch nicht verbunden. Der I2C-Teil des LCD und RFCOMM (Meilenstein 8) beginnen erst nach
  Freigabe.

## 3. Struktur

```
src/common/       bestehend, wird erweitert (bestehende Signaturen bleiben unverändert)
  crypto_utils.*  Platzhalter -> OpenSSL EVP AES-256-GCM
                  neu: crypto_init(key), crypto_load_key_file(path, key), crypto_cleanup()
  session.*       neu: Session-ID, Sequenznummern senden/empfangen, Ende bei UINT32_MAX
  transport.*     neu: TCP und RFCOMM; Client (Verbindungsaufbau mit Timeout), Server (listen/accept
                  für genau eine Verbindung), senden und empfangen mit poll-Timeout, schließen
  proto_stream.*  neu: Frames aus dem Datenstrom zusammensetzen (zuerst Typ und Länge prüfen)
src/pi1_host/     main_pi1.c, options.c, pi1_config.h, shared.c, bt_client.c (Kommunikations-Thread),
                  input.c, ui_lcd.c (Anzeige-Thread), lcd_i2c.c, lcd_console.c, logging.c
tools/mock_pi2/   mock_pi2.c (TCP, nutzt src/common/)
tools/gen_psk.sh  Schlüssel aus /dev/urandom
tests/            test_common.c, test_pi1.c (eigene kleine Assert-Makros)
documents/software/pi1/<modul>.md             (Ablage wie bei Pi 2)
```

`session` und `transport` sind rollenneutral (Client- und Server-Funktionen), damit Pi 2 sie nutzen kann. Ob
Pi 2 sie übernimmt, entscheidet der Kollege.

## 4. Threads und gemeinsame Daten (Pi 1)

Jeder Datensatz hat genau einen schreibenden Thread und einen eigenen `pthread_mutex_t`. Unter dem Mutex wird
nur kopiert, und jeder Thread hält immer nur einen Mutex. Leser arbeiten mit einer Kopie.

| Thread | Aufgabe | schreibt |
|---|---|---|
| main | Initialisierung, Signale (Worker-Threads blockieren Signale per `pthread_sigmask`), Ablauf beim Beenden, `join` | `app_ctrl` (Programmende angefordert) |
| input | `termios` im nicht-kanonischen Modus, `poll` auf stdin, Tasten 1/2/3/q | `operator_cmd` (Sollstufe, gewählt ja/nein, `quit`) |
| comm | Verbindungsaufbau, erste STATUS-Nachricht abwarten, Heartbeat mit `clock_nanosleep(TIMER_ABSTIME)`, `poll` mit 50 ms, Überwachung auf 300 ms, Reconnect jede Sekunde, STOP und Warten auf ACK (2 s) | `link_status` (Verbindung, letzter STATUS, Zeit des letzten gültigen STATUS, Ergebnis des STOP) |
| display | alle 100 ms eine Momentaufnahme lesen und nur geänderte Zeilen schreiben (ein Komplett-Update über I2C dauert rund 50 ms) | – |
| logging | alle Puffer leeren; jede Sekunde und bei Fehlerereignissen auf die Karte schreiben | – |

**Logging:** Jeder erzeugende Thread hat einen eigenen Ringpuffer mit eigenem Mutex, damit die Regel „ein
Schreiber“ gilt. Ist ein Puffer voll, werden Messwerte verworfen und gezählt; Ereignisse haben Vorrang.

**Ablauf beim Beenden:**
1. `q`, SIGINT oder SIGTERM: Der Signal-Handler setzt nur ein Flag.
2. main setzt `app_ctrl`.
3. comm sendet STOP und wartet höchstens 2 s auf ACK. Danach gibt es keine Heartbeats und keinen Reconnect.
4. comm endet, danach werden display und logging beendet (Log vollständig schreiben).
5. Das Terminal wird wiederhergestellt, alle Threads werden mit `join` beendet.

## 5. Meilensteine

1. Plan und offene Fragen – **erledigt** (dieses Dokument).
2. `src/common/` mit Unit-Tests. **Die Header werden zuerst committet**, damit der Kollege sie prüfen kann.
3. Transport mit TCP (Client und Server) und simulierter Pi 2.
4. Kommunikations-Thread.
5. Logging-Thread.
6. Anzeige: zuerst nur das Konsolen-Backend; I2C erst, wenn der Pi verbunden ist.
7. Bedienung und kontrolliertes Beenden.
8. RFCOMM – erst, wenn der Pi verbunden ist.
9. Abschluss: cppcheck/MISRA, Quellenangaben, README.

Nach jedem Meilenstein: bauen, testen, kurz berichten, committen.

## 6. Entscheidungen (Antworten auf die offenen Fragen)

**A – Wer besitzt `src/common/`:** Ich erweitere das bestehende `src/common/`; es gibt kein zweites
`common/`. Die bestehenden Signaturen bleiben unverändert. Alle Änderungen an `src/common/` erfolgen nur auf
Branch `pi1` und über einen Pull Request, dem der Kollege (Pi 2) zustimmen muss. `session` und `transport`
werden so bereitgestellt, dass Pi 2 sie nutzen kann; ob er sie übernimmt, entscheidet er.

**B – Pi 2 und echte Kryptografie:**
- Im selben Pull Request werden in `src/pi2_safety/main_pi2.c` und `tests/test_pi2.c` die Zeilen für
  `crypto_init` ergänzt. Sie stehen in einem **eigenen Commit** mit klarer Beschreibung, damit der Kollege sie
  prüfen kann.
- `tools/pi1_sim.py` wird **nicht** angefasst; das entscheidet der Kollege.
- Für den Kollegen wird eine kurze Notiz geschrieben: was sich ändert und warum.

**C – MISRA:**
- **Verbindlich** sind die neun Regeln aus `documents/vorgaben/MISRA_C_verpflichtende_Regeln.pdf`, ohne Ausnahme.
- Die Kategorie „Mandatory“ von MISRA C:2012 ist für Pi 1 ein **zusätzliches Ziel**. Abweichungen davon
  werden im Code begründet und im README gesammelt.

**D – Keine dynamische Speicherverwaltung und OpenSSL:**
- Je ein `EVP_CIPHER_CTX` zum Ver- und Entschlüsseln wird einmal in `crypto_init` angelegt. Pro Nachricht
  wird er nur mit neuer Nonce neu initialisiert.
- Die interne Speicherreservierung von OpenSSL wird als Abweichung dokumentiert und begründet.
- Ebenso wird begründet, warum nur der Kommunikations-Thread die Kryptografie nutzt: Die Kontexte sind
  nicht threadsicher, und so braucht es keinen weiteren Mutex.
- Das Tag prüft `EVP_DecryptFinal_ex`; es gibt keinen eigenen Vergleich.

**E – Programmende ohne Verbindung oder Session:** Das Ereignis „STOP nicht möglich“ wird protokolliert, und
Pi 1 beendet sich sofort ohne neuen Verbindungsversuch. Pi 2 schaltet über den Kommunikations-Timeout ab.

**F – Die erste STATUS-Nachricht bleibt aus:** Kommt nach dem Verbindungsaufbau innerhalb von 1 s keine
STATUS-Nachricht, wird die Verbindung abgebaut und im Sekundentakt neu aufgebaut.

**G – Hinweis zum Design (keine Änderung):** Die erste STATUS-Nachricht kann Pi 1 nur über das Tag prüfen,
nicht auf Aktualität. Ein Angreifer könnte direkt nach einem Neuaufbau eine alte erste STATUS-Nachricht
einspielen. Die Folge ist nur, dass Pi 2 die Heartbeats verwirft und in FEHLER geht, also in den sicheren
Zustand. Das wird in der Erklärungsdatei zur Kommunikation festgehalten.

**H – Kleinere Festlegungen:**
- Eine authentische Nachricht vom Typ HEARTBEAT oder STOP, die bei Pi 1 ankommt, wird verworfen und
  protokolliert.
- Bei „Verbindung getrennt“ (300 ms ohne STATUS) baut Pi 1 die Verbindung nicht ab und sendet weiter
  Heartbeats. Neu verbunden wird nur, wenn der Socket abbricht.
- Nach einem STOP gibt es keine Heartbeats und keinen Reconnect mehr.
- Die Schlüsseldatei gehört dem Benutzer, unter dem Pi 1 läuft; das steht im README.
- CI-Pipeline `.github/workflows/build.yml` (Installation von `libssl-dev`, `libbluetooth-dev` und
  `cppcheck`, bauen, CTest) – **erst nach Meilenstein 2**.

**Freigegebene Änderungen an bestehenden Dateien** (nur auf Branch `pi1`): `CMakeLists.txt`,
`src/common/crypto_utils.*`, `src/common/config.h`, `.gitignore`, `README.md` sowie die neue Datei
`.gitattributes` (`*.sh text eol=lf`). Dazu kommen nach B die Ergänzungen für `crypto_init` in `main_pi2.c` und
`tests/test_pi2.c`. Alle anderen bestehenden Dateien bleiben unverändert, sofern nichts anderes freigegeben
wird.

## 7. Nächste Schritte (neue Sitzung unter WSL)

1. Diese Datei und den Auftrag `pi1_agent_prompt.md` lesen.
2. Prüfen, ob `cmake`, `pkg-config`, `libssl-dev`, `libbluetooth-dev` und `cppcheck` (mit `misra.py`)
   installiert sind.
3. Meilenstein 2 beginnen: zuerst die Header `crypto_utils.h` (Erweiterung), `session.h`, `transport.h` und
   `proto_stream.h` committen, danach implementieren und testen.
