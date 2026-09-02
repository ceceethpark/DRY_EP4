#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class Vision {
public:
    using UploadHandler = esp_err_t (*)(const void *rgb565, size_t size, uint32_t width, uint32_t height, void *user_context);
    Vision() = default;
    ~Vision();
    esp_err_t toggle(lv_obj_t *parent);
    bool visible() const { return overlay_ != nullptr; }
    bool hasCapturedFrame() const { return captured_buffer_index_ >= 0; }
    const void *capturedFrameData() const { return hasCapturedFrame() ? display_buffers_[captured_buffer_index_] : nullptr; }
    size_t capturedFrameSize() const { return hasCapturedFrame() ? display_buffer_length_ : 0; }
    uint32_t capturedFrameWidth() const { return width_; }
    uint32_t capturedFrameHeight() const { return height_; }
    void setUploadHandler(UploadHandler handler, void *user_context = nullptr) { upload_handler_ = handler; upload_user_context_ = user_context; }

private:
    static void taskEntry(void *context);
    static void uploadTaskEntry(void *context);
    static void closeEvent(lv_event_t *event);
    static void freezeEvent(lv_event_t *event);
    static void sendEvent(lv_event_t *event);
    static void cancelEvent(lv_event_t *event);
    static void exitTimer(lv_timer_t *timer);
    void captureLoop();
    esp_err_t initializeCamera();
    esp_err_t startPreview(lv_obj_t *parent);
    void stopPreview();

    static constexpr int kBufferCount = 2;
    bool camera_initialized_ = false;
    volatile bool stop_requested_ = false;
    int video_fd_ = -1;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t upload_task_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *guide_box_ = nullptr;
    lv_obj_t *freeze_button_ = nullptr;
    lv_obj_t *freeze_label_ = nullptr;
    lv_obj_t *send_button_ = nullptr;
    lv_obj_t *send_label_ = nullptr;
    lv_obj_t *cancel_button_ = nullptr;
    lv_timer_t *exit_timer_ = nullptr;
    void *buffers_[kBufferCount]{};
    size_t buffer_lengths_[kBufferCount]{};
    void *display_buffers_[2]{};
    size_t display_buffer_length_ = 0;
    int display_buffer_index_ = 0;
    int captured_buffer_index_ = -1;
    volatile bool frozen_ = false;
    UploadHandler upload_handler_ = nullptr;
    void *upload_user_context_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};
