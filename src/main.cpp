#include "core/application.h"
#include "core/logger.h"

using sol::Logger;

int main() {
    Logger::SetLogFile("sol.log");
    Logger::Info("Sol application starting");
    
    sol::Application app;
    app.Run("Sol", 800, 600);
    
    Logger::Info("Sol application terminated");
    return 0;
}
