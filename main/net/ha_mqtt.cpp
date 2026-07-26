#include "ha_mqtt.h"
#include "../app_config.h"
#include <mqtt_client.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ha";

// ---- Konstanten des ArduinoHA-Drahtformats --------------------------------
// Die Kurznamen sind die von Home Assistant; ArduinoHA nutzt sie zugleich als
// Topic-Suffix ("stat_t" ist wirklich der Topic-Name, nicht nur der JSON-Key).
static constexpr const char *DISCOVERY_PREFIX = "homeassistant";
static constexpr const char *DATA_PREFIX      = "aha";
static char s_devid[13];      // "0010fa6e384a"
static char s_dev_json[192];  // eingebettetes "dev"-Objekt

enum class Kind : uint8_t { text, number, sw, setpoint };

struct Entity {
    const char *obj;       // objectId UND uniq_id — beides derselbe String
    const char *name;
    const char *icon;
    const char *dev_cla;   // nullptr = Key faellt weg
    const char *unit;      // nullptr = Key faellt weg
    Kind        kind;
    uint8_t     prec;      // Nachkommastellen (number/setpoint)
};

// ⚠️ Diese Strings sind die Identitaet in Home Assistant. Wer hier ein Zeichen
// aendert, legt eine NEUE Entity an und laesst die alte als "nicht verfuegbar"
// zurueck. Reihenfolge und Namen stammen aus src/main.cpp des Originals.
enum EntIdx : int {
    E_STATUS = 0, E_UPTIME, E_LASTERR, E_BATSTAT,
    E_ERRCNT, E_CHG_I, E_DIS_I, E_CHG_I_MAX, E_DIS_I_MAX,
    E_VOLT, E_CURR, E_SOC,
    E_CELLMAX_V, E_CELLMAX_NO, E_CELLMIN_V, E_CELLMIN_NO,
    E_CAPACITY, E_TEMP, E_CYCLE,
    E_CELL1,                       // .. E_CELL1 + 15
    E_RESTART = E_CELL1 + 16, E_MOS,
    E_SET_SOC, E_SET_CHG_MAX, E_SET_DIS_MAX,
    E_COUNT
};

