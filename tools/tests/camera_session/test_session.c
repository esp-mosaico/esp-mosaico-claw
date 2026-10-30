/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef int esp_err_t;
typedef int esp_gsp_err_t;
typedef void *esp_gsp_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
typedef int camera_vision_mode_t;
typedef void *mosaico_camera_handle_t;
typedef void *jpeg_encoder_handle_t;
enum { ESP_OK = 0, ESP_GSP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 257, ESP_ERR_INVALID_ARG = 258,
       ESP_ERR_INVALID_STATE = 259, ESP_ERR_INVALID_SIZE = 260,
       ESP_ERR_NOT_FOUND = 261,
       ESP_ERR_TIMEOUT = 263, ESP_ERR_NOT_FINISHED = 268 };
enum { MOSAIC_CAMERA_BUFFERS = 2, MOSAIC_CAMERA_THUMBNAIL_BUFFERS = 2,
       MOSAIC_CAMERA_CAPTURE_PATH_MAX = 256, MOSAIC_CAMERA_FRAME_BYTES = 460800,
       MOSAIC_CAMERA_W = 480, MOSAIC_CAMERA_H = 480, MOSAIC_CAMERA_JPEG_QUALITY = 90,
       MOSAIC_CAMERA_STRIDE = 960, MOSAIC_CAMERA_FRAME_DELAY_MS = 5,
       MOSAIC_CAMERA_JPEG_BUFFER_WAIT_US = 1000000,
       MOSAIC_CAMERA_JPEG_TIMEOUT_MS = 1000, MOSAIC_CAMERA_STOP_TIMEOUT_MS = 35000,
       CAMERA_VISION_MODE_OFF = 0, JPEG_ENC_ALLOC_OUTPUT_BUFFER = 1,
       JPEG_ENCODE_IN_FORMAT_RGB565 = 1, JPEG_DOWN_SAMPLING_YUV420 = 1,
       GSP_BIND_CAMERA_CANVAS = 1, GSP_BIND_CAMERA_ALBUM_THUMBNAIL = 2,
       GSP_BIND_CAMERA_RECOGNITION_STATUS = 3 };
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
typedef struct { int buffer_direction; } jpeg_encode_memory_alloc_cfg_t;
typedef struct { int intr_priority, timeout_ms; } jpeg_encode_engine_cfg_t;
typedef struct { int height, width, src_type, sub_sample, image_quality;
                 bool pixel_reverse; } jpeg_encode_cfg_t;

#include "camera_types.h"
#include "camera_status_type.h"

static unsigned allocations, allocation_calls, fail_allocation, engine_count;
static unsigned join_waits, semaphore_deletes;
static bool task_finished = true, release_pixels = true;
static int canvas_error, flush_error, save_fault;
static unsigned stop_mask;
static int native_default_error, native_fixed_error;
static unsigned native_failures, native_close_calls;
static bool preview_vote;
static char last_status[64];
static int64_t fake_time;
static unsigned delay_count, release_after_delay, cancel_after_delay;
static unsigned push_failures, pushes, encode_calls;
static bool borrow_test;
static bool submit_busy;
static unsigned borrow_source;
static void (*held_release)(void *);
static void *held_ctx;
static int64_t esp_timer_get_time(void) { return fake_time; }
static void vTaskDelay(unsigned ms)
{
    fake_time += (int64_t)ms * 1000;
    ++delay_count;
    if (cancel_after_delay && delay_count >= cancel_after_delay)
        s_camera.stopping = true;
    if (held_release && release_after_delay && delay_count >= release_after_delay) {
        held_release(held_ctx); held_release = NULL;
    }
}

