#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::core {

struct ConversionContext {
  std::string preceding_text;
  std::string preedit_text;
  std::vector<std::string> rejected_surfaces;
  const std::atomic<bool>* cancel{nullptr};
  std::optional<std::chrono::steady_clock::time_point> deadline;
  uint32_t max_candidates{0};
  bool live{false};
  // Optional instruction profile for explicit local AI transformations.
  std::string instruction_profile;
};

struct CorrectionHint {
  std::string rejected_surface;
  std::string intent;
};

class IConverter {
 public:
  virtual ~IConverter() = default;

  virtual std::vector<Candidate> Convert(const std::string& kana,
                                         const ConversionContext& context) = 0;
  virtual std::vector<Candidate> PredictNext(const std::string& kana,
                                             const ConversionContext& context) = 0;
  virtual std::vector<Candidate> Correct(const std::string& kana, const CorrectionHint& hint,
                                         const ConversionContext& context) = 0;
  virtual void Commit(const Candidate& selected_candidate, const ConversionContext& context) = 0;
  virtual void Learn(const std::string& committed_surface,
                     const std::string& committed_reading) = 0;

  // Whether (reading, surface) is a real dictionary entry of this converter, as
  // opposed to something Convert can synthesize heuristically or something
  // Learn recorded from a commit. New-word mining (M36-A) needs that
  // distinction: Convert always returns identity and long-vowel candidates, so
  // its output cannot answer "does the dictionary know this word".
  // The default is false, which makes a converter with no lexicon of its own
  // report nothing rather than block mining.
  virtual bool Contains(const std::string& reading, const std::string& surface) const {
    (void)reading;
    (void)surface;
    return false;
  }
};

}  // namespace azookey::core
