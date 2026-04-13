/**
 * @file face_detect.cpp
 * @brief ESP-DL face detection + recognition, wrapped with a C API.
 *
 * Vision task lifecycle:
 *   1. Subscribes to frame_pool, waits for frames at ~1-2 fps (STRIDE).
 *   2. Decodes JPEG → RGB888 (full resolution or ROI crop).
 *   3. Runs HumanFaceDetect (ESPDET_PICO_224_224_FACE — fast, single-stage).
 *   4. For each detected face, runs HumanFaceRecognizer.
 *   5. Publishes face_detect_result_t under mutex.
 *   6. In ENROLLMENT mode, auto-enrolls first N faces as owner.
 *
 * Memory: models loaded to PSRAM (~400-800 KB). RGB decode buffer
 * allocated once in PSRAM (~300 KB for VGA).
 */
#include "face_detect.h"
#include "frame_pool.h"
#include "cam_config.h"
#include "watchdog.h"
#include "img_converters.h"
#include "jpeg_decoder.h"

#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>
#include <cstdio>
#include <list>
#include <vector>
#include <string>

static const char *TAG = "face_det";

/* ── Config ──────────────────────────────────────────────────────────── */
#define FACE_TASK_STACK         12288
#define FACE_TASK_PRIO          3
#define FACE_TASK_CORE          0        /* Core 0: keeps Core 1 free for cam+motion */
#define FACE_FRAME_STRIDE       28       /* ~1 fps at 28 fps cam — face detection is expensive */
#define FACE_DECODE_W           320      /* SCALE_1_2: 640/2 */
#define FACE_DECODE_H           240      /* SCALE_1_2: 480/2 */
#define FACE_ENROLLMENT_MAX     5        /* auto-enroll first N faces */
#define FACE_RECOG_THRESHOLD    0.5f     /* similarity threshold for match */
#define FACE_OWNER_NAME         "Omer"   /* enrolled owner name */
#define FACE_DB_PATH            "/data/face_db"  /* NVS-based or PSRAM */

#define FPS_RING_SIZE           16

/* ── State ──────────────────────────────────────────────────────────── */
static struct {
    HumanFaceDetect        *detector;
    HumanFaceRecognizer    *recognizer;
    uint8_t                *rgb_buf;       /* VGA RGB888: 640*480*3 = 921600 B */
    size_t                  rgb_buf_size;
    face_detect_result_t    result;
    SemaphoreHandle_t       mutex;
    bool                    enrollment_mode;
    bool                    enroll_requested;
    int64_t                 fps_ring[FPS_RING_SIZE];
    uint32_t                fps_ring_head;
} s_fd;

static bool s_initialized = false;

/* ── FPS ──────────────────────────────────────────────────────────────── */
static uint32_t fps_compute(void)
{
    int64_t now = esp_timer_get_time();
    int64_t window = now - 1000000LL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < FPS_RING_SIZE; i++) {
        if (s_fd.fps_ring[i] >= window) n++;
    }
    return n;
}

static void fps_record(void)
{
    s_fd.fps_ring[s_fd.fps_ring_head % FPS_RING_SIZE] = esp_timer_get_time();
    s_fd.fps_ring_head++;
}

/* ── Vision task ──────────────────────────────────────────────────────── */

