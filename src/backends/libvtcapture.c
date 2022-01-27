#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <vtcapture/vtCaptureApi_c.h>
#include <halgal.h>
#include <libyuv.h>

#include "common.h"
#include "log.h"


pthread_t capture_thread = 0;

//Halgal
HAL_GAL_SURFACE surfaceInfo;
size_t nBytesGUI;
char *addrGUI;
int fdGUI;

//Other
const char *caller = "hyperion-webos_service";
VT_DRIVER *driver = NULL;
char client[128] = "00";
bool capture_run = false;

_LibVtCaptureProperties props;

char *videoY = NULL;
char *videoUV = NULL;
char *videoARGB = NULL;
char *guiABGR = NULL;
char *guiARGB = NULL;
char *outARGB = NULL;
char *outRGB = NULL;

//All
int strideGUI;
int strideVideo;
int width = 0;
int height = 0;

cap_backend_config_t config = {0, 0, 0, 0};
cap_imagedata_callback_t imagedata_cb = NULL;

// Prototypes
int vtcapture_initialize();
void *capture_thread_target(void *data);


uint64_t getticks_us()
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec * 1000000 + tp.tv_nsec / 1000;
}

int capture_preinit(cap_backend_config_t *backend_config, cap_imagedata_callback_t callback)
{
    INFO("Preinit called. Copying config..");
    memcpy(&config, backend_config, sizeof(cap_backend_config_t));

    INFO("Copying config done. Initialize vars..");

    imagedata_cb = callback;

    VT_RESOLUTION_T resolution = {config.resolution_width, config.resolution_height};
    VT_DUMP_T dump = 2;
    VT_LOC_T loc = {0, 0};
    VT_BUF_T buf_cnt = 3;

    // Sorry, no unlimited fps for you.
    if (config.fps == 0){
        config.fps = 60;
    }

    props.dump = dump;
    props.loc = loc;
    props.reg = resolution;
    props.buf_cnt = buf_cnt;
    props.frm = config.fps;
    
    INFO("Init finished.");
    return 0;
}

int capture_init()
{
    int done;
    _LibVtCaptureBufferInfo buff;

    INFO("Initialization of capture devices..");

    if (config.no_gui != 1) {

        INFO("Graphical capture enabled. Begin init..");
        if ((done = HAL_GAL_Init()) != 0) {
            ERR("HAL_GAL_Init failed: %x", done);
            return -1;
        }
        INFO("HAL_GAL_Init done! Exit: %d", done);   

        if ((done = HAL_GAL_CreateSurface(config.resolution_width, config.resolution_height, 0, &surfaceInfo)) != 0) {
            ERR("HAL_GAL_CreateSurface failed: %x", done);
            return -1;
        }
        INFO("HAL_GAL_CreateSurface done! SurfaceID: %d", surfaceInfo.vendorData);

        if ((done = HAL_GAL_CaptureFrameBuffer(&surfaceInfo)) != 0) {
            ERR("HAL_GAL_CaptureFrameBuffer failed: %x", done);
            return -1;
        }
        INFO("HAL_GAL_CaptureFrameBuffer done! %x", done);

        fdGUI = open("/dev/gfx",2);
        if (fdGUI < 0){
            ERR("HAL_GAL: gfx open fail result: %d", fdGUI);
            return -1;

        }else{
            INFO("HAL_GAL: gfx open ok result: %d", fdGUI);
        }

        if (width != 0 && width != surfaceInfo.width) {

            ERR("vtcapture and halgal width doesn't match");
            return -1;
        }
        if (height != 0 && height != surfaceInfo.height) {

            ERR("vtcapture and halgal height doesn't match");
            return -1;
        }
        width = surfaceInfo.width;
        height = surfaceInfo.height;
        strideGUI = surfaceInfo.pitch;

        nBytesGUI = surfaceInfo.property;
        if (nBytesGUI == 0){
            nBytesGUI = strideGUI * height;
        }

        if (config.no_gui != 1 && config.no_video == 1) //GUI only
        {
            DBG("Malloc halgal vars...");

            // stride = surfaceInfo.pitch/4;
            // rgbsize = sizeof(char) * surfaceInfo.width * surfaceInfo.height * 3;

            guiABGR = (char *) malloc(nBytesGUI);
            guiARGB = (char *) malloc(nBytesGUI);
            outARGB = (char *) malloc(nBytesGUI);
            outRGB = (char *) malloc(width * height * 3 * sizeof(char));

            addrGUI = (char *) mmap(0, nBytesGUI, 3, 1, fdGUI, surfaceInfo.offset);

            DBG("Malloc halgal vars finished.");
        }

        INFO("Halgal done!");
    }

    if (config.no_video != 1) {

        INFO("Init video capture..");
        driver = vtCapture_create();
        INFO("Driver created!");

        done = vtcapture_initialize();
        if (done == -1){
            ERR("vtcapture_initialize failed!");
            return -1;
        }else if (done == 11){
            ERR("vtcapture_initialize failed! No capture permissions!");
            return 11;
        }else if (done == 17 || done == 1){
            // vtcapture_initialized = false;
            INFO("vtcapture not ready yet!");
            return done;
        }else if (done == 0){
            // vtcapture_initialized = true;
            INFO("vtcapture initialized!");
        } else{
            ERR("vtcapture_initialize failed! Something not covered happend! Returncode: %d", done);
            return -2;
        }

        if (config.no_video != 1 && config.no_gui == 1) //Video only
        {
            DBG("Malloc vt vars...");
            
            vtCapture_currentCaptureBuffInfo(driver, &buff);
            
            videoY = (char *) malloc(buff.size0);
            videoUV = (char *) malloc(buff.size1);

            videoARGB = (char *) malloc(width * height * 4 * sizeof(char));
            outRGB = (char *) malloc(width * height * 3 * sizeof(char));

            DBG("Malloc vt vars finished.");
        }
    }

    
    if(config.no_video != 1 && config.no_gui != 1) //Both
    {
        INFO("Malloc hal+vt vars..");
            
        vtCapture_currentCaptureBuffInfo(driver, &buff);

        videoY = (char *) malloc(buff.size0);
        videoUV = (char *) malloc(buff.size1);

        // rgbasize = sizeof(char) * stride * h * 4;
        // rgbsize = sizeof(char) * stride * h * 3;

        videoARGB = (char *) malloc(width * height * 4 * sizeof(char)); 
        guiABGR = (char *) malloc(nBytesGUI * sizeof(char));
        guiARGB = (char *) malloc(nBytesGUI * sizeof(char));
        outARGB = (char *) malloc(nBytesGUI * sizeof(char));
        outRGB = (char *) malloc(width * height * 3 * sizeof(char));

        // stride = surfaceInfo.pitch/4;

        addrGUI = (char *) mmap(0, nBytesGUI, 3, 1, fdGUI, surfaceInfo.offset);

        INFO("Malloc hal+vt vars finished.");
    }

    // capture_initialized = true;
    return 0;
}

