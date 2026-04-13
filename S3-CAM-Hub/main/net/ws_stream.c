/**
 * @file ws_stream.c
 * @brief WebSocket binary video stream with motion overlay sidechannel.
 *
 * Per-client lifecycle:
 *   1. GET /ws/stream triggers the URI handler on the httpd task.
 *   2. Handler validates auth (cookie or Basic). If bad → 401, handshake
 *      aborts.
 *   3. httpd completes the WebSocket handshake automatically (Sec-WebSocket
 *      headers). Our handler then:
 *        - allocates a client slot (max WS_STREAM_MAX_CLIENTS),
 *        - grabs the socket fd via httpd_req_to_sockfd,
 *        - spawns ws_sender_task pinned to Core 0 prio 5,
 *        - returns ESP_OK. httpd keeps the socket open after return.
 *   4. ws_sender_task subscribes to frame_pool, loops waiting for newer
 *      frames, and pushes them as WS binary via httpd_ws_send_frame_async.
 *      Every WS_MOTION_EVERY_N frames it also sends a WS text frame with
 *      the motion JSON.
 *   5. On any send error or no-frame timeout → task tears down the
 *      session via httpd_sess_trigger_close and frees its slot.
 *
 * Thread-safety:
 *   - s_clients array is guarded by s_clients_mutex.
 *   - The count of active clients is exposed via ws_stream_get_client_count.
 */
#include "ws_stream.h"
#include "web_auth.h"
#include "frame_pool.h"
#include "motion_detect.h"
#include "face_detect.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ws_stream";

#define WS_STREAM_MAX_CLIENTS       2
#define WS_SENDER_STACK             6144
#define WS_SENDER_PRIO              5
#define WS_SENDER_CORE              0
#define WS_FRAME_WAIT_MS            1500   /* timeout per wait_newer */
#define WS_NO_FRAME_EXIT_MS         8000   /* kill session after this */
#define WS_MOTION_EVERY_N           1      /* send motion JSON per frame */
#define WS_MOTION_JSON_BUFSZ        1024

typedef struct {
    bool            in_use;
    httpd_handle_t  server;
    int             fd;
    TaskHandle_t    task;
    uint32_t        frames_sent;
    int64_t         connected_us;
} ws_client_t;

