#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <any>

namespace sol {

// Usage Example:
//
// // Create an event with a handler
// auto event = std::make_shared<sol::Event>("on_button_click");
// event->SetHandler([](const sol::EventData& data) {
//     int button_id = std::any_cast<int>(data.at("button_id"));
//     std::cout << "Button " << button_id << " clicked!" << std::endl;
//     return true; // true = success, false = failure
// })
// .SetSuccessCallback([](const sol::EventData& data) {
//     std::cout << "Event succeeded" << std::endl;
// })
// .SetFailureCallback([](const std::string& error) {
//     std::cout << "Event failed: " << error << std::endl;
// });
//
// // Register the event
// sol::EventSystem::GetInstance().RegisterEvent(event);
//
// // Execute the event with parameters
// sol::EventData data;
// data["button_id"] = 42;
// sol::EventSystem::GetInstance().Execute("on_button_click", data);
//
// // Unregister when done
// sol::EventSystem::GetInstance().UnregisterEvent("on_button_click");

using EventId = std::string;
using EventData = std::map<std::string, std::any>;

class Event {
public:
    using Handler = std::function<bool(const EventData&)>;
    using SuccessCallback = std::function<void(const EventData&)>;
    using FailureCallback = std::function<void(const std::string&)>;

    explicit Event(const EventId& id);
    ~Event() = default;

    Event& SetHandler(Handler handler);
    Event& SetSuccessCallback(SuccessCallback callback);
    Event& SetFailureCallback(FailureCallback callback);

    const EventId& GetId() const { return m_Id; }
    const Handler& GetHandler() const { return m_Handler; }
    const SuccessCallback& GetSuccessCallback() const { return m_SuccessCallback; }
    const FailureCallback& GetFailureCallback() const { return m_FailureCallback; }

private:
    EventId m_Id;
    Handler m_Handler;
    SuccessCallback m_SuccessCallback;
    FailureCallback m_FailureCallback;
};

class EventSystem {
public:
    static EventSystem& GetInstance();

    EventSystem(const EventSystem&) = delete;
    EventSystem& operator=(const EventSystem&) = delete;
    EventSystem(EventSystem&&) = delete;
    EventSystem& operator=(EventSystem&&) = delete;

    // Static convenience methods
    static void Register(std::shared_ptr<Event> event) { GetInstance().RegisterEvent(event); }
    static void Unregister(const EventId& id) { GetInstance().UnregisterEvent(id); }
    static bool Execute(const EventId& id, const EventData& data = {}) { return GetInstance().ExecuteEvent(id, data); }

private:
    EventSystem() = default;
    ~EventSystem() = default;

    void RegisterEvent(std::shared_ptr<Event> event);
    void UnregisterEvent(const EventId& id);
    bool ExecuteEvent(const EventId& id, const EventData& data = {});

    std::map<EventId, std::shared_ptr<Event>> m_Events;
    mutable std::mutex m_Mutex;
};

} // namespace sol
