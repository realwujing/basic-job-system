#include <assert.h>
#include <chrono>
#include <thread>
#include <iostream>

// jobsystem settings
// #define JOBSYSTEM_ENABLE_PROFILING                // Enables worker/job profiling, and an ascii profile dump on shutdown.
#define JOBSYSTEM_ASSERT(...) assert(__VA_ARGS__) // Directs internal system asserts to app-specific assert mechanism.

// jobsystem include
#include "jobsystem.h"

int max(int &num1, int &num2)
{
    return num1 > num2 ? 1 : 0;
}

int main()
{
    jobsystem::JobManagerDescriptor jobManagerDesc;

    const size_t kWorkerCount = 16;
    for (size_t i = 0; i < kWorkerCount; ++i)
    {
        jobManagerDesc.m_workers.emplace_back("Worker");
    }

    jobsystem::JobManager jobManager;
    if (!jobManager.Create(jobManagerDesc))
    {
        return 1;
    }

    const size_t kNumParallelJobs = 1000;
    const size_t kItersPerJob = 100000;

    float floats[64];

    int num1 = 2, num2 = 3;

    auto something = [&]()
    {
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[0] *= 5.f;
        std::cout << "something" << std::endl;
        std::cout << "max(num1, num2):" << max(num1, num2) << std::endl;
    };

    auto somethingAfterThat = [&]()
    {
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[8] *= 5.f;
        std::cout << "somethingAfterThat" << std::endl;
    };

    auto parallelThing1 = [&]()
    {
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[16] *= 5.f;
        std::cout << "parallelThing1" << std::endl;
    };

    auto parallelThing2 = [&]()
    {
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[24] *= 5.f;
    };

    auto parallelThing3 = [&]()
    {
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[32] *= 5.f;
    };

    auto finalThing = [&]()
    {
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[40] *= 5.f;
    };

    jobsystem::JobChainBuilder<10000> builder(jobManager);

    // Run a couple jobs in succession.
    builder
        .Do(something, 'a')
        .Then()
        .Do(somethingAfterThat, 'b')
        .Then()
        .Together();

    // Run 1k jobs in parallel.
    for (size_t i = 0; i < kNumParallelJobs; ++i)
    {
        const char c = 'A' + (char)(i % ('z' - 'A'));
        builder.Do(parallelThing1, c);
    }

    // Run a final "join" job.
    builder
        .Close()
        .Then()
        .Do(finalThing, 'Z');

    // Run the jobs and assist until complete.
    builder
        .Go()
        .AssistAndWaitForAll();

    // dex::ICaptureStop();

    return builder.Failed() ? 1 : 0;
}