static ws_client_t      s_clients[WS_STREAM_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mutex = NULL;
static atomic_uint       s_client_count = 0;

/* Used by the sender task to locate its own slot index via pvParameters. */
typedef struct {
    httpd_handle_t server;
    int            fd;
    int            slot_index;
} sender_args_t;

/* ── Client slot management ────────────────────────────────────────────── */

static void clients_mutex_init_once(void)
{
    if (!s_clients_mutex) {
        s_clients_mutex = xSemaphoreCreateMutex();
    }
}

static int clients_acquire_slot(httpd_handle_t server, int fd)
{
    clients_mutex_init_once();
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < WS_STREAM_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            s_clients[i].in_use       = true;
            s_clients[i].server       = server;
            s_clients[i].fd           = fd;
            s_clients[i].task         = NULL;
            s_clients[i].frames_sent  = 0;
            s_clients[i].connected_us = esp_timer_get_time();
            idx = i;
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
    if (idx >= 0) atomic_fetch_add(&s_client_count, 1u);
    return idx;
}

static void clients_release_slot(int idx)
{
    if (idx < 0 || idx >= WS_STREAM_MAX_CLIENTS) return;
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    if (s_clients[idx].in_use) {
        s_clients[idx].in_use = false;
        s_clients[idx].server = NULL;
        s_clients[idx].fd     = -1;
        s_clients[idx].task   = NULL;
        atomic_fetch_sub(&s_client_count, 1u);
    }
    xSemaphoreGive(s_clients_mutex);
}

uint32_t ws_stream_get_client_count(void)
{
    return (uint32_t)atomic_load(&s_client_count);
}

/* ── Sender task ───────────────────────────────────────────────────────── */

static void ws_sender_task(void *pv)
{
    sender_args_t args = *(sender_args_t *)pv;
    free(pv);

    ESP_LOGI(TAG, "sender[%d] start fd=%d core=%d prio=%u",
             args.slot_index, args.fd, xPortGetCoreID(),
             (unsigned)uxTaskPriorityGet(NULL));

    frame_pool_subscribe(NULL);

    uint32_t last_seq = 0;
    int64_t  last_frame_us = esp_timer_get_time();
    uint32_t motion_tick = 0;
    char *motion_json = (char *)malloc(WS_MOTION_JSON_BUFSZ);
    if (!motion_json) {
        ESP_LOGE(TAG, "sender[%d] motion json alloc failed", args.slot_index);
    }

    bool terminate = false;
    while (!terminate) {
        frame_slot_t *slot = frame_pool_wait_newer(last_seq, WS_FRAME_WAIT_MS);
        if (!slot) {
            int64_t idle_ms = (esp_timer_get_time() - last_frame_us) / 1000;
            if (idle_ms > WS_NO_FRAME_EXIT_MS) {
                ESP_LOGW(TAG, "sender[%d] no frame for %lld ms — exit",
                         args.slot_index, idle_ms);
                break;
            }
            continue;
        }
        last_seq = slot->seq;
        last_frame_us = esp_timer_get_time();

        /* ── Binary frame (the JPEG) ─────────────────────────────────── */
        httpd_ws_frame_t wsf;
        memset(&wsf, 0, sizeof(wsf));
        wsf.type    = HTTPD_WS_TYPE_BINARY;
        wsf.payload = slot->buf;
        wsf.len     = slot->len;

        esp_err_t err = httpd_ws_send_frame_async(args.server, args.fd, &wsf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sender[%d] binary send failed: %s — exit",
                     args.slot_index, esp_err_to_name(err));
            frame_pool_release(slot);
            terminate = true;
            break;
        }

        frame_pool_release(slot);

        s_clients[args.slot_index].frames_sent++;

        /* ── Motion + Face JSON sidechannel (text frames) ──────────── */
        motion_tick++;
        if (motion_json && (motion_tick % WS_MOTION_EVERY_N == 0)) {
            /* Motion data */
            size_t n = motion_detect_build_json(motion_json, WS_MOTION_JSON_BUFSZ);
            if (n > 0) {
                httpd_ws_frame_t tf;
                memset(&tf, 0, sizeof(tf));
                tf.type    = HTTPD_WS_TYPE_TEXT;
                tf.payload = (uint8_t *)motion_json;
                tf.len     = n;
                err = httpd_ws_send_frame_async(args.server, args.fd, &tf);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "sender[%d] motion text send failed: %s — exit",
                             args.slot_index, esp_err_to_name(err));
                    terminate = true;
                    break;
                }
            }
            /* Face detection data */
            n = face_detect_build_json(motion_json, WS_MOTION_JSON_BUFSZ);
            if (n > 0) {
                httpd_ws_frame_t tf;
                memset(&tf, 0, sizeof(tf));
                tf.type    = HTTPD_WS_TYPE_TEXT;
                tf.payload = (uint8_t *)motion_json;
                tf.len     = n;
                err = httpd_ws_send_frame_async(args.server, args.fd, &tf);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "sender[%d] face text send failed: %s — exit",
                             args.slot_index, esp_err_to_name(err));
                    terminate = true;
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "sender[%d] exit — %lu frames sent",
             args.slot_index, (unsigned long)s_clients[args.slot_index].frames_sent);

    if (motion_json) free(motion_json);

    /* CRITICAL: unsubscribe BEFORE deleting the task. If we don't,
     * frame_pool_publish keeps calling xTaskNotifyGive on a freed TCB
     * → use-after-free → task watchdog crash. */
    frame_pool_unsubscribe(NULL);

    httpd_sess_trigger_close(args.server, args.fd);
    clients_release_slot(args.slot_index);

    vTaskDelete(NULL);
}

/* ── URI handler: completes handshake + spawns sender ──────────────────── */

