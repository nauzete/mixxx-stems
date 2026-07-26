#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
// windows.h must precede psapi.h.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif defined(__linux__)
#include <sys/resource.h>
#endif

#include "stems/demucsonnxrunner.h"

namespace {

constexpr double kSampleRate = 44100.0;

std::uint64_t peakResidentSetBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                &counters,
                sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(
            counters.PeakWorkingSetSize);
#elif defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#else
    return 0;
#endif
}

const char* architectureName() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x64";
#else
    return "other";
#endif
}

std::string makeReport(double elapsedSeconds,
        double realTimeFactor,
        std::uint64_t peakRssBytes,
        int threadCount,
        float maximumAbsoluteOutput) {
    std::ostringstream report;
    report << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"architecture\": \"" << architectureName() << "\",\n"
           << "  \"audio_seconds\": "
           << mixxx::stems::DemucsOnnxRunner::kSegmentSampleCount /
                    kSampleRate
           << ",\n"
           << "  \"elapsed_seconds\": " << elapsedSeconds << ",\n"
           << "  \"intra_op_threads\": " << threadCount << ",\n"
           << "  \"maximum_absolute_output\": "
           << maximumAbsoluteOutput << ",\n"
           << "  \"peak_rss_bytes\": " << peakRssBytes << ",\n"
           << "  \"real_time_factor\": " << realTimeFactor << ",\n"
           << "  \"runtime_version\": \""
           << mixxx::stems::DemucsOnnxRunner::runtimeVersion() << "\"\n"
           << "}\n";
    return report.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: demucs-onnx-benchmark <htdemucs.onnx> "
                     "[threads] [report.json]\n";
        return 2;
    }

    try {
        const std::filesystem::path modelPath(argv[1]);
        if (!std::filesystem::is_regular_file(modelPath)) {
            throw std::runtime_error(
                    "Model file does not exist");
        }
        const int threadCount = argc >= 3 ? std::stoi(argv[2]) : 1;
        if (threadCount < 1) {
            throw std::runtime_error(
                    "Thread count must be positive");
        }

        std::vector<float> input(
                mixxx::stems::DemucsOnnxRunner::kInputElementCount);
        for (std::size_t sample = 0;
                sample <
                mixxx::stems::DemucsOnnxRunner::kSegmentSampleCount;
                ++sample) {
            const auto time =
                    static_cast<double>(sample) / kSampleRate;
            input[sample] = static_cast<float>(
                    0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * time));
            input[mixxx::stems::DemucsOnnxRunner::kSegmentSampleCount +
                    sample] = static_cast<float>(0.1 *
                    std::sin(2.0 * std::numbers::pi * 660.0 * time));
        }

        mixxx::stems::DemucsOnnxRunner runner(
                modelPath, threadCount);
        const auto start = std::chrono::steady_clock::now();
        const auto output = runner.run(input);
        const auto elapsed =
                std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start)
                        .count();
        if (output.size() !=
                        mixxx::stems::DemucsOnnxRunner::
                                kOutputElementCount ||
                !std::all_of(output.begin(),
                        output.end(),
                        [](float sample) {
                            return std::isfinite(sample);
                        })) {
            throw std::runtime_error(
                    "Inference returned invalid output");
        }
        const auto maximum = std::max_element(output.begin(),
                output.end(),
                [](float left, float right) {
                    return std::abs(left) < std::abs(right);
                });
        const auto audioSeconds =
                mixxx::stems::DemucsOnnxRunner::
                        kSegmentSampleCount /
                kSampleRate;
        const auto report = makeReport(elapsed,
                elapsed / audioSeconds,
                peakResidentSetBytes(),
                threadCount,
                std::abs(*maximum));
        std::cout << report;
        if (argc == 4) {
            std::ofstream outputFile(
                    argv[3], std::ios::binary);
            outputFile.exceptions(
                    std::ios::failbit | std::ios::badbit);
            outputFile << report;
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "HTDemucs benchmark failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
