//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/schema/catalog/sequence.h"

#include <cstdint>
#include <optional>
#include <string>

#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.pb.h"
#include "googlesql/public/value.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "backend/schema/graph/schema_graph_editor.h"
#include "backend/schema/graph/schema_node.h"
#include "backend/schema/updater/schema_validation_context.h"
#include "common/bit_reverse.h"
#include "common/constants.h"
#include "common/errors.h"
#include "common/limits.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// How far ahead of the values handed out a sequence saves its counter. A
// restart skips at most this many counter values, as production sequences can
// also skip values.
constexpr int64_t kSavedCounterAhead = 1000;

}  // namespace

absl::Status Sequence::Validate(SchemaValidationContext* context) const {
  return validate_(this, context);
}
absl::Status Sequence::ValidateUpdate(const SchemaNode* old,
                                      SchemaValidationContext* context) const {
  return validate_update_(this, old->template As<const Sequence>(), context);
}
std::string Sequence::DebugString() const {
  std::string debug_string = absl::Substitute("Sequence $0. Sequence kind: $1",
                                              name_, sequence_kind_name());
  if (start_with_counter().has_value()) {
    absl::StrAppend(&debug_string,
                    "\n  start_with_counter: ", start_with_counter().value());
  }
  if (skip_range_min().has_value() && skip_range_max().has_value()) {
    absl::StrAppend(&debug_string, "\n  skipped range: [",
                    skip_range_min().value(), ", ", skip_range_max().value(),
                    "]");
  }
  return debug_string;
}
absl::Status Sequence::DeepClone(SchemaGraphEditor* editor,
                                 const SchemaNode* orig) {
  return absl::OkStatus();
}

absl::StatusOr<googlesql::Value> Sequence::GetNextSequenceValue(
    SequenceStateStore* state_store) const {
  absl::MutexLock lock(SequenceMutex);
  if (!Sequence::SequenceLastValues.contains(id_)) {
    if (start_with_.has_value()) {
      Sequence::SequenceLastValues[id_] = start_with_.value();
    } else {
      Sequence::SequenceLastValues[id_] = kSequenceDefaultStartWith;
    }
    // Continue from the counter saved before a restart, if there is one.
    if (state_store != nullptr) {
      GOOGLESQL_ASSIGN_OR_RETURN(std::optional<int64_t> saved_counter,
                       state_store->Load(name_));
      if (saved_counter.has_value()) {
        Sequence::SequenceLastValues[id_] = *saved_counter;
      }
    }
  }
  if (Sequence::SequenceLastValues[id_] < 0) {
    return error::InvalidSequenceStartWithCounterValue();
  }

  // Retrieve the next value and make sure that it doesn't fall into the skipped
  // range. If it does, keep trying until we have one.
  int attempt_count = 1;
  int64_t value = -1;
  do {
    if (attempt_count > limits::kMaxGetSequenceValueAttempt) {
      ABSL_LOG(INFO) << "Attempted to get sequence values more than "
                << limits::kMaxGetSequenceValueAttempt
                << "times. Current skipped range is ["
                << skip_range_min_.value() << ", " << skip_range_max_.value()
                << "].";
      return error::SequenceExhausted(name_);
    }
    if (Sequence::SequenceLastValues[id_] == kInt64Max) {
      ABSL_LOG(INFO) << "No additional value can be obtained. The current sequence "
                << "counter is already at int64max.";
      return error::SequenceExhausted(name_);
    }
    // In a bit-reversed-positive sequence, we bit-reverse the counter and
    // preserve its sign.
    value = BitReverse(Sequence::SequenceLastValues[id_]++,
                       /*preserve_sign=*/true);

    ++attempt_count;
  } while (
      skip_range_min_.has_value() && skip_range_max_.has_value() &&
      (value <= skip_range_max_.value() && value >= skip_range_min_.value()));

  // Keep the saved counter above every counter handed out.
  if (state_store != nullptr) {
    const int64_t next_counter = Sequence::SequenceLastValues[id_];
    auto saved = Sequence::SequenceSavedCounters.find(id_);
    if (saved == Sequence::SequenceSavedCounters.end() ||
        next_counter > saved->second) {
      const int64_t counter_to_save =
          next_counter > kInt64Max - kSavedCounterAhead
              ? kInt64Max
              : next_counter + kSavedCounterAhead;
      GOOGLESQL_RETURN_IF_ERROR(state_store->Save(name_, counter_to_save));
      Sequence::SequenceSavedCounters[id_] = counter_to_save;
    }
  }

  return googlesql::Value::Int64(value);
}

absl::StatusOr<googlesql::Value> Sequence::GetInternalSequenceState(
    const SequenceStateStore* state_store) const {
  // If no sequence value has been retrieved before, then the current state is
  // NULL.
  absl::MutexLock lock(SequenceMutex);
  if (!Sequence::SequenceLastValues.contains(id_)) {
    if (state_store != nullptr) {
      GOOGLESQL_ASSIGN_OR_RETURN(std::optional<int64_t> saved_counter,
                       state_store->Load(name_));
      if (saved_counter.has_value()) {
        return googlesql::Value::Int64(*saved_counter);
      }
    }
    return googlesql::Value::NullInt64();
  }
  return googlesql::Value::Int64(Sequence::SequenceLastValues[id_]);
}

void Sequence::ResetSequenceLastValue() const {
  absl::MutexLock lock(SequenceMutex);
  // Save the counter again on the next value, from the new start.
  Sequence::SequenceSavedCounters.erase(id_);
  if (!Sequence::SequenceLastValues.contains(id_)) {
    return;
  }
  if (start_with_.has_value()) {
    Sequence::SequenceLastValues[id_] = start_with_.value();
  } else {
    Sequence::SequenceLastValues[id_] = kSequenceDefaultStartWith;
  }
}

void Sequence::RemoveSequenceFromLastValuesMap() const {
  absl::MutexLock lock(SequenceMutex);
  absl::flat_hash_map<std::string, int64_t>::iterator it =
      Sequence::SequenceLastValues.find(id_);
  if (it != Sequence::SequenceLastValues.end()) {
    Sequence::SequenceLastValues.erase(it);
  }
  Sequence::SequenceSavedCounters.erase(id_);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
