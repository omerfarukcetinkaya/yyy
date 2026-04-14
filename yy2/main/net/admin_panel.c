/**
 * @file admin_panel.c
 * @brief Scout admin dashboard — served on 2.4G network, port 80.
 *
 * Basic Auth: tenten / 1234. Auto-refresh 1 Hz. Shows:
 *   - Scout self card: uptime, CPU, heap, internal DRAM, task count,
 *     reset reason, reboot count, Telegram send/poll stats, WiFi band
 *   - Alarm card: current alarm state, schedule, mute
 *   - Per-sensor cards: one per registered sensor with full telemetry
 */
#include "admin_panel.h"
#include "wifi_dual.h"
#include "telegram_client.h"
#include "espnow_bridge.h"
#include "sensor_registry.h"
#include "scout_health.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "scout_admin";

/* Pre-computed Basic Auth: tenten:1234 → base64 */
static char s_expected_auth[128] = {0};
static bool s_auth_init = false;

static void init_auth(void)
{
    if (s_auth_init) return;
    const char *creds = CONFIG_SCOUT_S3_USER ":" CONFIG_SCOUT_S3_PASS;
    unsigned char b64[96] = {0};
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                          (const unsigned char *)creds, strlen(creds));
    snprintf(s_expected_auth, sizeof(s_expected_auth), "Basic %.*s",
             (int)b64_len, b64);
    s_auth_init = true;
}

static bool check_auth(httpd_req_t *req)
{
    init_auth();
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }
    return (strncmp(auth_hdr, s_expected_auth, strlen(s_expected_auth)) == 0);
}

static void send_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Scout Bridge\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized.");
}

/* ── Render helpers ─────────────────────────────────────────────────────── */

static const char HTML_HEAD[] =
"<!DOCTYPE html>"
"<html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<meta http-equiv=refresh content=1>"
"<title>Scout Bridge</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#0a0a1a;color:#4fc3f7;font-family:'Courier New',monospace;padding:12px;font-size:13px}"
"h1{color:#81d4fa;margin-bottom:10px;font-size:1.1em;letter-spacing:2px}"
".grid{display:flex;flex-wrap:wrap;gap:10px}"
".card{background:#111;border:1px solid #1565c0;border-radius:5px;padding:10px;flex:1;min-width:300px}"
".card.scout{border-color:#7b1fa2}"
".card.alarm{border-color:#d32f2f}"
".card.sensor{border-color:#2e7d32}"
".card h2{color:#64b5f6;font-size:0.8em;margin-bottom:6px;border-bottom:1px solid #1565c0;padding-bottom:3px;letter-spacing:1px}"
".card.scout h2{color:#ce93d8;border-color:#7b1fa2}"
".card.alarm h2{color:#ef9a9a;border-color:#d32f2f}"
".card.sensor h2{color:#a5d6a7;border-color:#2e7d32}"
".row{display:flex;justify-content:space-between;padding:1px 0;font-size:0.78em}"
".lbl{color:#4db6ac}.val{color:#e0e0e0;font-weight:bold}"
".ok{color:#00e676}.warn{color:#ffeb3b}.bad{color:#f44336}.dim{color:#546e7a}"
".bar{height:6px;background:#1b2430;border-radius:3px;overflow:hidden;margin-top:2px}"
".bar>span{display:block;height:100%;background:linear-gradient(90deg,#00e676,#ffeb3b,#f44336);transition:width .3s}"
".ts{color:#546e7a;font-size:0.65em;margin-top:8px;text-align:right}"
"</style></head><body>"
"<h1>&#128737; Scout Bridge — ESP32-C5</h1>"
"<div class=grid>";

