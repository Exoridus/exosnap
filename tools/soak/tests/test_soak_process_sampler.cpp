#include "soak_execution_state.h"
#include "soak_process_sampler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <exosnap/engine/recorder_session.h>
#include <fstream>

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

TEST(SoakProcessSampler, StorageReadsTheActualEngineMediaForMkvAndMp4) {
    using exosnap::engine::Container;
    const auto prefix =
        std::filesystem::temp_directory_path() /
        ("exosnap-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (const auto container : {Container::Matroska, Container::Mp4}) {
        const auto output = std::filesystem::path(prefix.string() + (container == Container::Mp4 ? ".mp4" : ".mkv"));
        const auto active = container == Container::Mp4 ? exosnap::engine::DeriveTransientMkvPath(output) : output;
        {
            std::ofstream stream(active, std::ios::binary);
            ASSERT_TRUE(stream.good());
            stream << "proof";
        }
        const auto observation = exosnap::soak::ReadSoakStorage(output, container, prefix.root_path(), 1.0);
        EXPECT_EQ(observation.file_bytes, 5u) << output.string();
        EXPECT_TRUE(observation.output_free_bytes);
        EXPECT_TRUE(observation.system_free_bytes);
        std::error_code error;
        std::filesystem::remove(active, error);
    }
}

TEST(SoakExecutionState, DefersSystemAndDisplayIdleThenRestoresTheCreatingThread) {
    const auto original = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    ASSERT_NE(original, 0u);
    {
        exosnap::soak::SoakExecutionState lease;
        const auto held = SetThreadExecutionState(ES_CONTINUOUS);
        EXPECT_EQ(held & (ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED), ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    }
    const auto restored = SetThreadExecutionState(ES_CONTINUOUS);
    EXPECT_NE(restored & ES_SYSTEM_REQUIRED, 0u);
    EXPECT_EQ(restored & ES_DISPLAY_REQUIRED, 0u);
    EXPECT_NE(SetThreadExecutionState(original), 0u);
}
