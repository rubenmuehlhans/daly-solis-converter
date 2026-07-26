// Home-Assistant-Anbindung ueber MQTT-Discovery.
//
// Ersetzt die Arduino-Bibliothek ArduinoHA und bildet deren Drahtformat
// ABSICHTLICH exakt nach — gleiche Topics, gleiche unique_ids. Dadurch findet
// Home Assistant nach dem Firmware-Wechsel dieselben Entities wieder, mit
// Historie, Dashboards und Automationen.
//
//   config : homeassistant/<komponente>/0010fa6e384a/<objekt>/config   retained
//   state  : aha/0010fa6e384a/<objekt>/stat_t                          retained
//   command: aha/0010fa6e384a/<objekt>/cmd_t
//   client-id = 0010fa6e384a, keepalive 15 s
//
// Zwei bewusste Abweichungen vom Original, beide dokumentiert in ha_mqtt.cpp:
//   1. Verfuegbarkeit (avty_t + Last Will) — ArduinoHA hatte hier nichts, damit
//      zeigte Home Assistant bei totem Adapter weiter den letzten SOC an.
//   2. Zahlen-Kommandos werden auch mit Nachkommastelle akzeptiert.
#pragma once
#include "../model.h"

void ha_mqtt_start();
