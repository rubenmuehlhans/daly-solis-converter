#!/usr/bin/env python3
"""grab_screens.py — Screenshots vom Gerät über die serielle Konsole holen.

Gegenstück zu main/selftest.cpp (Build mit SOLIS_SELFTEST=1): das Gerät liest
seinen eigenen Framebuffer zurück und schiebt ihn base64-kodiert raus. Damit ist
die Optik am Entwicklungsrechner prüfbar, statt sie abfotografieren zu müssen.

Zweistufig — erst den Rohstrom mitschneiden, dann parsen. Live-Parsen war
fehleranfällig (ein Aussetzer im Stream kostete den ganzen 90-s-Lauf); mit dem
Mitschnitt lässt sich beliebig oft nachverarbeiten, ohne das Gerät erneut zu
bemühen.

  ~/panelvenv/bin/python3 tools/grab_screens.py              # mitschneiden + parsen
  ~/panelvenv/bin/python3 tools/grab_screens.py --file /tmp/solis_raw.txt   # nur parsen

Die Bilder landen in tools/shots_device/ — getrennt von tools/shots/, das die
am Rechner gerenderte Layout-Vorschau (preview_ui.py) enthält.
"""
import argparse
import base64
import re
import sys
import time
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
DEFAULT_RAW = Path("/tmp/solis_raw.txt")


def rgb565_to_rgb(rows, w, h):
    """RGB565 → RGB888, Bytes im Strom BIG-endian.

    ⚠️ Nicht aus `@@PROBE` allein ableiten! Die Probe meldet
    `wrote=F800 read=00F8`, also einen byte-vertauschten Readback — daraus
    „der Host muss little-endian lesen" zu schließen ist FALSCH und war genau
    der Fehlschluss beim ersten Versuch (Screenshots kamen braun statt
    dunkelblau). Es sind ZWEI Vertauschungen, die sich aufheben:
      readRect liefert 0x00F8  →  im ESP-RAM (little-endian) Bytes [F8,00]
      → im Strom [F8,00]       →  (b0<<8)|b1 = 0xF800 = das echte Rot.
    Belastbar ist nur der End-to-End-Check unten (check_sanity).
    """
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = rows[y]
        for x in range(w):
            v = (row[x * 2] << 8) | row[x * 2 + 1]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            px[x, y] = ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)
    return img


def check_sanity(img, name):
    """Ecke gegen die erwartete Hintergrundfarbe prüfen.

    Der Tag-Hintergrund ist ds::background = rgb(7,17,22) (dunkles Petrol),
    die Nachtseite ist Schwarz. Beides ist DUNKEL — ein brauner/heller Wert
    bedeutet vertauschte Kanäle. Fängt genau den Fehlschluss oben ab.
    ⚠️ Der OTA-Screen hat oben links denselben dunklen Grund, taugt also auch
    als Prüfpunkt; ein heller Screen gäbe hier einen Fehlalarm.
    """
    r, g, b = img.getpixel((2, 2))
    if max(r, g, b) > 60 or r > b + 12:
        print(f"  ⚠️ {name}: Ecke ist rgb({r},{g},{b}) — erwartet dunkel/bläulich. "
              f"Byte-Order oder Kanäle prüfen!", file=sys.stderr)
        return False
    return True


def record(port, baud, seconds, raw_path):
    import serial
    s = serial.Serial(port, baud, timeout=1)
    # ⚠️ Reihenfolge zählt: DTR steuert GPIO0, RTS steuert EN. Wird RTS
    # gepulst, während GPIO0 noch nicht sicher HIGH ist, bootet der ESP32 in den
    # DOWNLOAD-Modus („waiting for download") statt in die App — genau das ist
    # 2026-07-25 passiert und sah aus wie ein Absturz mitten im Dump.
    # Deshalb: erst beide Leitungen definiert setzen, kurz warten, dann EN pulsen.
    s.setDTR(False)          # GPIO0 = HIGH → normaler Boot
    s.setRTS(False)
    time.sleep(0.05)
    s.setRTS(True)           # EN = LOW  (Reset)
    time.sleep(0.15)
    s.setDTR(False)          # GPIO0 sicherheitshalber nochmal HIGH
    s.setRTS(False)          # EN = HIGH (Boot)
    time.sleep(0.05)
    s.reset_input_buffer()
    print(f"schneide {seconds} s mit …")
    with open(raw_path, "wb") as f:
        t0 = time.time()
        while time.time() - t0 < seconds:
            d = s.read(4096)
            if d:
                f.write(d)
    s.close()
    print(f"Mitschnitt: {raw_path} ({raw_path.stat().st_size} B)")


def parse(raw_path, out_dir):
    data = raw_path.read_bytes()
    out_dir.mkdir(parents=True, exist_ok=True)

    name = None
    w = h = 0
    rows = {}          # y → Zeilenbytes; Lücken werden am Ende gefüllt
    saved = []
    for raw in data.split(b"\n"):
        line = re.sub(rb"\x1b\[[0-9;]*m", b"", raw).strip()      # ANSI-Codes weg
        if line.startswith(b"@@PROBE"):
            print(line.decode())                                  # Byte-Order-Check
        elif line.startswith(b"@@SHOT "):
            parts = line.decode().split()
            name, w, h = parts[1], int(parts[2]), int(parts[3])
            rows = {}
        elif line.startswith(b"@@L") and name:
            # Format: @@L<yyy> <base64>. Die Nummer trägt den Verlust: eine
            # gekürzte Zeile kostet nur sich selbst, nicht den Screenshot.
            m = re.match(rb"@@L(\d{3}) ([A-Za-z0-9+/=]+)", line)
            if m and len(m.group(2)) == (w * 2 + 2) // 3 * 4:
                rows[int(m.group(1))] = base64.b64decode(m.group(2))
        elif line.startswith(b"@@END") and name:
            missing = [y for y in range(h) if y not in rows]
            if len(missing) > h // 20:      # >5 % kaputt → nicht brauchbar
                print(f"UNVOLLSTÄNDIG {name}: {len(missing)}/{h} Zeilen fehlen",
                      file=sys.stderr)
            else:
                # Lücken mit der Zeile darüber füllen — bei <5 % optisch
                # unauffällig und allemal besser als kein Screenshot.
                blank = bytes(w * 2)
                ordered = []
                for y in range(h):
                    ordered.append(rows.get(y, ordered[-1] if ordered else blank))
                p = out_dir / f"{name}.png"
                img = rgb565_to_rgb(ordered, w, h)
                img.save(p)
                check_sanity(img, name)
                saved.append(p.name)
                note = f"  ({len(missing)} Zeile(n) interpoliert)" if missing else ""
                print(f"gespeichert: {p}{note}")
            name = None
    print(f"\n{len(saved)} Screenshot(s): {', '.join(saved) or '—'}")
    return len(saved)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True,
                    help="z. B. /dev/cu.usbserial-XXXX oder COM5")
    ap.add_argument("--baud", type=int, default=115200)   # s. sdkconfig.defaults
    ap.add_argument("--out", default=str(HERE / "shots_device"))
    ap.add_argument("--raw", default=str(DEFAULT_RAW))
    ap.add_argument("--seconds", type=float, default=190.0)  # 8 Screens ≈ 150 s
    ap.add_argument("--file", help="nur parsen: vorhandenen Mitschnitt verwenden")
    args = ap.parse_args()

    raw_path = Path(args.file) if args.file else Path(args.raw)
    if not args.file:
        record(args.port, args.baud, args.seconds, raw_path)
    sys.exit(0 if parse(raw_path, Path(args.out)) else 1)


if __name__ == "__main__":
    main()
