/**
 * @file stream_handler.c
 * @brief MJPEG multipart HTTP stream on GET /stream.
 *
 * The stream runs in a dedicated FreeRTOS task (one per client).
 * httpd_req_async_handler_begin transfers the socket to the async task;
 * the httpd task is then free to serve other requests immediately.
 *
 * Raw socket path (no chunked encoding):
 *   We get the socket fd via httpd_req_to_sockfd() and send the HTTP
 *   response + MJPEG multipart frames ourselves using send(). This avoids
 *   httpd's chunked Transfer-Encoding (incompatible with MJPEG in <img>
 *   tags on some browsers) and the 5-second SO_SNDTIMEO set by httpd at
 *   connection accept time.
 *
 * MJPEG over HTTP multipart:
 *   HTTP/1.1 200 OK
 *   Content-Type: multipart/x-mixed-replace; boundary=frame
 *   [blank line]
 *   --frame\r\n
 *   Content-Type: image/jpeg\r\n
 *   Content-Length: <n>\r\n\r\n
 *   <jpeg bytes>\r\n
 *   (repeat)
 */
#include "stream_handler.h"
#include "web_auth.h"
#include "cam_pipeline.h"
#include "cam_driver.h"
#include "cam_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>

static const char *TAG = "stream";

#define STREAM_TASK_STACK    6144
/* Highest user-space priority on Core 1 — stream_task is the camera's
 * primary consumer; it must never be preempted by motion or classifier
 * while holding a frame or blocking in esp_camera_fb_get(). */
#define STREAM_TASK_PRIORITY 6

/* Max simultaneous MJPEG clients — prevents socket pool exhaustion. */
#define STREAM_MAX_CONCURRENT  2

/* If the camera delivers no frames for this many ms, close the stream.
 * Prevents dead sockets accumulating when camera is not producing.
 * esp_camera_fb_get() blocks ~4 s on each NULL return, so 3 fails ≈ 12 s. */
#define STREAM_NO_FRAME_TIMEOUT_MS  12000U

/* HTTP response header sent once at stream start (no Transfer-Encoding: chunked) */
static const char STREAM_HTTP_HDR[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n";

/* Per-frame multipart header template */
#define PART_BOUNDARY   "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n"
#define PART_TAIL       "\r\n"

/* Stream send timeout per send() call.
 * iOS power-save mode can stall a TCP receive window for 5-15 s while the
 * device transitions power states. With 5 s, SO_SNDTIMEO fired mid-stall,
 * closing the stream after only ~10 frames. 15 s survives typical iOS stalls
 * while still catching genuinely dead connections within a reasonable bound.
 * Worst-case blocked time per frame: 15 s × 2 send() calls ≈ 30 s. */
#define STREAM_SEND_TIMEOUT_S  15

/* If a single frame's full send (header + JPEG + tail) takes longer than
 * this many ms, log a warning. Helps detect Wi-Fi throughput degradation.
 * Normal send time: 6-100 ms. Above 2000 ms implies TCP window stall. */
#define STREAM_FRAME_WARN_MS  2000

static atomic_uint s_client_count = 0;

/* ── Reliable send helper ───────────────────────────────────────────────────── */

static bool stream_send_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        int sent = send(fd, p, len, 0);
        if (sent <= 0) return false;
        p   += sent;
        len -= (size_t)sent;
    }
    return true;
}

/* ── Async stream task ───────────────────────────────────────────────────────── */

