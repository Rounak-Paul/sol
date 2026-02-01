#include "job_system.h"
#include <thread>

namespace sol {

Job::Job(Function function)
    : m_Function(std::move(function)) {}

Job& Job::SetSuccessCallback(SuccessCallback callback) {
    m_SuccessCallback = std::move(callback);
    return *this;
}

Job& Job::SetFailureCallback(FailureCallback callback) {
    m_FailureCallback = std::move(callback);
    return *this;
}

JobSystem& JobSystem::GetInstance() {
    static JobSystem instance;
    return instance;
}

JobSystem::JobSystem() : m_Running(true) {
    // Create n-1 worker threads where n is number of CPU cores
    uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);

    for (uint32_t i = 0; i < numThreads; ++i) {
        m_WorkerThreads.emplace_back(&JobSystem::WorkerThread, this);
    }
}

JobSystem::~JobSystem() {
    ShutdownWorkers();
}

void JobSystem::SubmitJob(std::shared_ptr<Job> job, const JobData& data) {
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (!m_Running) return;
        m_JobQueue.push({job, data});
    }
    m_QueueCV.notify_one();
}

void JobSystem::WorkerThread() {
    while (true) {
        JobTask task;
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_QueueCV.wait(lock, [this] { return !m_JobQueue.empty() || !m_Running; });

            if (!m_Running && m_JobQueue.empty()) {
                break;
            }

            if (m_JobQueue.empty()) {
                continue;
            }

            task = m_JobQueue.front();
            m_JobQueue.pop();
        }

        // Execute the job outside the lock
        if (task.job) {
            try {
                bool success = task.job->GetFunction()(task.data);

                if (success) {
                    if (task.job->GetSuccessCallback()) {
                        task.job->GetSuccessCallback()(task.data);
                    }
                } else {
                    if (task.job->GetFailureCallback()) {
                        task.job->GetFailureCallback()("Job execution returned false");
                    }
                }
            } catch (const std::exception& e) {
                if (task.job->GetFailureCallback()) {
                    task.job->GetFailureCallback()(std::string("Job exception: ") + e.what());
                }
            }
        }
    }
}

void JobSystem::ShutdownWorkers() {
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_Running = false;
    }
    m_QueueCV.notify_all();

    for (auto& thread : m_WorkerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    m_WorkerThreads.clear();
}

} // namespace sol