static void *allocate(size_t size)
{
    if (++allocation_calls == fail_allocation) return NULL;
    void *p = malloc(size);
    if (p) ++allocations;
    return p;
}
static void release(void *p)
{
    if (p) { assert(allocations > 0); --allocations; }
    free(p);
}
static uint8_t *camera_alloc_preview_pixels(void)
{ return allocate(MOSAIC_CAMERA_FRAME_BYTES); }
static int jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *config,
                                   jpeg_encoder_handle_t *out)
{
    (void)config;
    if (save_fault == 1) return ESP_FAIL;
    *out = malloc(1);
    assert(*out);
    ++engine_count;
    return ESP_OK;
}
static int jpeg_encoder_process(jpeg_encoder_handle_t encoder,
    const jpeg_encode_cfg_t *config, const uint8_t *input, uint32_t input_size,
    uint8_t *output, uint32_t capacity, uint32_t *size)
{
    assert(encoder && config && input && input_size == MOSAIC_CAMERA_FRAME_BYTES);
    ++encode_calls;
    assert(output != input && capacity >= MOSAIC_CAMERA_FRAME_BYTES);
    if (borrow_test) {
        assert(output == s_camera.frames[1 - borrow_source].pixels);
        assert(s_camera.frames[1 - borrow_source].state == MOSAIC_FRAME_WRITING);
        assert(!held_release);
        for (unsigned i = 0; i < 16; ++i) assert(input[i] == 0x5a);
    }
    if (save_fault == 2) return ESP_FAIL;
    memcpy(output, "JPEGDATA", 8);
    *size = save_fault == 3 ? 0 : 8;
    return ESP_OK;
}
static int jpeg_del_encoder_engine(jpeg_encoder_handle_t encoder)
{ free(encoder); --engine_count; return ESP_OK; }
static size_t fault_write(const void *p, size_t size, size_t count, FILE *file)
{ return fwrite(p, size, save_fault == 4 ? count / 2 : count, file); }
static int fault_close(FILE *file)
{ int result = fclose(file); return save_fault == 5 ? EOF : result; }
static FILE *fault_fdopen(int fd, const char *mode)
{ return save_fault == 6 ? NULL : fdopen(fd, mode); }
static int xSemaphoreTake(void *semaphore, unsigned wait)
{
    if (semaphore == s_camera.submit_lock && borrow_test) {
        assert(wait == 0);
        if (submit_busy) return 0;
    }
    if (semaphore == s_camera.task_done) { ++join_waits; return task_finished; }
    return pdTRUE;
}
static void xSemaphoreGive(void *semaphore) { (void)semaphore; }
static void vSemaphoreDelete(void *semaphore)
{ ++semaphore_deletes; release(semaphore); }
static int esp_gsp_canvas_stop(void *ui, unsigned bind)
{ (void)ui; if (!canvas_error) stop_mask |= 1U << bind; return canvas_error; }
static int esp_gsp_canvas_try_push(void *ui, unsigned bind, const void *pixels,
    unsigned stride, void (*release_cb)(void *), void *ctx)
{
    (void)ui; (void)release_cb; (void)ctx;
    assert(bind == GSP_BIND_CAMERA_CANVAS && stride == MOSAIC_CAMERA_STRIDE);
    ++pushes;
    if (push_failures) { --push_failures; return ESP_FAIL; }
    assert(pixels == s_camera.frames[borrow_source].pixels);
    return ESP_GSP_OK;
}
static int esp_gsp_flush(void *ui, unsigned wait)
{
    (void)ui; (void)wait;
    if (!flush_error && release_pixels) {
        if (stop_mask & (1U << GSP_BIND_CAMERA_CANVAS))
            for (unsigned i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i)
                s_camera.frames[i].state = MOSAIC_FRAME_FREE;
        if (stop_mask & (1U << GSP_BIND_CAMERA_ALBUM_THUMBNAIL))
            for (unsigned i = 0; i < MOSAIC_CAMERA_THUMBNAIL_BUFFERS; ++i)
                s_camera.thumbnails[i].busy = false;
    }
    if (!flush_error) stop_mask = 0;
    return flush_error;
}
static void camera_release_thumbnail_buffers(void)
{
    for (unsigned i = 0; i < MOSAIC_CAMERA_THUMBNAIL_BUFFERS; ++i) {
        assert(!s_camera.thumbnails[i].busy);
        release(s_camera.thumbnails[i].pixels);
        s_camera.thumbnails[i].pixels = NULL;
    }
}
static void camera_vision_deinit(void) {}
static int mosaico_camera_service_get_default(mosaico_camera_handle_t *out)
{ *out = native_default_error ? NULL : (void *)(uintptr_t)1; return native_default_error; }
static int mosaico_camera_close(mosaico_camera_handle_t camera)
{
    assert(camera); ++native_close_calls;
    if (native_fixed_error) return native_fixed_error;
    if (native_failures) { --native_failures; return ESP_FAIL; }
    return ESP_OK;
}
static void mosaic_ui_set_camera_preview_active(bool active) { preview_vote = active; }
static int esp_gsp_set_text(void *ui, unsigned bind, const char *text)
{ (void)ui; assert(bind == GSP_BIND_CAMERA_RECOGNITION_STATUS); snprintf(last_status, sizeof(last_status), "%s", text); return ESP_OK; }

