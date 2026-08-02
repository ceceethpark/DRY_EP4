#include "Vision.hpp"
#include "Buzzer.hpp"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include <cinttypes>
#include "linux/videodev2.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace {
constexpr const char *TAG="Vision";
constexpr uint32_t kDetectStep=8;
constexpr uint32_t kMaxGridWidth=192;
constexpr uint32_t kMaxGridHeight=128;
constexpr size_t kMaxGridPixels=kMaxGridWidth*kMaxGridHeight;
uint8_t s_mask[kMaxGridPixels];
uint8_t s_merged_mask[kMaxGridPixels];
uint16_t s_queue[kMaxGridPixels];
struct HeldBox {uint32_t x0=0,y0=0,x1=0,y1=0;int64_t detected_at=0;bool valid=false;} s_held_box;
int64_t s_last_detection_scan=0;

bool isSweetPotatoColor(uint16_t pixel)
{
    const uint32_t r=((pixel>>11)&0x1f)*255/31;
    const uint32_t g=((pixel>>5)&0x3f)*255/63;
    const uint32_t b=(pixel&0x1f)*255/31;
    // The sample stick is tan/brown: red dominates green, while green clearly
    // dominates blue.  The latter condition rejects much of the pink skin
    // range even when brightness is similar.
    const uint32_t maxc=(r>g)?r:g;
    const uint32_t minc=(b<g)?b:g;
    const bool saturated=(maxc-minc)*100>maxc*12;
    return r>55 && g>25 && r>g+5 && g*100>b*105 && saturated;
}

void drawBox(uint16_t *frame,uint32_t width,uint32_t height,uint32_t x0,uint32_t y0,uint32_t x1,uint32_t y1)
{
    constexpr uint16_t color=0x07e0; // green RGB565
    constexpr uint32_t thickness=4;
    x1=(x1>=width)?width-1:x1;y1=(y1>=height)?height-1:y1;
    for(uint32_t t=0;t<thickness;t++){
        if(y0+t<height)for(uint32_t x=x0;x<=x1;x++)frame[(y0+t)*width+x]=color;
        if(y1>=t)for(uint32_t x=x0;x<=x1;x++)frame[(y1-t)*width+x]=color;
        if(x0+t<width)for(uint32_t y=y0;y<=y1;y++)frame[y*width+x0+t]=color;
        if(x1>=t)for(uint32_t y=y0;y<=y1;y++)frame[y*width+x1-t]=color;
    }
}

