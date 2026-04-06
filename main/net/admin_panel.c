/**
 * @file admin_panel.c
 * @brief Authenticated HTML admin dashboard served from GET /.
 *
 * Stream is loaded on-demand (click to start) to avoid:
 *   - Browser "loading" spinner stuck forever (MJPEG has no Content-Length)
 *   - Unnecessary stream connections when just checking telemetry
 *
 * Session cookie is set on every successful auth so the browser doesn't
 * re-prompt for Basic Auth credentials on subsequent requests.
 */
#include "admin_panel.h"
#include "web_auth.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "admin";

/* ── Embedded HTML dashboard ─────────────────────────────────────────────── */
static const char ADMIN_HTML[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Vision Hub</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0a0a0a;color:#00e676;font-family:'Courier New',monospace;padding:12px}"
"h1{color:#69f0ae;margin-bottom:12px;font-size:1.1em;letter-spacing:2px;display:flex;justify-content:space-between;align-items:center}"
"a.logout{font-size:0.7em;color:#546e7a;text-decoration:none;border:1px solid #546e7a;padding:2px 8px;border-radius:2px}"
".grid{display:flex;flex-wrap:wrap;gap:12px}"
".panel{background:#111;border:1px solid #1b5e20;border-radius:4px;padding:12px;flex:1;min-width:300px}"
".panel h2{color:#69f0ae;font-size:0.85em;letter-spacing:1px;margin-bottom:10px;border-bottom:1px solid #1b5e20;padding-bottom:5px}"
"#stream-box{width:100%;background:#000;border:1px solid #1b5e20;border-radius:2px;min-height:180px;display:flex;align-items:center;justify-content:center}"
"#stream-box img{width:100%;display:block}"
"#play-btn{background:#1b5e20;color:#69f0ae;border:1px solid #69f0ae;padding:10px 24px;cursor:pointer;font-family:inherit;font-size:0.9em;border-radius:2px}"
"#play-btn:hover{background:#2e7d32}"
"pre{font-size:0.72em;line-height:1.55;white-space:pre-wrap;word-break:break-all}"
".ok{color:#00e676}.warn{color:#ffeb3b}.alarm{color:#f44336}"
".label{color:#4db6ac}.val{color:#e0e0e0}"
"#status{font-size:0.65em;color:#546e7a;margin-top:6px}"
"</style>"
"</head>"
"<body>"
"<h1>&#9679; ESP32-S3 Vision Hub"
"  <a class=\"logout\" href=\"/logout\">logout</a>"
"</h1>"
"<div class=\"grid\">"
"  <div class=\"panel\">"
"    <h2>LIVE STREAM</h2>"
"    <div id=\"stream-box\">"
"      <button id=\"play-btn\" onclick=\"startStream()\">&#9654; START STREAM</button>"
"    </div>"
"  </div>"
"  <div class=\"panel\">"
"    <h2>TELEMETRY</h2>"
"    <pre id=\"telem\">Connecting...</pre>"
"    <div id=\"status\"></div>"
"  </div>"
"</div>"
"<script>"
"(function(){"
"var el=document.getElementById('telem');"
"var st=document.getElementById('status');"
"window.startStream=function(){"
"  var box=document.getElementById('stream-box');"
"  var img=document.createElement('img');"
"  img.alt='stream';img.style.cssText='width:100%;display:block';"
"  var retries=0;"
"  img.onerror=function(){"
"    retries++;"
"    if(retries>4){"
"      box.innerHTML='<p style=\"color:#f44336;text-align:center;padding:20px\">"
"Stream unavailable.<br>"
"<button onclick=\"startStream()\" "
"style=\"margin-top:8px;background:#1b5e20;color:#69f0ae;"
"border:1px solid #69f0ae;padding:8px 16px;cursor:pointer;"
"font-family:inherit;border-radius:2px\">&#9654; RETRY</button></p>';"
"      return;"
"    }"
"    var delay=retries*3000;"
"    st.textContent='Stream error. Retry '+retries+'/4 in '+(delay/1000)+'s...';"
"    setTimeout(function(){img.src='/stream?t='+Date.now();},delay);"
"  };"
"  img.src='/stream';"
"  box.innerHTML='';"
"  box.appendChild(img);"
"};"
"function fmt(d){"
"  var lines=[];"
"  var alarm=d.alarm&&d.alarm.active;"
"  function row(k,v,cls){"
"    lines.push('<span class=\"label\">'+k.padEnd(24)+'</span>"
"<span class=\"'+(cls||'val')+'\">'+v+'</span>');"
"  }"
"  row('Uptime',d.system.uptime_s+'s');"
"  row('Reset',d.system.reset_reason);"
"  row('Wi-Fi',d.wifi.connected?'connected ▸ '+d.wifi.ip:'DISCONNECTED',d.wifi.connected?'ok':'alarm');"
"  if(d.wifi.connected)row('RSSI',d.wifi.rssi_dbm+' dBm',d.wifi.rssi_dbm>-70?'ok':'warn');"
"  row('Camera FPS',d.camera.fps_1s,d.camera.fps_1s>=10?'ok':'warn');"
"  row('Cam frames / drops',d.camera.frame_count+' / '+d.camera.drop_count,d.camera.drop_count>0?'warn':'ok');"
"  row('Stream clients',d.stream.client_count);"
"  if(d.cpu){row('CPU Core0/1',d.cpu.core0_pct+'% / '+d.cpu.core1_pct+'%',d.cpu.core0_pct<80&&d.cpu.core1_pct<80?'ok':'warn');}"
"  row('Heap free',Math.round(d.memory.heap_free_b/1024)+'KB');"
"  row('PSRAM free',Math.round(d.memory.psram_free_b/1024)+'KB',d.memory.psram_free_b>1048576?'ok':'warn');"
"  row('Internal RAM',Math.round(d.memory.internal_free_b/1024)+'KB');"
"  if(d.sensors.temp_c||d.sensors.humidity_pct){"
"    row('Temp / Hum',d.sensors.temp_c.toFixed(1)+'°C  '+d.sensors.humidity_pct.toFixed(1)+'%',d.sensors.temp_c<50?'ok':'alarm');"
"    row('Pressure',d.sensors.pressure_hpa.toFixed(1)+' hPa');"
"    row('CO / NH3',d.sensors.co_ppm.toFixed(0)+' / '+d.sensors.nh3_ppm.toFixed(0)+' ppm',d.sensors.co_ppm<50?'ok':'alarm');"
"  }"
"  if(alarm)row('!! ALARM !!',d.alarm.last_reason,'alarm');"
"  return lines.join('\\n');"
"}"
"var t0=0;"
"function poll(){"
"  fetch('/api/telemetry')"
"    .then(function(r){return r.json();})"
"    .then(function(d){"
"      el.innerHTML=fmt(d);"
"      var lag=t0?Date.now()-t0:0;"
"      t0=Date.now();"
"      st.textContent=lag?'poll '+lag+'ms ago':'';"
"    })"
"    .catch(function(e){st.textContent='ERR: '+e.message;});"
"}"
"poll();"
"setInterval(poll,2000);"
"})();"
"</script>"
"</body>"
"</html>";

/* ── Logout page ─────────────────────────────────────────────────────────── */
static const char LOGOUT_HTML[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta http-equiv=\"refresh\" content=\"2;url=/\">"
"<title>Logged out</title></head>"
"<body style=\"background:#0a0a0a;color:#69f0ae;font-family:monospace;padding:32px\">"
"<p>Logged out. Redirecting...</p></body></html>";

static esp_err_t admin_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        web_auth_send_challenge(req);
        return ESP_OK;
    }

    /* Set/refresh session cookie so browser doesn't re-prompt next time */
    web_auth_set_session_cookie(req);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, private");
    httpd_resp_sendstr(req, ADMIN_HTML);
    return ESP_OK;
}

static esp_err_t logout_get_handler(httpd_req_t *req)
{
    web_auth_clear_session_cookie(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, LOGOUT_HTML);
    return ESP_OK;
}

static const httpd_uri_t s_root_uri = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = admin_get_handler,
};

static const httpd_uri_t s_index_uri = {
    .uri     = "/index.html",
    .method  = HTTP_GET,
    .handler = admin_get_handler,
};

static const httpd_uri_t s_logout_uri = {
    .uri     = "/logout",
    .method  = HTTP_GET,
    .handler = logout_get_handler,
};

void admin_panel_register(httpd_handle_t server)
{
    httpd_register_uri_handler(server, &s_root_uri);
    httpd_register_uri_handler(server, &s_index_uri);
    httpd_register_uri_handler(server, &s_logout_uri);
    ESP_LOGI(TAG, "Admin panel registered at '/' (lazy stream, session cookie).");
}
