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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_DATABASE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_DATABASE_H_

#include <atomic>
#include <string>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Database represents a database resource within the frontend.
//
// This class provides frontend-level functionality (e.g. URI, proto conversion)
// and wraps the 'backend::Database' class which actually implements the core
// database functionality. We do this to keep the backend database a completely
// separate module from the frontend and isolate it from gRPC API details.
class Database {
 public:
  Database(const std::string& database_uri,
           std::unique_ptr<backend::Database> backend, absl::Time create_time)
      : database_uri_(database_uri),
        backend_(std::move(backend)),
        create_time_(create_time) {}

  // Returns the URI for this database.
  const std::string& database_uri() const { return database_uri_; }

  // Returns the handle to the backend database.
  backend::Database* backend() const { return backend_.get(); }

  // Serializes schema changes with backup metadata capture so a checkpoint
  // cannot pair rows with a different schema generation.
  absl::Mutex& schema_change_mutex() { return schema_change_mu_; }

  // Converts this database object to its proto representation.
  absl::Status ToProto(admin::database::v1::Database* database);

  bool enable_drop_protection() const {
    return enable_drop_protection_.load(std::memory_order_relaxed);
  }
  void set_enable_drop_protection(bool enabled) {
    enable_drop_protection_.store(enabled, std::memory_order_relaxed);
  }

 private:
  // The URI for this database.
  const std::string database_uri_;

  // The backend object which implements core database functionality.
  std::unique_ptr<backend::Database> backend_;

  absl::Mutex schema_change_mu_;

  // The time at which this database was created.
  const absl::Time create_time_;

  std::atomic<bool> enable_drop_protection_{false};
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_DATABASE_H_
