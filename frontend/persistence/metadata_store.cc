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

#include "frontend/persistence/metadata_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/log/absl_log.h"
#include "nlohmann/json.hpp"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

using json = nlohmann::json;

MetadataStore::MetadataStore(const std::string& data_dir)
    : metadata_path_(absl::StrCat(data_dir, "/metadata.json")) {}

absl::Status MetadataStore::Load() {
  absl::MutexLock lock(&mu_);
  has_metadata_ = false;
  instances_.clear();

  if (!std::filesystem::exists(metadata_path_)) {
    return absl::OkStatus();  // First run — no metadata file.
  }

  std::ifstream file(metadata_path_);
  if (!file.is_open()) {
    ABSL_LOG(ERROR) << "Failed to open metadata file: " << metadata_path_;
    return absl::OkStatus();  // Safe fallback.
  }

  json j;
  try {
    file >> j;
  } catch (const json::parse_error& e) {
    ABSL_LOG(ERROR) << "Corrupt metadata file " << metadata_path_ << ": "
                    << e.what();
    return absl::OkStatus();  // Safe fallback.
  }

  if (!j.contains("instances") || !j["instances"].is_object()) {
    return absl::OkStatus();
  }

  for (auto& [inst_name, inst_json] : j["instances"].items()) {
    InstanceInfo info;
    info.display_name = inst_json.value("displayName", inst_name);
    info.config = inst_json.value("config", "emulator-config");
    info.processing_units = inst_json.value("processingUnits", 1000);
    info.create_time = inst_json.value("createTime", "");

    if (inst_json.contains("labels") && inst_json["labels"].is_object()) {
      for (auto& [k, v] : inst_json["labels"].items()) {
        info.labels[k] = v.get<std::string>();
      }
    }

    if (inst_json.contains("databases") &&
        inst_json["databases"].is_object()) {
      for (auto& [db_name, db_json] : inst_json["databases"].items()) {
        DatabaseInfo db_info;
        db_info.dialect =
            db_json.value("dialect", "GOOGLE_STANDARD_SQL");

        if (db_json.contains("ddlStatements") &&
            db_json["ddlStatements"].is_array()) {
          for (const auto& stmt : db_json["ddlStatements"]) {
            db_info.ddl_statements.push_back(stmt.get<std::string>());
          }
        }

        if (db_json.contains("idCounters") &&
            db_json["idCounters"].is_object()) {
          const auto& c = db_json["idCounters"];
          db_info.id_counters.table_id = c.value("tableId", int64_t{0});
          db_info.id_counters.column_id = c.value("columnId", int64_t{0});
          db_info.id_counters.change_stream_id =
              c.value("changeStreamId", int64_t{0});
          db_info.id_counters.sequence_id =
              c.value("sequenceId", int64_t{0});
          db_info.id_counters.named_schema_id =
              c.value("namedSchemaId", int64_t{0});
        }

        info.databases[db_name] = std::move(db_info);
      }
    }

    instances_[inst_name] = std::move(info);
  }

  has_metadata_ = !instances_.empty();
  return absl::OkStatus();
}

absl::Status MetadataStore::Save() {
  absl::MutexLock lock(&mu_);

  json j;
  j["version"] = 1;
  j["instances"] = json::object();

  for (const auto& [inst_name, info] : instances_) {
    json inst_json;
    inst_json["displayName"] = info.display_name;
    inst_json["config"] = info.config;
    inst_json["processingUnits"] = info.processing_units;
    inst_json["createTime"] = info.create_time;
    inst_json["labels"] = info.labels;
    inst_json["databases"] = json::object();

    for (const auto& [db_name, db_info] : info.databases) {
      json db_json;
      db_json["dialect"] = db_info.dialect;
      db_json["ddlStatements"] = db_info.ddl_statements;
      db_json["idCounters"] = {
          {"tableId", db_info.id_counters.table_id},
          {"columnId", db_info.id_counters.column_id},
          {"changeStreamId", db_info.id_counters.change_stream_id},
          {"sequenceId", db_info.id_counters.sequence_id},
          {"namedSchemaId", db_info.id_counters.named_schema_id},
      };
      inst_json["databases"][db_name] = std::move(db_json);
    }

    j["instances"][inst_name] = std::move(inst_json);
  }

  // Atomic write: write to .tmp, then rename.
  std::string tmp_path = metadata_path_ + ".tmp";
  {
    std::ofstream file(tmp_path);
    if (!file.is_open()) {
      return absl::InternalError(
          absl::StrCat("Failed to open ", tmp_path, " for writing"));
    }
    file << j.dump(2);
    if (file.fail()) {
      return absl::InternalError(
          absl::StrCat("Failed to write to ", tmp_path));
    }
  }

  if (std::rename(tmp_path.c_str(), metadata_path_.c_str()) != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to rename ", tmp_path, " to ", metadata_path_));
  }

  return absl::OkStatus();
}

void MetadataStore::AddInstance(
    const std::string& name, const std::string& display_name,
    const std::string& config, int32_t processing_units,
    const std::map<std::string, std::string>& labels,
    const std::string& create_time) {
  absl::MutexLock lock(&mu_);
  InstanceInfo info;
  info.display_name = display_name;
  info.config = config;
  info.processing_units = processing_units;
  info.labels = labels;
  info.create_time = create_time;
  instances_[name] = std::move(info);
}

void MetadataStore::RemoveInstance(const std::string& name) {
  absl::MutexLock lock(&mu_);
  instances_.erase(name);
}

void MetadataStore::AddDatabase(
    const std::string& instance_name, const std::string& db_name,
    const std::string& dialect,
    const std::vector<std::string>& ddl_statements) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  DatabaseInfo db_info;
  db_info.dialect = dialect;
  db_info.ddl_statements = ddl_statements;
  it->second.databases[db_name] = std::move(db_info);
}

void MetadataStore::RemoveDatabase(const std::string& instance_name,
                                   const std::string& db_name) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  it->second.databases.erase(db_name);
}

void MetadataStore::UpdateDdl(
    const std::string& instance_name, const std::string& db_name,
    const std::vector<std::string>& ddl_statements) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  auto db_it = it->second.databases.find(db_name);
  if (db_it == it->second.databases.end()) return;
  db_it->second.ddl_statements = ddl_statements;
}

void MetadataStore::UpdateIdCounters(const std::string& instance_name,
                                     const std::string& db_name,
                                     const IdCounters& counters) {
  absl::MutexLock lock(&mu_);
  auto it = instances_.find(instance_name);
  if (it == instances_.end()) return;
  auto db_it = it->second.databases.find(db_name);
  if (db_it == it->second.databases.end()) return;
  db_it->second.id_counters = counters;
}

std::map<std::string, MetadataStore::InstanceInfo>
MetadataStore::instances() const {
  absl::ReaderMutexLock lock(&mu_);
  return instances_;  // Return a copy for thread safety.
}

bool MetadataStore::has_metadata() const {
  absl::ReaderMutexLock lock(&mu_);
  return has_metadata_;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