int vtcapture_initialize()
{
    _LibVtCapturePlaneInfo plane;
    _LibVtCaptureBufferInfo buff;

    INFO("Starting vtcapture initialization.");
    int innerdone = 0;
    innerdone = vtCapture_init(driver, caller, client);
    if (innerdone == 17) {
        ERR("vtCapture_init not ready yet return: %d", innerdone);
        return 17;
    }else if (innerdone == 11){
        ERR("vtCapture_init failed: %d Permission denied! Quitting...", innerdone);
        return 11;
    }else if (innerdone != 0){
        ERR("vtCapture_init failed: %d Quitting...", innerdone);
        return -1;
    }
    INFO("vtCapture_init done! Caller_ID: %s Client ID: %s", caller, client);

    
    // //Donno why, but we have to skip first try after autostart. Otherwise only first frame is captured
    // if (startuptries < 1){
    //     INFO("Skipping successfull vtCapture_init to prevent start after first try.");
    //     startuptries++;

    //     done = vtCapture_postprocess(driver, client);
    //     if (done == 0){
    //         INFO("vtCapture_postprocess done!");
    //         done = vtCapture_finalize(driver, client);
    //         if (done == 0) {
    //             INFO("vtCapture_finalize done!");
    //         } else{
    //             ERR("vtCapture_finalize failed: %x", done);
    //         }
    //     } else{
    //         done = vtCapture_finalize(driver, client);
    //         if (done == 0) {
    //             INFO("vtCapture_finalize done!");
    //         } else{
    //             ERR("vtCapture_finalize failed: %x", done);
    //         }
    //     }

    //     return 17; //Just simulate init failed
    // }

    innerdone = vtCapture_preprocess(driver, client, &props);
    if (innerdone == 1){
        ERR("vtCapture_preprocess not ready yet return: %d", innerdone);
        return 1;
    }else if (innerdone != 0) {
        ERR("vtCapture_preprocess failed: %x Quitting...", innerdone);
        return -1;
    }
    INFO("vtCapture_preprocess done!");

    innerdone = vtCapture_planeInfo(driver, client, &plane);
    if (innerdone == 0 ) {

        if (width != 0 && width != plane.planeregion.c) {

            ERR("vtcapture and halgal width doesn't match");
            return -1;
        }
        if (height != 0 && height != plane.planeregion.d) {

            ERR("vtcapture and halgal height doesn't match");
            return -1;
        }
        
        width = plane.planeregion.c;
        height = plane.planeregion.d;
        strideVideo = plane.stride;
    }
    else {

        ERR("vtCapture_planeInfo failed: %xQuitting...", innerdone);
        return -1;
    }
    INFO("vtCapture_planeInfo done! stride: %d Region: x: %d, y: %d, w: %d, h: %d Active Region: x: %d, y: %d w: %d h: %d", 
            plane.stride, plane.planeregion.a, plane.planeregion.b, plane.planeregion.c, plane.planeregion.d, 
            plane.activeregion.a, plane.activeregion.b, plane.activeregion.c, plane.activeregion.d);

    innerdone = vtCapture_process(driver, client);
    if (innerdone != 0) {

        ERR("vtCapture_process failed: %xQuitting...", innerdone);
        return -1;
    }
    INFO("vtCapture_process done!");

    int cnter = 0;
    do {
        usleep(100000);
        innerdone = vtCapture_currentCaptureBuffInfo(driver, &buff);
        if (innerdone == 0 ) {
            break;
        }else if (innerdone != 2){
            ERR("vtCapture_currentCaptureBuffInfo failed: %x Quitting...", innerdone);
            capture_terminate();
            return -1;
        }
        cnter++;
    } while(innerdone != 0);

    INFO("vtCapture_currentCaptureBuffInfo done after %d tries! addr0: %p addr1: %p size0: %d size1: %d", 
            cnter, buff.start_addr0, buff.start_addr1, buff.size0, buff.size1);

    INFO("vtcapture initialization finished.");
    return 0;
}