static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    atomic_fetch_add(&s_client_count, 1u);
    ESP_LOGI(TAG, "Stream client connected. Active: %u",
             (unsigned)atomic_load(&s_client_count));

    /* Get raw socket fd and override send timeout */
    int fd = httpd_req_to_sockfd(req);
    struct timeval tv = { .tv_sec = STREAM_SEND_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Send HTTP response header (plain streaming, no chunked encoding) */
    if (!stream_send_all(fd, STREAM_HTTP_HDR, sizeof(STREAM_HTTP_HDR) - 1)) {
        ESP_LOGW(TAG, "Failed to send HTTP header, client gone.");
        goto cleanup;
    }

    char part_hdr[128];
    cam_frame_t frame;
    uint32_t frames_sent = 0;
    uint32_t cam_fails   = 0;
    int64_t  no_frame_since_us = esp_timer_get_time();  /* reset on each success */

    /* Frame period tick for FPS cap. vTaskDelayUntil absorbs camera acquisition
     * time: if get+send < period → sleep remainder; if get+send > period →
     * fire immediately (camera is the bottleneck, no artificial slowdown). */
    TickType_t next_wake = xTaskGetTickCount();

    while (true) {
        /* cam_pipeline_get_frame uses esp_camera_fb_get() which has a
         * 4-second internal blocking timeout. */
        esp_err_t cam_ret = cam_pipeline_get_frame(&frame, 5000);
        if (cam_ret != ESP_OK) {
            cam_fails++;
            int64_t no_frame_ms = (esp_timer_get_time() - no_frame_since_us) / 1000;
            if (cam_fails == 1 || cam_fails % 5 == 0) {
                ESP_LOGW(TAG, "cam_pipeline_get_frame fail #%lu (%lld ms without frame)",
                         (unsigned long)cam_fails, no_frame_ms);
            }
            /* Dead-socket guard: if the camera produces nothing for
             * STREAM_NO_FRAME_TIMEOUT_MS, close this connection.
             * Without this, the socket stays in CLOSE_WAIT indefinitely
             * (send() is never called → TCP RST from browser goes undetected)
             * and exhausts the LWIP socket pool. */
            if ((uint64_t)no_frame_ms >= STREAM_NO_FRAME_TIMEOUT_MS) {
                ESP_LOGW(TAG, "No frame for %lld ms. Closing stream to free socket.",
                         no_frame_ms);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* Frame received — reset fail state */
        cam_fails = 0;
        no_frame_since_us = esp_timer_get_time();

        size_t frame_len = frame.fb->len;
        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                               PART_BOUNDARY, frame_len);

        int64_t t_send_start = esp_timer_get_time();
        bool ok = stream_send_all(fd, part_hdr, (size_t)hdr_len) &&
                  stream_send_all(fd, frame.fb->buf, frame_len) &&
                  stream_send_all(fd, PART_TAIL, sizeof(PART_TAIL) - 1);
        int64_t t_send_ms = (esp_timer_get_time() - t_send_start) / 1000;

        cam_pipeline_release_frame(&frame);

        if (!ok) {
            ESP_LOGW(TAG, "Stream send failed after %lu frames: errno=%d  last_frame=%lld ms.",
                     (unsigned long)frames_sent, errno, t_send_ms);
            break;
        }
        /* Stall warning: SO_SNDTIMEO is 15s; if a frame took > STREAM_FRAME_WARN_MS,
         * the TCP window was stalled — log it so we can see iOS power-save patterns. */
        if (t_send_ms > STREAM_FRAME_WARN_MS) {
            ESP_LOGW(TAG, "[#%lu] TCP stall: frame send took %lld ms (size=%zu B).",
                     (unsigned long)frames_sent, t_send_ms, frame_len);
        } else if (frames_sent < 5 || frames_sent % 50 == 0) {
            /* Log first 5 frames and every 50th: size + send timing */
            ESP_LOGI(TAG, "[#%lu] sent %zu B in %lld ms",
                     (unsigned long)frames_sent, frame_len, t_send_ms);
        }
        frames_sent++;

        /* FPS cap: sleep until next frame slot.
         * next_wake advances by FRAME_PERIOD each call regardless of
         * how long get+send took — correct pacing with no drift. */
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(CAM_FRAME_PERIOD_MS));
    }
    ESP_LOGI(TAG, "Stream loop ended: %lu frames sent, %lu cam fails.",
             (unsigned long)frames_sent, (unsigned long)cam_fails);

cleanup:
    atomic_fetch_sub(&s_client_count, 1u);
    ESP_LOGI(TAG, "Stream client disconnected. Active: %u",
             (unsigned)atomic_load(&s_client_count));

    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

/* ── URI handler (runs in httpd task — must return immediately) ──────────────── */

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        ESP_LOGW(TAG, "/stream: auth failed.");
        web_auth_send_challenge(req);
        return ESP_OK;
    }

    /* Reject if too many streams already active */
    unsigned cur = (unsigned)atomic_load(&s_client_count);
    if (cur >= STREAM_MAX_CONCURRENT) {
        ESP_LOGW(TAG, "/stream: max concurrent clients (%d) reached.", STREAM_MAX_CONCURRENT);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Max concurrent streams reached.");
        return ESP_OK;
    }

    /* Reject immediately if camera driver is not up */
    if (!cam_driver_is_ready()) {
        ESP_LOGW(TAG, "/stream: camera not ready.");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Camera not ready.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "/stream: starting async stream (current clients: %u).", cur);

    /* Transfer socket ownership to async task */
    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        ESP_LOGE(TAG, "/stream: httpd_req_async_handler_begin failed.");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        stream_task, "stream_t",
        STREAM_TASK_STACK, async_req,
        STREAM_TASK_PRIORITY, NULL,
        1   /* Core 1: camera DMA is also on Core 1 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "/stream: xTaskCreatePinnedToCore failed.");
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }

    return ESP_OK;  /* httpd task is free immediately */
}

