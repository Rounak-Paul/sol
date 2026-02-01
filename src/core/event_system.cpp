#include "event_system.h"

namespace sol {

Event::Event(const EventId& id)
    : m_Id(id),
      m_Handler(nullptr),
      m_SuccessCallback(nullptr),
      m_FailureCallback(nullptr) {
}

EventSystem& EventSystem::GetInstance() {
    static EventSystem instance;
    return instance;
}

Event& Event::SetHandler(Handler handler) {
    m_Handler = handler;
    return *this;
}

Event& Event::SetSuccessCallback(SuccessCallback callback) {
    m_SuccessCallback = callback;
    return *this;
}

Event& Event::SetFailureCallback(FailureCallback callback) {
    m_FailureCallback = callback;
    return *this;
}

void EventSystem::RegisterEvent(std::shared_ptr<Event> event) {
    if (!event) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Events[event->GetId()] = event;
}

void EventSystem::UnregisterEvent(const EventId& id) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Events.erase(id);
}

bool EventSystem::ExecuteEvent(const EventId& id, const EventData& data) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    auto it = m_Events.find(id);
    if (it == m_Events.end()) {
        return false;
    }

    auto event = it->second;
    const auto& handler = event->GetHandler();

    if (!handler) {
        return false;
    }

    try {
        bool success = handler(data);

        if (success) {
            const auto& successCallback = event->GetSuccessCallback();
            if (successCallback) {
                successCallback(data);
            }
        } else {
            const auto& failureCallback = event->GetFailureCallback();
            if (failureCallback) {
                failureCallback("Event handler returned false");
            }
        }

        return success;
    } catch (const std::exception& e) {
        const auto& failureCallback = event->GetFailureCallback();
        if (failureCallback) {
            failureCallback(std::string("Exception: ") + e.what());
        }
        return false;
    }
}

} // namespace sol
