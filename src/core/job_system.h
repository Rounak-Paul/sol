#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <condition_variable>
#include <any>
#include <map>

namespace sol {

// Usage Example:
//
// // Create a job function that does work
// auto job = std::make_shared<sol::Job>([](const sol::JobData& data) {
//     int task_id = std::any_cast<int>(data.at("task_id"));
//     std::cout << "Processing task " << task_id << std::endl;
//     // Do some work...
//     return true; // true = success, false = failure
// });
//
// // Configure success and failure callbacks
// job->SetSuccessCallback([](const sol::JobData& data) {
//     std::cout << "Job completed successfully" << std::endl;
// })
// .SetFailureCallback([](const std::string& error) {
//     std::cout << "Job failed: " << error << std::endl;
// });
//
// // Submit the job with parameters
// sol::JobData data;
// data["task_id"] = 42;
// sol::JobSystem::Submit(job, data);
//
// // Jobs run on n-1 worker threads automatically
// // On application shutdown, call:
// // sol::JobSystem::Shutdown();

using JobData = std::map<std::string, std::any>;

class Job {
public:
    using Function = std::function<bool(const JobData&)>;
    using SuccessCallback = std::function<void(const JobData&)>;
    using FailureCallback = std::function<void(const std::string&)>;

    explicit Job(Function function);
    ~Job() = default;

    Job& SetSuccessCallback(SuccessCallback callback);
    Job& SetFailureCallback(FailureCallback callback);

    const Function& GetFunction() const { return m_Function; }
    const SuccessCallback& GetSuccessCallback() const { return m_SuccessCallback; }
    const FailureCallback& GetFailureCallback() const { return m_FailureCallback; }

private:
    Function m_Function;
    SuccessCallback m_SuccessCallback;
    FailureCallback m_FailureCallback;
};

class JobSystem {
public:
    static JobSystem& GetInstance();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Static convenience methods
    static void Submit(std::shared_ptr<Job> job, const JobData& data = {}) {
        GetInstance().SubmitJob(job, data);
    }
    static void Shutdown() { GetInstance().ShutdownWorkers(); }
    static uint32_t GetWorkerCount() { return GetInstance().GetWorkerThreadCount(); }

private:
    JobSystem();
    ~JobSystem();

    void SubmitJob(std::shared_ptr<Job> job, const JobData& data);
    void ShutdownWorkers();
    uint32_t GetWorkerThreadCount() const { return m_WorkerThreads.size(); }

    void WorkerThread();

    struct JobTask {
        std::shared_ptr<Job> job;
        JobData data;
    };

    std::vector<std::thread> m_WorkerThreads;
    std::queue<JobTask> m_JobQueue;
    mutable std::mutex m_QueueMutex;
    std::condition_variable m_QueueCV;
    bool m_Running;
};

} // namespace sol
