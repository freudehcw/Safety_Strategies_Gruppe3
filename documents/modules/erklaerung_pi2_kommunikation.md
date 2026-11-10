# Pi 2 – Kommunikation (`bt_server.c`, `src/common/protocol.c`)

Erstellt mit Unterstützung eines Sprachmodells (Claude Code, Claude Fable 5.2), geprüft von: <Name>

## Aufgabe

Der Kommunikations-Thread nimmt genau eine Verbindung von Pi 1 an, prüft eingehende Nachrichten, schreibt
gültige Befehle nach `comm_data` und sendet alle 100 ms den Status (SAF-05). Er entscheidet nichts über den
Motor. Ob die Verbindung noch aktuell ist, bewertet die Safety-Logik über den Zeitstempel.

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

1. **Warten:** `poll()` auf dem lauschenden Socket, höchstens 50 ms.
2. **Verbindungsaufbau:** neue Session-ID aus `getrandom()`, Sequenznummern auf 0, sofort ein STATUS.
3. **Empfang:** Bytes sammeln. Sobald drei Bytes da sind, prüft `proto_peek()` Typ und Länge. Bei unbekanntem
   Typ oder falscher Länge wird die Verbindung abgebaut, weil der Datenstrom nicht mehr synchron ist.
4. **Prüfung eines vollständigen Frames** in dieser Reihenfolge: Tag, Session-ID, Sequenznummer größer als
   die zuletzt akzeptierte, Wertebereich. Schlägt eine Prüfung fehl, wird die Nachricht verworfen und gezählt.
5. **Gültiger HEARTBEAT:** Stufe und Empfangszeit nach `comm_data`. **Gültiges STOP:** Stop-Flag setzen.
6. **STATUS** alle 100 ms: Zustand, Fehlercode, aktive Stufe, d1, d2 (0xFFFF bei ungültigem Messwert).
7. **SHUTDOWN:** Der Thread wartet, bis die Motorsteuerung in `motor_status` „Motor AUS“ für eine Generation
   ab SHUTDOWN zurückgemeldet hat. Dann sendet er einen letzten STATUS und das ACK (SAF-09).

## Wichtige Entscheidungen

- **RFCOMM oder TCP:** Im Betrieb RFCOMM auf Kanal 1. Mit `--tcp PORT` lauscht Pi 2 auf `127.0.0.1`, damit
  sich das Programm ohne Bluetooth mit `tools/pi1_sim.py` testen lässt.
- **Die Sequenznummer wird nach bestandener Tag- und Session-Prüfung übernommen,** auch wenn der Inhalt
  danach ungültig ist. Dieselbe Nachricht kann so nicht noch einmal eingespielt werden.
- **Sendefehler bauen die Verbindung ab.** Ein nur teilweise gesendeter Frame wäre für Pi 1 nicht mehr lesbar.
- **Kein ACK ohne bestätigtes Motor AUS.** Kommt die Rückmeldung nicht innerhalb von 500 ms, wird kein ACK
  gesendet. Pi 1 wartet höchstens 2 Sekunden und protokolliert das Ausbleiben.
- **Die Sequenznummer läuft nie über.** Ist 2³² − 1 erreicht, sendet Pi 2 nicht mehr und baut die Verbindung
  ab. Nach dem Neuaufbau gibt es eine neue Session-ID, und die Zählung beginnt bei 1 (Designdokument 5.2).
- **Kein Zustand „Verbindung OK“.** Der Thread schreibt nur den Zeitstempel. Bei Verbindungsverlust läuft
  der Timeout in der Safety-Logik ab (300 ms).

## Krypto-Platzhalter

`src/common/crypto_utils.c` verschlüsselt nicht. Ins Tag-Feld kommt eine CRC-32 über AAD, Nonce und
Nutzdaten. Das erkennt Übertragungsfehler, schützt aber nicht vor einem Angreifer. Die Schnittstelle
(`crypto_seal`, `crypto_open`) entspricht bereits AES-256-GCM. Laut Auftrag liefert Pi 1 die echte
Umsetzung mit OpenSSL in `common/`.

## Stellen, an denen man Fehler leicht übersieht

- Mit dem Platzhalter kann Pi 2 nicht mit einem Pi 1 sprechen, der schon AES-256-GCM verwendet.
- Der Überlauf der Sequenznummer ist nicht durch einen automatischen Test abgedeckt. Er wurde einmal mit
  einem hochgesetzten Startwert nachgestellt.
- Eine Nachricht mit ungültiger Stufe wird nur verworfen. Der Auftrag (Ergänzung 6) verlangt in diesem Fall
  sofort FEHLER / KOMMUNIKATION.
- Pi 1 sendet erst Heartbeats, wenn der Bediener eine Stufe gewählt hat. Bis dahin steht Pi 2 in
  FEHLER / KOMMUNIKATION. Das ist beabsichtigt.

## Tests

`tests/test_pi2.c` prüft Roundtrip, Byte-Reihenfolge, jedes verfälschte Byte, falsche Längen und unbekannte
Typen. Mit `tools/pi1_sim.py` lassen sich `pause`, `replay`, `corrupt`, `badlevel` und `stop` gegen ein
laufendes Pi 2 ausführen (TC-03, TC-06, TC-08, TC-09).