#define free release
#define heap_caps_free release
#define fwrite fault_write
#define fclose fault_close
#define fdopen fault_fdopen
#include "camera_under_test.h"
#undef free
#undef fwrite
#undef fclose
#undef fdopen

static void clear_workspace(void)
{
    for (unsigned i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i)
        release(s_camera.frames[i].pixels);
    memset(&s_camera, 0, sizeof(s_camera));
    assert(allocations == 0 && engine_count == 0);
    fail_allocation = 0;
    held_release = NULL; held_ctx = NULL;
    fake_time = 0; delay_count = 0; release_after_delay = 0;
    cancel_after_delay = 0; push_failures = 0; pushes = 0; borrow_test = false;
    borrow_source = 0;
    submit_busy = false;
}

static void test_admission(void)
{
    for (unsigned fail = 1; fail <= 2; ++fail) {
        allocation_calls = 0;
        fail_allocation = fail;
        assert(camera_ensure_preview_buffers(2) == ESP_ERR_NO_MEM);
        assert(allocations == 0);
        assert(s_camera.frames[0].pixels == NULL && s_camera.frames[1].pixels == NULL);
        fail_allocation = 0;
        assert(camera_ensure_preview_buffers(2) == ESP_OK);
        const unsigned calls = allocation_calls;
        assert(camera_ensure_preview_buffers(2) == ESP_OK && allocation_calls == calls);
        clear_workspace();
    }
    /* An already-owned preview is not rolled back by a later admission failure. */
    s_camera.frames[0].pixels = allocate(MOSAIC_CAMERA_FRAME_BYTES);
    uint8_t *owned = s_camera.frames[0].pixels;
    fail_allocation = allocation_calls + 1;
    assert(camera_ensure_preview_buffers(2) == ESP_ERR_NO_MEM);
    assert(allocations == 1 && s_camera.frames[0].pixels == owned);
    clear_workspace();
    puts("PASS all workspace allocation failures roll back only new allocations");
}

static void test_inactive_startup_status(void)
{
    mosaic_camera_capture_status_t status;
    assert(mosaic_camera_get_capture_status(&status) == ESP_OK);
    assert(!status.ready && status.startup_result == ESP_ERR_INVALID_STATE);
    s_camera.startup_result = ESP_ERR_NO_MEM;
    assert(mosaic_camera_get_capture_status(&status) == ESP_OK && status.startup_result == ESP_ERR_NO_MEM);
    s_camera.started = true; s_camera.startup_result = ESP_ERR_NOT_FINISHED;
    assert(mosaic_camera_get_capture_status(&status) == ESP_OK && status.startup_result == ESP_ERR_NOT_FINISHED);
    memset(&s_camera, 0, sizeof(s_camera));
    puts("PASS a deferred start that never ran remains retryable without discarding its original error");
}

static void test_save(const char *directory)
{
    assert(camera_ensure_preview_buffers(2) == ESP_OK);
    const unsigned calls = allocation_calls;
    char path[1024];
    for (save_fault = 0; save_fault <= 6; ++save_fault) {
        snprintf(path, sizeof(path), "%s/photo-%d.jpg", directory, save_fault);
        int result = camera_save_jpeg(path, s_camera.frames[0].pixels,
                                      s_camera.frames[1].pixels, MOSAIC_CAMERA_FRAME_BYTES);
        assert(engine_count == 0 && allocations == 2 && allocation_calls == calls);
        struct stat info;
        if (save_fault == 0) {
            assert(result == ESP_OK && stat(path, &info) == 0 && info.st_size == 8);
            unlink(path);
        } else {
            assert(result != ESP_OK && access(path, F_OK) != 0);
        }
    }
    save_fault = 0;
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite("KEEP", 1, 4, file) == 4 && fclose(file) == 0);
    assert(camera_save_jpeg(path, s_camera.frames[0].pixels,
                            s_camera.frames[1].pixels, MOSAIC_CAMERA_FRAME_BYTES) != ESP_OK);
    char data[4]; file = fopen(path, "rb"); assert(file);
    assert(fread(data, 1, 4, file) == 4 && !memcmp(data, "KEEP", 4)); fclose(file); unlink(path);
    assert(camera_save_jpeg(path, s_camera.frames[0].pixels,
                            s_camera.frames[1].pixels, MOSAIC_CAMERA_FRAME_BYTES) == ESP_OK); unlink(path);
    assert(allocation_calls == calls && engine_count == 0);
    clear_workspace();
    puts("PASS failed photos use supplied output, remove residues and protect old files");
}