static const Entity ENT[E_COUNT] = {
    {"Status",              "BMS Status",                          "mdi:information",   nullptr,      nullptr, Kind::text,   0},
    {"M5StackUptime",       "M5 Stack Uptime",                     "mdi:clock-start",   nullptr,      nullptr, Kind::text,   0},
    {"LastErrorMessage",    "Last Error Message",                  "mdi:alert-circle",  nullptr,      nullptr, Kind::text,   0},
    {"BatteryStatus",       "BMS Battery Status",                  "mdi:flash",         nullptr,      nullptr, Kind::text,   0},
    {"BMSReadErrorCount",   "BMS Read Error Count",                "mdi:counter",       nullptr,      nullptr, Kind::number, 0},
    {"ChargeCurrent",       "BMS currently Max Charge Current",     "mdi:flash",         "current",    "A",     Kind::number, 0},
    {"DischargeCurrent",    "BMS currently Max Discharge Current",  "mdi:flash",         "current",    "A",     Kind::number, 0},
    {"ChargeCurrentMax",    "BMS Max Charge Current",              "mdi:flash",         "current",    "A",     Kind::number, 0},
    {"DischargeCurrentMax", "BMS Max Discharge Current",           "mdi:flash",         "current",    "A",     Kind::number, 0},
    {"BatteryVoltage",      "BMS Battery Voltage",                 "mdi:flash",         "voltage",    "V",     Kind::number, 1},
    {"BatteryCurrent",      "BMS Battery Current",                 "mdi:flash",         "current",    "A",     Kind::number, 0},
    {"BatterySoc",          "BMS Battery SOC",                     "mdi:flash",         "battery",    "%",     Kind::number, 1},
    {"BatteryMaxVoltage",   "BMS Battery Max Voltage",             "mdi:flash",         "voltage",    "V",     Kind::number, 3},
    {"BatteryMaxVoltageCell", "BMS Battery Max Voltage Cell",      "mdi:flash",         nullptr,      nullptr, Kind::number, 0},
    {"BatteryMinVoltage",   "BMS Battery Min Voltage",             "mdi:flash",         "voltage",    "V",     Kind::number, 3},
    {"BatteryMinVoltageCell", "BMS Battery Min Voltage Cell",      "mdi:flash",         nullptr,      nullptr, Kind::number, 0},
    {"BatteryCapacity",     "BMS Battery Capacity",                "mdi:flash",         nullptr,      "Ah",    Kind::number, 0},
    {"BatteryTemperature",  "BMS Battery Temperature",             "mdi:flash",         "temperature", "\xC2\xB0" "C", Kind::number, 0},
    {"BatteryCycle",        "BMS Battery Cycle",                   "mdi:flash",         nullptr,      nullptr, Kind::number, 0},
    {"BatteryCellVoltage1",  "BMS Cell 1 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage2",  "BMS Cell 2 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage3",  "BMS Cell 3 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage4",  "BMS Cell 4 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage5",  "BMS Cell 5 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage6",  "BMS Cell 6 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage7",  "BMS Cell 7 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage8",  "BMS Cell 8 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage9",  "BMS Cell 9 Voltage",  "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage10", "BMS Cell 10 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage11", "BMS Cell 11 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage12", "BMS Cell 12 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage13", "BMS Cell 13 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage14", "BMS Cell 14 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage15", "BMS Cell 15 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"BatteryCellVoltage16", "BMS Cell 16 Voltage", "mdi:flash", "voltage", "V", Kind::number, 3},
    {"restart",             "Neustart",                            "mdi:restart",       nullptr, nullptr, Kind::sw,       0},
    {"mos",                 "MOSFet",                              "mdi:toggle-switch", nullptr, nullptr, Kind::sw,       0},
    {"setSOC",              "set SOC",                             "mdi:battery",       nullptr, nullptr, Kind::setpoint, 0},
    {"setChargecurrentMax", "set Max Charge Current",              "mdi:flash",         nullptr, nullptr, Kind::setpoint, 0},
    {"setDischargecurrentMax", "set Max Discharge Current",        "mdi:flash",         nullptr, nullptr, Kind::setpoint, 0},
};

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static volatile bool s_need_announce;
static uint64_t      s_sent_hash[E_COUNT];   // 0 = noch nie gesendet

// ---- Topics ---------------------------------------------------------------

static void topic_data(char *buf, size_t cap, const char *obj, const char *suffix)
{
    if (obj) snprintf(buf, cap, "%s/%s/%s/%s", DATA_PREFIX, s_devid, obj, suffix);
    else     snprintf(buf, cap, "%s/%s/%s",    DATA_PREFIX, s_devid, suffix);
}

static const char *component_of(Kind k)
{
    switch (k) {
        case Kind::sw:       return "switch";
        case Kind::setpoint: return "number";
        default:             return "sensor";
    }
}

// ---- Zahlformat -----------------------------------------------------------
// ArduinoHA rechnet intern mit base = wert * 10^precision und gibt IMMER
// genau precision Nachkommastellen aus — ausser bei exakt 0, da nur "0".
// Nachgebaut, weil Home Assistant sonst bei jedem Wertwechsel einen anderen
// String sieht als bisher (fuer Statistiken egal, fuer Vorlagen nicht).
static void fmt_scaled(char *out, size_t cap, int64_t base, uint8_t prec)
{
    if (base == 0) { snprintf(out, cap, "0"); return; }
    const char *sign = base < 0 ? "-" : "";
    const unsigned long long a = (unsigned long long)(base < 0 ? -base : base);
    // Feste Formate statt "%0*llu": mit variabler Breite kann der Compiler die
    // Ausgabelaenge nicht abschaetzen und -Wformat-truncation schlaegt zu.
    switch (prec) {
        case 1:  snprintf(out, cap, "%s%llu.%01llu", sign, a / 10,   a % 10);   break;
        case 2:  snprintf(out, cap, "%s%llu.%02llu", sign, a / 100,  a % 100);  break;
        case 3:  snprintf(out, cap, "%s%llu.%03llu", sign, a / 1000, a % 1000); break;
        default: snprintf(out, cap, "%lld", (long long)base);                   break;
    }
}

static void fmt_uptime(char *out, size_t cap, uint32_t s)
{
    const uint32_t d = s / 86400;
    const uint32_t h = (s % 86400) / 3600;
    const uint32_t m = (s % 3600) / 60;
    out[0] = '\0';
    size_t n = 0;
    if (d) n += snprintf(out + n, cap - n, "%ud ", (unsigned)d);
    if (h) n += snprintf(out + n, cap - n, "%uh ", (unsigned)h);
    snprintf(out + n, cap - n, "%um ", (unsigned)m);
}

// Liefert den zu veroeffentlichenden Payload. false = fuer diese Entity gibt es
// (noch) nichts zu sagen.
static bool ent_value(const Model &md, int i, char *out, size_t cap)
{
    const BmsData &b = md.bms;
    const uint8_t p = ENT[i].prec;

    if (i >= E_CELL1 && i < E_CELL1 + CELL_COUNT) {
        if (!b.cells_valid) return false;
        fmt_scaled(out, cap, b.cell_mv[i - E_CELL1], p);   // mV = V mit 3 Stellen
        return true;
    }

    switch (i) {
        case E_STATUS:
            snprintf(out, cap, "%s", b.fresh == Fresh::fresh ? "Normal" : "BMS Read Error");
            return true;
        case E_UPTIME:
            fmt_uptime(out, cap, md.diag.uptime_s);
            return true;
        case E_LASTERR:
            snprintf(out, cap, "%s", md.diag.last_error[0] ? md.diag.last_error : "no message");
            return true;
        case E_BATSTAT:
            snprintf(out, cap, "%s", b.flow == Flow::charge    ? "Charging"
                                   : b.flow == Flow::discharge ? "Discharging"
                                                               : "Floating");
            return true;
        case E_ERRCNT:      fmt_scaled(out, cap, md.diag.err_count, p); return true;
        case E_CHG_I:       fmt_scaled(out, cap, (md.lim.charge_ma + 500) / 1000, p); return true;
        case E_DIS_I:       fmt_scaled(out, cap, (md.lim.discharge_ma + 500) / 1000, p); return true;
        case E_CHG_I_MAX:   fmt_scaled(out, cap, (md.lim.charge_max_ma + 500) / 1000, p); return true;
        case E_DIS_I_MAX:   fmt_scaled(out, cap, (md.lim.discharge_max_ma + 500) / 1000, p); return true;
        case E_VOLT:        fmt_scaled(out, cap, b.pack_dv / 10, p); return true;   // 10 mV -> 0,1 V
        case E_CURR:        fmt_scaled(out, cap, b.pack_ci / 10, p); return true;   // 0,1 A -> A
        case E_SOC:         fmt_scaled(out, cap, b.soc_pm, p); return true;         // 0,1 %
        case E_CELLMAX_V:   fmt_scaled(out, cap, b.cell_max_mv, p); return true;
        case E_CELLMAX_NO:  fmt_scaled(out, cap, b.cell_max_no, p); return true;
        case E_CELLMIN_V:   fmt_scaled(out, cap, b.cell_min_mv, p); return true;
        case E_CELLMIN_NO:  fmt_scaled(out, cap, b.cell_min_no, p); return true;
        case E_CAPACITY:    fmt_scaled(out, cap, b.rest_mah / 1000, p); return true;
        case E_TEMP:        fmt_scaled(out, cap, b.temp_c, p); return true;
        case E_CYCLE:       fmt_scaled(out, cap, b.cycles, p); return true;
        case E_RESTART:     snprintf(out, cap, "OFF"); return true;
        case E_MOS:
            snprintf(out, cap, "%s", (b.charge_mos && b.discharge_mos) ? "ON" : "OFF");
            return true;
        // Die Sollwerte spiegeln zurueck, was wirklich gilt. ArduinoHA haette
        // hier bis zum ersten Setzen "None" gemeldet — die echte Grenze ist
        // nuetzlicher, und HA zeigt das Eingabefeld dann vorbelegt.
        case E_SET_CHG_MAX: fmt_scaled(out, cap, (md.lim.charge_max_ma + 500) / 1000, p); return true;
        case E_SET_DIS_MAX: fmt_scaled(out, cap, (md.lim.discharge_max_ma + 500) / 1000, p); return true;
        case E_SET_SOC:     return false;   // reiner Schreibwert (Kalibrierung)
        default:            return false;
    }
}

// ---- Discovery ------------------------------------------------------------

static void publish_config(int i)
{
    const Entity &e = ENT[i];
    char topic[128];
    snprintf(topic, sizeof topic, "%s/%s/%s/%s/config",
             DISCOVERY_PREFIX, component_of(e.kind), s_devid, e.obj);

    char avty[80], stat[80], cmd[80];
    topic_data(avty, sizeof avty, nullptr, "avty_t");
    topic_data(stat, sizeof stat, e.obj, "stat_t");
    topic_data(cmd,  sizeof cmd,  e.obj, "cmd_t");

    // Reihenfolge der Keys wie in ArduinoHA. Nicht gesetzte Properties fehlen
    // komplett (kein "dev_cla":null) — genau wie dort.
    // ⚠️ Nicht mit "n += snprintf(js + n, sizeof js - n, ...)" bauen: snprintf
    // liefert bei Abschneidung die GEWUENSCHTE Laenge, n liefe ueber den Puffer
    // hinaus und "sizeof js - n" liefe als size_t unter — der naechste Aufruf
    // schriebe ins Nirgendwo. Der Helfer unten deckelt n statt dessen.
    char js[640];
    size_t n = 0;
    auto add = [&](const char *fmt, auto... args) {
        if (n >= sizeof js) return;
        const int w = snprintf(js + n, sizeof js - n, fmt, args...);
        n = (w < 0 || (size_t)w >= sizeof js - n) ? sizeof js : n + (size_t)w;
    };

    add("{\"name\":\"%s\",\"uniq_id\":\"%s\"", e.name, e.obj);
    if (e.dev_cla) add(",\"dev_cla\":\"%s\"", e.dev_cla);
    if (e.icon)    add(",\"ic\":\"%s\"", e.icon);
    if (e.unit)    add(",\"unit_of_meas\":\"%s\"", e.unit);
    // mode "box" wie im Original; min/max/step unquoted (Zahlen).
    if (e.kind == Kind::setpoint) add(",\"mode\":\"box\",\"min\":0,\"max\":100,\"step\":1");
    add(",\"dev\":%s", s_dev_json);
    add(",\"avty_t\":\"%s\"", avty);
    add(",\"stat_t\":\"%s\"", stat);
    if (e.kind == Kind::sw || e.kind == Kind::setpoint) add(",\"cmd_t\":\"%s\"", cmd);
    add("}");

    if (n >= sizeof js) {
        ESP_LOGE(TAG, "Config von '%s' passt nicht in %u B — Entity fehlt in HA",
                 e.obj, (unsigned)sizeof js);
        return;
    }
    esp_mqtt_client_publish(s_client, topic, js, 0, 0, 1);   // retained
}

static void announce()
{
    char avty[80];
    topic_data(avty, sizeof avty, nullptr, "avty_t");
    esp_mqtt_client_publish(s_client, avty, "online", 0, 0, 1);

    for (int i = 0; i < E_COUNT; i++) {
        publish_config(i);
        s_sent_hash[i] = 0;               // Zustaende danach neu senden
        if ((i & 7) == 7) vTaskDelay(pdMS_TO_TICKS(20));  // Broker Luft lassen
    }

    char t[80];
    for (int i = 0; i < E_COUNT; i++) {
        if (ENT[i].kind != Kind::sw && ENT[i].kind != Kind::setpoint) continue;
        topic_data(t, sizeof t, ENT[i].obj, "cmd_t");
        esp_mqtt_client_subscribe(s_client, t, 0);
    }
    // Nicht im Original: nach einem Neustart von Home Assistant sind dessen
    // Entities weg, bis jemand die Configs erneut schickt. Retain deckt den
    // Normalfall ab, die Birth-Message den Rest.
    esp_mqtt_client_subscribe(s_client, "homeassistant/status", 0);

    ESP_LOGI(TAG, "%d Entities angemeldet", (int)E_COUNT);
}

// ---- Zustaende ------------------------------------------------------------

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h ? h : 1;      // 0 ist als "noch nie gesendet" reserviert
}

