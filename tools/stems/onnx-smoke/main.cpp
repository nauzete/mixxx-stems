#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>

#include "stems/demucsonnxrunner.h"
#include "stems/stemchunkpipeline.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: demucs-onnx-smoke <htdemucs.onnx>\n";
        return 2;
    }

    try {
        const std::filesystem::path modelPath(argv[1]);
        if (!std::filesystem::is_regular_file(modelPath)) {
            std::cerr << "Model file does not exist: " << modelPath << '\n';
            return 2;
        }

        mixxx::stems::DemucsOnnxRunner runner(modelPath);
        mixxx::stems::StemChunkPipeline pipeline(runner);
        const auto frameCount =
                mixxx::stems::StemChunkPipeline::kStrideSampleCount + 1024;
        std::size_t writtenFrameCount = 0;
        float maximumAbsoluteOutput = 0.0F;
        float finalProgress = 0.0F;
        const auto result = pipeline.run(
                frameCount,
                [](std::size_t offset, std::span<float> destination) {
                    for (std::size_t sample = 0;
                            sample < destination.size() / 2;
                            ++sample) {
                        const auto time =
                                static_cast<double>(offset + sample) / 44100.0;
                        destination[sample * 2] = static_cast<float>(
                                0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * time));
                        destination[sample * 2 + 1] = static_cast<float>(
                                0.1 * std::sin(2.0 * std::numbers::pi * 660.0 * time));
                    }
                },
                [&](std::size_t offset,
                        std::size_t outputFrameCount,
                        std::span<const float> output) {
                    if (offset != writtenFrameCount ||
                            !std::all_of(output.begin(),
                                    output.end(),
                                    [](float sample) {
                                        return std::isfinite(sample);
                                    })) {
                        throw std::runtime_error(
                                "Chunk pipeline returned invalid output");
                    }
                    for (const auto sample : output) {
                        maximumAbsoluteOutput =
                                std::max(maximumAbsoluteOutput,
                                        std::abs(sample));
                    }
                    writtenFrameCount += outputFrameCount;
                },
                [&](float progress) {
                    if (progress < finalProgress) {
                        throw std::runtime_error(
                                "Chunk pipeline progress moved backwards");
                    }
                    finalProgress = progress;
                });
        if (result !=
                        mixxx::stems::StemChunkPipeline::Result::Completed ||
                writtenFrameCount != frameCount || finalProgress != 1.0F) {
            throw std::runtime_error(
                    "Chunk pipeline did not complete the entire input");
        }

        std::cout << "ONNX Runtime " << runner.runtimeVersion() << '\n'
                  << "Input tensor: "
                  << runner.contract().inputName << " [1, 2, 343980]\n"
                  << "Output tensor: "
                  << runner.contract().outputName << " [1, 4, 2, 343980]\n"
                  << "Separated frames: " << writtenFrameCount << '\n'
                  << "Maximum absolute output: " << maximumAbsoluteOutput
                  << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "HTDemucs smoke test failed: " << exception.what()
                  << '\n';
        return 1;
    }
}
