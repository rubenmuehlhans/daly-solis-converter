// Firmware-Update ueber das espota-Protokoll (ArduinoOTA-kompatibel).
//
// Damit bleibt der bisherige Weg unveraendert:
//     pio run -t upload            (upload_protocol = espota)
//     python espota.py -i <ip> -f firmware.bin
//
// Ablauf: espota schickt per UDP an Port 3232 "<cmd> <port> <groesse> <md5>",
// das Geraet antwortet "OK" und holt sich die Datei per TCP beim Absender ab.
// Kein Passwort — wie bisher. ⚠️ Das heisst: wer im WLAN ist, kann flashen.
#pragma once
#include <stdint.h>

void ota_start();
bool ota_active();      // laeuft gerade ein Update? (UI zeigt dann Vollbild)
int  ota_percent();
