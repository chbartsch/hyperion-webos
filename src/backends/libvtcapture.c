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

// Halgal
HAL_GAL_SURFACE surfaceInfo;
size_t nBytesGUI;
char *addrGUI;
int fdGUI;

// Video
const char *caller = "hyperion-webos_service";
VT_DRIVER *driver = NULL;
char client[128] = "00";
_LibVtCaptureProperties props;

// Buffer
char *videoY = NULL;
char *videoUV = NULL;
char *videoARGB = NULL;
char *guiABGR = NULL;
char *guiARGB = NULL;
char *outARGB = NULL;
char *outRGB = NULL;

// All
bool capture_run = false;
int strideGUI;
int strideVideo;
int width = 0;
int height = 0;
cap_backend_config_t config = {0, 0, 0, 0};
cap_imagedata_callback_t imagedata_cb = NULL;

// Prototypes
int halgal_initialize();
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

    // Important check
    if (config.no_gui == 1 && config.no_video == 1) {

        ERR("Neither UI nor video capture is enabled. Enable at least one of them.");
        return -1;
    }

    INFO("Copying config done. Initialize vars..");

    imagedata_cb = callback;

    VT_RESOLUTION_T resolution = {config.resolution_width, config.resolution_height};
    VT_DUMP_T dump = 2;
    VT_LOC_T loc = {0, 0};
    VT_BUF_T buf_cnt = 3;

    // Sorry, no unlimited fps for you.
    if (config.fps == 0)
        config.fps = 60;

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
    int rval;
    _LibVtCaptureBufferInfo buff;

    INFO("Initialization of capture devices..");

    // GUI
    if (config.no_gui != 1) {

        if ((rval = halgal_initialize()) != 0) {
            
            ERR("halgal_initialize() failed: %x", rval);
            return rval;
        }

        guiABGR = (char *) malloc(nBytesGUI);
        guiARGB = (char *) malloc(nBytesGUI);
        outARGB = (char *) malloc(nBytesGUI);

        addrGUI = (char *) mmap(0, nBytesGUI, 3, 1, fdGUI, surfaceInfo.offset);
    }

    // Video
    if (config.no_video != 1) {

        if ((rval = vtcapture_initialize()) != 0) {
            
            ERR("vtcapture_initialize() failed: %x", rval);
            return rval;
        }
        
        vtCapture_currentCaptureBuffInfo(driver, &buff);

        videoY = (char *) malloc(buff.size0);
        videoUV = (char *) malloc(buff.size1);
        videoARGB = (char *) malloc(width * height * 4 * sizeof(char));
    }
    
    // Output
    outRGB = (char *) malloc(width * height * 3 * sizeof(char));
    
    return 0;
}

int halgal_initialize()
{
    int rval;

    INFO("Graphical capture enabled. Begin init..");
    if ((rval = HAL_GAL_Init()) != 0) {

        ERR("HAL_GAL_Init failed: %x", rval);
        return -1;
    }
    INFO("HAL_GAL_Init done! Exit: %d", rval);   

    if ((rval = HAL_GAL_CreateSurface(config.resolution_width, config.resolution_height, 0, &surfaceInfo)) != 0) {

        ERR("HAL_GAL_CreateSurface failed: %x", rval);
        return -1;
    }
    INFO("HAL_GAL_CreateSurface done! SurfaceID: %d", surfaceInfo.vendorData);

    if ((rval = HAL_GAL_CaptureFrameBuffer(&surfaceInfo)) != 0) {

        ERR("HAL_GAL_CaptureFrameBuffer failed: %x", rval);
        return -1;
    }
    INFO("HAL_GAL_CaptureFrameBuffer done! %x", rval);

    if ((fdGUI = open("/dev/gfx", 2)) < 0) {

        ERR("HAL_GAL: gfx open fail result: %d", fdGUI);
        return -1;
    }
    INFO("HAL_GAL: gfx open ok result: %d", fdGUI);

    // Just in case vtcapture was initialized first
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
    if (nBytesGUI == 0)
        nBytesGUI = strideGUI * height;

    INFO("Halgal done!");

    return 0;
}

int vtcapture_initialize()
{
    int rval = 0;

    _LibVtCapturePlaneInfo plane;
    _LibVtCaptureBufferInfo buff;

    INFO("Starting vtcapture initialization.");

    if (!(driver = vtCapture_create())) {
        
        ERR("Could not create vtcapture driver.");
        return -1;
    }
    INFO("Driver created!");

    if ((rval = vtCapture_init(driver, caller, client)) == 17) {

        ERR("vtCapture_init not ready yet return: %d", rval);
        return 17;
    }
    else if (rval == 11) {

        ERR("vtCapture_init failed: %d Permission denied! Quitting...", rval);
        return 11;
    }
    else if (rval != 0) {

        ERR("vtCapture_init failed: %d Quitting...", rval);
        return -1;
    }
    INFO("vtCapture_init done! Caller_ID: %s Client ID: %s", caller, client);

    if ((vtCapture_preprocess(driver, client, &props)) == 1) {

        ERR("vtCapture_preprocess not ready yet return: %d", rval);
        return 1;
    }
    else if (rval != 0) {
        
        ERR("vtCapture_preprocess failed: %x Quitting...", rval);
        return -1;
    }
    INFO("vtCapture_preprocess done!");

    if ((vtCapture_planeInfo(driver, client, &plane)) != 0 ) {

        ERR("vtCapture_planeInfo failed: %xQuitting...", rval);
        return -1;
    }

    // Just in case halgal was initialized first
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

    INFO("vtCapture_planeInfo done! stride: %d Region: x: %d, y: %d, w: %d, h: %d Active Region: x: %d, y: %d w: %d h: %d", 
            plane.stride, plane.planeregion.a, plane.planeregion.b, plane.planeregion.c, plane.planeregion.d, 
            plane.activeregion.a, plane.activeregion.b, plane.activeregion.c, plane.activeregion.d);

    if ((vtCapture_process(driver, client)) != 0) {

        ERR("vtCapture_process failed: %xQuitting...", rval);
        return -1;
    }
    INFO("vtCapture_process done!");

    int cnter = 0;
    do {

        usleep(100000);

        if ((rval = vtCapture_currentCaptureBuffInfo(driver, &buff)) == 0 ) {

            break;
        }
        else if (rval != 2) {

            ERR("vtCapture_currentCaptureBuffInfo failed: %x Quitting...", rval);
            capture_terminate();
            return -1;
        }
        cnter++;
    } while(rval != 0);

    INFO("vtCapture_currentCaptureBuffInfo done after %d tries! addr0: %p addr1: %p size0: %d size1: %d", 
            cnter, buff.start_addr0, buff.start_addr1, buff.size0, buff.size1);

    INFO("vtcapture initialization finished.");

    return 0;
}

int capture_start()
{
    INFO("Starting capture thread..");
    
    capture_run = true;
    if (pthread_create(&capture_thread, NULL, capture_thread_target, NULL) != 0)
        return -1;

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
        memcpy(guiABGR, addrGUI, nBytesGUI);
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

            // Fix for double capture thread
            if (pthread_self() != capture_thread) {
                DBG("We are not the main thread, exiting");
                pthread_exit(NULL);
            }

            frame_counter = 0;
            frame_counter_start = getticks_us();
        }
    }
}