int capture_start()
{
    INFO("Starting capture thread..");
    capture_run = true;
    if (pthread_create(&capture_thread, NULL, capture_thread_target, NULL) != 0) {
        return -1;
    }
    return 0;
}

int capture_cleanup()
{
    INFO("Capture cleanup...");

    if (videoY)
        free(videoY);
    videoY = NULL;
    if (videoUV)
        free(videoUV);
    videoUV = NULL;
    if (videoARGB)
        free(videoARGB);
    videoARGB = NULL;
    if (guiABGR)
        free(guiABGR);
    guiABGR = NULL;
    if (guiARGB)
        free(guiARGB);
    guiARGB = NULL;
    if (outARGB)
        free(outARGB);
    outARGB = NULL;
    if (outRGB)
        free(outRGB);
    outRGB = NULL;

    if (config.no_gui != 1) {

        munmap(addrGUI, nBytesGUI);
        close(fdGUI);
    }

    return 0;
}

int capture_terminate()
{
    INFO("Called termination of vtcapture");

    capture_run = false;
    if (capture_thread != 0) {

        INFO("Stopping capture thread...");
        pthread_join(capture_thread, NULL);
    }

    if (config.no_video != 1) {
        
        INFO("Stopping video capture...");
        if (driver) {

            vtCapture_stop(driver, client);
            vtCapture_postprocess(driver, client);
            vtCapture_finalize(driver, client);
            vtCapture_release(driver);
            driver = NULL;
        }
    }

    if (config.no_gui != 1) {

        INFO("Stopping GUI capture...");
        HAL_GAL_DestroySurface(&surfaceInfo);
    }

    return 0;
}

void capture_frame()
{
    int indone = 0;
    _LibVtCaptureBufferInfo buff;

    // Get video data
    if (config.no_video != 1) {

        vtCapture_currentCaptureBuffInfo(driver, &buff);
        memcpy(videoY, buff.start_addr0, buff.size0);
        memcpy(videoUV, buff.start_addr1, buff.size1);
    }

    // Get GUI data
    if (config.no_gui != 1) {

        if ((indone = HAL_GAL_CaptureFrameBuffer(&surfaceInfo)) != 0) {

            ERR("HAL_GAL_CaptureFrameBuffer failed: %x", indone);
            return;
        }
        memcpy(guiABGR, addrGUI, surfaceInfo.property);
    }

    // Convert Video+GUI
    if(config.no_video != 1 && config.no_gui != 1 ) {

        // YUV -> ARGB
        NV21ToARGBMatrix(videoY, strideVideo, videoUV, strideVideo, videoARGB, strideGUI, &kYvuH709Constants, width, height);
        // ABGR -> ARGB
        ABGRToARGB(guiABGR, strideGUI, guiARGB, strideGUI, width, height);
        // blend video and gui
        ARGBBlend(guiARGB, strideGUI, videoARGB, strideGUI, outARGB, strideGUI, width, height);
        // remove alpha channel
        ARGBToRGB24(outARGB, strideGUI, outRGB, width * 3, width, height);
    }
    // Convert Video only
    else if (config.no_video != 1 && config.no_gui == 1) {

        // YUV -> RGB
        NV21ToRGB24Matrix(videoY, strideVideo, videoUV, strideVideo, outRGB, width * 3, &kYvuH709Constants, width, height);
    }
    // Convert GUI only
    else if (config.no_gui != 1 && config.no_video == 1) {

        // ABGR -> ARGB
        ABGRToARGB(guiABGR, strideGUI, guiARGB, strideGUI, width, height);
        // remove alpha channel
        ARGBToRGB24(guiARGB, strideGUI, outRGB, width * 3, width, height);
    }
    
    imagedata_cb(width, height, outRGB);
}

void* capture_thread_target(void* data)
{
    uint64_t frame_counter = 0;
    uint64_t frame_counter_start = getticks_us();
    while (capture_run) {
        uint64_t frame_start = getticks_us();
        capture_frame();
        int64_t wait_time = (1000000 / config.fps) - (getticks_us() - frame_start);

        if (wait_time > 0)
            usleep(wait_time);

        frame_counter += 1;

        if (frame_counter >= 60) {
            double fps = (frame_counter * 1000000.0) / (getticks_us() - frame_counter_start);
            DBG("framerate: %.6f FPS", fps);
            frame_counter = 0;
            frame_counter_start = getticks_us();
        }
    }
}