static void face_detect_task(void *arg)
{
    watchdog_subscribe_current_task();
    frame_pool_subscribe(NULL);

    ESP_LOGI(TAG, "face_detect_task running on core %d, prio %u",
             xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));

    uint32_t last_seq = 0;

    while (true) {
        uint32_t target = last_seq + FACE_FRAME_STRIDE;
        frame_slot_t *slot = frame_pool_wait_newer(target - 1, 3000);
        if (!slot) {
            watchdog_reset();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        last_seq = slot->seq;

        int64_t t0 = esp_timer_get_time();

        /* Decode JPEG → RGB888 at half resolution (320×240).
         * Uses esp_jpeg_decode directly with SCALE_1_2 — 4× fewer pixels,
         * ~50ms decode vs ~200ms at full VGA. The face detection model's
         * ImagePreprocessor resizes to the model's input size internally. */
        static uint8_t jpeg_work[3100];  /* tjpgd workspace */
        esp_jpeg_image_cfg_t jpeg_cfg = {};
        jpeg_cfg.indata = slot->buf;
        jpeg_cfg.indata_size = slot->len;
        jpeg_cfg.outbuf = s_fd.rgb_buf;
        jpeg_cfg.outbuf_size = s_fd.rgb_buf_size;
        jpeg_cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
        jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
        jpeg_cfg.flags.swap_color_bytes = 0;
        jpeg_cfg.advanced.working_buffer = jpeg_work;
        jpeg_cfg.advanced.working_buffer_size = sizeof(jpeg_work);

        esp_jpeg_image_output_t jpeg_out = {};
        esp_err_t dec_err = esp_jpeg_decode(&jpeg_cfg, &jpeg_out);
        frame_pool_release(slot);

        bool ok = (dec_err == ESP_OK);
        uint16_t src_w = ok ? jpeg_out.width : 0;
        uint16_t src_h = ok ? jpeg_out.height : 0;

        if (!ok) {
            ESP_LOGW(TAG, "JPEG decode failed (seq=%lu)", (unsigned long)last_seq);
            watchdog_reset();
            continue;
        }

        int64_t t_decode = esp_timer_get_time();

        /* Prepare image struct for ESP-DL */
        dl::image::img_t img;
        img.data     = s_fd.rgb_buf;
        img.width    = src_w;
        img.height   = src_h;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

        /* Run face detection — guard against model not loaded */
        if (!s_fd.detector) {
            watchdog_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        auto &detect_results = s_fd.detector->run(img);

        int64_t t_detect = esp_timer_get_time();

        /* Run recognition on each detected face */
        std::vector<dl::recognition::result_t> recog_results;
        if (!detect_results.empty() && s_fd.recognizer) {
            recog_results = s_fd.recognizer->recognize(img, detect_results);
        }

        /* Auto-enrollment in enrollment mode */
        if (s_fd.enrollment_mode && !detect_results.empty() && s_fd.recognizer) {
            int enrolled = s_fd.recognizer->get_num_feats();
            if (enrolled < FACE_ENROLLMENT_MAX) {
                esp_err_t enr = s_fd.recognizer->enroll(img, detect_results);
                if (enr == ESP_OK) {
                    enrolled = s_fd.recognizer->get_num_feats();
                    ESP_LOGI(TAG, "Auto-enrolled face #%d/%d", enrolled, FACE_ENROLLMENT_MAX);
                    if (enrolled >= FACE_ENROLLMENT_MAX) {
                        s_fd.enrollment_mode = false;
                        ESP_LOGI(TAG, "Enrollment complete — switching to GUARD mode.");
                    }
                }
            }
        }

        /* Manual enrollment trigger */
        if (s_fd.enroll_requested && !detect_results.empty() && s_fd.recognizer) {
            s_fd.recognizer->enroll(img, detect_results);
            s_fd.enroll_requested = false;
            ESP_LOGI(TAG, "Manual enrollment done (total: %d)", s_fd.recognizer->get_num_feats());
        }

        int64_t t_recog = esp_timer_get_time();

        /* ── Publish results ────────────────────────────────────────── */
        fps_record();
        uint32_t fps1 = fps_compute();
        int enrolled_count = s_fd.recognizer ? s_fd.recognizer->get_num_feats() : 0;

        xSemaphoreTake(s_fd.mutex, portMAX_DELAY);
        s_fd.result.seq = last_seq;
        s_fd.result.fps_1s = fps1;
        s_fd.result.enrolled_count = (uint32_t)enrolled_count;
        s_fd.result.enrollment_mode = s_fd.enrollment_mode;
        s_fd.result.face_count = 0;

        /* Scale factor: decode was at SCALE_1_2, so bbox coords are in
         * 320×240 space. Multiply by 2 to map back to source 640×480. */
        const int scale_up = 2;
        auto recog_it = recog_results.begin();
        for (auto &det : detect_results) {
            if (s_fd.result.face_count >= FACE_MAX_RESULTS) break;
            face_result_t *f = &s_fd.result.faces[s_fd.result.face_count];
            f->x = (uint16_t)(det.box[0] * scale_up);
            f->y = (uint16_t)(det.box[1] * scale_up);
            f->w = (uint16_t)((det.box[2] - det.box[0]) * scale_up);
            f->h = (uint16_t)((det.box[3] - det.box[1]) * scale_up);
            f->score = det.score;
            f->id = 0;
            f->similarity = 0.0f;
            strncpy(f->name, "?", sizeof(f->name));

            if (recog_it != recog_results.end()) {
                if (recog_it->similarity >= FACE_RECOG_THRESHOLD) {
                    f->id = (int16_t)recog_it->id;
                    f->similarity = recog_it->similarity;
                    snprintf(f->name, sizeof(f->name), "%s", FACE_OWNER_NAME);
                } else {
                    f->id = -1;
                    f->similarity = recog_it->similarity;
                    strncpy(f->name, "Unknown", sizeof(f->name));
                }
                ++recog_it;
            }
            s_fd.result.face_count++;
        }
        xSemaphoreGive(s_fd.mutex);

        /* Log */
        if (s_fd.result.face_count > 0 || last_seq % 60 == 0) {
            ESP_LOGI(TAG, "[#%lu] dec=%lld ms det=%lld ms rec=%lld ms faces=%lu enrolled=%d %s",
                     (unsigned long)last_seq,
                     (t_decode - t0) / 1000,
                     (t_detect - t_decode) / 1000,
                     (t_recog - t_detect) / 1000,
                     (unsigned long)s_fd.result.face_count,
                     enrolled_count,
                     s_fd.enrollment_mode ? "[ENROLLING]" : "[GUARD]");
        }

        watchdog_reset();
    }
}

/* ── Public C API ───────────────────────────────────────────────────── */

extern "C" {

esp_err_t face_detect_init(void)
{
    if (s_initialized) return ESP_OK;

    memset(&s_fd, 0, sizeof(s_fd));
    s_fd.mutex = xSemaphoreCreateMutex();
    if (!s_fd.mutex) return ESP_ERR_NO_MEM;

    /* Allocate RGB888 decode buffer in PSRAM (320×240 @ SCALE_1_2) */
    s_fd.rgb_buf_size = FACE_DECODE_W * FACE_DECODE_H * 3;
    s_fd.rgb_buf = (uint8_t *)heap_caps_malloc(s_fd.rgb_buf_size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fd.rgb_buf) {
        ESP_LOGE(TAG, "RGB buffer alloc failed (%u B)", (unsigned)s_fd.rgb_buf_size);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize face detection model.
     * Use MSRMNP_S8_V1 — the two-stage model that is compiled into flash
     * via CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1=y in sdkconfig.
     * lazy_load=false → model loads NOW so we can validate it here. */
    ESP_LOGI(TAG, "Loading face detection model (MSRMNP_S8_V1)...");
    s_fd.detector = new (std::nothrow) HumanFaceDetect(
        HumanFaceDetect::MSRMNP_S8_V1, /*lazy_load=*/false);
    if (!s_fd.detector) {
        ESP_LOGE(TAG, "Face detection model alloc failed");
        return ESP_FAIL;
    }
    /* Verify the internal model actually loaded — new(nothrow) returns
     * non-null even when load_model() fails silently inside the
     * constructor, leaving m_model as garbage. get_raw_model returns the
     * internal pointer; if null, the model binary wasn't found in flash. */
    if (!s_fd.detector->get_raw_model(0)) {
        ESP_LOGE(TAG, "Face detection model load FAILED — check sdkconfig "
                 "(CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1 must be y)");
        delete s_fd.detector;
        s_fd.detector = nullptr;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Face detection model loaded OK.");

    /* Initialize face recognition — detection only (no DB).
     * Full recognition with enrollment requires SPIFFS or NVS-backed
     * storage which is not yet mounted. For now, detection + embedding
     * extraction work without a persistent DB. We skip the recognizer
     * to avoid the "Failed to open db" crash path, and add it later
     * once SPIFFS is properly configured. */
    ESP_LOGI(TAG, "Face recognition: detection-only mode (no persistent DB yet).");
    s_fd.recognizer = nullptr;

    int enrolled = s_fd.recognizer ? s_fd.recognizer->get_num_feats() : 0;
    s_fd.enrollment_mode = (enrolled < FACE_ENROLLMENT_MAX);
    s_fd.enroll_requested = false;

    s_initialized = true;
    ESP_LOGI(TAG, "Face detect init OK  enrolled=%d  mode=%s",
             enrolled, s_fd.enrollment_mode ? "ENROLLMENT" : "GUARD");
    return ESP_OK;
}

esp_err_t face_detect_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    BaseType_t r = xTaskCreatePinnedToCore(
        face_detect_task, "face_det",
        FACE_TASK_STACK, NULL,
        FACE_TASK_PRIO, NULL,
        FACE_TASK_CORE);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

void face_detect_get_result(face_detect_result_t *out)
{
    if (!out || !s_initialized) { if (out) memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_fd.mutex, portMAX_DELAY);
    memcpy(out, &s_fd.result, sizeof(face_detect_result_t));
    xSemaphoreGive(s_fd.mutex);
}

void face_detect_enroll_next(void)
{
    s_fd.enroll_requested = true;
}

esp_err_t face_detect_clear_enrollments(void)
{
    if (!s_fd.recognizer) return ESP_ERR_INVALID_STATE;
    esp_err_t r = s_fd.recognizer->clear_all_feats();
    s_fd.enrollment_mode = true;
    return r;
}

size_t face_detect_build_json(char *buf, size_t buflen)
{
    if (!buf || buflen < 64 || !s_initialized) return 0;

    xSemaphoreTake(s_fd.mutex, portMAX_DELAY);
    face_detect_result_t r = s_fd.result;
    xSemaphoreGive(s_fd.mutex);

    int n = snprintf(buf, buflen,
        "{\"t\":\"face\",\"seq\":%lu,\"fps\":%lu,\"enrolled\":%lu,\"mode\":\"%s\",\"faces\":[",
        (unsigned long)r.seq,
        (unsigned long)r.fps_1s,
        (unsigned long)r.enrolled_count,
        r.enrollment_mode ? "enrollment" : "guard");
    if (n < 0 || (size_t)n >= buflen) return 0;

    for (uint32_t i = 0; i < r.face_count; i++) {
        const face_result_t *f = &r.faces[i];
        int m = snprintf(buf + n, buflen - (size_t)n,
            "%s{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,"
            "\"score\":%.2f,\"id\":%d,\"sim\":%.2f,\"name\":\"%s\"}",
            (i == 0) ? "" : ",",
            (unsigned)f->x, (unsigned)f->y,
            (unsigned)f->w, (unsigned)f->h,
            f->score, (int)f->id, f->similarity, f->name);
        if (m < 0 || (size_t)(n + m) >= buflen) return 0;
        n += m;
    }

    int m = snprintf(buf + n, buflen - (size_t)n, "]}");
    if (m < 0 || (size_t)(n + m) >= buflen) return 0;
    n += m;
    return (size_t)n;
}

} /* extern "C" */
