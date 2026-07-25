#include "stems/demucsonnxrunner.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mixxx::stems {
namespace {

constexpr std::array<int64_t, 3> kExpectedInputShape = {
        static_cast<int64_t>(DemucsOnnxRunner::kBatchSize),
        static_cast<int64_t>(DemucsOnnxRunner::kAudioChannelCount),
        static_cast<int64_t>(DemucsOnnxRunner::kSegmentSampleCount),
};
constexpr std::array<int64_t, 4> kExpectedOutputShape = {
        static_cast<int64_t>(DemucsOnnxRunner::kBatchSize),
        static_cast<int64_t>(DemucsOnnxRunner::kSourceCount),
        static_cast<int64_t>(DemucsOnnxRunner::kAudioChannelCount),
        static_cast<int64_t>(DemucsOnnxRunner::kSegmentSampleCount),
};

Ort::SessionOptions makeSessionOptions(int intraOpThreadCount) {
    if (intraOpThreadCount < 1) {
        throw std::invalid_argument(
                "ONNX Runtime intra-op thread count must be positive");
    }

    Ort::SessionOptions options;
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetIntraOpNumThreads(intraOpThreadCount);
    options.SetInterOpNumThreads(1);
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

template<std::size_t Size>
void requireExactShape(
        const std::vector<int64_t>& actual,
        const std::array<int64_t, Size>& expected,
        const char* tensorName) {
    if (!std::equal(actual.begin(),
                actual.end(),
                expected.begin(),
                expected.end())) {
        throw std::runtime_error(std::string("Unexpected ") + tensorName +
                " shape: " + shapeString(actual));
    }
}

void requireDeclaredOutputShape(const std::vector<int64_t>& actual) {
    if (actual.size() != kExpectedOutputShape.size()) {
        throw std::runtime_error(
                "Unexpected output rank: " + shapeString(actual));
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index] != -1 && actual[index] != kExpectedOutputShape[index]) {
            throw std::runtime_error(
                    "Unexpected declared output shape: " +
                    shapeString(actual));
        }
    }
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
        requireExactShape(
                m_contract.inputShape, kExpectedInputShape, "input");
        requireDeclaredOutputShape(m_contract.declaredOutputShape);
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

std::vector<float> DemucsOnnxRunner::run(
        std::span<const float> input) const {
    if (input.size() != kInputElementCount) {
        throw std::invalid_argument(
                "HTDemucs input contains an unexpected number of elements");
    }

    const auto memoryInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputTensor = Ort::Value::CreateTensor<float>(memoryInfo,
            const_cast<float*>(input.data()),
            input.size(),
            kExpectedInputShape.data(),
            kExpectedInputShape.size());
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
    requireExactShape(
            outputInfo.GetShape(), kExpectedOutputShape, "runtime output");
    if (outputInfo.GetElementCount() != kOutputElementCount) {
        throw std::runtime_error(
                "ONNX Runtime returned an unexpected output element count");
    }

    const auto* const outputData = outputs.front().GetTensorData<float>();
    return {outputData, outputData + kOutputElementCount};
}

std::string DemucsOnnxRunner::runtimeVersion() {
    return Ort::GetVersionString();
}

} // namespace mixxx::stems
