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

#include "frontend/entities/operation.h"

#include <string>

#include "frontend/common/protos.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

Operation::Operation(const std::string& operation_uri) {
  operation_.set_name(operation_uri);
}

Operation::Operation(const google::longrunning::Operation& operation)
    : operation_(operation) {}

void Operation::SetMetadata(const google::protobuf::Message& metadata) {
  absl::MutexLock lock(mu_);
  ToAnyProto(metadata, operation_.mutable_metadata());
}

void Operation::SetError(const absl::Status& status) {
  absl::MutexLock lock(mu_);
  operation_.set_done(true);
  operation_.clear_response();
  operation_.mutable_error()->set_code(status.raw_code());
  operation_.mutable_error()->set_message(std::string(status.message()));
}

void Operation::SetResponse(const google::protobuf::Message& response) {
  absl::MutexLock lock(mu_);
  operation_.set_done(true);
  operation_.clear_error();
  ToAnyProto(response, operation_.mutable_response());
}

void Operation::ToProto(google::longrunning::Operation* operation_pb) const {
  absl::ReaderMutexLock lock(mu_);
  *operation_pb = operation_;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