static esp_err_t ws_stream_handler(httpd_req_t *req)
{
    /* On handshake, method is GET. Authenticate before upgrade. */
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "/ws/stream: handshake request (fd=%d)",
                 httpd_req_to_sockfd(req));

        /* Auth path 1: explicit ?token=... query param. Used by the
         * embedded admin panel JS to bypass cookie policy differences
         * on WebSocket upgrade across mobile browsers. */
        bool authed = false;
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 256) {
            char qbuf[256] = {0};
            if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
                char token[64] = {0};
                if (httpd_query_key_value(qbuf, "token",
                                          token, sizeof(token)) == ESP_OK) {
                    authed = web_auth_token_matches(token);
                    if (authed) {
                        ESP_LOGI(TAG, "/ws/stream: auth via token OK");
                    }
                }
            }
        }

        /* Auth path 2: cookie/Basic (original flow). */
        if (!authed) authed = web_auth_check(req);

        if (!authed) {
            char cookie_hdr[192] = {0};
            char auth_hdr[128] = {0};
            esp_err_t ck = httpd_req_get_hdr_value_str(req, "Cookie",
                                                       cookie_hdr, sizeof(cookie_hdr));
            esp_err_t au = httpd_req_get_hdr_value_str(req, "Authorization",
                                                       auth_hdr, sizeof(auth_hdr));
            ESP_LOGW(TAG, "/ws/stream: AUTH FAILED  cookie=%s  auth=%s  qlen=%u",
                     (ck == ESP_OK) ? "present" : "missing",
                     (au == ESP_OK) ? "present" : "missing",
                     (unsigned)qlen);
            web_auth_send_challenge(req);
            return ESP_OK;
        }
        if (atomic_load(&s_client_count) >= WS_STREAM_MAX_CLIENTS) {
            ESP_LOGW(TAG, "/ws/stream: max clients reached");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "Max clients");
            return ESP_OK;
        }
        httpd_handle_t server = req->handle;
        int fd = httpd_req_to_sockfd(req);
        if (fd < 0) {
            ESP_LOGE(TAG, "/ws/stream: invalid socket");
            return ESP_FAIL;
        }
        int slot = clients_acquire_slot(server, fd);
        if (slot < 0) {
            ESP_LOGW(TAG, "/ws/stream: no free slot");
            return ESP_FAIL;
        }
        sender_args_t *args = (sender_args_t *)malloc(sizeof(sender_args_t));
        if (!args) {
            clients_release_slot(slot);
            return ESP_ERR_NO_MEM;
        }
        args->server     = server;
        args->fd         = fd;
        args->slot_index = slot;

        BaseType_t r = xTaskCreatePinnedToCore(
            ws_sender_task, "ws_send",
            WS_SENDER_STACK, args,
            WS_SENDER_PRIO, &s_clients[slot].task,
            WS_SENDER_CORE);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "ws_sender_task create failed");
            free(args);
            clients_release_slot(slot);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "/ws/stream: client connected fd=%d slot=%d (total=%u)",
                 fd, slot, (unsigned)atomic_load(&s_client_count));
        /* httpd keeps the socket open after return. */
        return ESP_OK;
    }

    /* Control / data frames from client: drain (we don't expect any). */
    httpd_ws_frame_t wsf;
    memset(&wsf, 0, sizeof(wsf));
    wsf.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &wsf, 0);
    if (ret == ESP_OK && wsf.len > 0 && wsf.len < 256) {
        uint8_t tmp[256];
        wsf.payload = tmp;
        httpd_ws_recv_frame(req, &wsf, sizeof(tmp));
    }
    return ESP_OK;
}

static const httpd_uri_t s_ws_uri = {
    .uri          = "/ws/stream",
    .method       = HTTP_GET,
    .handler      = ws_stream_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true,
};

void ws_stream_register(httpd_handle_t server)
{
    clients_mutex_init_once();
    esp_err_t ret = httpd_register_uri_handler(server, &s_ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws/stream: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "/ws/stream registered (max %d clients, binary+motion)",
             WS_STREAM_MAX_CLIENTS);
}