void detectAndDrawSticks(void *buffer,uint32_t width,uint32_t height)
{
    const int64_t now=esp_timer_get_time();
    if(s_last_detection_scan!=0&&now-s_last_detection_scan<1000000){
        if(s_held_box.valid)drawBox(static_cast<uint16_t*>(buffer),width,height,s_held_box.x0,s_held_box.y0,s_held_box.x1,s_held_box.y1);
        return;
    }
    s_last_detection_scan=now;
    const uint32_t gw=(width+kDetectStep-1)/kDetectStep;
    const uint32_t gh=(height+kDetectStep-1)/kDetectStep;
    if(gw>kMaxGridWidth||gh>kMaxGridHeight)return;
    auto *frame=static_cast<uint16_t*>(buffer);
    const size_t cells=gw*gh;
    memset(s_mask,0,cells);
    for(uint32_t gy=0;gy<gh;gy++)for(uint32_t gx=0;gx<gw;gx++){
        const uint32_t x=gx*kDetectStep,y=gy*kDetectStep;
        s_mask[gy*gw+gx]=isSweetPotatoColor(frame[y*width+x])?1:0;
    }
    // The stick surface contains highlights and a dotted texture. Join color
    // fragments that are within 16 px horizontally / 8 px vertically before
    // evaluating the object's overall rectangular shape.
    memset(s_merged_mask,0,cells);
    for(uint32_t y=0;y<gh;y++)for(uint32_t x=0;x<gw;x++)if(s_mask[y*gw+x]){
        const uint32_t x0=(x>2)?x-2:0,x1=(x+2<gw)?x+2:gw-1;
        const uint32_t y0=(y>1)?y-1:0,y1=(y+1<gh)?y+1:gh-1;
        for(uint32_t yy=y0;yy<=y1;yy++)for(uint32_t xx=x0;xx<=x1;xx++)s_merged_mask[yy*gw+xx]=1;
    }
    memcpy(s_mask,s_merged_mask,cells);
    uint32_t best_count=0,best_minx=0,best_miny=0,best_maxx=0,best_maxy=0;
    for(uint32_t sy=0;sy<gh;sy++)for(uint32_t sx=0;sx<gw;sx++){
        const uint32_t start=sy*gw+sx;if(s_mask[start]!=1)continue;
        size_t head=0,tail=0;s_queue[tail++]=static_cast<uint16_t>(start);s_mask[start]=2;
        uint32_t minx=sx,miny=sy,maxx=sx,maxy=sy,count=0;
        while(head<tail){const uint32_t p=s_queue[head++],x=p%gw,y=p/gw;count++;
            minx=(x<minx)?x:minx;maxx=(x>maxx)?x:maxx;miny=(y<miny)?y:miny;maxy=(y>maxy)?y:maxy;
            const uint32_t n[4]={p-1,p+1,p-gw,p+gw};
            if(x>0&&s_mask[n[0]]==1){s_mask[n[0]]=2;s_queue[tail++]=n[0];}
            if(x+1<gw&&s_mask[n[1]]==1){s_mask[n[1]]=2;s_queue[tail++]=n[1];}
            if(y>0&&s_mask[n[2]]==1){s_mask[n[2]]=2;s_queue[tail++]=n[2];}
            if(y+1<gh&&s_mask[n[3]]==1){s_mask[n[3]]=2;s_queue[tail++]=n[3];}
        }
        const uint32_t bw=maxx-minx+1,bh=maxy-miny+1,box_cells=bw*bh;
        const uint32_t long_side=(bw>bh)?bw:bh,short_side=(bw>bh)?bh:bw;
        const bool area_ok=box_cells*100>=cells*3&&box_cells*10<=cells*7;
        const bool stick_shape=short_side>0&&long_side*2>=short_side*3&&long_side<=short_side*10;
        const bool filled=count*10>=box_cells;
        if(area_ok&&stick_shape&&filled&&count>best_count){best_count=count;best_minx=minx;best_miny=miny;best_maxx=maxx;best_maxy=maxy;}
    }
    if(best_count){
        // Refine the box against full-resolution source pixels. The expanded
        // grid above is used only to join texture fragments, never as the
        // visible box boundary.
        uint32_t rx0=width,ry0=height,rx1=0,ry1=0,matched=0;
        const uint32_t sx0=best_minx*kDetectStep,sy0=best_miny*kDetectStep;
        const uint32_t sx1=((best_maxx+1)*kDetectStep<width)?(best_maxx+1)*kDetectStep:width-1;
        const uint32_t sy1=((best_maxy+1)*kDetectStep<height)?(best_maxy+1)*kDetectStep:height-1;
        for(uint32_t y=sy0;y<=sy1;y+=2)for(uint32_t x=sx0;x<=sx1;x+=2)if(isSweetPotatoColor(frame[y*width+x])){
            matched++;rx0=(x<rx0)?x:rx0;rx1=(x>rx1)?x:rx1;ry0=(y<ry0)?y:ry0;ry1=(y>ry1)?y:ry1;
        }
        if(matched){s_held_box={rx0,ry0,rx1,ry1,now,true};}
    }
    else if(s_held_box.valid&&now-s_held_box.detected_at>=1000000){s_held_box.valid=false;}
    if(s_held_box.valid)drawBox(frame,width,height,s_held_box.x0,s_held_box.y0,s_held_box.x1,s_held_box.y1);
}
}

Vision::~Vision(){stopPreview();}

esp_err_t Vision::initializeCamera()
{
    if(camera_initialized_)return ESP_OK;
    esp_video_init_csi_config_t csi{};
    csi.sccb_config.init_sccb=false;
    csi.sccb_config.i2c_handle=bsp_i2c_get_handle();
    csi.sccb_config.freq=100000;
    csi.reset_pin=GPIO_NUM_NC;csi.pwdn_pin=GPIO_NUM_NC;
    esp_video_init_config_t config{};config.csi=&csi;
    // Match the known-good Brookesia camera path.  esp_video_init() also
    // initializes the ISP pipeline needed to convert OV02C10 RAW10 to RGB565.
    esp_err_t err=esp_video_init(&config);
    if(err==ESP_OK)camera_initialized_=true;
    return err;
}

