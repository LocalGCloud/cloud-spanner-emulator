//
// Copyright 2026 Google LLC
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

#include "frontend/persistence/backup_catalog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "google/longrunning/operations.pb.h"
#include "google/spanner/admin/database/v1/backup.pb.h"
#include "google/spanner/admin/database/v1/backup_schedule.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "leveldb/db.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

using ::googlesql_base::testing::IsOk;
using json = nlohmann::json;
namespace operations_api = ::google::longrunning;

std::string TemporaryCatalogDirectory() {
  return (std::filesystem::temp_directory_path() /
          ("spanner-backup-catalog-" +
           std::to_string(absl::ToUnixNanos(absl::Now()))))
      .string();
}

void CreateSnapshot(BackupCatalog* catalog, const std::string& backup_name) {
  const std::string directory = catalog->SnapshotDirectory(backup_name);
  std::filesystem::create_directories(
      std::filesystem::path(directory).parent_path());
  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::DB* raw_database = nullptr;
  ASSERT_TRUE(leveldb::DB::Open(options, directory, &raw_database).ok());
  std::unique_ptr<leveldb::DB> database(raw_database);
  ASSERT_TRUE(
      database->Put(leveldb::WriteOptions(), "key", "value").ok());
}

operations_api::Operation TerminalBackupOperation(
    const BackupCatalog::BackupEntry& entry) {
  operations_api::Operation operation;
  operation.set_name(entry.operation_name);
  operation.set_done(true);
  operation.mutable_response()->PackFrom(entry.backup);
  return operation;
}

