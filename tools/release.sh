#!/usr/bin/env bash
#
# release.sh — eine Version bauen, in den Web-Flasher eintragen und veroeffentlichen.
#
# Der Web-Flasher zeigt genau die Versionen an, die in docs/versions.json stehen,
# und laedt je Version das Manifest aus docs/manifests/. Wer von Hand released,
# vergisst eines von beidem — deshalb macht es dieses Skript in einem Rutsch:
#
#   1. Version aus version.txt uebernehmen (oder als Argument)
#   2. bauen
#   3. Bootloader + Partitionstabelle + otadata + App zu EINEM Abbild zusammenfuehren
#   4. Manifest schreiben, versions.json fortschreiben
#   5. committen, taggen, pushen
#   6. GitHub-Release anlegen und die Abbilder anhaengen
#
# Nutzung:
#   source ~/esp/esp-idf/export.sh
#   tools/release.sh                 # Version aus version.txt
#   tools/release.sh 2.1.0           # Version setzen (schreibt auch version.txt)
#   NOTES="Was neu ist" tools/release.sh
#
set -euo pipefail

cd "$(dirname "$0")/.."

command -v idf.py   >/dev/null || { echo "idf.py fehlt — erst 'source ~/esp/esp-idf/export.sh'"; exit 1; }
command -v gh       >/dev/null || { echo "gh fehlt (GitHub CLI)"; exit 1; }
command -v esptool.py >/dev/null || { echo "esptool.py fehlt"; exit 1; }

VER="${1:-$(cat version.txt)}"
echo "$VER" > version.txt
NOTES="${NOTES:-}"
BIN="docs/firmware/solism5-v$VER.bin"
MANIFEST="docs/manifests/v$VER.json"

if git rev-parse "v$VER" >/dev/null 2>&1; then
  echo "Tag v$VER gibt es schon — Version in version.txt erhoehen."; exit 1
fi

echo "==> bauen ($VER)"
idf.py build >/dev/null

echo "==> Abbild zusammenfuehren"
mkdir -p docs/firmware docs/manifests
# ⚠️ Offsets muessen zu partitions.csv passen. Das Ergebnis wird ab 0x0
# geschrieben; die ersten 0x1000 Byte sind Fuellung, dort liegt beim ESP32 nichts.
esptool.py --chip esp32 merge_bin -o "$BIN" \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xe000  build/ota_data_initial.bin \
  0x20000 build/solism5.bin >/dev/null

cat > "$MANIFEST" <<EOF
{
  "name": "SolisM5 — DALY-BMS zu SOLIS",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "../firmware/solism5-v$VER.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

echo "==> versions.json fortschreiben"
VER="$VER" NOTES="$NOTES" DATE="$(date +%F)" python3 - <<'PY'
import json, os, pathlib
ver, notes, date = os.environ["VER"], os.environ["NOTES"], os.environ["DATE"]
p = pathlib.Path("docs/versions.json")
d = json.loads(p.read_text(encoding="utf-8")) if p.exists() else {"versions": []}
entry = {"version": ver, "date": date,
         "manifest": f"manifests/v{ver}.json", "notes": notes}
# Vorhandenen Eintrag ersetzen, sonst vorne einfuegen: neueste zuerst.
d["versions"] = [entry] + [v for v in d.get("versions", []) if v["version"] != ver]
d["latest"] = ver
p.write_text(json.dumps(d, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print(f"   {len(d['versions'])} Version(en), neueste: {ver}")
PY

echo "==> committen und taggen"
git add version.txt "$BIN" "$MANIFEST" docs/versions.json
git commit -q -m "Release $VER" || echo "   (nichts zu committen)"
git tag -a "v$VER" -m "SolisM5 $VER"
git push -q origin main
git push -q origin "v$VER"

echo "==> GitHub-Release"
# ⚠️ Bei `gh release create datei#label` setzt das # nur die BESCHRIFTUNG, nicht den
# Dateinamen. Ohne umbenannte Kopie hiesse die Anwendung in jedem Release gleich
# ("solism5.bin") und man saehe der heruntergeladenen Datei die Version nicht an.
APP="$(mktemp -d)/solism5-app-v$VER.bin"
cp build/solism5.bin "$APP"
gh release create "v$VER" \
  --title "SolisM5 $VER" \
  --notes "${NOTES:-Firmware $VER fuer den M5Stack Core Basic.}

**Im Browser flashen:** https://rubenmuehlhans.github.io/daly-solis-converter/

\`solism5-v$VER.bin\` ist das vollstaendige Abbild (Bootloader, Partitionstabelle,
Anwendung) und wird ab Offset 0 geschrieben:

    esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX write_flash 0x0 solism5-v$VER.bin

\`solism5-app-v$VER.bin\` enthaelt nur die Anwendung — das ist die Datei fuer ein
Update per espota ueber WLAN." \
  "$BIN#Vollstaendiges Abbild (ab 0x0 flashen)" \
  "$APP#Nur die Anwendung (fuer espota ueber WLAN)"

echo
echo "fertig: v$VER"
echo "   Flasher:  https://rubenmuehlhans.github.io/daly-solis-converter/"
echo "   Release:  https://github.com/rubenmuehlhans/daly-solis-converter/releases/tag/v$VER"