/* ── Snapshot handler (single JPEG per request) ─────────────────────────────
 *
 * Replaces MJPEG for iOS Safari. iOS holds the TCP receive window at 0 while
 * rendering MJPEG frames (15+ s stalls observed). A complete HTTP response
 * like a JPEG snapshot is read immediately by the browser — no stall possible.
 *
 * The JS admin panel polls /snapshot at 10 fps using img.onload as the
 * completion signal. This gives natural back-pressure: next request starts
 * only after the previous frame is decoded and painted.
 *
 * Runs in the httpd task (not async): camera acquisition (~15 ms) + JPEG
 * send (~5-50 ms) blocks httpd for ~65 ms/frame. At 10 fps, httpd is busy
 * ~65% of the time. Telemetry (GET /api/telemetry) is fast (<1 ms) and
 * will queue behind a snapshot send for at most ~65 ms — imperceptible. */
static esp_err_t snapshot_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        web_auth_send_challenge(req);
        return ESP_OK;
    }

    if (!cam_driver_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Camera not ready.");
        return ESP_OK;
    }

    cam_frame_t frame;
    esp_err_t ret = cam_pipeline_get_frame(&frame, 5000);
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "No frame available.");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ret = httpd_resp_send(req, (const char *)frame.fb->buf, (ssize_t)frame.fb->len);
    cam_pipeline_release_frame(&frame);
    return ret;
}

static const httpd_uri_t s_stream_uri = {
    .uri     = "/stream",
    .method  = HTTP_GET,
    .handler = stream_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t s_snapshot_uri = {
    .uri     = "/snapshot",
    .method  = HTTP_GET,
    .handler = snapshot_get_handler,
    .user_ctx = NULL,
};

void stream_handler_register(httpd_handle_t server)
{
    esp_err_t ret = httpd_register_uri_handler(server, &s_stream_uri);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register /stream: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "/stream registered (MJPEG, desktop Chrome).");
    }

    ret = httpd_register_uri_handler(server, &s_snapshot_uri);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register /snapshot: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "/snapshot registered (JPEG polling, iOS-safe).");
    }
}

uint32_t stream_handler_get_client_count(void)
{
    return (uint32_t)atomic_load(&s_client_count);
}
