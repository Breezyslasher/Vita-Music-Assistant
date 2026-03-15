#include "app.h"
#include "view/media_item_cell.hpp"
#include "view/recycling_grid.hpp"
#include "view/now_playing_view.hpp"
#include <borealis.hpp>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/ssl.h>
#include <psp2/http.h>
#include <psp2/ime.h>
#include <psp2/pgf.h>
#include <psp2/apputil.h>

// Memory configuration
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024; // 2MB main stack
int _newlib_heap_size_user = 172 * 1024 * 1024;            // 172MB heap

static char net_memory[2 * 1024 * 1024];     // 2MB NET buffer
static char ssl_memory[512 * 1024];            // 512KB SSL buffer
static char http_memory[1024 * 1024];          // 1MB HTTP buffer

static void vita_init_modules() {
    // Network
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam netParam;
    netParam.memory = net_memory;
    netParam.size = sizeof(net_memory);
    netParam.flags = 0;
    sceNetInit(&netParam);
    sceNetCtlInit();

    // SSL & HTTP
    sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    sceSslInit(sizeof(ssl_memory));
    sceHttpInit(sizeof(http_memory));

    // Input
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);

    // App utilities
    SceAppUtilInitParam initParam;
    SceAppUtilBootParam bootParam;
    memset(&initParam, 0, sizeof(SceAppUtilInitParam));
    memset(&bootParam, 0, sizeof(SceAppUtilBootParam));
    sceAppUtilInit(&initParam, &bootParam);

    // Set CPU/GPU clocks for smooth UI
    scePowerSetArmClockFrequency(333);
    scePowerSetBusClockFrequency(166);
    scePowerSetGpuClockFrequency(166);
    scePowerSetGpuXbarClockFrequency(166);
}

static void vita_cleanup_modules() {
    sceHttpTerm();
    sceSslTerm();
    sceNetCtlTerm();
    sceNetTerm();

    sceSysmoduleUnloadModule(SCE_SYSMODULE_PGF);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_SSL);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}
#endif

int main(int argc, char* argv[]) {
#ifdef __vita__
    vita_init_modules();
#endif

    // Initialize Borealis
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    if (!brls::Application::init()) {
        brls::Logger::error("Failed to initialize Borealis");
#ifdef __vita__
        vita_cleanup_modules();
        sceKernelExitProcess(0);
#endif
        return 1;
    }

    brls::Application::setGlobalQuit(false);

    // Register custom views
    brls::Application::registerXMLView("MediaItemCell", vita_ma::MediaItemCell::create);
    brls::Application::registerXMLView("RecyclingGrid", vita_ma::RecyclingGrid::create);
    brls::Application::registerXMLView("NowPlayingView", vita_ma::NowPlayingView::create);

    // Initialize app
    auto& app = vita_ma::App::instance();
    app.init();

    // Log startup
    brls::Logger::info("Vita Music Assistant started");
    brls::Logger::info("Data path: {}", app.getDataPath());

    // Start the app - will push login or main activity
    // (handled by Application::init based on saved settings)

    // Main loop
    while (brls::Application::mainLoop()) {
        // Borealis handles rendering and input
    }

    // Cleanup
    app.shutdown();

#ifdef __vita__
    vita_cleanup_modules();
    sceKernelExitProcess(0);
#endif

    return 0;
}
