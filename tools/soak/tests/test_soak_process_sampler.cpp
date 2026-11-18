#include "soak_process_sampler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

TEST(SoakProcessSampler, CpuNeedsARealIntervalAndGpuNeedsAnAttributedAdapter) {
    exosnap::soak::WinProcessSampler sampler;
    const auto initial = sampler.SampleForAdapter(0);
    EXPECT_FALSE(initial.process_cpu_percent);
    EXPECT_FALSE(initial.system_cpu_percent);
    EXPECT_FALSE(initial.gpu_utilization_percent);
    EXPECT_FALSE(initial.encoder_utilization_percent);
    EXPECT_FALSE(initial.process_gpu_memory_bytes);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto next = sampler.Sample();
    ASSERT_TRUE(next.process_cpu_percent);
    ASSERT_TRUE(next.system_cpu_percent);
    EXPECT_GE(*next.process_cpu_percent, 0.0);
    EXPECT_LE(*next.process_cpu_percent, 100.0);
    EXPECT_GE(*next.system_cpu_percent, 0.0);
    EXPECT_LE(*next.system_cpu_percent, 100.0);
}