esp_err_t Vision::toggle(lv_obj_t *parent)
{
    if(visible()){stopPreview();return ESP_OK;}
    esp_err_t err=initializeCamera();if(err!=ESP_OK)return err;
    return startPreview(parent);
}

esp_err_t Vision::startPreview(lv_obj_t *parent)
{
    video_fd_=open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME,O_RDONLY);
    if(video_fd_<0)return ESP_FAIL;
    v4l2_format native_format{};native_format.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(ioctl(video_fd_,VIDIOC_G_FMT,&native_format)!=0){ESP_LOGE(TAG,"VIDIOC_G_FMT failed (errno=%d)",errno);stopPreview();return ESP_FAIL;}
    ESP_LOGI(TAG,"camera native format: %" PRIu32 "x%" PRIu32,native_format.fmt.pix.width,native_format.fmt.pix.height);
    // Do not reuse the RAW10 format structure: its bytesperline/sizeimage
    // fields are invalid for RGB565 and esp_video rejects the mixed request.
    v4l2_format rgb_format{};
    rgb_format.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rgb_format.fmt.pix.width=native_format.fmt.pix.width;
    rgb_format.fmt.pix.height=native_format.fmt.pix.height;
    rgb_format.fmt.pix.pixelformat=V4L2_PIX_FMT_RGB565;
    if(ioctl(video_fd_,VIDIOC_S_FMT,&rgb_format)!=0){ESP_LOGE(TAG,"VIDIOC_S_FMT RGB565 failed (errno=%d)",errno);stopPreview();return ESP_FAIL;}
    width_=rgb_format.fmt.pix.width;height_=rgb_format.fmt.pix.height;
    size_t alignment=0;
    if(esp_cache_get_alignment(MALLOC_CAP_SPIRAM,&alignment)!=ESP_OK){ESP_LOGE(TAG,"failed to get PSRAM cache alignment");stopPreview();return ESP_FAIL;}
    v4l2_requestbuffers req{};req.count=kBufferCount;req.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;req.memory=V4L2_MEMORY_USERPTR;
    if(ioctl(video_fd_,VIDIOC_REQBUFS,&req)!=0||req.count<kBufferCount){stopPreview();return ESP_FAIL;}
    for(int i=0;i<kBufferCount;i++){v4l2_buffer b{};b.type=req.type;b.memory=req.memory;b.index=i;if(ioctl(video_fd_,VIDIOC_QUERYBUF,&b)!=0){stopPreview();return ESP_FAIL;}buffer_lengths_[i]=b.length;buffers_[i]=heap_caps_aligned_alloc(alignment,b.length,MALLOC_CAP_SPIRAM);if(!buffers_[i]){stopPreview();return ESP_ERR_NO_MEM;}b.m.userptr=reinterpret_cast<unsigned long>(buffers_[i]);b.length=buffer_lengths_[i];if(ioctl(video_fd_,VIDIOC_QBUF,&b)!=0){stopPreview();return ESP_FAIL;}}
    display_buffer_length_=width_*height_*sizeof(uint16_t);
    for(void *&display_buffer:display_buffers_){display_buffer=heap_caps_aligned_alloc(alignment,display_buffer_length_,MALLOC_CAP_SPIRAM);if(!display_buffer){stopPreview();return ESP_ERR_NO_MEM;}memset(display_buffer,0,display_buffer_length_);}
    display_buffer_index_=0;
    captured_buffer_index_=-1;frozen_=false;
    constexpr uint32_t preview_w=640,preview_h=360;
    overlay_=lv_obj_create(parent);lv_obj_set_size(overlay_,preview_w+16,preview_h+16);lv_obj_center(overlay_);lv_obj_set_style_bg_color(overlay_,lv_color_black(),0);lv_obj_set_style_bg_opa(overlay_,LV_OPA_COVER,0);lv_obj_set_style_border_width(overlay_,4,0);lv_obj_set_style_border_color(overlay_,lv_color_white(),0);lv_obj_set_style_pad_all(overlay_,4,0);lv_obj_clear_flag(overlay_,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(overlay_,closeEvent,LV_EVENT_CLICKED,this);
    canvas_=lv_canvas_create(overlay_);lv_canvas_set_buffer(canvas_,display_buffers_[0],width_,height_,LV_IMG_CF_TRUE_COLOR);lv_img_set_zoom(canvas_,static_cast<uint16_t>((preview_w*256U)/width_));lv_obj_center(canvas_);lv_obj_clear_flag(canvas_,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    guide_box_=lv_obj_create(overlay_);lv_obj_set_size(guide_box_,preview_w*60/100,preview_h*60/100);lv_obj_center(guide_box_);lv_obj_set_style_bg_opa(guide_box_,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(guide_box_,4,0);lv_obj_set_style_border_color(guide_box_,lv_color_hex(0x00FF66),0);lv_obj_set_style_radius(guide_box_,0,0);lv_obj_set_style_pad_all(guide_box_,0,0);lv_obj_clear_flag(guide_box_,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_EVENT_BUBBLE);
    freeze_button_=lv_btn_create(overlay_);lv_obj_set_size(freeze_button_,150,52);lv_obj_align(freeze_button_,LV_ALIGN_TOP_RIGHT,-8,8);lv_obj_clear_flag(freeze_button_,LV_OBJ_FLAG_EVENT_BUBBLE);lv_obj_add_event_cb(freeze_button_,freezeEvent,LV_EVENT_CLICKED,this);
    freeze_label_=lv_label_create(freeze_button_);lv_label_set_text(freeze_label_,"FREEZE");lv_obj_center(freeze_label_);
    send_button_=lv_btn_create(overlay_);lv_obj_set_size(send_button_,120,42);lv_obj_align(send_button_,LV_ALIGN_BOTTOM_MID,-70,-6);lv_obj_clear_flag(send_button_,LV_OBJ_FLAG_EVENT_BUBBLE);lv_obj_add_event_cb(send_button_,sendEvent,LV_EVENT_CLICKED,this);lv_obj_add_flag(send_button_,LV_OBJ_FLAG_HIDDEN);
    send_label_=lv_label_create(send_button_);lv_label_set_text(send_label_,"SEND");lv_obj_center(send_label_);
    cancel_button_=lv_btn_create(overlay_);lv_obj_set_size(cancel_button_,120,42);lv_obj_align(cancel_button_,LV_ALIGN_BOTTOM_MID,70,-6);lv_obj_clear_flag(cancel_button_,LV_OBJ_FLAG_EVENT_BUBBLE);lv_obj_add_event_cb(cancel_button_,cancelEvent,LV_EVENT_CLICKED,this);lv_obj_add_flag(cancel_button_,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *cancel_label=lv_label_create(cancel_button_);lv_label_set_text(cancel_label,"CANCEL");lv_obj_center(cancel_label);
    Buzzer::instance().attachToTree(overlay_);
    int type=V4L2_BUF_TYPE_VIDEO_CAPTURE;if(ioctl(video_fd_,VIDIOC_STREAMON,&type)!=0){stopPreview();return ESP_FAIL;}
    stop_requested_=false;if(xTaskCreate(taskEntry,"vision",6144,this,6,&task_)!=pdPASS){stopPreview();return ESP_ERR_NO_MEM;}return ESP_OK;
}

void Vision::stopPreview()
{
    if(exit_timer_){lv_timer_del(exit_timer_);exit_timer_=nullptr;}
    if(upload_task_){while(upload_task_)vTaskDelay(pdMS_TO_TICKS(10));}
    if(task_){stop_requested_=true;while(task_)vTaskDelay(pdMS_TO_TICKS(10));}
    if(video_fd_>=0){int type=V4L2_BUF_TYPE_VIDEO_CAPTURE;ioctl(video_fd_,VIDIOC_STREAMOFF,&type);for(int i=0;i<kBufferCount;i++){if(buffers_[i])heap_caps_free(buffers_[i]);buffers_[i]=nullptr;}close(video_fd_);video_fd_=-1;}
    for(void *&display_buffer:display_buffers_){if(display_buffer)heap_caps_free(display_buffer);display_buffer=nullptr;}display_buffer_length_=0;
    if(overlay_){lv_obj_del(overlay_);overlay_=nullptr;canvas_=nullptr;guide_box_=nullptr;freeze_button_=nullptr;freeze_label_=nullptr;send_button_=nullptr;send_label_=nullptr;cancel_button_=nullptr;}
    captured_buffer_index_=-1;frozen_=false;
}

void Vision::taskEntry(void *context){static_cast<Vision*>(context)->captureLoop();}
void Vision::uploadTaskEntry(void *context)
{
    auto *vision=static_cast<Vision*>(context);
    const esp_err_t err=vision->upload_handler_
        ? vision->upload_handler_(vision->capturedFrameData(),vision->capturedFrameSize(),vision->width_,vision->height_,vision->upload_user_context_)
        : ESP_ERR_INVALID_STATE;
    if(bsp_display_lock(1000)){
        if(vision->send_label_)lv_label_set_text(vision->send_label_,err==ESP_OK?"SENT":"FAILED");
        if(vision->overlay_){vision->exit_timer_=lv_timer_create(exitTimer,1500,vision);lv_timer_set_repeat_count(vision->exit_timer_,1);}
        bsp_display_unlock();
    }
    ESP_LOGI(TAG,"HTTP image upload result: %s",esp_err_to_name(err));
    vision->upload_task_=nullptr;
    vTaskDelete(nullptr);
}
void Vision::captureLoop()
{
    while(!stop_requested_){v4l2_buffer b{};b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;b.memory=V4L2_MEMORY_USERPTR;if(ioctl(video_fd_,VIDIOC_DQBUF,&b)!=0)break;if(b.index<kBufferCount&&!frozen_){const int next=(captured_buffer_index_>=0)?1-captured_buffer_index_:1-display_buffer_index_;memcpy(display_buffers_[next],buffers_[b.index],display_buffer_length_);if(bsp_display_lock(100)){if(!frozen_){lv_canvas_set_buffer(canvas_,display_buffers_[next],width_,height_,LV_IMG_CF_TRUE_COLOR);lv_obj_invalidate(canvas_);lv_refr_now(nullptr);display_buffer_index_=next;}bsp_display_unlock();}}b.m.userptr=reinterpret_cast<unsigned long>(buffers_[b.index]);b.length=buffer_lengths_[b.index];if(ioctl(video_fd_,VIDIOC_QBUF,&b)!=0)break;}
    task_=nullptr;vTaskDelete(nullptr);
}
void Vision::closeEvent(lv_event_t *event){static_cast<Vision*>(lv_event_get_user_data(event))->stopPreview();}
void Vision::freezeEvent(lv_event_t *event)
{
    auto *vision=static_cast<Vision*>(lv_event_get_user_data(event));
    if(!vision->frozen_){vision->captured_buffer_index_=vision->display_buffer_index_;vision->frozen_=true;lv_obj_add_flag(vision->freeze_button_,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(vision->send_button_,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(vision->cancel_button_,LV_OBJ_FLAG_HIDDEN);ESP_LOGI(TAG,"frame captured: %" PRIu32 "x%" PRIu32 ", %u bytes",vision->width_,vision->height_,static_cast<unsigned>(vision->display_buffer_length_));}
    else{vision->frozen_=false;lv_label_set_text(vision->freeze_label_,"FREEZE");}
    lv_event_stop_bubbling(event);
}
void Vision::cancelEvent(lv_event_t *event)
{
    auto *vision=static_cast<Vision*>(lv_event_get_user_data(event));
    vision->frozen_=false;vision->captured_buffer_index_=-1;
    lv_obj_add_flag(vision->send_button_,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(vision->cancel_button_,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(vision->freeze_button_,LV_OBJ_FLAG_HIDDEN);lv_label_set_text(vision->freeze_label_,"FREEZE");
    lv_event_stop_bubbling(event);
}
void Vision::sendEvent(lv_event_t *event)
{
    auto *vision=static_cast<Vision*>(lv_event_get_user_data(event));
    lv_label_set_text(vision->send_label_,"SENDING...");lv_obj_add_state(vision->send_button_,LV_STATE_DISABLED);lv_obj_add_state(vision->cancel_button_,LV_STATE_DISABLED);
    if(!vision->upload_handler_||xTaskCreate(uploadTaskEntry,"image_upload",8192,vision,5,&vision->upload_task_)!=pdPASS){lv_label_set_text(vision->send_label_,"FAILED");vision->exit_timer_=lv_timer_create(exitTimer,1500,vision);lv_timer_set_repeat_count(vision->exit_timer_,1);}
    lv_event_stop_bubbling(event);
}
void Vision::exitTimer(lv_timer_t *timer)
{
    auto *vision=static_cast<Vision*>(timer->user_data);vision->exit_timer_=nullptr;lv_timer_del(timer);vision->stopPreview();
}