static int render_scout_card(char *buf, int n, int cap, const scout_health_t *h)
{
    int cpu_color = h->cpu_pct < 40 ? 0 : (h->cpu_pct < 80 ? 1 : 2);
    const char *cpu_cls = cpu_color == 0 ? "ok" : (cpu_color == 1 ? "warn" : "bad");

    uint32_t heap_pct = (h->heap_free_b > 0 && h->heap_free_b < 1000000)
                        ? (h->heap_free_b * 100 / 300000) : 100;
    if (heap_pct > 100) heap_pct = 100;

    return snprintf(buf + n, cap - n,
        "<div class='card scout'>"
        "<h2>🛡 SCOUT C5 SELF</h2>"
        "<div class=row><span class=lbl>Uptime</span><span class=val>%lu s</span></div>"
        "<div class=row><span class=lbl>Reset reason</span><span class=val>%s</span></div>"
        "<div class=row><span class=lbl>Reboot count</span><span class=val>%lu</span></div>"
        "<div class=row><span class=lbl>CPU busy</span><span class='val %s'>%u%%</span></div>"
        "<div class=bar><span style='width:%u%%'></span></div>"
        "<div class=row><span class=lbl>Heap free</span><span class=val>%lu B</span></div>"
        "<div class=row><span class=lbl>Heap min ever</span><span class=val>%lu B</span></div>"
        "<div class=row><span class=lbl>Largest block</span><span class=val>%lu B</span></div>"
        "<div class=row><span class=lbl>Internal DRAM</span><span class=val>%lu B</span></div>"
        "<div class=row><span class=lbl>Task count</span><span class=val>%lu</span></div>"
        "<div class=row><span class=lbl>WiFi</span><span class=val>%s [%s] %d dBm</span></div>"
        "<div class=row><span class=lbl>Local IP</span><span class=val>%s</span></div>"
        "<div class=row><span class=lbl>TG polls</span><span class=val>%lu OK / %lu fail</span></div>"
        "<div class=row><span class=lbl>TG sent</span><span class=val>%lu OK / %lu fail</span></div>"
        "</div>",
        (unsigned long)h->uptime_s,
        h->reset_reason ? h->reset_reason : "?",
        (unsigned long)h->reboot_count,
        cpu_cls, h->cpu_pct,
        h->cpu_pct,
        (unsigned long)h->heap_free_b,
        (unsigned long)h->heap_min_free_b,
        (unsigned long)h->heap_largest_block_b,
        (unsigned long)h->internal_free_b,
        (unsigned long)h->task_count,
        h->wifi_connected ? "UP" : "DOWN",
        h->wifi_on_5g ? "5G" : "2.4G",
        (int)h->wifi_rssi,
        h->wifi_ip,
        (unsigned long)h->tg_poll_ok, (unsigned long)h->tg_poll_fail,
        (unsigned long)h->tg_sent_ok, (unsigned long)h->tg_sent_fail);
}

static int render_alarm_card(char *buf, int n, int cap)
{
    bool any = sensor_registry_any_alarm();
    const sensor_t *af = sensor_registry_first_alarm();
    const char *reason = (af && af->alarm_reason[0]) ? af->alarm_reason : "—";
    return snprintf(buf + n, cap - n,
        "<div class='card alarm'>"
        "<h2>🚨 ALARM / TELEGRAM</h2>"
        "<div class=row><span class=lbl>State</span><span class='val %s'>%s</span></div>"
        "<div class=row><span class=lbl>Source</span><span class=val>%s</span></div>"
        "<div class=row><span class=lbl>Reason</span><span class=val>%s</span></div>"
        "<div class=row><span class=lbl>Mute</span><span class='val %s'>%s</span></div>"
        "<div class=row><span class=lbl>Schedule</span><span class=val>%02d:00 – %02d:00</span></div>"
        "<div class=row><span class=lbl>BIST cadence</span><span class=val>15 min heartbeat</span></div>"
        "</div>",
        any ? "bad" : "ok",
        any ? "ACTIVE" : "Clear",
        af ? af->name : "—",
        reason,
        telegram_is_muted() ? "warn" : "ok",
        telegram_is_muted() ? "MUTED" : "Active",
        CONFIG_SCOUT_SCHEDULE_START_HOUR,
        CONFIG_SCOUT_SCHEDULE_END_HOUR);
}

