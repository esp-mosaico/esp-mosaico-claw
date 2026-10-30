#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "mosaico_module_camera.h"
#include "esp_cam_sensor_types.h"
#include "esp_video_ioctl.h"
#include "sc101iot_types.h"
#define SC101IOT_YUV422_FMT_UYVY 0x02
#include "sensor_fixtures.h"

#define BASE15 sc101iot_dvp_8bit_20Minput_1280x720_yuv422_uyvy_15fps
#define BASE25 sc101iot_dvp_8bit_20Minput_1280x720_yuv422_uyvy_25fps
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
enum { FAIL_NONE, FAIL_STATS, FAIL_WAKE, FAIL_OPEN, FAIL_ID, FAIL_GET, FAIL_SET, FAIL_CLOSE, FAIL_SLEEP };
static int failure, opens, closes, sets, gets, wakes, sleeps;
static unsigned pid;
static mosaico_camera_pipeline_stats_t pipeline;
static esp_cam_sensor_format_t base;
static const esp_cam_sensor_format_t *borrowed;
static const void *camera = (void *)1;

static int test_open(const char *path, int flags, ...)
{ assert(!strcmp(path,"/dev/video2") && flags==O_RDWR); opens++; return failure==FAIL_OPEN ? -1 : 17; }
static int test_close(int fd)
{ assert(fd==17); closes++; return failure==FAIL_CLOSE ? -1 : 0; }
static int test_ioctl(int fd, unsigned long operation, ...)
{
    assert(fd==17);
    va_list args; va_start(args,operation); void *value=va_arg(args,void *); va_end(args);
    if (operation==VIDIOC_G_EXT_CTRLS) {
        struct v4l2_ext_controls *ctrl=value;
        assert(ctrl->ctrl_class==V4L2_CTRL_CLASS_ESP_CAM_IOCTL && ctrl->count==1);
        assert(ctrl->controls->id==ESP_CAM_SENSOR_IOC_G_CHIP_ID);
        ((esp_cam_sensor_id_t *)ctrl->controls->p_u8)->pid=(uint16_t)pid;
        return failure==FAIL_ID ? -1 : 0;
    }
    if (operation==VIDIOC_G_SENSOR_FMT) { gets++; *(esp_cam_sensor_format_t *)value=base; return failure==FAIL_GET ? -1 : 0; }
    assert(operation==VIDIOC_S_SENSOR_FMT);
    sets++; borrowed=value; /* Includes a failure after the sensor borrowed it. */
    return failure==FAIL_SET ? -1 : 0;
}
esp_err_t mosaico_camera_get_pipeline_stats(mosaico_camera_handle_t handle, mosaico_camera_pipeline_stats_t *stats)
{ assert(handle==camera); *stats=pipeline; return failure==FAIL_STATS ? ESP_FAIL : ESP_OK; }
esp_err_t mosaico_camera_set_power_down(mosaico_camera_handle_t handle, bool sleep)
{
    assert(handle==camera);
    if(sleep) sleeps++; else wakes++;
    if (failure==(sleep ? FAIL_SLEEP : FAIL_WAKE)) return ESP_FAIL;
    pipeline.power_down=sleep; return ESP_OK;
}
#define open test_open
#define close test_close
#define ioctl test_ioctl
#include ROI_SOURCE_FILE
#undef open
#undef close
#undef ioctl

