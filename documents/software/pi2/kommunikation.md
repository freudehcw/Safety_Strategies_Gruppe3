# Pi 2 – Kommunikation (`bt_server.c`)

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Fable 5.2 und Claude Opus 5.5), geprüft von: <Name>

## Aufgabe

Der Kommunikations-Thread nimmt genau eine Verbindung von Pi 1 an, prüft eingehende Nachrichten, schreibt
gültige Befehle nach `comm_data` und sendet alle 100 ms den Status (SAF-05). Er entscheidet nichts über den
Motor. Ob die Verbindung noch aktuell ist, bewertet die Safety-Logik über den Zeitstempel.

Die Protokollregeln selbst kommen aus `src/common/` und werden von Pi 1 genauso verwendet:

| Modul in `common/` | Aufgabe für Pi 2 |
|---|---|
| `transport` | lauschender Socket (RFCOMM Kanal 1 oder TCP auf `127.0.0.1`), Annehmen, Senden, Empfangen mit Timeout |
| `session` | neue Session-ID je Verbindung, Prüfung von Session-ID und Sequenznummer, nächste eigene Sequenznummer ohne Überlauf |
| `proto_stream` | Zerlegt den Datenstrom in Frames und erkennt unbekannten Typ oder falsche Länge |
| `protocol`, `crypto_utils` | Frame bilden und prüfen, AES-256-GCM |

## Frame-Aufbau

| Offset | Feld | Größe |
|---|---|---|
| 0 | Typ | 1 Byte |
| 1 | Länge n | 2 Byte |
| 3 | Session-ID | 4 Byte |
| 7 | Sequenznummer | 4 Byte |
| 11 | Nonce | 12 Byte |
| 23 | Nutzdaten | n Byte |
| 23 + n | Tag | 16 Byte |

Alle Felder werden einzeln in Big-Endian-Reihenfolge geschrieben, nie als `struct` kopiert. Die Bytes 0 bis
10 sind die zusätzlich authentifizierten Daten (AAD).

## Ablauf

1. **Warten:** `transport_accept()` wartet höchstens 50 ms auf eine Verbindung.
2. **Verbindungsaufbau:** `session_start_server()` erzeugt eine neue Session-ID, danach geht sofort ein STATUS hinaus.
3. **Empfang:** `transport_recv()` wartet höchstens bis zum nächsten STATUS-Zeitpunkt. Die Bytes gehen an
   `proto_stream`, das vollständige Frames liefert. Bei unbekanntem Typ oder falscher Länge wird die
   Verbindung abgebaut, weil der Datenstrom nicht mehr synchron ist.
4. **Prüfung eines vollständigen Frames** in dieser Reihenfolge: Tag (`proto_decode`), Session-ID und
   Sequenznummer größer als die zuletzt akzeptierte (`session_check_rx`), Wertebereich. Schlägt eine Prüfung fehl, wird die Nachricht verworfen und gezählt.
   Stimmen Tag, Session-ID und Sequenznummer, ist aber der Inhalt ungültig (unbekannte Stufe oder ein Typ,
   den nur Pi 2 sendet), gilt das als ungültiger Befehl: `comm_data` wird sofort entwertet.
5. **Gültiger HEARTBEAT:** Stufe und Empfangszeit nach `comm_data`. **Gültiges STOP:** Stop-Flag setzen.
6. **STATUS** alle 100 ms: Zustand, Fehlercode, aktive Stufe, d1, d2 (0xFFFF bei ungültigem Messwert).
7. **SHUTDOWN:** Der Thread wartet, bis die Motorsteuerung in `motor_status` „Motor AUS“ für eine Generation
   ab SHUTDOWN zurückgemeldet hat. Dann sendet er einen letzten STATUS und, falls Pi 1 STOP geschickt hat,
   das ACK (SAF-09). Endet Pi 2 aus eigenem Grund (Signal), gibt es kein ACK, weil keine Anfrage vorliegt.

## Wichtige Entscheidungen

- **RFCOMM oder TCP:** Im Betrieb RFCOMM auf Kanal 1. Mit `--tcp PORT` lauscht Pi 2 auf `127.0.0.1`, damit
  sich das Programm ohne Bluetooth mit `tools/pi1_sim.py` testen lässt.
- **Verwerfen oder Fehler?** Eine Nachricht mit falschem Tag, fremder Session-ID oder alter Sequenznummer
  stammt nicht nachweislich von Pi 1 und wird nur verworfen. Eine echte Nachricht mit ungültigem Inhalt führt
  im nächsten Zyklus der Safety-Logik zu FEHLER / KOMMUNIKATION (Klarstellung 6 im Auftrag, Abschnitt 2.6).
  Der nächste gültige Heartbeat hebt den Fehler wieder auf. Ein gesetztes Stop-Flag bleibt erhalten.
- **Die Sequenznummer wird nach bestandener Tag- und Session-Prüfung übernommen,** auch wenn der Inhalt
  danach ungültig ist. Dieselbe Nachricht kann so nicht noch einmal eingespielt werden.
- **Sendefehler bauen die Verbindung ab.** Ein nur teilweise gesendeter Frame wäre für Pi 1 nicht mehr lesbar.
- **Kein ACK ohne bestätigtes Motor AUS.** Kommt die Rückmeldung nicht innerhalb von 500 ms, wird kein ACK
  gesendet. Pi 1 wartet höchstens 2 Sekunden und protokolliert das Ausbleiben.
- **Die Sequenznummer läuft nie über.** Ist 2³² − 1 erreicht, sendet Pi 2 nicht mehr und baut die Verbindung
  ab. Nach dem Neuaufbau gibt es eine neue Session-ID, und die Zählung beginnt bei 1 (Designdokument 5.2).
- **Kein Zustand „Verbindung OK“.** Der Thread schreibt nur den Zeitstempel. Bei Verbindungsverlust läuft
  der Timeout in der Safety-Logik ab (300 ms).

## Kryptografie

`src/common/crypto_utils.c` (von Pi 1) schützt die Nutzdaten mit AES-256-GCM über OpenSSL. `main_pi2.c` lädt
beim Start den Schlüssel (`--key`, Standard `/etc/ssm/psk.bin`) und ruft `crypto_init()` auf. Fehlt der
Schlüssel oder ist er ungültig, startet Pi 2 nicht. Die OpenSSL-Kontexte werden nur einmal angelegt; deshalb
darf nur der Kommunikations-Thread `crypto_seal` und `crypto_open` aufrufen.

## Stellen, an denen man Fehler leicht übersieht

- Ein zweiter Thread, der Kryptofunktionen aufruft, würde die gemeinsamen OpenSSL-Kontexte stören.
- Der Überlauf der Sequenznummer ist nicht durch einen automatischen Test abgedeckt. Er wurde einmal mit
  einem hochgesetzten Startwert nachgestellt.
- Pi 1 sendet erst Heartbeats, wenn der Bediener eine Stufe gewählt hat. Bis dahin steht Pi 2 in
  FEHLER / KOMMUNIKATION. Das ist beabsichtigt.

## Tests

`tests/test_pi2.c` prüft Roundtrip, Byte-Reihenfolge, jedes verfälschte Byte, falsche Längen und unbekannte
Typen. Mit `tools/pi1_sim.py` lassen sich `pause`, `replay`, `corrupt`, `tamper`, `badsession`, `badlevel` und `stop` gegen ein
laufendes Pi 2 ausführen (TC-03, TC-06, TC-08, TC-09). Erwartet: `replay`, `corrupt`, `tamper` und `badsession` ändern den Zustand
nicht, `badlevel` führt kurz zu FEHLER / KOMMUNIKATION.
