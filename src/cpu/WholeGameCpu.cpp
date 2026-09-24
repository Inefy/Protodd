#include "WholeGameCpu.hpp"
#include "WholeGameIntent.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define PROTODD_WHOLE_GAME_SSE2 1
#endif

namespace protodd::cpu {
namespace {

std::uint32_t read32(std::istream& input) {
    std::uint32_t value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw std::runtime_error("truncated whole-game weights");
    return value;
}

std::uint16_t read16(std::istream& input) {
    std::uint16_t value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw std::runtime_error("truncated whole-game weights");
    return value;
}

float multiplySum(const float* left, const float* right, std::size_t count) {
    float sum = 0.0f;
    std::size_t index = 0;
#ifdef PROTODD_WHOLE_GAME_SSE2
    __m128 lanes = _mm_setzero_ps();
    for (; index + 4 <= count; index += 4)
        lanes = _mm_add_ps(lanes, _mm_mul_ps(
            _mm_loadu_ps(left + index), _mm_loadu_ps(right + index)));
    alignas(16) float partial[4];
    _mm_store_ps(partial, lanes);
    sum = (partial[0] + partial[1]) + (partial[2] + partial[3]);
#endif
    for (; index < count; ++index) sum += left[index] * right[index];
    return sum;
}

std::vector<float> linear(const Tensor& weight, const Tensor& bias,
                          const std::vector<float>& input) {
    if (weight.shape.size() != 2 || bias.shape != std::vector<std::uint32_t>{weight.shape[0]}
        || input.size() != weight.shape[1]) throw std::runtime_error("linear tensor shape mismatch");
    const auto rows = weight.shape[0], columns = weight.shape[1];
    std::vector<float> result(rows);
    for (std::uint32_t row = 0; row < rows; ++row) {
        float value = bias.values[row];
        const float* weights = weight.values.data() + static_cast<std::size_t>(row) * columns;
        value += multiplySum(weights, input.data(), columns);
        result[row] = value;
    }
    return result;
}

void layerNorm(std::vector<float>& values, const Tensor& weight, const Tensor& bias) {
    if (values.size() != weight.values.size() || values.size() != bias.values.size())
        throw std::runtime_error("layer norm shape mismatch");
    const float mean = std::accumulate(values.begin(), values.end(), 0.0f) / values.size();
    float variance = 0;
    for (const auto value : values) variance += (value - mean) * (value - mean);
    variance /= values.size();
    const float inverse = 1.0f / std::sqrt(variance + 1e-5f);
    for (std::size_t index = 0; index < values.size(); ++index)
        values[index] = (values[index] - mean) * inverse * weight.values[index] + bias.values[index];
}

void gelu(std::vector<float>& values) {
    for (auto& value : values)
        value = 0.5f * value * (1.0f + std::erf(value * 0.7071067811865475f));
}

std::vector<float> convolution(const std::vector<float>& input,
                               std::uint32_t inputChannels, std::uint32_t height,
                               std::uint32_t width, const Tensor& weight,
                               const Tensor& bias, std::uint32_t kernel,
                               std::uint32_t padding) {
    if (weight.shape != std::vector<std::uint32_t>{bias.shape[0], inputChannels, kernel, kernel}
        || input.size() != static_cast<std::size_t>(inputChannels) * height * width)
        throw std::runtime_error("convolution tensor shape mismatch");
    const auto outputChannels = bias.shape[0];
    const auto outputHeight = (height + 2 * padding - kernel) / 2 + 1;
    const auto outputWidth = (width + 2 * padding - kernel) / 2 + 1;
    std::vector<float> output(static_cast<std::size_t>(outputChannels) * outputHeight * outputWidth);
    const auto patchSize = static_cast<std::size_t>(inputChannels) * kernel * kernel;
    std::vector<float> patch(patchSize);
    for (std::uint32_t y = 0; y < outputHeight; ++y)
        for (std::uint32_t x = 0; x < outputWidth; ++x) {
            std::size_t patchIndex = 0;
            for (std::uint32_t in = 0; in < inputChannels; ++in)
                for (std::uint32_t ky = 0; ky < kernel; ++ky)
                    for (std::uint32_t kx = 0; kx < kernel; ++kx) {
                        const auto iy = static_cast<int>(y * 2 + ky) - static_cast<int>(padding);
                        const auto ix = static_cast<int>(x * 2 + kx) - static_cast<int>(padding);
                        patch[patchIndex++] = iy < 0 || ix < 0 || iy >= static_cast<int>(height)
                            || ix >= static_cast<int>(width) ? 0.0f
                            : input[(static_cast<std::size_t>(in) * height + iy) * width + ix];
                    }
            for (std::uint32_t out = 0; out < outputChannels; ++out) {
                const auto* weights = weight.values.data() + static_cast<std::size_t>(out) * patchSize;
                float value = bias.values[out];
                value += multiplySum(weights, patch.data(), patchSize);
                output[(static_cast<std::size_t>(out) * outputHeight + y) * outputWidth + x] = value;
            }
        }
    gelu(output);
    return output;
}

void append(std::vector<float>& target, const std::vector<float>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

float dot(const std::vector<float>& left, const std::vector<float>& right) {
    if (left.size() != right.size()) throw std::runtime_error("dot shape mismatch");
    return multiplySum(left.data(), right.data(), left.size());
}

float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

}  // namespace

WholeGameCpu::WholeGameCpu(const std::filesystem::path& weights) {
    std::ifstream input(weights, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open whole-game weights");
    load(input);
}

WholeGameCpu::WholeGameCpu(std::span<const std::uint8_t> weights) {
    if (weights.empty()) throw std::runtime_error("empty embedded whole-game weights");
    const std::string bytes(reinterpret_cast<const char*>(weights.data()), weights.size());
    std::istringstream input(bytes, std::ios::binary);
    load(input);
}

void WholeGameCpu::load(std::istream& input) {
    std::array<char, 8> magic{};
    input.read(magic.data(), magic.size());
    if (magic != std::array<char, 8>{'P', 'W', 'G', 'M', '1', 0, 0, 0})
        throw std::runtime_error("whole-game weight magic mismatch");
    version_ = read32(input);
    width_ = read32(input); components_ = read32(input);
    const auto count = read32(input);
    if ((version_ != 1 && version_ != 2) || width_ < 64 || width_ > 2048 || components_ < 1 || components_ > 64
        || count < 40 || count > 256) throw std::runtime_error("unsupported whole-game weight architecture");
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto nameLength = read16(input);
        std::uint8_t rank{};
        input.read(reinterpret_cast<char*>(&rank), 1);
        if (!input || nameLength == 0 || nameLength > 128 || rank > 4)
            throw std::runtime_error("invalid whole-game tensor header");
        std::string name(nameLength, '\0');
        input.read(name.data(), nameLength);
        Tensor tensor;
        std::size_t elements = 1;
        for (std::uint8_t dimension = 0; dimension < rank; ++dimension) {
            const auto size = read32(input);
            if (size == 0 || size > 4096 || elements > 50000000 / size)
                throw std::runtime_error("invalid whole-game tensor shape");
            tensor.shape.push_back(size);
            elements *= size;
        }
        tensor.values.resize(elements);
        input.read(reinterpret_cast<char*>(tensor.values.data()),
                   static_cast<std::streamsize>(elements * sizeof(float)));
        if (!input || !parameters_.emplace(name, std::move(tensor)).second)
            throw std::runtime_error("truncated or duplicate whole-game tensor");
        parameterCount_ += elements;
    }
    if (input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("trailing whole-game weight data");
    // Check the tensors needed for the first forward pass now, not mid-game.
    if (parameter("memory.weight_hh").shape != std::vector<std::uint32_t>{width_ * 3, width_}
        || parameter("position.weight").shape != std::vector<std::uint32_t>{components_ * 5, width_})
        throw std::runtime_error("whole-game model dimensions disagree");
    if (version_ == 2 &&
        (parameter("slot_stop.weight").shape != std::vector<std::uint32_t>{2, width_}
         || parameter("slot_transition.weight_hh").shape !=
            std::vector<std::uint32_t>{width_ * 3, width_}
         || parameter("slot_transition.weight_ih").shape !=
            std::vector<std::uint32_t>{width_ * 3, width_ * 2 + 160}))
        throw std::runtime_error("multi-slot model dimensions disagree");
}

const Tensor& WholeGameCpu::parameter(const std::string& name) const {
    const auto found = parameters_.find(name);
    if (found == parameters_.end()) throw std::runtime_error("missing whole-game tensor: " + name);
    return found->second;
}

std::vector<std::vector<float>> WholeGameCpu::encodeEntities(const Observation& row) const {
    const auto count = row.type.size();
    const auto layer = [this](const std::string& name, const std::vector<float>& input) {
        return linear(parameter(name + ".weight"), parameter(name + ".bias"), input);
    };
    const auto norm = [this](const std::string& name, std::vector<float>& values) {
        layerNorm(values, parameter(name + ".weight"), parameter(name + ".bias"));
    };
    std::vector<std::vector<float>> entity(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<float> input;
        input.reserve(72);
        const auto addEmbedding = [&](const std::string& name, int raw, int high) {
            const auto& tensor = parameter(name + ".weight");
            const auto id = static_cast<std::size_t>(std::clamp(raw, 0, high));
            const auto size = tensor.shape[1];
            input.insert(input.end(), tensor.values.begin() + id * size,
                         tensor.values.begin() + (id + 1) * size);
        };
        addEmbedding("unit_type", row.type[index], 255);
        addEmbedding("relation", row.relation[index], 2);
        addEmbedding("order", row.order[index], 255);
        input.insert(input.end(), row.entityNumeric.begin() + index * 16,
                     row.entityNumeric.begin() + (index + 1) * 16);
        entity[index] = layer("entity.0", input);
        norm("entity.1", entity[index]); gelu(entity[index]);
        entity[index] = layer("entity.3", entity[index]); gelu(entity[index]);
    }
    return entity;
}

Output WholeGameCpu::infer(const Observation& row, const std::vector<float>& previousMemory) const {
    return inferImpl(row, previousMemory, true);
}

Output WholeGameCpu::inferImpl(const Observation& row,
                              const std::vector<float>& previousMemory,
                              bool includeHeads,
                              std::vector<std::vector<float>>* encodedEntities) const {
    const auto count = row.type.size();
    if (count == 0 || count > 4096 || row.relation.size() != count || row.order.size() != count
        || row.entityNumeric.size() != count * 16 || row.spatial.size() != 3ull * row.height * row.width
        || row.global.size() != 216 || row.height == 0 || row.width == 0
        || (!previousMemory.empty() && previousMemory.size() != width_))
        throw std::runtime_error("invalid whole-game observation dimensions");
    const auto layer = [this](const std::string& name, const std::vector<float>& input) {
        return linear(parameter(name + ".weight"), parameter(name + ".bias"), input);
    };
    const auto norm = [this](const std::string& name, std::vector<float>& values) {
        layerNorm(values, parameter(name + ".weight"), parameter(name + ".bias"));
    };
    auto entity = encodeEntities(row);
    auto spatial = convolution(row.spatial, 3, row.height, row.width,
                               parameter("terrain.0.weight"), parameter("terrain.0.bias"), 5, 2);
    const auto halfHeight = (row.height + 4 - 5) / 2 + 1;
    const auto halfWidth = (row.width + 4 - 5) / 2 + 1;
    spatial = convolution(spatial, 32, halfHeight, halfWidth,
                          parameter("terrain.2.weight"), parameter("terrain.2.bias"), 3, 1);
    const auto quarterHeight = (halfHeight + 2 - 3) / 2 + 1;
    const auto quarterWidth = (halfWidth + 2 - 3) / 2 + 1;
    std::vector<float> pooledSpatial(64);
    for (std::size_t channel = 0; channel < 64; ++channel)
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(quarterHeight) * quarterWidth; ++pixel)
            pooledSpatial[channel] += spatial[channel * quarterHeight * quarterWidth + pixel]
                                      / (quarterHeight * quarterWidth);
    auto global = layer("global_state.0", row.global);
    norm("global_state.1", global); gelu(global);
    auto queryInput = global;
    append(queryInput, pooledSpatial);
    const auto query = layer("query", queryInput);
    std::vector<float> scores(count);
    const auto divisor = std::sqrt(static_cast<float>(width_));
    for (std::size_t index = 0; index < count; ++index) scores[index] = dot(entity[index], query) / divisor;
    const auto maximum = *std::max_element(scores.begin(), scores.end());
    float total = 0;
    for (auto& value : scores) { value = std::exp(value - maximum); total += value; }
    std::vector<float> pooledEntity(width_);
    for (std::size_t index = 0; index < count; ++index)
        for (std::size_t feature = 0; feature < width_; ++feature)
            pooledEntity[feature] += scores[index] / total * entity[index][feature];
    auto fusionInput = global;
    append(fusionInput, pooledSpatial); append(fusionInput, pooledEntity);
    auto fused = layer("fusion.0", fusionInput);
    norm("fusion.1", fused); gelu(fused);
    const std::vector<float> previous = previousMemory.empty() ? std::vector<float>(width_) : previousMemory;
    const auto inputGates = linear(parameter("memory.weight_ih"), parameter("memory.bias_ih"), fused);
    const auto hiddenGates = linear(parameter("memory.weight_hh"), parameter("memory.bias_hh"), previous);
    Output output;
    output.memory.resize(width_);
    for (std::size_t index = 0; index < width_; ++index) {
        const auto reset = sigmoid(inputGates[index] + hiddenGates[index]);
        const auto update = sigmoid(inputGates[width_ + index] + hiddenGates[width_ + index]);
        const auto candidate = std::tanh(inputGates[2 * width_ + index]
                                         + reset * hiddenGates[2 * width_ + index]);
        output.memory[index] = candidate + (previous[index] - candidate) * update;
    }
    if (!includeHeads) {
        if (encodedEntities) *encodedEntities = std::move(entity);
        return output;
    }
    for (const auto& name : {"event", "domain", "kind", "queued", "target_mode", "position"})
        output.heads.emplace(name, layer(name, output.memory));
    for (const auto& name : {"order", "unit_type", "technology", "upgrade", "queue_slot"})
        output.heads.emplace(name, layer(std::string("argument_heads.") + name, output.memory));
    for (const auto& horizon : {"24", "240"})
        output.heads.emplace(std::string("forecast_") + horizon,
                             layer(std::string("forecast.") + horizon, output.memory));
    const auto actorQuery = layer("actor_query", output.memory);
    const auto targetQuery = layer("target_query", output.memory);
    std::vector<float> actors(count), targets(count);
    for (std::size_t index = 0; index < count; ++index) {
        actors[index] = dot(layer("actor_key", entity[index]), actorQuery) / divisor;
        targets[index] = dot(layer("target_key", entity[index]), targetQuery) / divisor;
    }
    output.heads.emplace("actor", std::move(actors));
    output.heads.emplace("target", std::move(targets));
    return output;
}

SlotOutput WholeGameCpu::inferSlots(const Observation& row,
                                    const std::vector<float>& previousMemory) const {
    if (!multiSlot()) throw std::runtime_error("whole-game weights have no command slots");
    std::vector<std::vector<float>> entities;
    const auto base = inferImpl(row, previousMemory, false, &entities);
    const auto count = entities.size();
    if (count == 0) throw std::runtime_error("multi-slot observation has no entities");
    const auto layer = [this](const std::string& name, const std::vector<float>& input) {
        return linear(parameter(name + ".weight"), parameter(name + ".bias"), input);
    };
    const auto embedding = [this](const std::string& name, std::size_t index) {
        const auto& tensor = parameter(name + ".weight");
        if (tensor.shape.size() != 2 || index >= tensor.shape[0])
            throw std::runtime_error("multi-slot embedding index invalid: " + name);
        const auto width = tensor.shape[1];
        return std::vector<float>(tensor.values.begin() + index * width,
                                  tensor.values.begin() + (index + 1) * width);
    };
    const auto top = [](const std::vector<float>& logits) {
        return static_cast<std::size_t>(std::max_element(logits.begin(), logits.end())
                                        - logits.begin());
    };
    const auto scale = std::sqrt(static_cast<float>(width_));
    std::vector<std::vector<float>> actorKeys(count), targetKeys(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (row.relation[index] == 0)
            actorKeys[index] = layer("slot_actor_key", entities[index]);
        if (row.relation[index] == 0 || row.entityNumeric[index * 16 + 4] > 0.5f)
            targetKeys[index] = layer("slot_target_key", entities[index]);
    }
    SlotOutput output;
    output.memory = base.memory;
    auto state = base.memory;
    for (int slot = 0; slot < 6; ++slot) {
        Output proposal;
        auto stop = layer("slot_stop", state);
        proposal.heads.emplace("stop", stop);
        if (top(stop) == 1) break;
        const auto actorQuery = layer("slot_actor_query", state);
        std::vector<float> actors(count, -1e9f);
        for (std::size_t index = 0; index < count; ++index)
            if (!actorKeys[index].empty())
                actors[index] = dot(actorKeys[index], actorQuery) / scale;
        const auto actorIndex = top(actors);
        if (row.relation[actorIndex] != 0)
            throw std::runtime_error("multi-slot observation has no owned actor");
        std::vector<float> actorState = state;
        append(actorState, entities[actorIndex]);
        auto kinds = layer("slot_kind", actorState);
        for (std::size_t index = 0; index < kinds.size(); ++index)
            if (index >= kindTargetModeMask.size() || !kindTargetModeMask[index]) kinds[index] = -1e9f;
        const auto kindIndex = top(kinds);
        const auto kindContext = embedding("slot_kind_context", kindIndex);
        auto arguments = actorState;
        append(arguments, kindContext);
        auto modes = layer("slot_mode", arguments);
        for (std::size_t index = 0; index < modes.size(); ++index)
            if ((kindTargetModeMask[kindIndex] & (1u << index)) == 0) modes[index] = -1e9f;
        const auto modeIndex = top(modes);
        const auto targetQuery = layer("slot_target_query", arguments);
        std::vector<float> targets(count, -1e9f);
        for (std::size_t index = 0; index < count; ++index)
            if (!targetKeys[index].empty())
                targets[index] = dot(targetKeys[index], targetQuery) / scale;
        const auto targetIndex = top(targets);
        auto delays = layer("slot_delay", arguments);
        const auto delayIndex = top(delays);
        auto positions = layer("slot_position", arguments);
        std::size_t component = 0;
        for (std::size_t index = 1; index < components_; ++index)
            if (positions[index * 5] > positions[component * 5]) component = index;
        std::vector<float> xy{sigmoid(positions[component * 5 + 1]),
                              sigmoid(positions[component * 5 + 2])};
        if (modeIndex != 2) xy = {0.0f, 0.0f};
        proposal.heads.emplace("event", std::vector<float>{100.0f});
        proposal.heads.emplace("actor", std::move(actors));
        proposal.heads.emplace("kind", std::move(kinds));
        proposal.heads.emplace("target_mode", std::move(modes));
        proposal.heads.emplace("target", std::move(targets));
        proposal.heads.emplace("delay", std::move(delays));
        proposal.heads.emplace("position", std::move(positions));
        proposal.heads.emplace("domain", layer("slot_domain", arguments));
        proposal.heads.emplace("queued", layer("slot_queued", arguments));
        for (const auto& name : {"order", "unit_type", "technology", "upgrade", "queue_slot"})
            proposal.heads.emplace(name, layer(std::string("slot_arguments.") + name, arguments));
        const auto unitIndex = top(proposal.heads.at("unit_type"));
        output.slots.push_back(std::move(proposal));

        std::vector<float> token = entities[actorIndex];
        append(token, kindContext);
        append(token, embedding("slot_mode_context", modeIndex));
        append(token, embedding("slot_delay_context", delayIndex));
        if (modeIndex == 1) append(token, entities[targetIndex]);
        else token.resize(token.size() + width_, 0.0f);
        if (modeIndex == 2) append(token, layer("slot_position_context", xy));
        else token.resize(token.size() + 32, 0.0f);
        append(token, embedding("slot_unit_context", unitIndex));
        const auto inputGates = linear(parameter("slot_transition.weight_ih"),
                                       parameter("slot_transition.bias_ih"), token);
        const auto hiddenGates = linear(parameter("slot_transition.weight_hh"),
                                        parameter("slot_transition.bias_hh"), state);
        std::vector<float> next(width_);
        for (std::size_t index = 0; index < width_; ++index) {
            const auto reset = sigmoid(inputGates[index] + hiddenGates[index]);
            const auto update = sigmoid(inputGates[width_ + index] + hiddenGates[width_ + index]);
            const auto candidate = std::tanh(inputGates[2 * width_ + index]
                                             + reset * hiddenGates[2 * width_ + index]);
            next[index] = candidate + (state[index] - candidate) * update;
        }
        state = std::move(next);
    }
    return output;
}

}  // namespace protodd::cpu