TEST(BackupCatalogTest, PersistsBackupAndScheduleControlMetadata) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string parent = "projects/p/instances/i";
  const std::string backup_name = parent + "/backups/b";
  const std::string schedule_name = parent + "/backupSchedules/s";

  operations_api::Operation expected_operation;
  {
    BackupCatalog catalog(directory);
    EXPECT_THAT(catalog.Load(), IsOk());
    BackupCatalog::BackupEntry entry;
    entry.backup.set_name(backup_name);
    entry.backup.set_database(parent + "/databases/d");
    entry.backup.set_state(database_api::Backup::READY);
    entry.backup.set_size_bytes(42);
    entry.ddl_statements = {
        "CREATE TABLE Rows (Id INT64 NOT NULL) PRIMARY KEY (Id)"};
    entry.schema_change_batches = {
        {.statements = entry.ddl_statements,
         .proto_descriptor_bytes = "descriptor-bundle"}};
    entry.id_counters.table_id = 7;
    entry.operation_name = backup_name + "/operations/_auto1";
    entry.source_instance_config = "emulator-config";
    CreateSnapshot(&catalog, backup_name);
    expected_operation = TerminalBackupOperation(entry);
    EXPECT_THAT(catalog.CreateBackup(entry, expected_operation), IsOk());

    database_api::BackupSchedule schedule;
    schedule.set_name(schedule_name);
    EXPECT_THAT(catalog.CreateBackupSchedule(schedule), IsOk());
  }
  {
    std::ifstream input(directory + "/backup_catalog.json");
    json persisted;
    input >> persisted;
    auto& counters = persisted["backups"][backup_name]["idCounters"];
    EXPECT_FALSE(counters.contains("sequenceId"));
    EXPECT_FALSE(counters.contains("namedSchemaId"));
    counters["sequenceId"] = 11;
    counters["namedSchemaId"] = 13;
    std::ofstream output(directory + "/backup_catalog.json");
    output << persisted.dump(2);
  }

  {
    BackupCatalog restored(directory);
    EXPECT_THAT(restored.Load(), IsOk());
    auto entry = restored.GetBackup(backup_name);
    ASSERT_THAT(entry, IsOk());
    EXPECT_EQ(entry->backup.database(), parent + "/databases/d");
    EXPECT_EQ(entry->backup.size_bytes(), 42);
    EXPECT_EQ(entry->ddl_statements.size(), 1);
    ASSERT_EQ(entry->schema_change_batches.size(), 1);
    EXPECT_EQ(entry->schema_change_batches.front().statements,
              entry->ddl_statements);
    EXPECT_EQ(entry->schema_change_batches.front().proto_descriptor_bytes,
              "descriptor-bundle");
    EXPECT_EQ(entry->id_counters.table_id, 7);
    EXPECT_EQ(entry->operation_name, backup_name + "/operations/_auto1");
    EXPECT_EQ(entry->source_instance_config, "emulator-config");
    ASSERT_EQ(restored.AllOperations().size(), 1);
    EXPECT_EQ(restored.AllOperations().front().name(), entry->operation_name);
    EXPECT_EQ(restored.AllOperations().front().SerializeAsString(),
              expected_operation.SerializeAsString());
    EXPECT_EQ(restored.ListBackups(parent).size(), 1);

    auto schedule = restored.GetBackupSchedule(schedule_name);
    ASSERT_THAT(schedule, IsOk());
    EXPECT_EQ(schedule->name(), schedule_name);
    EXPECT_EQ(restored.ListBackupSchedules(parent).size(), 1);

    database_api::Backup updated = entry->backup;
    updated.mutable_expire_time()->set_seconds(1234);
    EXPECT_THAT(restored.UpdateBackup(updated), IsOk());
    EXPECT_EQ(restored.GetBackup(backup_name)->backup.expire_time().seconds(),
              1234);
    EXPECT_THAT(restored.DeleteBackupSchedule(schedule_name), IsOk());
    EXPECT_THAT(restored.DeleteOperation(entry->operation_name), IsOk());
    EXPECT_THAT(restored.DeleteBackup(backup_name), IsOk());
  }

  {
    BackupCatalog empty(directory);
    EXPECT_THAT(empty.Load(), IsOk());
    EXPECT_TRUE(empty.AllBackups().empty());
    EXPECT_TRUE(empty.ListBackupSchedules(parent).empty());
    EXPECT_TRUE(empty.AllOperations().empty());
  }
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, DeletedOperationLinkageDoesNotReturnAfterReload) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name = "projects/p/instances/i/backups/b";
  const std::string operation_name = backup_name + "/operations/_auto1";
  {
    BackupCatalog catalog(directory);
    EXPECT_THAT(catalog.Load(), IsOk());
    BackupCatalog::BackupEntry entry;
    entry.backup.set_name(backup_name);
    entry.operation_name = operation_name;
    EXPECT_THAT(
        catalog.CreateBackup(entry, TerminalBackupOperation(entry)), IsOk());
    EXPECT_THAT(catalog.DeleteOperation(operation_name), IsOk());
  }
  {
    BackupCatalog restored(directory);
    EXPECT_THAT(restored.Load(), IsOk());
    auto entry = restored.GetBackup(backup_name);
    ASSERT_THAT(entry, IsOk());
    EXPECT_TRUE(entry->operation_name.empty());
  }
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RejectsNonObjectCatalogRoot) {
  const std::string directory = TemporaryCatalogDirectory();
  std::filesystem::create_directories(directory);
  {
    std::ofstream output(directory + "/backup_catalog.json");
    output << "[]";
  }

  BackupCatalog catalog(directory);
  EXPECT_THAT(catalog.Load(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kDataLoss));
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, MissingReadySnapshotReturnsDataLoss) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name = "projects/p/instances/i/backups/missing";
  {
    BackupCatalog catalog(directory);
    BackupCatalog::BackupEntry entry;
    entry.backup.set_name(backup_name);
    entry.backup.set_state(database_api::Backup::READY);
    entry.operation_name = backup_name + "/operations/_auto1";
    CreateSnapshot(&catalog, backup_name);
    ASSERT_THAT(
        catalog.CreateBackup(entry, TerminalBackupOperation(entry)), IsOk());
    std::filesystem::remove_all(
        std::filesystem::path(catalog.SnapshotDirectory(backup_name))
            .parent_path());
  }

  BackupCatalog restored(directory);
  EXPECT_THAT(restored.Load(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kDataLoss));
  EXPECT_TRUE(restored.AllBackups().empty());
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, UnreadableReadySnapshotReturnsDataLoss) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name = "projects/p/instances/i/backups/corrupt";
  std::string snapshot_directory;
  {
    BackupCatalog catalog(directory);
    BackupCatalog::BackupEntry entry;
    entry.backup.set_name(backup_name);
    entry.backup.set_state(database_api::Backup::READY);
    entry.operation_name = backup_name + "/operations/_auto1";
    CreateSnapshot(&catalog, backup_name);
    snapshot_directory = catalog.SnapshotDirectory(backup_name);
    ASSERT_THAT(
        catalog.CreateBackup(entry, TerminalBackupOperation(entry)), IsOk());
  }
  {
    std::ofstream current(snapshot_directory + "/CURRENT",
                          std::ios::trunc);
    current << "not-a-manifest\n";
  }

  BackupCatalog restored(directory);
  EXPECT_THAT(restored.Load(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kDataLoss));
  EXPECT_TRUE(restored.AllBackups().empty());
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RecoversStagedSnapshotDeletionOnLoad) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name = "projects/p/instances/i/backups/staged";
  std::filesystem::path snapshot_root;
  {
    BackupCatalog catalog(directory);
    BackupCatalog::BackupEntry entry;
    entry.backup.set_name(backup_name);
    entry.backup.set_state(database_api::Backup::READY);
    entry.operation_name = backup_name + "/operations/_auto1";
    CreateSnapshot(&catalog, backup_name);
    snapshot_root =
        std::filesystem::path(catalog.SnapshotDirectory(backup_name))
            .parent_path();
    ASSERT_THAT(
        catalog.CreateBackup(entry, TerminalBackupOperation(entry)), IsOk());
  }
  std::filesystem::rename(snapshot_root,
                          snapshot_root.string() + ".deleting");

  BackupCatalog restored(directory);
  EXPECT_THAT(restored.Load(), IsOk());
  EXPECT_TRUE(std::filesystem::exists(snapshot_root));
  EXPECT_FALSE(
      std::filesystem::exists(snapshot_root.string() + ".deleting"));
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RemovesUncataloguedSnapshotDirectoriesOnLoad) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::filesystem::path orphan =
      std::filesystem::path(directory) / "backups" / "orphan";
  std::filesystem::create_directories(orphan / "storage.tmp-incomplete");

  BackupCatalog catalog(directory);
  EXPECT_THAT(catalog.Load(), IsOk());
  EXPECT_FALSE(std::filesystem::exists(orphan));
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, PersistsStandaloneTerminalOperation) {
  const std::string directory = TemporaryCatalogDirectory();
  operations_api::Operation operation;
  operation.set_name(
      "projects/p/instances/i/databases/restored/operations/_auto40");
  operation.set_done(true);
  operation.mutable_error()->set_code(
      static_cast<int>(absl::StatusCode::kAborted));
  operation.mutable_error()->set_message("terminal restore error");
  {
    BackupCatalog catalog(directory);
    EXPECT_THAT(catalog.SaveOperation(operation), IsOk());
  }
  {
    BackupCatalog restored(directory);
    EXPECT_THAT(restored.Load(), IsOk());
    ASSERT_EQ(restored.AllOperations().size(), 1);
    EXPECT_EQ(restored.AllOperations().front().SerializeAsString(),
              operation.SerializeAsString());
    EXPECT_THAT(restored.DeleteOperation(operation.name()), IsOk());
  }
  {
    BackupCatalog empty(directory);
    EXPECT_THAT(empty.Load(), IsOk());
    EXPECT_TRUE(empty.AllOperations().empty());
  }
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RecoversAtomicTemporaryCatalog) {
  const std::string directory = TemporaryCatalogDirectory();
  operations_api::Operation operation;
  operation.set_name("projects/p/instances/i/operations/_auto7");
  operation.set_done(true);
  {
    BackupCatalog catalog(directory);
    ASSERT_THAT(catalog.SaveOperation(operation), IsOk());
  }
  std::filesystem::rename(directory + "/backup_catalog.json",
                          directory + "/backup_catalog.json.tmp");

  BackupCatalog restored(directory);
  EXPECT_THAT(restored.Load(), IsOk());
  ASSERT_EQ(restored.AllOperations().size(), 1);
  EXPECT_EQ(restored.AllOperations().front().name(), operation.name());
  EXPECT_TRUE(std::filesystem::exists(directory + "/backup_catalog.json"));
  EXPECT_FALSE(
      std::filesystem::exists(directory + "/backup_catalog.json.tmp"));
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, MigratesVersionOneOperationLinkage) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name =
      "projects/p/instances/i/backups/legacy";
  BackupCatalog catalog(directory);
  BackupCatalog::BackupEntry entry;
  entry.backup.set_name(backup_name);
  entry.backup.set_state(database_api::Backup::READY);
  entry.ddl_statements = {"CREATE TABLE T (K INT64) PRIMARY KEY (K)"};
  entry.operation_name = backup_name + "/operations/_auto1";
  CreateSnapshot(&catalog, backup_name);
  ASSERT_THAT(catalog.CreateBackup(entry, TerminalBackupOperation(entry)),
              IsOk());

  json persisted;
  {
    std::ifstream input(directory + "/backup_catalog.json");
    input >> persisted;
  }
  persisted["version"] = 1;
  persisted.erase("schedules");
  persisted.erase("operations");
  auto& backup_json = persisted["backups"][backup_name];
  backup_json["ddlStatements"] = entry.ddl_statements;
  backup_json["protoDescriptors"] = "";
  backup_json.erase("ddlBatches");
  {
    std::ofstream output(directory + "/backup_catalog.json");
    output << persisted.dump(2);
  }

  BackupCatalog restored(directory);
  EXPECT_THAT(restored.Load(), IsOk());
  ASSERT_EQ(restored.AllOperations().size(), 1);
  EXPECT_EQ(restored.AllOperations().front().name(), entry.operation_name);
  EXPECT_TRUE(restored.AllOperations().front().done());
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RejectsMissingVersionedBackupEntryFields) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name = "projects/p/instances/i/backups/strict";
  {
    BackupCatalog catalog(directory);
    BackupCatalog::BackupEntry entry;
    entry.backup.set_name(backup_name);
    entry.backup.set_state(database_api::Backup::READY);
    entry.ddl_statements = {"CREATE TABLE T (K INT64) PRIMARY KEY (K)"};
    entry.schema_change_batches = {
        {.statements = entry.ddl_statements,
         .proto_descriptor_bytes = "descriptors"}};
    entry.operation_name = backup_name + "/operations/_auto1";
    entry.source_instance_config = "emulator-config";
    CreateSnapshot(&catalog, backup_name);
    ASSERT_THAT(catalog.CreateBackup(entry, TerminalBackupOperation(entry)),
                IsOk());
  }
  json current;
  {
    std::ifstream input(directory + "/backup_catalog.json");
    input >> current;
  }
  auto expect_rejected = [&](const json& catalog_json) {
    std::ofstream(directory + "/backup_catalog.json") << catalog_json.dump();
    BackupCatalog catalog(directory);
    EXPECT_THAT(catalog.Load(),
                googlesql_base::testing::StatusIs(
                    absl::StatusCode::kDataLoss));
  };

  for (const char* field : {"proto", "ddlBatches", "dialect", "idCounters",
                            "operationName", "sourceInstanceConfig"}) {
    json missing = current;
    missing["backups"][backup_name].erase(field);
    expect_rejected(missing);
  }
  for (const char* field : {"tableId", "columnId", "changeStreamId"}) {
    json missing = current;
    missing["backups"][backup_name]["idCounters"].erase(field);
    expect_rejected(missing);
  }

  json legacy = current;
  legacy["version"] = 3;
  auto& legacy_entry = legacy["backups"][backup_name];
  legacy_entry["ddlStatements"] =
      std::vector<std::string>{"CREATE TABLE T (K INT64) PRIMARY KEY (K)"};
  legacy_entry["protoDescriptors"] = "";
  legacy_entry.erase("ddlBatches");
  for (const char* field :
       {"proto", "ddlStatements", "protoDescriptors", "dialect",
        "idCounters", "operationName", "sourceInstanceConfig"}) {
    json missing = legacy;
    missing["backups"][backup_name].erase(field);
    expect_rejected(missing);
  }
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RejectsMissingUnsupportedAndSymlinkedCatalogs) {
  const std::string directory = TemporaryCatalogDirectory();
  std::filesystem::create_directories(directory);
  {
    std::ofstream(directory + "/backup_catalog.json")
        << R"({"backups":{}})";
  }
  BackupCatalog missing_version(directory);
  EXPECT_THAT(
      missing_version.Load(),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::HasSubstr("version must be present")));

  {
    std::ofstream(directory + "/backup_catalog.json")
        << R"({"version":5,"backups":{}})";
  }
  BackupCatalog unsupported_version(directory);
  EXPECT_THAT(
      unsupported_version.Load(),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::HasSubstr("Unsupported backup catalog version")));

  std::filesystem::remove(directory + "/backup_catalog.json");
  const std::string outside = TemporaryCatalogDirectory();
  std::filesystem::create_directories(outside);
  {
    std::ofstream(outside + "/catalog") << R"({"version":3})";
  }
  std::filesystem::create_symlink(outside + "/catalog",
                                  directory + "/backup_catalog.json");
  BackupCatalog linked_primary(directory);
  EXPECT_THAT(linked_primary.Load(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kDataLoss,
                  testing::HasSubstr("not a regular file")));
  operations_api::Operation operation;
  operation.set_name("projects/p/instances/i/operations/_auto1");
  operation.set_done(true);
  EXPECT_THAT(linked_primary.SaveOperation(operation),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kFailedPrecondition,
                  testing::HasSubstr("symbolic link")));

  std::filesystem::remove(directory + "/backup_catalog.json");
  std::filesystem::create_symlink(outside + "/catalog",
                                  directory + "/backup_catalog.json.tmp");
  BackupCatalog linked_temporary(directory);
  EXPECT_THAT(linked_temporary.Load(),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kDataLoss,
                  testing::HasSubstr("not a regular file")));
  EXPECT_THAT(linked_temporary.SaveOperation(operation),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kFailedPrecondition,
                  testing::HasSubstr("symbolic link")));
  std::filesystem::remove_all(directory);
  std::filesystem::remove_all(outside);
}

