// WLAN-Station mit selbsttaetigem Wiederverbinden.
//
// Bewusst nicht blockierend: der Adapter muss den SOLIS auch dann bedienen,
// wenn kein Netz da ist. Das Original hat in setup() auf die Verbindung
// gewartet — ein Router-Neustart hat den Konverter damit stillgelegt.
#pragma once

void wifi_start();     // kehrt sofort zurueck; Zustand steht in Model::net
