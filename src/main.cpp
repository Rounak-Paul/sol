#include "core/application.h"
#include "core/logger.h"

using sol::Logger;

int main(int argc, char* argv[]) {
    Logger::SetLogFile("sol.log");
    Logger::Info("Sol application starting");
    
    sol::Application app;
    app.SetArgs(argc, argv);
    tvk::AppConfig config;
    config.title = "Sol";
    config.width = 1280;
    config.height = 720;
    config.vsync = true;
    config.enableKeyboardNav = false;   // Sol handles all keyboard input itself
    config.enableIdleThrottling = true;
    config.idleFrameRate = 30.0f;
    app.Run(config);
    
    Logger::Info("Sol application terminated");
    return 0;
}
