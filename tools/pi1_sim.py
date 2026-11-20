#!/usr/bin/env python3
"""Einfache Gegenstelle fuer Pi 2: simuliert Pi 1 (Heartbeat, Stufe, Stop).

Nur ein Entwicklungs- und Testwerkzeug, kein Ersatz fuer die Pi-1-Software.
Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
geprueft von: <Name>
Das Frame-Format entspricht src/common/protocol.h. Die Nutzdaten werden wie in
src/common/crypto_utils.c mit AES-256-GCM geschuetzt (Bytes 0-10 als AAD,
96-Bit-Nonce, 128-Bit-Tag). Quelle: Python-Paket "cryptography"
(https://cryptography.io), Klasse cryptography.hazmat.primitives.ciphers.aead.AESGCM,
Verfahren nach NIST SP 800-38D. Nonce aus os.urandom.

Aufruf:
    tools/pi1_sim.py --tcp 5555 --key psk.bin            # Pi 2 laeuft mit --tcp 5555
    tools/pi1_sim.py --bt AA:BB:CC:DD:EE:FF              # Bluetooth-RFCOMM
    Ohne --key wird /etc/ssm/psk.bin verwendet.

Befehle (eine Zeile je Befehl, auch per Pipe):
    low | medium | high   Sollstufe waehlen
    pause | resume        Heartbeat anhalten / fortsetzen (TC-03)
    replay                letzten Heartbeat unveraendert erneut senden (TC-09)
    corrupt               Heartbeat mit verfaelschtem Tag senden (TC-08)
    tamper                Heartbeat mit veraenderten Nutzdaten senden (TC-08)
    badsession            Heartbeat mit fremder Session-ID senden (TC-09)
    badlevel              Heartbeat mit ungueltiger Stufe senden
    sleep <s>             warten (fuer Skripte)
    stop                  Stop/Shutdown senden und auf ACK warten
    quit                  ohne Stop beenden
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

MSG_HEARTBEAT, MSG_STOP, MSG_STATUS, MSG_ACK = 1, 2, 3, 4
PAYLOAD_LEN = {MSG_HEARTBEAT: 1, MSG_STOP: 0, MSG_STATUS: 7, MSG_ACK: 0}
LEVELS = {"low": 1, "medium": 2, "high": 3}
STATE = ["INIT", "BETRIEB", "OBJEKT_STOPP", "FEHLER", "SHUTDOWN"]
ERROR = ["-", "SENSOR", "KOMMUNIKATION", "DATENALTER"]
LEVEL = ["AUS", "LOW", "MEDIUM", "HIGH"]
HEARTBEAT_PERIOD_S = 0.1
RFCOMM_CHANNEL = 1
DEFAULT_KEY = "/etc/ssm/psk.bin"
AEAD = None  # wird in main() mit dem Schluessel angelegt


def load_key(path):
    with open(path, "rb") as f:
        key = f.read()
    if len(key) != 32:
        raise SystemExit(f"Fehler: {path} hat {len(key)} statt 32 Byte")
    return key


def encode(msg_type, session_id, seq, payload=b""):
    aad = struct.pack(">BHII", msg_type, len(payload), session_id, seq)
    nonce = os.urandom(12)
    # AESGCM.encrypt liefert Chiffretext || 16-Byte-Tag, wie im Frame vorgesehen
    return aad + nonce + AEAD.encrypt(nonce, payload, aad)


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Verbindung getrennt")
        data += chunk
    return data


def recv_frame(sock):
    head = recv_exact(sock, 3)
    msg_type, length = struct.unpack(">BH", head)
    if PAYLOAD_LEN.get(msg_type) != length:
        raise ConnectionError("unbekannter Typ oder falsche Laenge")
    rest = recv_exact(sock, 8 + 12 + length + 16)
    session_id, seq = struct.unpack(">II", rest[:8])
    nonce, sealed = rest[8:20], rest[20:]
    try:
        payload = AEAD.decrypt(nonce, sealed, head + rest[:8])
    except InvalidTag:
        return None
    return msg_type, session_id, seq, payload


class Pi1Sim:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.session_id = None
        self.seq = 0
        self.level = LEVELS["low"]
        self.paused = False
        self.running = True
        self.acked = threading.Event()
        self.last_frame = None
        self.last_status = None

    def send(self, msg_type, payload=b"", corrupt=False, tamper=False, foreign_session=False):
        with self.lock:
            if self.session_id is None:
                return
            self.seq += 1
            session = self.session_id ^ 0xFFFFFFFF if foreign_session else self.session_id
            frame = encode(msg_type, session, self.seq, payload)
            if corrupt:
                frame = frame[:-16] + bytes([frame[-16] ^ 0xFF]) + frame[-15:]
            elif tamper:
                # erstes Byte der verschluesselten Nutzdaten (Offset 23) kippen
                frame = frame[:23] + bytes([frame[23] ^ 0x01]) + frame[24:]
            elif msg_type == MSG_HEARTBEAT and not foreign_session:
                self.last_frame = frame
            self.sock.sendall(frame)

    def replay(self):
        with self.lock:
            if self.last_frame is not None:
                self.sock.sendall(self.last_frame)

    def receiver(self):
        try:
            while self.running:
                frame = recv_frame(self.sock)
                if frame is None:
                    print("[pi1] Nachricht mit ungueltigem Tag verworfen", flush=True)
                    continue
                msg_type, session_id, _seq, payload = frame
                if self.session_id is None and msg_type == MSG_STATUS:
                    with self.lock:
                        self.session_id = session_id
                    print(f"[pi1] Session-ID {session_id:08x}", flush=True)
                if session_id != self.session_id:
                    continue
                if msg_type == MSG_ACK:
                    print("[pi1] ACK empfangen", flush=True)
                    self.acked.set()
                elif msg_type == MSG_STATUS:
                    state, error, level, d1, d2 = struct.unpack(">BBBHH", payload)
                    key = (state, error, level)
                    if key != self.last_status:
                        self.last_status = key
                        print(f"[pi1] STATUS {STATE[state]:<12} Fehler={ERROR[error]:<13} "
                              f"Motor={LEVEL[level]:<6} d1={d1} mm d2={d2} mm", flush=True)
        except (ConnectionError, OSError) as exc:
            if self.running:
                print(f"[pi1] Verbindung getrennt: {exc}", flush=True)
        self.running = False

    def heartbeat(self):
        next_time = time.monotonic()
        try:
            while self.running:
                if not self.paused:
                    self.send(MSG_HEARTBEAT, bytes([self.level]))
                next_time += HEARTBEAT_PERIOD_S
                time.sleep(max(0.0, next_time - time.monotonic()))
        except OSError:
            self.running = False

    def command(self, line):
        parts = line.split()
        if not parts:
            return True
        cmd = parts[0].lower()
        if cmd in LEVELS:
            self.level = LEVELS[cmd]
        elif cmd == "pause":
            self.paused = True
        elif cmd == "resume":
            self.paused = False
        elif cmd == "replay":
            self.replay()
        elif cmd == "corrupt":
            self.send(MSG_HEARTBEAT, bytes([self.level]), corrupt=True)
        elif cmd == "tamper":
            self.send(MSG_HEARTBEAT, bytes([self.level]), tamper=True)
        elif cmd == "badsession":
            self.send(MSG_HEARTBEAT, bytes([self.level]), foreign_session=True)
        elif cmd == "badlevel":
            self.send(MSG_HEARTBEAT, bytes([7]))
        elif cmd == "sleep" and len(parts) == 2:
            time.sleep(float(parts[1]))
        elif cmd == "stop":
            self.paused = True
            self.send(MSG_STOP)
            if not self.acked.wait(2.0):
                print("[pi1] kein ACK erhalten", flush=True)
            return False
        elif cmd == "quit":
            return False
        else:
            print(__doc__)
        return True


def main():
    parser = argparse.ArgumentParser(description="Simulierter Pi 1 fuer Tests von Pi 2")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--tcp", type=int, metavar="PORT")
    group.add_argument("--bt", metavar="MAC")
    parser.add_argument("--key", default=DEFAULT_KEY, help="Schluesseldatei (32 Byte)")
    args = parser.parse_args()

    global AEAD
    AEAD = AESGCM(load_key(args.key))

    if args.tcp is not None:
        sock = socket.create_connection(("127.0.0.1", args.tcp))
    else:
        sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        sock.connect((args.bt, RFCOMM_CHANNEL))

    sim = Pi1Sim(sock)
    threading.Thread(target=sim.receiver, daemon=True).start()
    threading.Thread(target=sim.heartbeat, daemon=True).start()

    try:
        for line in sys.stdin:
            if not sim.running or not sim.command(line):
                break
    except KeyboardInterrupt:
        pass
    sim.running = False
    sock.close()


if __name__ == "__main__":
    main()