static int render_sensor_card(char *buf, int n, int cap, int idx, const sensor_t *s)
{
    if (!s) return 0;
    const char *status_cls = s->online ? "ok" : "bad";
    const char *status_txt = s->online ? "ONLINE" : "OFFLINE";

    int written = snprintf(buf + n, cap - n,
        "<div class='card sensor'>"
        "<h2>[%d] 📡 %s</h2>"
        "<div class=row><span class=lbl>Status</span><span class='val %s'>%s</span></div>"
        "<div class=row><span class=lbl>IP</span><span class=val>%s</span></div>",
        idx, s->name, status_cls, status_txt, s->ip);

    if (s->online) {
        int m = snprintf(buf + n + written, cap - n - written,
            "<div class=row><span class=lbl>Uptime</span><span class=val>%lu s</span></div>"
            "<div class=row><span class=lbl>Camera</span><span class=val>%lu fps</span></div>"
            "<div class=row><span class=lbl>Motion</span><span class='val %s'>%lu fps · score %.3f</span></div>"
            "<div class=row><span class=lbl>Alarm</span><span class='val %s'>%s</span></div>"
            "<div class=row><span class=lbl>CPU core0/1</span><span class=val>%u%% / %u%%</span></div>"
            "<div class=bar><span style='width:%u%%'></span></div>"
            "<div class=row><span class=lbl>Heap free</span><span class=val>%lu KB</span></div>"
            "<div class=row><span class=lbl>PSRAM free</span><span class=val>%lu KB</span></div>"
            "<div class=row><span class=lbl>WiFi RSSI</span><span class=val>%d dBm</span></div>"
            "<div class=row><span class=lbl>Poll OK/Fail</span><span class=val>%lu / %lu</span></div>",
            (unsigned long)s->uptime_s,
            (unsigned long)s->cam_fps,
            s->motion_detected ? "warn" : "dim",
            (unsigned long)s->motion_fps,
            s->motion_score,
            s->alarm_active ? "bad" : "ok",
            s->alarm_active ? (s->alarm_reason[0] ? s->alarm_reason : "ACTIVE") : "Clear",
            s->cpu0_pct, s->cpu1_pct,
            (s->cpu0_pct > s->cpu1_pct ? s->cpu0_pct : s->cpu1_pct),
            (unsigned long)(s->heap_free_b / 1024),
            (unsigned long)(s->psram_free_b / 1024),
            (int)s->rssi,
            (unsigned long)s->poll_ok,
            (unsigned long)s->poll_fail);
        written += m;
    } else {
        int m = snprintf(buf + n + written, cap - n - written,
            "<div class=row><span class=lbl>Poll fails</span><span class=val>%lu</span></div>",
            (unsigned long)s->poll_fail);
        written += m;
    }
    written += snprintf(buf + n + written, cap - n - written, "</div>");
    return written;
}

static esp_err_t admin_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_challenge(req);
        return ESP_OK;
    }

    /* Heap-alloc 8KB buffer — enough for Scout + Alarm + up to 8 sensor cards */
    const int cap = 8192;
    char *buf = malloc(cap);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int n = 0;

    /* Header */
    memcpy(buf, HTML_HEAD, sizeof(HTML_HEAD) - 1);
    n += sizeof(HTML_HEAD) - 1;

    /* Scout self card */
    scout_health_t h;
    scout_health_snapshot(&h);
    n += render_scout_card(buf, n, cap, &h);

    /* Alarm / Telegram card */
    n += render_alarm_card(buf, n, cap);

    /* Per-sensor cards */
    int total = sensor_registry_count();
    for (int i = 0; i < total && n < cap - 1500; i++) {
        n += render_sensor_card(buf, n, cap, i, sensor_registry_get(i));
    }

    /* Footer */
    n += snprintf(buf + n, cap - n,
        "</div>"
        "<p class=ts>Auto-refresh 1s · %d sensors registered · %d online</p>"
        "</body></html>",
        total, sensor_registry_online_count());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static const httpd_uri_t s_root = {
    .uri = "/", .method = HTTP_GET, .handler = admin_handler,
};

static const httpd_uri_t s_index = {
    .uri = "/index.html", .method = HTTP_GET, .handler = admin_handler,
};

void scout_admin_register(httpd_handle_t server)
{
    httpd_register_uri_handler(server, &s_root);
    httpd_register_uri_handler(server, &s_index);
    ESP_LOGI(TAG, "Scout admin panel registered at '/' (full telemetry)");
}