TEST(BackupCatalogTest, SnapshotLeaseBlocksDeletionUntilReleased) {
  const std::string directory = TemporaryCatalogDirectory();
  const std::string backup_name = "projects/p/instances/i/backups/leased";
  BackupCatalog catalog(directory);
  BackupCatalog::BackupEntry entry;
  entry.backup.set_name(backup_name);
  entry.backup.set_state(database_api::Backup::READY);
  entry.operation_name = backup_name + "/operations/_auto1";
  CreateSnapshot(&catalog, backup_name);
  ASSERT_THAT(catalog.CreateBackup(entry, TerminalBackupOperation(entry)),
              IsOk());
  auto lease_or = catalog.AcquireSnapshot(backup_name);
  ASSERT_THAT(lease_or, IsOk());
  std::unique_ptr<BackupCatalog::SnapshotLease> lease =
      std::move(*lease_or);

  std::promise<void> started;
  std::future<absl::Status> deletion =
      std::async(std::launch::async, [&catalog, &backup_name, &started] {
        started.set_value();
        return catalog.DeleteBackup(backup_name);
      });
  started.get_future().wait();
  EXPECT_EQ(deletion.wait_for(std::chrono::milliseconds(25)),
            std::future_status::timeout);
  lease.reset();
  EXPECT_THAT(deletion.get(), IsOk());
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(catalog.SnapshotDirectory(backup_name))
          .parent_path()));
  std::filesystem::remove_all(directory);
}

TEST(BackupCatalogTest, RejectsNonterminalOperation) {
  const std::string directory = TemporaryCatalogDirectory();
  BackupCatalog catalog(directory);
  operations_api::Operation operation;
  operation.set_name("projects/p/instances/i/operations/_auto1");
  EXPECT_THAT(catalog.SaveOperation(operation),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInvalidArgument));
  EXPECT_TRUE(catalog.AllOperations().empty());
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