static void reset(bool reset_profile)
{
    failure=FAIL_NONE; opens=closes=sets=gets=wakes=sleeps=0; pid=SC101IOT_PID;
    pipeline=(mosaico_camera_pipeline_stats_t){.power_down=true}; borrowed=NULL;
    base=(esp_cam_sensor_format_t){.name="base",.format=ESP_CAM_SENSOR_PIXFORMAT_YUV422_UYVY,
        .port=ESP_CAM_SENSOR_DVP,.xclk=20000000,.width=1280,.height=720,
        .regs=BASE25,.regs_size=COUNT(BASE25),.fps=25,.isp_info=(void *)2,.reserved=(void *)3};
    if(reset_profile) { s_parent_regs=NULL; memset(&s_roi_format,0,sizeof(s_roi_format)); memset(s_roi_regs,0,sizeof(s_roi_regs)); }
}
static void verify_profile(const sc101iot_reginfo_t *original, int count)
{
    assert(borrowed==&s_roi_format && borrowed->regs==s_roi_regs);
    assert(borrowed->width==768 && borrowed->height==720 && borrowed->regs_size==count);
    assert(borrowed->format==base.format && borrowed->port==base.port && borrowed->fps==base.fps);
    assert(borrowed->xclk==base.xclk && borrowed->isp_info==base.isp_info && borrowed->reserved==base.reserved);
    unsigned page=0, changes=0;
    for(int i=0;i<count;i++) {
        if(original[i].reg==0xf0) page=original[i].val;
        assert(s_roi_regs[i].reg==original[i].reg);
        if(page==1 && original[i].reg==0x72) { assert(original[i].val==0x50 && s_roi_regs[i].val==0x41); changes++; }
        else assert(s_roi_regs[i].val==original[i].val);
    }
    assert(changes==1);
}
int main(void)
{
    for (int fps=15;fps<=25;fps+=10) {
        reset(true);
        if(fps==15) { base.regs=BASE15; base.regs_size=COUNT(BASE15); base.fps=15; }
        assert(mosaico_camera_configure_equivalent_roi((void *)camera)==ESP_OK);
        verify_profile(base.regs,base.regs_size);
        assert(opens==1 && closes==1 && sets==1 && wakes==1 && sleeps==1 && pipeline.power_down);
        esp_cam_sensor_format_t snapshot=*borrowed;
        sc101iot_reginfo_t registers[ROI_MAX_REGS]; memcpy(registers,s_roi_regs,sizeof(registers));
        assert(prepare_roi(&base)==borrowed && !memcmp(&snapshot,borrowed,sizeof(snapshot)));
        base=*borrowed; assert(prepare_roi(&base)==borrowed);
        assert(!memcmp(registers,s_roi_regs,sizeof(registers)));
    }
    puts("PASS actual 15/25fps tables: equivalent ROI, full register preservation, permanent borrowed storage");

    reset(true); pipeline.power_down=false;
    assert(mosaico_camera_configure_equivalent_roi((void *)camera)==ESP_OK);
    assert(!pipeline.power_down && wakes==0 && sleeps==0);
    reset(true); pid=0x364c;
    assert(mosaico_camera_configure_equivalent_roi((void *)camera)==ESP_OK && gets==0 && sets==0);
    assert(pipeline.power_down && opens==closes);
    puts("PASS awake state and non-SC101IOT native mode are preserved");

    for(int field=0;field<10;field++) {
        reset(true);
        switch(field) {
            case 0:base.width=640;break; case 1:base.height=480;break; case 2:base.fps=30;break;
            case 3:base.xclk=24000000;break;case 4:base.format=99;break;case 5:base.port=99;break;
            case 6:base.regs=NULL;break;case 7:base.regs_size=0;break;case 8:base.regs_size=-1;break;
            case 9:base.regs_size=ROI_MAX_REGS+1;break;
        }
        assert(mosaico_camera_configure_equivalent_roi((void *)camera)==ESP_OK && sets==0 && s_parent_regs==NULL);
        assert(opens==closes && pipeline.power_down);
    }
    sc101iot_reginfo_t damaged[ROI_MAX_REGS];
    for(int kind=0;kind<3;kind++) {
        reset(true);memcpy(damaged,BASE25,sizeof(BASE25));base.regs=damaged;
        unsigned page=0;
        for(int i=0;i<base.regs_size;i++) {
            if(damaged[i].reg==0xf0)page=damaged[i].val;
            if(page==1 && damaged[i].reg==0x70) {
                if(kind==0)damaged[i].val=0;
                if(kind==1)damaged[i].reg=0x69;
                if(kind==2)damaged[i+1].reg=0x70;
                break;
            }
        }
        assert(prepare_roi(&base)==NULL && s_parent_regs==NULL);
    }
    puts("PASS unsupported modes, sizes, origins and register layouts retain native format");

    for(int fault=FAIL_STATS;fault<=FAIL_SLEEP;fault++) {
        reset(true);failure=fault;
        assert(mosaico_camera_configure_equivalent_roi((void *)camera)!=ESP_OK);
        assert(closes==((fault==FAIL_STATS || fault==FAIL_WAKE || fault==FAIL_OPEN) ? 0 : 1));
        if(fault!=FAIL_STATS && fault!=FAIL_WAKE && fault!=FAIL_SLEEP) assert(pipeline.power_down);
        if(borrowed) verify_profile(BASE25,COUNT(BASE25));
    }
    for(int mode=0;mode<3;mode++) {
        reset(true);
        if(mode==0)pipeline.streaming=true;
        if(mode==1)pipeline.buffer_count=1;
        if(mode==2)pipeline.outstanding_count=1;
        assert(mosaico_camera_configure_equivalent_roi((void *)camera)==ESP_ERR_INVALID_STATE);
        assert(opens==0 && wakes==0 && sleeps==0);
    }
    puts("PASS active-buffer rejection and all I/O/power failure cleanup paths");

    reset(true);assert(mosaico_camera_configure_equivalent_roi((void *)camera)==ESP_OK);
    sc101iot_reginfo_t saved[ROI_MAX_REGS];memcpy(saved,s_roi_regs,sizeof(saved));
    esp_cam_sensor_format_t snapshot=s_roi_format;
    memcpy(damaged,BASE25,sizeof(BASE25));base.regs=damaged;
    assert(prepare_roi(&base)==NULL);
    assert(!memcmp(saved,s_roi_regs,sizeof(saved)) && !memcmp(&snapshot,&s_roi_format,sizeof(snapshot)));
    puts("PASS a later base-mode change cannot overwrite a retained sensor profile");
}
