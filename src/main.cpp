#include "core/application.h"
#include "core/logger.h"

int main() {
    sol::Logger::GetInstance().SetLogFilePath("sol.log");
    sol::Logger::GetInstance().Info("Sol application starting");
    
    sol::Application app;
    app.Run("Sol", 800, 600);
    
    sol::Logger::GetInstance().Info("Sol application terminated");
    return 0;
}
