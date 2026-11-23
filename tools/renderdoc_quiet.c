// Hides RenderDoc's overlay and turns off its capture keys (F12, PrtScn).
// Preloaded into Proton's wine by tools/xvalker; it only acts when RenderDoc's Vulkan layer
// is already in the process (it never loads RenderDoc itself).
#include <dlfcn.h>
#include <pthread.h>
#include <renderdoc_app.h>
#include <unistd.h>

static void *worker(void *arg) {
    (void)arg;
    // the layer is loaded when the title makes its Vulkan instance: wait up to 60 s, and look
    // often so that the overlay is gone before the first frame is shown
    for (int i = 0; i < 6000; i++) {
        void *module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (module) {
            pRENDERDOC_GetAPI get = (pRENDERDOC_GetAPI)dlsym(module, "RENDERDOC_GetAPI");
            RENDERDOC_API_1_1_2 *api = 0;
            if (get && get(eRENDERDOC_API_Version_1_1_2, (void **)&api) == 1) {
                api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
                api->SetCaptureKeys(0, 0);
            }
            return 0;
        }
        usleep(10000);
    }
    return 0;
}

__attribute__((constructor)) static void init(void) {
    pthread_t thread;
    if (pthread_create(&thread, 0, worker, 0) == 0)
        pthread_detach(thread);
}
