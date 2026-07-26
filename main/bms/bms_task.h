// Der eine Task, dem UART und CAN gehoeren.
//
// Taktet im Sekundentakt: BMS lesen -> regeln -> Plausibilitaet -> an den SOLIS
// senden -> Modell aktualisieren. Kommandos (MOSFET, SOC, Stromgrenzen) werden
// am Anfang eines Takts abgearbeitet, nie mitten in einem Lesevorgang.
#pragma once

void bms_task_start();
