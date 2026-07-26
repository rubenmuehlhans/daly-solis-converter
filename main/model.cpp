#include "model.h"
#include "app_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <string.h>

static Model             s_model;
static SemaphoreHandle_t s_mtx;
static QueueHandle_t     s_cmds;

void model_init()
{
    memset(&s_model, 0, sizeof s_model);
    s_model.bms.fresh = Fresh::offline;
    s_model.lim.charge_mv        = CHARGE_VOLTAGE_MV;
    s_model.lim.charge_max_ma    = CHARGE_CURRENT_MAX_MA;
    s_model.lim.discharge_max_ma = DISCHARGE_CURRENT_MAX_MA;
    s_model.diag.beep            = false;   // wie bisher: startet stumm
    s_mtx = xSemaphoreCreateMutex();
}

Model *model_lock()
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    return &s_model;
}

void model_unlock() { xSemaphoreGive(s_mtx); }

void model_read(Model *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_model;
    xSemaphoreGive(s_mtx);
}

void model_notice(const char *text)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(s_model.diag.notice, text, sizeof s_model.diag.notice);
    s_model.diag.notice_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_mtx);
}

void cmd_init() { s_cmds = xQueueCreate(8, sizeof(Cmd)); }

bool cmd_post(CmdType t, int32_t arg)
{
    Cmd c{t, arg};
    return xQueueSend(s_cmds, &c, 0) == pdTRUE;
}

bool cmd_take(Cmd *out, uint32_t wait_ms)
{
    return xQueueReceive(s_cmds, out, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

// ---- Bewertung -----------------------------------------------------------
// Die Schwellen sind bewusst NICHT die Regelschwellen aus bms/control.cpp:
// dort geht es um Eingreifen, hier um Vorwarnung fuer das Auge. Ein Wert soll
// rot sein, BEVOR die Regelung abriegelt.

Zone zone_soc(uint16_t soc_pm)
{
    if (soc_pm <= 100) return Zone::critical;   // <= 10 %
    if (soc_pm <= 200) return Zone::low;        // <= 20 %
    return Zone::ok;
}

Zone zone_cell_max(uint16_t mv)
{
    if (!mv) return Zone::none;
    if (mv >= 3500) return Zone::critical;
    if (mv >= CELL_MAX_ALLOWED_MV) return Zone::low;
    return Zone::none;
}

Zone zone_cell_min(uint16_t mv)
{
    if (!mv) return Zone::none;
    if (mv <= CELL_MIN_ALLOWED_MV) return Zone::critical;
    if (mv <= 2900) return Zone::low;
    return Zone::none;
}

Zone zone_temp(int16_t c)
{
    if (c <= 0 || c >= 55) return Zone::critical;   // LiFePO4: Laden unter 0 °C
    if (c <= 5 || c >= 45) return Zone::low;
    return Zone::none;
}

const char *flow_text(Flow f)
{
    switch (f) {
        case Flow::charge:    return "Laden";
        case Flow::discharge: return "Entladen";
        default:              return "Ruhe";
    }
}
