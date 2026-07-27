#include "stems/demucsonnxrunner.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mixxx::stems {
namespace {

constexpr std::size_t kMinimumSegmentSampleCount = 44100;
constexpr std::size_t kMaximumSegmentSampleCount =
        DemucsOnnxRunner::kDefaultSegmentSampleCount;

Ort::SessionOptions makeSessionOptions(int intraOpThreadCount) {
    if (intraOpThreadCount < 1) {
        throw std::invalid_argument(
                "ONNX Runtime intra-op thread count must be positive");
    }

    Ort::SessionOptions options;
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetIntraOpNumThreads(intraOpThreadCount);
    options.SetInterOpNumThreads(1);
#if defined(_M_ARM64) || defined(__aarch64__)
    // HTDemucs activations dominate peak memory on 4 GiB Raspberry Pi
    // systems. Direct CPU allocations trade some inference latency for a
    // substantially lower peak than the reusable arena.
    options.DisableCpuMemArena();
#endif
    options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
    return options;
}

std::string shapeString(std::span<const int64_t> shape) {
    std::ostringstream stream;
    stream << '[';
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << shape[index];
    }
    stream << ']';
    return stream.str();
}

void requireFloatTensor(
        const Ort::TypeInfo& typeInfo,
        const char* tensorName) {
    const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    if (tensorInfo.GetElementType() !=
            ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error(
                std::string("Expected float ") + tensorName + " tensor");
    }
}

} // namespace

class DemucsOnnxRunner::Impl final {
  public:
    Impl(const std::filesystem::path& modelPath, int intraOpThreadCount)
            : m_environment(ORT_LOGGING_LEVEL_WARNING, "mixxx-stems"),
              m_sessionOptions(makeSessionOptions(intraOpThreadCount)),
              m_session(m_environment, modelPath.c_str(), m_sessionOptions) {
        validateContract();
    }

    void validateContract() {
        if (m_session.GetInputCount() != 1 ||
                m_session.GetOutputCount() != 1) {
            throw std::runtime_error(
                    "HTDemucs must expose exactly one input and one output");
        }

        Ort::AllocatorWithDefaultOptions allocator;
        const auto inputName = m_session.GetInputNameAllocated(0, allocator);
        const auto outputName = m_session.GetOutputNameAllocated(0, allocator);
        m_contract.inputName = inputName.get();
        m_contract.outputName = outputName.get();
        if (m_contract.inputName != "input" ||
                m_contract.outputName != "output") {
            throw std::runtime_error(
                    "Unexpected HTDemucs input or output tensor name");
        }

        const auto inputType = m_session.GetInputTypeInfo(0);
        const auto outputType = m_session.GetOutputTypeInfo(0);
        requireFloatTensor(inputType, "input");
        requireFloatTensor(outputType, "output");
        m_contract.inputShape =
                inputType.GetTensorTypeAndShapeInfo().GetShape();
        m_contract.declaredOutputShape =
                outputType.GetTensorTypeAndShapeInfo().GetShape();
        if (m_contract.inputShape.size() != 3 ||
                m_contract.inputShape[0] !=
                        static_cast<int64_t>(kBatchSize) ||
                m_contract.inputShape[1] !=
                        static_cast<int64_t>(kAudioChannelCount) ||
                m_contract.inputShape[2] <
                        static_cast<int64_t>(kMinimumSegmentSampleCount) ||
                m_contract.inputShape[2] >
                        static_cast<int64_t>(kMaximumSegmentSampleCount)) {
            throw std::runtime_error(
                    "Unexpected input shape: " +
                    shapeString(m_contract.inputShape));
        }
        const std::array<int64_t, 4> expectedOutputShape = {
                static_cast<int64_t>(kBatchSize),
                static_cast<int64_t>(kSourceCount),
                static_cast<int64_t>(kAudioChannelCount),
                m_contract.inputShape[2],
        };
        if (m_contract.declaredOutputShape.size() !=
                expectedOutputShape.size()) {
            throw std::runtime_error(
                    "Unexpected output rank: " +
                    shapeString(m_contract.declaredOutputShape));
        }
        for (std::size_t index = 0;
                index < expectedOutputShape.size();
                ++index) {
            if (m_contract.declaredOutputShape[index] != -1 &&
                    m_contract.declaredOutputShape[index] !=
                            expectedOutputShape[index]) {
                throw std::runtime_error(
                        "Unexpected declared output shape: " +
                        shapeString(m_contract.declaredOutputShape));
            }
        }
    }

    TensorContract m_contract;
    Ort::Env m_environment;
    Ort::SessionOptions m_sessionOptions;
    Ort::Session m_session;
};

DemucsOnnxRunner::DemucsOnnxRunner(
        const std::filesystem::path& modelPath,
        int intraOpThreadCount)
        : m_pImpl(
                  std::make_unique<Impl>(modelPath, intraOpThreadCount)) {
}

DemucsOnnxRunner::~DemucsOnnxRunner() = default;

const DemucsOnnxRunner::TensorContract& DemucsOnnxRunner::contract()
        const noexcept {
    return m_pImpl->m_contract;
}

std::size_t DemucsOnnxRunner::segmentSampleCount() const noexcept {
    return static_cast<std::size_t>(
            m_pImpl->m_contract.inputShape[2]);
}

std::vector<float> DemucsOnnxRunner::run(
        std::span<const float> input) const {
    if (input.size() != inputElementCount()) {
        throw std::invalid_argument(
                "HTDemucs input contains an unexpected number of elements");
    }
    const std::array<int64_t, 3> inputShape = {
            static_cast<int64_t>(kBatchSize),
            static_cast<int64_t>(kAudioChannelCount),
            static_cast<int64_t>(segmentSampleCount()),
    };
    const std::array<int64_t, 4> outputShape = {
            static_cast<int64_t>(kBatchSize),
            static_cast<int64_t>(kSourceCount),
            static_cast<int64_t>(kAudioChannelCount),
            static_cast<int64_t>(segmentSampleCount()),
    };

    const auto memoryInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputTensor = Ort::Value::CreateTensor<float>(memoryInfo,
            const_cast<float*>(input.data()),
            input.size(),
            inputShape.data(),
            inputShape.size());
    const std::array inputNames = {m_pImpl->m_contract.inputName.c_str()};
    const std::array outputNames = {m_pImpl->m_contract.outputName.c_str()};
    auto outputs = m_pImpl->m_session.Run(Ort::RunOptions{nullptr},
            inputNames.data(),
            &inputTensor,
            inputNames.size(),
            outputNames.data(),
            outputNames.size());
    if (outputs.size() != 1 || !outputs.front().IsTensor()) {
        throw std::runtime_error(
                "ONNX Runtime returned an unexpected output value");
    }

    const auto outputInfo = outputs.front().GetTensorTypeAndShapeInfo();
    if (outputInfo.GetElementType() !=
            ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("ONNX Runtime returned a non-float output");
    }
    if (outputInfo.GetShape() !=
            std::vector<int64_t>(
                    outputShape.begin(), outputShape.end())) {
        throw std::runtime_error(
                "Unexpected runtime output shape: " +
                shapeString(outputInfo.GetShape()));
    }
    if (outputInfo.GetElementCount() != outputElementCount()) {
        throw std::runtime_error(
                "ONNX Runtime returned an unexpected output element count");
    }

    const auto* const outputData = outputs.front().GetTensorData<float>();
    return {outputData, outputData + outputElementCount()};
}

std::string DemucsOnnxRunner::runtimeVersion() {
    return Ort::GetVersionString();
}

} // namespace mixxx::stems
