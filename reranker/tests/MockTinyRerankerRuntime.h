#pragma once

#include <utility>

#include "azookey/reranker/TinyRerankerRuntime.h"

namespace azookey::reranker::test {

class MockTinyRerankerRuntime final : public ITinyRerankerRuntime {
 public:
  using Forward =
      std::function<std::vector<float>(std::span<const std::array<float, 12>>,
                                       const std::atomic<bool>&)>;

  explicit MockTinyRerankerRuntime(Forward forward) : forward_(std::move(forward)) {}

  std::vector<float> Infer(std::span<const std::array<float, 12>> features,
                           const std::atomic<bool>& stop) override {
    return forward_(features, stop);
  }

 private:
  Forward forward_;
};

}  // namespace azookey::reranker::test
