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

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "absl/flags/parse.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "backend/database/database.h"
#include "backend/schema/updater/schema_updater.h"
#include "common/config.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/server/server.h"
#include "absl/log/absl_log.h"

using Server = ::google::spanner::emulator::frontend::Server;
namespace config = ::google::spanner::emulator::config;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace database_api = ::google::spanner::admin::database::v1;

// Restores instances and databases from persisted metadata.
static void RestoreFromMetadata(Server* server) {
  auto* env = server->env();
  auto* ms = env->metadata_store();
  if (ms == nullptr) return;

  auto status = ms->Load();
  if (!status.ok()) {
    ABSL_LOG(ERROR) << "Failed to load metadata: " << status;
    return;
  }
  if (!ms->has_metadata()) return;

  const auto& instances = ms->instances();
  int restored_instances = 0;
  int restored_databases = 0;

  for (const auto& [inst_name, inst_info] : instances) {
    // Build instance proto for CreateInstance.
    instance_api::Instance inst_pb;
    inst_pb.set_name(inst_name);
    inst_pb.set_display_name(inst_info.display_name);
    inst_pb.set_config(inst_info.config);
    inst_pb.set_processing_units(inst_info.processing_units);
    for (const auto& [k, v] : inst_info.labels) {
      (*inst_pb.mutable_labels())[k] = v;
    }

    auto inst_or = env->instance_manager()->CreateInstance(inst_name, inst_pb);
    if (!inst_or.ok()) {
      ABSL_LOG(ERROR) << "Failed to restore instance " << inst_name << ": "
                      << inst_or.status();
      continue;
    }
    restored_instances++;

    // Restore databases under this instance.
    for (const auto& [db_name, db_info] : inst_info.databases) {
      std::string database_uri =
          absl::StrCat(inst_name, "/databases/", db_name);

      // Determine dialect.
      database_api::DatabaseDialect dialect =
          database_api::DatabaseDialect::GOOGLE_STANDARD_SQL;
      if (db_info.dialect == "POSTGRESQL") {
        dialect = database_api::DatabaseDialect::POSTGRESQL;
      }

      // Build DDL statements, skipping any CREATE DATABASE statement
      // (CreateDatabase handles database creation internally).
      std::vector<std::string> extra_ddl;
      for (const auto& stmt : db_info.ddl_statements) {
        if (absl::StartsWithIgnoreCase(stmt, "CREATE DATABASE")) continue;
        extra_ddl.push_back(stmt);
      }

      // Seed ID generators from persisted counters so restored schemas get
      // the same table/column IDs used to write data to LevelDB.
      google::spanner::emulator::backend::Database::IdCounterValues counters{
          .table_id = db_info.id_counters.table_id,
          .column_id = db_info.id_counters.column_id,
          .change_stream_id = db_info.id_counters.change_stream_id,
      };

      auto db_or = env->database_manager()->CreateDatabase(
          database_uri,
          google::spanner::emulator::backend::SchemaChangeOperation{
              .statements = extra_ddl,
              .database_dialect = dialect,
          },
          counters);

      if (!db_or.ok()) {
        ABSL_LOG(ERROR) << "Failed to restore database " << database_uri
                        << ": " << db_or.status();
        continue;
      }
      restored_databases++;
    }
  }

  ABSL_LOG(INFO) << "Restored " << restored_instances << " instance(s) and "
                 << restored_databases << " database(s) from "
                 << config::data_dir() << "/metadata.json";
}

int main(int argc, char** argv) {
  // Start the emulator gRPC server.
  absl::ParseCommandLine(argc, argv);
  Server::Options options;
  options.server_address = config::grpc_host_port();
  std::unique_ptr<Server> server = Server::Create(options);
  if (!server) {
    ABSL_LOG(ERROR) << "Failed to start gRPC server.";
    return EXIT_FAILURE;
  }

  // Restore persisted state if --data_dir is set.
  if (!config::data_dir().empty()) {
    RestoreFromMetadata(server.get());
  }

  ABSL_LOG(INFO) << "Cloud Spanner Emulator running.";
  ABSL_LOG(INFO) << "Server address: "
                 << absl::StrCat(server->host(), ":", server->port());

  // Block forever until the server is terminated.
  server->WaitForShutdown();

  return EXIT_SUCCESS;
}
