/**
 * @file admin_panel.c
 * @brief Authenticated HTML admin dashboard served from GET /.
 *
 * Pipeline:
 *   - Session cookie set on each auth pass so browsers don't re-prompt.
 *   - Live stream is a WebSocket binary push (/ws/stream). Motion
 *     bounding boxes arrive as text frames and are overlaid on a
 *     canvas atop the JPEG image element.
 *   - Telemetry is a separate 2 Hz poll against /api/telemetry.
 */
#include "admin_panel.h"
#include "web_auth.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "admin";

/* ── Embedded HTML dashboard ──────────────────────────────────────────────
 * Split into two halves: ADMIN_HTML_HEAD ends just after the opening
 * <script> tag, ADMIN_HTML_BODY contains the JS/body/close tags. Between
 * the two halves, admin_get_handler injects `window.WS_TOKEN='...';`
 * so the WebSocket URL can authenticate via query parameter — immune
 * to cookie policy variations on mobile browsers. */
static const char ADMIN_HTML_HEAD[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>yyy Vision Hub</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0a0a0a;color:#00e676;font-family:'Courier New',monospace;padding:12px}"
"h1{color:#69f0ae;margin-bottom:12px;font-size:1.1em;letter-spacing:2px;display:flex;justify-content:space-between;align-items:center}"
"a.logout{font-size:0.7em;color:#546e7a;text-decoration:none;border:1px solid #546e7a;padding:2px 8px;border-radius:2px}"
".grid{display:flex;flex-wrap:wrap;gap:12px}"
".panel{background:#111;border:1px solid #1b5e20;border-radius:4px;padding:12px;flex:1;min-width:320px}"
".panel h2{color:#69f0ae;font-size:0.85em;letter-spacing:1px;margin-bottom:10px;border-bottom:1px solid #1b5e20;padding-bottom:5px}"
"#stream-box{width:100%;background:#000;border:1px solid #1b5e20;border-radius:2px;min-height:180px;position:relative;display:flex;align-items:center;justify-content:center}"
"#stream-wrap{position:relative;width:100%;line-height:0}"
"#frame{width:100%;display:block}"
"#overlay{position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none}"
"#play-btn{background:#1b5e20;color:#69f0ae;border:1px solid #69f0ae;padding:10px 24px;cursor:pointer;font-family:inherit;font-size:0.9em;border-radius:2px}"
"#play-btn:hover{background:#2e7d32}"
".stream-ctl{display:flex;justify-content:space-between;align-items:center;margin-top:6px;font-size:0.7em;color:#546e7a}"
".stream-ctl button{background:#1b2020;color:#546e7a;border:1px solid #263238;padding:3px 10px;cursor:pointer;font-family:inherit;font-size:0.72em;border-radius:2px}"
"pre{font-size:0.72em;line-height:1.55;white-space:pre-wrap;word-break:break-all}"
".ok{color:#00e676}.warn{color:#ffeb3b}.alarm{color:#f44336}"
".label{color:#4db6ac}.val{color:#e0e0e0}"
"#motion-list{font-size:0.7em;margin-top:6px;color:#80deea;max-height:120px;overflow:auto}"
"#motion-list div{padding:2px 0;border-bottom:1px dashed #1b5e20}"
"#status{font-size:0.65em;color:#546e7a;margin-top:6px}"
"</style>"
"</head>"
"<body>"
"<h1>&#9679; yyy Vision Hub"
"  <a class=\"logout\" href=\"/logout\">logout</a>"
"</h1>"
"<div class=\"grid\">"
"  <div class=\"panel\">"
"    <h2>LIVE STREAM</h2>"
"    <div id=\"stream-box\">"
"      <button id=\"play-btn\" onclick=\"startStream()\">&#9654; START STREAM</button>"
"    </div>"
"    <div class=\"stream-ctl\">"
"      <span id=\"stream-stat\">idle</span>"
"      <button id=\"stop-btn\" onclick=\"stopStream()\" style=\"display:none\">&#9209; STOP</button>"
"    </div>"
"    <div id=\"motion-list\"></div>"
"  </div>"
"  <div class=\"panel\">"
"    <h2>TELEMETRY</h2>"
"    <pre id=\"telem\">Connecting...</pre>"
"    <div id=\"status\"></div>"
"  </div>"
"</div>"
"<script>";

/* Injected between HEAD and BODY at request time:
 *   window.WS_TOKEN = '<session-token>';
 */

static const char ADMIN_HTML_BODY[] =
"(function(){"
"var el=document.getElementById('telem');"
"var st=document.getElementById('status');"
"var strStat=document.getElementById('stream-stat');"
"var motionList=document.getElementById('motion-list');"
"var playBtn=document.getElementById('play-btn');"
"var stopBtn=document.getElementById('stop-btn');"
"var box=document.getElementById('stream-box');"
"var ws=null;"
"var lastMotion=null;"
"var frameCount=0;"
"var lastFpsT=Date.now();"
"var curUrl=null;"
"var frameEl=null;"
"var canvasEl=null;"
"function clearStream(){"
"  if(ws){try{ws.close();}catch(e){}ws=null;}"
"  if(curUrl){URL.revokeObjectURL(curUrl);curUrl=null;}"
"  box.innerHTML='<button id=\"play-btn\" onclick=\"startStream()\">&#9654; START STREAM</button>';"
"  playBtn=document.getElementById('play-btn');"
"  stopBtn.style.display='none';"
"  strStat.textContent='idle';"
"  motionList.innerHTML='';"
"  lastMotion=null;"
"}"
"window.stopStream=clearStream;"
"function drawOverlay(){"
"  if(!canvasEl||!frameEl||!lastMotion)return;"
"  var w=frameEl.naturalWidth||lastMotion.w||640;"
"  var h=frameEl.naturalHeight||lastMotion.h||480;"
"  canvasEl.width=w;canvasEl.height=h;"
"  var ctx=canvasEl.getContext('2d');"
"  ctx.clearRect(0,0,w,h);"
"  var tracks=lastMotion.tracks||[];"
"  var rows=[];"
"  for(var i=0;i<tracks.length;i++){"
"    var t=tracks[i];"
"    var activeColor=t.lost?'rgba(255,193,7,':'rgba(244,67,54,';"
"    var alpha=Math.min(1,0.5+t.s*0.5);"
"    ctx.lineWidth=4;"
"    ctx.strokeStyle=activeColor+alpha+')';"
"    ctx.strokeRect(t.x,t.y,t.w,t.h);"
"    ctx.fillStyle='rgba(0,0,0,0.65)';"
"    ctx.fillRect(t.x,t.y-20,120,20);"
"    ctx.fillStyle='#fff';"
"    ctx.font='bold 13px monospace';"
"    ctx.fillText('#'+t.id+' h'+t.hits+' a'+t.age,t.x+4,t.y-5);"
"    rows.push('<div>"
"<span style=\"color:'+(t.lost?'#ffc107':'#f44336')+'\">#'+t.id+'</span>"
" box '+t.x+','+t.y+' '+t.w+'x'+t.h+"
"' score '+t.s.toFixed(3)+' age '+t.age+' hits '+t.hits+(t.lost?' LOST':'')+'</div>');"
"  }"
"  if(lastMotion.det){"
"    ctx.fillStyle='rgba(244,67,54,0.9)';"
"    ctx.fillRect(0,0,150,26);"
"    ctx.fillStyle='#fff';"
"    ctx.font='bold 15px monospace';"
"    ctx.fillText('MOTION x'+tracks.length,6,19);"
"  }"
"  motionList.innerHTML="
"    '<div style=\"color:#69f0ae\">motion fps '+(lastMotion.fps||0)+"
"    '  score '+((lastMotion.score||0).toFixed(3))+"
"    '  tracks '+tracks.length+(lastMotion.det?'  <span style=\\'color:#f44336\\'>ALARM</span>':'')+'</div>'"
"    +rows.join('');"
"}"
"window.startStream=function(){"
"  if(ws)return;"
"  box.innerHTML="
"    '<div id=\"stream-wrap\">"
"       <img id=\"frame\" alt=\"stream\">"
"       <canvas id=\"overlay\"></canvas>"
"     </div>';"
"  frameEl=document.getElementById('frame');"
"  canvasEl=document.getElementById('overlay');"
"  stopBtn.style.display='inline-block';"
"  strStat.textContent='connecting...';"
"  var proto=location.protocol==='https:'?'wss:':'ws:';"
"  try{"
"    ws=new WebSocket(proto+'//'+location.host+'/ws/stream?token='+window.WS_TOKEN);"
"    ws.binaryType='blob';"
"  }catch(e){strStat.textContent='ws err '+e;return;}"
"  ws.onopen=function(){strStat.textContent='live';frameCount=0;lastFpsT=Date.now();};"
"  ws.onmessage=function(ev){"
"    if(typeof ev.data==='string'){"
"      try{lastMotion=JSON.parse(ev.data);drawOverlay();}catch(e){}"
"      return;"
"    }"
"    var prevUrl=curUrl;"
"    curUrl=URL.createObjectURL(ev.data);"
"    frameEl.onload=function(){"
"      if(prevUrl)URL.revokeObjectURL(prevUrl);"
"      drawOverlay();"
"    };"
"    frameEl.src=curUrl;"
"    frameCount++;"
"    var now=Date.now();"
"    if(now-lastFpsT>1000){"
"      var fps=(frameCount*1000/(now-lastFpsT)).toFixed(1);"
"      strStat.textContent='live · '+fps+' fps';"
"      frameCount=0;lastFpsT=now;"
"    }"
"  };"
"  ws.onerror=function(){strStat.textContent='ws error';};"
"  ws.onclose=function(){"
"    strStat.textContent='disconnected';"
"    ws=null;"
"  };"
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
"  if(d.vision)row('Motion FPS/score',d.vision.motion_fps_1s+' / '+(d.vision.motion_score||0).toFixed(3),"
"      d.vision.motion_detected?'alarm':'ok');"
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

    web_auth_set_session_cookie(req);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, private");

    /* Build the full HTML in one buffer so the response is a single
     * Content-Length HTTP response — avoids chunked Transfer-Encoding
     * which some mobile browsers handle poorly for text/html. */
    const char *tok = web_auth_get_session_token();
    size_t head_len = sizeof(ADMIN_HTML_HEAD) - 1;
    size_t body_len = sizeof(ADMIN_HTML_BODY) - 1;
    char inject[96];
    int inj_len = snprintf(inject, sizeof(inject),
                           "window.WS_TOKEN='%s';", tok);
    size_t total = head_len + (size_t)inj_len + body_len;

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    memcpy(buf, ADMIN_HTML_HEAD, head_len);
    memcpy(buf + head_len, inject, (size_t)inj_len);
    memcpy(buf + head_len + (size_t)inj_len, ADMIN_HTML_BODY, body_len);
    buf[total] = '\0';

    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)total);
    free(buf);
    return err;
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
    ESP_LOGI(TAG, "Admin panel registered at '/' (WebSocket stream, motion overlay).");
}
