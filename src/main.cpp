#include "core/application.h"
#include "core/logger.h"

using sol::Logger;

int main(int argc, char* argv[]) {
    Logger::SetLogFile("sol.log");
    Logger::Info("Sol application starting");
    
    sol::Application app;
    app.SetArgs(argc, argv);
    app.Run("Sol", 600, 400);
    
    Logger::Info("Sol application terminated");
    return 0;
}
