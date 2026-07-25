#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "stems/demucsonnxrunner.h"

namespace {

std::vector<float> makeInput() {
    using mixxx::stems::DemucsOnnxRunner;

    std::vector<float> input(DemucsOnnxRunner::kInputElementCount);
    for (std::size_t sample = 0;
            sample < DemucsOnnxRunner::kSegmentSampleCount;
            ++sample) {
        const auto time = static_cast<double>(sample) / 44100.0;
        input[sample] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * time));
        input[DemucsOnnxRunner::kSegmentSampleCount + sample] =
                static_cast<float>(
                        0.1 * std::sin(2.0 * std::numbers::pi * 660.0 * time));
    }
    return input;
}

} // namespace

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
        const auto input = makeInput();
        const auto output = runner.run(input);
        if (!std::all_of(output.begin(), output.end(), [](float sample) {
                return std::isfinite(sample);
            })) {
            throw std::runtime_error(
                    "ONNX Runtime output contains non-finite samples");
        }

        const auto maxSample = std::max_element(
                output.begin(), output.end(), [](float left, float right) {
                    return std::abs(left) < std::abs(right);
                });
        std::cout << "ONNX Runtime " << runner.runtimeVersion() << '\n'
                  << "Input tensor: "
                  << runner.contract().inputName << " [1, 2, 343980]\n"
                  << "Output tensor: "
                  << runner.contract().outputName << " [1, 4, 2, 343980]\n"
                  << "Output elements: " << output.size() << '\n'
                  << "Maximum absolute output: " << std::abs(*maxSample)
                  << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "HTDemucs smoke test failed: " << exception.what()
                  << '\n';
        return 1;
    }
}