static void test_released_preview_as_jpeg_output(const char *directory)
{
    for (unsigned scenario = 0; scenario < 15; ++scenario) {
        assert(camera_ensure_preview_buffers(2) == ESP_OK);
        s_camera.started = true;
        s_camera.submit_lock = allocate(1);
        borrow_source = scenario == 11 ? 1 : 0;
        const unsigned other = 1 - borrow_source;
        s_camera.frames[borrow_source].state = MOSAIC_FRAME_WRITING;
        s_camera.frames[other].state = MOSAIC_FRAME_IN_FLIGHT;
        s_camera.preview_resume_time_us = 300000;
        memset(s_camera.frames[borrow_source].pixels, 0x5a, 16);
        held_release = frame_released; held_ctx = (void *)(uintptr_t)other;
        release_after_delay = 4;
        borrow_test = true;
        save_fault = scenario == 1 ? 1 : 0;
        if (scenario >= 6 && scenario <= 10) save_fault = (int)scenario - 4;
        if (scenario == 2) push_failures = 3;
        if (scenario == 3 || scenario == 4) release_after_delay = 0;
        if (scenario == 4) cancel_after_delay = 1;
        if (scenario == 12) { push_failures = 1000; release_after_delay = 0; }
        if (scenario == 13) s_camera.stopping = true;
        if (scenario == 14) { submit_busy = true; release_after_delay = 0; }
        if (scenario == 5) {
            s_camera.frames[1].state = MOSAIC_FRAME_FREE;
            held_release = NULL;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/borrow-%u.jpg", directory, scenario);
        const unsigned calls = allocation_calls, encoded_before = encode_calls;
        const int result = camera_save_frame(NULL, path, borrow_source);
        assert(allocation_calls == calls && engine_count == 0);
        assert(s_camera.preview_resume_time_us == 300000);
        for (unsigned i = 0; i < 16; ++i) assert(s_camera.frames[borrow_source].pixels[i] == 0x5a);
        if (scenario == 3 || scenario == 4 || scenario >= 12) {
            assert(result == (scenario == 3 || scenario == 12 || scenario == 14 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE));
            assert(encode_calls == encoded_before && access(path, F_OK) != 0);
            assert(s_camera.frames[other].state == MOSAIC_FRAME_IN_FLIGHT);
        } else {
            assert(s_camera.frames[other].state == MOSAIC_FRAME_FREE);
            if (scenario == 1 || (scenario >= 6 && scenario <= 10)) {
                assert(result != ESP_OK && access(path, F_OK) != 0);
            } else {
                assert(result == ESP_OK);
                assert(encode_calls == encoded_before + 1 && access(path, F_OK) == 0);
                unlink(path);
            }
            if (scenario == 2) assert(pushes >= 4);
        }
        release(s_camera.submit_lock); s_camera.submit_lock = NULL;
        clear_workspace();
        save_fault = 0;
    }
    puts("PASS JPEG waits for preview retirement, keeps source immutable, retries rejection and cancels without allocation");
}

static void test_retirement(void)
{
    assert(camera_ensure_preview_buffers(2) == ESP_OK);
    s_camera.started = true;
    s_camera.task_done = allocate(1);
    s_camera.submit_lock = allocate(1);
    s_camera.frames[0].state = MOSAIC_FRAME_IN_FLIGHT;
    s_camera.thumbnails[0].pixels = allocate(48 * 48 * 2);
    s_camera.thumbnails[0].busy = true;
    const unsigned owned = allocations;
    task_finished = false;
    assert(mosaic_camera_stop(NULL) == ESP_ERR_TIMEOUT);
    assert(allocations == owned && semaphore_deletes == 0 && !s_camera.task_joined);
    task_finished = true;
    canvas_error = ESP_FAIL;
    assert(mosaic_camera_stop(NULL) == ESP_FAIL && s_camera.task_joined);
    const unsigned waits = join_waits;
    canvas_error = 0;
    flush_error = ESP_FAIL;
    assert(mosaic_camera_stop(NULL) == ESP_ERR_NOT_FINISHED);
    assert(join_waits == waits && allocations == owned && semaphore_deletes == 0);
    flush_error = 0;
    release_pixels = false;
    assert(mosaic_camera_stop(NULL) == ESP_ERR_NOT_FINISHED && allocations == owned);
    release_pixels = true;
    assert(mosaic_camera_stop(NULL) == ESP_OK);
    assert(join_waits == waits && allocations == 0 && semaphore_deletes == 2);
    assert(!s_camera.started && !s_camera.frames[0].pixels);
    assert(mosaic_camera_stop(NULL) == ESP_OK && allocations == 0);
    puts("PASS task, command, flush and borrowed-pixel failures retain ownership until safe retry");
}

static void test_retry_retires_only_preview_workspace(void)
{
    assert(camera_ensure_preview_buffers(2) == ESP_OK);
    s_camera.submit_lock = allocate(1);
    s_camera.frames[0].state = MOSAIC_FRAME_IN_FLIGHT;
    s_camera.thumbnails[0].pixels = allocate(48 * 48 * 2);
    s_camera.thumbnails[0].busy = true;
    const unsigned owned = allocations;
    flush_error = ESP_FAIL;
    assert(camera_retire_working_buffers(NULL) == ESP_ERR_NOT_FINISHED && allocations == owned);
    flush_error = 0; release_pixels = false;
    assert(camera_retire_working_buffers(NULL) == ESP_ERR_NOT_FINISHED && allocations == owned);
    release_pixels = true;
    assert(camera_retire_working_buffers(NULL) == ESP_OK && allocations == 2);
    assert(!s_camera.frames[0].pixels && !s_camera.frames[1].pixels);
    assert(s_camera.thumbnails[0].pixels && s_camera.thumbnails[0].busy);
    assert(camera_ensure_preview_buffers(2) == ESP_OK);
    s_camera.stopping = true;
    assert(camera_retire_working_buffers(NULL) == ESP_ERR_INVALID_STATE && allocations == owned);
    s_camera.stopping = false;
    s_camera.thumbnails[0].busy = false;
    camera_release_thumbnail_buffers();
    release(s_camera.submit_lock); s_camera.submit_lock = NULL;
    clear_workspace();
    puts("PASS failed-start retries retire preview workspace safely while retaining the album thumbnail");
}

static void test_native_close_is_required(void)
{
    assert(camera_ensure_preview_buffers(2) == ESP_OK);
    s_camera.started = true;
    s_camera.task_done = allocate(1); s_camera.submit_lock = allocate(1);
    s_camera.shutdown_result = ESP_FAIL;
    native_failures = 1; native_close_calls = 0;
    const unsigned owned = allocations, deleted = semaphore_deletes;
    assert(mosaic_camera_stop(NULL) == ESP_FAIL && native_close_calls == 1);
    assert(s_camera.started && s_camera.stopping && s_camera.task_joined);
    assert(allocations == owned && semaphore_deletes == deleted && preview_vote);
    assert(!strcmp(last_status, "Camera busy. Try Back again"));
    assert(mosaic_camera_stop(NULL) == ESP_OK && native_close_calls == 2);
    assert(!s_camera.started && !preview_vote && allocations == 0 && semaphore_deletes == deleted + 2);
    native_fixed_error = ESP_ERR_INVALID_STATE;
    assert(camera_stop_registered_stream() == ESP_ERR_INVALID_STATE && preview_vote);
    native_fixed_error = 0; native_default_error = ESP_ERR_NOT_FOUND;
    assert(camera_stop_registered_stream() == ESP_OK && !preview_vote);
    native_default_error = 0;
    puts("PASS native close errors retain session, pixels and power vote until checked shutdown succeeds");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_inactive_startup_status();
    test_admission();
    test_save(argv[1]);
    test_released_preview_as_jpeg_output(argv[1]);
    test_retry_retires_only_preview_workspace();
    test_retirement();
    test_native_close_is_required();
    return 0;
}