static void publish_states(const Model &md)
{
    char val[96], topic[80];
    for (int i = 0; i < E_COUNT; i++) {
        if (!ent_value(md, i, val, sizeof val)) continue;
        const uint64_t h = fnv1a(val);
        if (h == s_sent_hash[i]) continue;      // unveraendert -> Broker schonen
        s_sent_hash[i] = h;
        topic_data(topic, sizeof topic, ENT[i].obj, "stat_t");
        esp_mqtt_client_publish(s_client, topic, val, 0, 0, 1);
    }
}

// ---- Kommandos ------------------------------------------------------------

// Zahl aus dem Payload holen. ArduinoHA konnte bei Precision 0 NUR ganze Zahlen
// und hat "50.0" stillschweigend verworfen — je nach Home-Assistant-Version
// kommt aber genau das an. Hier wird gerundet statt weggeworfen.
static bool parse_number(const char *s, int len, int32_t *out)
{
    char buf[24];
    if (len <= 0 || len >= (int)sizeof buf) return false;
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end = nullptr;
    double v = strtod(buf, &end);
    if (end == buf) return false;
    *out = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
    return true;
}

static void on_command(const char *topic, int tlen, const char *data, int dlen)
{
    // Objekt-Id aus aha/<dev>/<obj>/cmd_t herausschneiden.
    char t[128];
    if (tlen >= (int)sizeof t) return;
    memcpy(t, topic, tlen);
    t[tlen] = '\0';

    if (strcmp(t, "homeassistant/status") == 0) {
        if (dlen == 6 && strncmp(data, "online", 6) == 0) {
            ESP_LOGI(TAG, "Home Assistant ist wieder da — Discovery erneut senden");
            s_need_announce = true;
        }
        return;
    }

    const char *obj = strrchr(t, '/');
    if (!obj) return;
    *(char *)obj = '\0';
    const char *slash = strrchr(t, '/');
    if (!slash) return;
    obj = slash + 1;

    const bool on = (dlen == 2);   // "ON" = 2 Zeichen, "OFF" = 3 (wie ArduinoHA)
    int32_t num = 0;

    if (strcmp(obj, "mos") == 0) {
        cmd_post(on ? CmdType::mos_on : CmdType::mos_off);
    } else if (strcmp(obj, "restart") == 0) {
        if (on) cmd_post(CmdType::restart);
    } else if (strcmp(obj, "setSOC") == 0) {
        if (parse_number(data, dlen, &num)) cmd_post(CmdType::set_soc, num);
    } else if (strcmp(obj, "setChargecurrentMax") == 0) {
        if (parse_number(data, dlen, &num)) cmd_post(CmdType::set_charge_max, num);
    } else if (strcmp(obj, "setDischargecurrentMax") == 0) {
        if (parse_number(data, dlen, &num)) cmd_post(CmdType::set_discharge_max, num);
    }
}

// ---- Client ---------------------------------------------------------------

static void on_mqtt_event(void *, esp_event_base_t, int32_t id, void *event_data)
{
    auto *e = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Broker verbunden");
            s_connected     = true;
            s_need_announce = true;   // ⚠️ Veroeffentlichen NICHT hier: aus dem
                                      // Event-Handler heraus kann sich der
                                      // MQTT-Task selbst blockieren.
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Broker weg");
            s_connected = false;
            break;
        case MQTT_EVENT_DATA:
            on_command(e->topic, e->topic_len, e->data, e->data_len);
            break;
        default:
            break;
    }
    Model *m = model_lock();
    m->net.mqtt = s_connected;
    model_unlock();
}

static void ha_task(void *)
{
    for (;;) {
        if (s_connected) {
            if (s_need_announce) {
                s_need_announce = false;
                announce();
            }
            Model md;
            model_read(&md);
            publish_states(md);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ha_mqtt_start()
{
    const AppConfig &cfg = config_get();
    if (!cfg.mqtt_host[0]) {
        ESP_LOGW(TAG, "kein Broker konfiguriert — MQTT bleibt aus");
        return;
    }

    // Device-Id: die feste MAC als Kleinbuchstaben-Hex ohne Trenner. Genau so
    // baut HAUtils::byteArrayToStr sie, und genau so steht sie in allen Topics,
    // die Home Assistant schon kennt.
    for (int i = 0; i < 6; i++)
        snprintf(s_devid + i * 2, 3, "%02x", HA_DEVICE_MAC[i]);

    snprintf(s_dev_json, sizeof s_dev_json,
             "{\"ids\":\"%s\",\"name\":\"Daly Solis Converter\",\"sw\":\"%s\","
             "\"mf\":\"RM\",\"mdl\":\"DalySolisConverter\"}",
             s_devid, esp_app_get_description()->version);

    char uri[96];
    snprintf(uri, sizeof uri, "mqtt://%s:%u", cfg.mqtt_host, (unsigned)cfg.mqtt_port);
    char avty[80];
    topic_data(avty, sizeof avty, nullptr, "avty_t");

    esp_mqtt_client_config_t mc = {};
    mc.broker.address.uri          = uri;
    mc.credentials.client_id       = s_devid;
    mc.credentials.username        = cfg.mqtt_user[0] ? cfg.mqtt_user : nullptr;
    mc.credentials.authentication.password = cfg.mqtt_pass[0] ? cfg.mqtt_pass : nullptr;
    mc.session.keepalive           = 15;
    // Der laengste Discovery-Config liegt bei ~460 B plus Topic. Der Standard
    // (1024 B) reicht knapp; 2048 gibt Luft fuer zusaetzliche Entities.
    mc.buffer.size                 = 2048;
    mc.buffer.out_size             = 2048;
    mc.session.last_will.topic     = avty;
    mc.session.last_will.msg       = "offline";
    mc.session.last_will.qos       = 0;
    mc.session.last_will.retain    = 1;
    mc.network.reconnect_timeout_ms = 5000;

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) { ESP_LOGE(TAG, "esp_mqtt_client_init fehlgeschlagen"); return; }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, nullptr);
    esp_mqtt_client_start(s_client);

    xTaskCreate(ha_task, "ha", 5120, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "Broker %s, Geraete-Id %s", uri, s_devid);
}
