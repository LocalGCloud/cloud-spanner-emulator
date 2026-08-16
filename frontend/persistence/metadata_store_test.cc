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
#include "frontend/persistence/backup_catalog.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/status.h"
#include "google/iam/v1/policy.pb.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "nlohmann/json.hpp"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

using googlesql_base::testing::StatusIs;
using json = nlohmann::json;
namespace iam_api = ::google::iam::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;

std::string MakeTempDir(const std::string& suffix) {
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  std::string base = test_tmpdir ? test_tmpdir : "/tmp";
  std::string path =
      base + "/metadata_store_test_" + suffix + "_" + std::to_string(getpid());
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

TEST(MetadataStoreTest, SaveAndLoadRoundtrip) {
  std::string dir = MakeTempDir("roundtrip");
  {
    MetadataStore store(dir);
    store.AddInstance("projects/p/instances/i1", "Instance 1",
                      "emulator-config", 1000, {{"env", "test"}},
                      "2026-04-10T00:00:00Z");
    store.UpdateInstanceNodeCount("projects/p/instances/i1", 2);
    store.AddDatabase("projects/p/instances/i1", "db1", "GOOGLE_STANDARD_SQL",
                      {"CREATE TABLE T (K INT64) PRIMARY KEY (K)"});
    store.UpdateIdCounters("projects/p/instances/i1", "db1",
                           {.table_id = 3, .column_id = 8});
    store.SetInstanceConfig("projects/p/instanceConfigs/custom", "AQID");
    instance_api::InstancePartition partition;
    partition.set_name(
        "projects/p/instances/i1/instancePartitions/partition-1");
    partition.set_config("projects/p/instanceConfigs/emulator-config");
    partition.set_display_name("Partition 1");
    partition.set_node_count(1);
    partition.mutable_create_time()->set_seconds(100);
    partition.mutable_update_time()->set_seconds(200);
    store.SetInstancePartition(partition);
    ::google::longrunning::Operation pending;
    pending.set_name("projects/p/instanceConfigs/custom/operations/create");
    pending.set_done(true);
    pending.mutable_error()->set_code(13);
    store.SetPendingOperation(pending);
    store.SetPendingDdlOperation(
        "projects/p/instances/i1/databases/db1",
        {.operation_name =
             "projects/p/instances/i1/databases/db1/operations/ddl-1",
         .statements = {"ALTER TABLE T ADD COLUMN V STRING(MAX)"},
         .proto_descriptor_bytes = "descriptors",
         .has_rollback_checkpoint = true});
    EXPECT_TRUE(store.Save().ok());
  }
  {
    std::ifstream file(dir + "/metadata.json");
    json persisted;
    file >> persisted;
    const auto& counters =
        persisted["instances"]["projects/p/instances/i1"]["databases"]["db1"]
                 ["idCounters"];
    EXPECT_FALSE(counters.contains("sequenceId"));
    EXPECT_FALSE(counters.contains("namedSchemaId"));
    EXPECT_TRUE(
        persisted["pendingOperations"]
                 ["projects/p/instanceConfigs/custom/operations/create"]
                     .is_string());
    EXPECT_EQ(
        persisted["pendingDdlOperations"]
                 ["projects/p/instances/i1/databases/db1"]["operationName"],
        "projects/p/instances/i1/databases/db1/operations/ddl-1");
    EXPECT_TRUE(
        persisted["pendingDdlOperations"]
                 ["projects/p/instances/i1/databases/db1"]
                 ["hasRollbackCheckpoint"]);
  }
  {
    MetadataStore store(dir);
    EXPECT_TRUE(store.Load().ok());
    EXPECT_TRUE(store.has_metadata());

    const auto& instances = store.instances();
    ASSERT_EQ(instances.size(), 1);

    const auto& inst = instances.at("projects/p/instances/i1");
    EXPECT_EQ(inst.display_name, "Instance 1");
    EXPECT_EQ(inst.config, "emulator-config");
    EXPECT_EQ(inst.node_count, 2);
    EXPECT_EQ(inst.processing_units, 0);
    EXPECT_EQ(inst.labels.at("env"), "test");
    EXPECT_EQ(inst.create_time, "2026-04-10T00:00:00Z");

    ASSERT_EQ(inst.databases.size(), 1);
    const auto& db = inst.databases.at("db1");
    EXPECT_EQ(db.dialect, "GOOGLE_STANDARD_SQL");
    ASSERT_EQ(db.ddl_statements.size(), 1);
    EXPECT_EQ(db.ddl_statements[0], "CREATE TABLE T (K INT64) PRIMARY KEY (K)");
    EXPECT_EQ(db.id_counters.table_id, 3);
    EXPECT_EQ(db.id_counters.column_id, 8);
    ASSERT_EQ(store.instance_configs().size(), 1);
    EXPECT_EQ(store.instance_configs().at("projects/p/instanceConfigs/custom"),
              "AQID");
    ASSERT_EQ(store.instance_partitions().size(), 1);
    EXPECT_EQ(
        store.instance_partitions()
            .at("projects/p/instances/i1/instancePartitions/partition-1")
            .display_name(),
        "Partition 1");
    ASSERT_EQ(store.AllPendingOperations().size(), 1);
    EXPECT_EQ(store.AllPendingOperations()
                  .at("projects/p/instanceConfigs/custom/operations/create")
                  .error()
                  .code(),
              13);
    ASSERT_EQ(store.AllPendingDdlOperations().size(), 1);
    const auto ddl_intent = store.AllPendingDdlOperations().at(
        "projects/p/instances/i1/databases/db1");
    EXPECT_EQ(ddl_intent.operation_name,
              "projects/p/instances/i1/databases/db1/operations/ddl-1");
    EXPECT_EQ(ddl_intent.statements,
              std::vector<std::string>(
                  {"ALTER TABLE T ADD COLUMN V STRING(MAX)"}));
    EXPECT_EQ(ddl_intent.proto_descriptor_bytes, "descriptors");
    EXPECT_TRUE(ddl_intent.has_rollback_checkpoint);
  }
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, ReconcilesPendingOperationAfterCrossStoreCrash) {
  const std::string dir = MakeTempDir("pending-operation");
  ::google::longrunning::Operation operation;
  operation.set_name(
      "projects/p/instances/i/databases/restored/operations/_auto9");
  operation.set_done(true);
  operation.mutable_error()->set_code(10);
  {
    MetadataStore metadata(dir);
    metadata.AddInstance("projects/p/instances/i", "I", "emulator-config",
                         1000, {}, "");
    metadata.SetPendingOperation(operation);
    ASSERT_TRUE(metadata.Save().ok());
  }
  {
    MetadataStore metadata(dir);
    BackupCatalog catalog(dir);
    ASSERT_TRUE(metadata.Load().ok());
    ASSERT_TRUE(catalog.Load().ok());
    ASSERT_TRUE(metadata.ReconcilePendingOperations(&catalog).ok());
    EXPECT_TRUE(metadata.AllPendingOperations().empty());
    ASSERT_EQ(catalog.AllOperations().size(), 1);
    EXPECT_EQ(catalog.AllOperations().front().SerializeAsString(),
              operation.SerializeAsString());
  }
  {
    MetadataStore metadata(dir);
    BackupCatalog catalog(dir);
    ASSERT_TRUE(metadata.Load().ok());
    ASSERT_TRUE(catalog.Load().ok());
    ASSERT_TRUE(metadata.ReconcilePendingOperations(&catalog).ok());
    EXPECT_TRUE(metadata.AllPendingOperations().empty());
    ASSERT_EQ(catalog.AllOperations().size(), 1);
  }
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, ClearsJournalAfterAlreadyPromotedOperation) {
  const std::string dir = MakeTempDir("promoted-operation");
  ::google::longrunning::Operation operation;
  operation.set_name(
      "projects/p/instances/i/databases/restored/operations/_auto10");
  operation.set_done(true);
  operation.mutable_error()->set_code(10);
  {
    MetadataStore metadata(dir);
    metadata.SetPendingOperation(operation);
    ASSERT_TRUE(metadata.Save().ok());
    BackupCatalog catalog(dir);
    ASSERT_TRUE(catalog.Load().ok());
    ASSERT_TRUE(catalog.SaveOperation(operation).ok());
  }
  {
    MetadataStore metadata(dir);
    BackupCatalog catalog(dir);
    ASSERT_TRUE(metadata.Load().ok());
    ASSERT_TRUE(catalog.Load().ok());
    ASSERT_TRUE(metadata.ReconcilePendingOperations(&catalog).ok());
    EXPECT_TRUE(metadata.AllPendingOperations().empty());
    ASSERT_EQ(catalog.AllOperations().size(), 1);
  }
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, MissingFileReturnsEmptyState) {
  std::string dir = MakeTempDir("missing");
  MetadataStore store(dir);
  EXPECT_TRUE(store.Load().ok());
  EXPECT_FALSE(store.has_metadata());
  EXPECT_TRUE(store.instances().empty());
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, IamPoliciesPersistWithoutInstanceMetadata) {
  std::string dir = MakeTempDir("iam");
  const std::string resource = "projects/p/instances/i1";
  iam_api::Policy policy;
  policy.set_version(3);
  policy.set_etag("localcloud");
  auto* binding = policy.add_bindings();
  binding->set_role("roles/spanner.databaseReader");
  binding->add_members("user:developer@example.com");
  {
    MetadataStore store(dir);
    store.SetIamPolicy(resource, policy);
    EXPECT_TRUE(store.Save().ok());
  }
  {
    std::ifstream file(dir + "/metadata.json");
    json persisted;
    file >> persisted;
    EXPECT_TRUE(persisted["iamPolicies"][resource].is_string());
  }
  {
    MetadataStore store(dir);
    EXPECT_TRUE(store.Load().ok());
    EXPECT_TRUE(store.has_metadata());
    ASSERT_TRUE(store.GetIamPolicy(resource).has_value());
    EXPECT_EQ(store.GetIamPolicy(resource)->SerializeAsString(),
              policy.SerializeAsString());
    ASSERT_EQ(store.AllIamPolicies().size(), 1);
    EXPECT_EQ(store.AllIamPolicies().at(resource).SerializeAsString(),
              policy.SerializeAsString());
    EXPECT_FALSE(
        store.GetIamPolicy("projects/p/instances/missing").has_value());
  }
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, ResourceDeletionRemovesIamPolicies) {
  std::string dir = MakeTempDir("iam-delete");
  MetadataStore store(dir);
  const std::string instance = "projects/p/instances/i1";
  const std::string database = instance + "/databases/d1";
  const std::string backup = instance + "/backups/b1";
  store.AddInstance(instance, "I1", "emulator-config", 1000, {}, "");
  store.AddDatabase(instance, "d1", "GOOGLE_STANDARD_SQL", {});
  iam_api::Policy policy;
  store.SetIamPolicy(instance, policy);
  store.SetIamPolicy(database, policy);
  store.SetIamPolicy(backup, policy);

  store.RemoveDatabase(instance, "d1");
  EXPECT_FALSE(store.GetIamPolicy(database).has_value());
  EXPECT_TRUE(store.GetIamPolicy(instance).has_value());
  EXPECT_TRUE(store.GetIamPolicy(backup).has_value());

  store.RemoveIamPolicy(backup);
  EXPECT_FALSE(store.GetIamPolicy(backup).has_value());
  store.SetIamPolicy(database, policy);
  store.SetIamPolicy(backup, policy);
  store.RemoveInstance(instance);
  EXPECT_FALSE(store.GetIamPolicy(instance).has_value());
  EXPECT_FALSE(store.GetIamPolicy(database).has_value());
  EXPECT_FALSE(store.GetIamPolicy(backup).has_value());
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, CorruptFileReturnsDataLoss) {
  std::string dir = MakeTempDir("corrupt");
  {
    std::ofstream file(dir + "/metadata.json");
    file << "{{not valid json!!!";
  }
  MetadataStore store(dir);
  EXPECT_THAT(store.Load(), StatusIs(absl::StatusCode::kDataLoss));
  EXPECT_FALSE(store.has_metadata());
  EXPECT_TRUE(store.instances().empty());
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, MalformedIamPolicyReturnsDataLoss) {
  std::string dir = MakeTempDir("malformed-iam");
  {
    std::ofstream file(dir + "/metadata.json");
    file << R"({"version":3,"iamPolicies":{"projects/p/instances/i1":"gA=="}})";
  }
  MetadataStore store(dir);
  EXPECT_THAT(store.Load(), StatusIs(absl::StatusCode::kDataLoss));
  EXPECT_FALSE(store.has_metadata());
  EXPECT_TRUE(store.AllIamPolicies().empty());
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, NegativeIdCounterReturnsDataLoss) {
  std::string dir = MakeTempDir("negative-counter");
  {
    std::ofstream file(dir + "/metadata.json");
    file << R"({"version":3,"instances":{"projects/p/instances/i1":{"databases":{"db1":{"idCounters":{"tableId":-1}}}}}})";
  }
  MetadataStore store(dir);
  EXPECT_THAT(store.Load(), StatusIs(absl::StatusCode::kDataLoss));
  EXPECT_TRUE(store.instances().empty());
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, UnreadableExistingFileReturnsDataLoss) {
  std::string dir = MakeTempDir("unreadable");
  std::filesystem::create_directory(dir + "/metadata.json");
  MetadataStore store(dir);
  EXPECT_THAT(store.Load(), StatusIs(absl::StatusCode::kDataLoss));
  EXPECT_FALSE(store.has_metadata());
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, RejectsMissingAndUnsupportedVersions) {
  std::string dir = MakeTempDir("unsupported-version");
  std::ofstream(dir + "/metadata.json") << R"({"instances":{}})";
  MetadataStore missing_version(dir);
  EXPECT_THAT(missing_version.Load(),
              StatusIs(absl::StatusCode::kDataLoss,
                       testing::HasSubstr("version must be present")));

  std::ofstream(dir + "/metadata.json")
      << R"({"version":9,"instances":{}})";
  MetadataStore unsupported_version(dir);
  EXPECT_THAT(unsupported_version.Load(),
              StatusIs(absl::StatusCode::kDataLoss,
                       testing::HasSubstr("Unsupported metadata version")));
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, RejectsMissingVersionFourFields) {
  const std::string dir = MakeTempDir("missing-v4-fields");
  const std::string instance_name = "projects/p/instances/i";
  json valid = json::parse(R"({
    "version": 4,
    "instances": {
      "projects/p/instances/i": {
        "displayName": "i",
        "config": "emulator-config",
        "processingUnits": 1000,
        "createTime": "2026-01-01T00:00:00Z",
        "updateTime": "2026-01-01T00:00:00Z",
        "labels": {},
        "databases": {
          "db": {
            "dialect": "GOOGLE_STANDARD_SQL",
            "ddlBatches": [],
            "createTime": "2026-01-01T00:00:00Z",
            "enableDropProtection": false,
            "idCounters": {
              "tableId": 0,
              "columnId": 0,
              "changeStreamId": 0
            }
          }
        }
      }
    },
    "iamPolicies": {},
    "instanceConfigs": {},
    "pendingOperations": {},
    "pendingBackupDeletions": []
  })");
  auto expect_rejected = [&](const json& metadata) {
    std::ofstream(dir + "/metadata.json") << metadata.dump();
    MetadataStore store(dir);
    EXPECT_THAT(store.Load(), StatusIs(absl::StatusCode::kDataLoss));
  };

  for (const char* field : {"iamPolicies", "instanceConfigs",
                            "pendingOperations", "pendingBackupDeletions"}) {
    json missing = valid;
    missing.erase(field);
    expect_rejected(missing);
  }
  for (const char* field :
       {"displayName", "config", "processingUnits", "createTime",
        "updateTime", "labels", "databases"}) {
    json missing = valid;
    missing["instances"][instance_name].erase(field);
    expect_rejected(missing);
  }
  for (const char* field : {"dialect", "ddlBatches", "createTime",
                            "enableDropProtection", "idCounters"}) {
    json missing = valid;
    missing["instances"][instance_name]["databases"]["db"].erase(field);
    expect_rejected(missing);
  }
  for (const char* field : {"tableId", "columnId", "changeStreamId"}) {
    json missing = valid;
    missing["instances"][instance_name]["databases"]["db"]["idCounters"].erase(
        field);
    expect_rejected(missing);
  }
  json version_five_missing_partitions = valid;
  version_five_missing_partitions["version"] = 5;
  expect_rejected(version_five_missing_partitions);
  json version_six_missing_node_count = valid;
  version_six_missing_node_count["version"] = 6;
  version_six_missing_node_count["instancePartitions"] = json::object();
  expect_rejected(version_six_missing_node_count);
  json version_seven_missing_ddl_operations = valid;
  version_seven_missing_ddl_operations["version"] = 7;
  version_seven_missing_ddl_operations["instancePartitions"] = json::object();
  version_seven_missing_ddl_operations["instances"][instance_name]
                                      ["nodeCount"] = 0;
  expect_rejected(version_seven_missing_ddl_operations);
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, RejectsSymlinkedPrimaryAndTemporaryFiles) {
  std::string dir = MakeTempDir("symlink-files");
  std::string outside = MakeTempDir("symlink-target");
  std::ofstream(outside + "/metadata.json") << R"({"version":3})";
  std::filesystem::create_symlink(outside + "/metadata.json",
                                  dir + "/metadata.json");
  MetadataStore linked_primary(dir);
  EXPECT_THAT(linked_primary.Load(),
              StatusIs(absl::StatusCode::kDataLoss,
                       testing::HasSubstr("not a regular file")));
  EXPECT_THAT(linked_primary.Save(),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr("symbolic link")));

  std::filesystem::remove(dir + "/metadata.json");
  std::filesystem::create_symlink(outside + "/metadata.json",
                                  dir + "/metadata.json.tmp");
  MetadataStore linked_temporary(dir);
  EXPECT_THAT(linked_temporary.Load(),
              StatusIs(absl::StatusCode::kDataLoss,
                       testing::HasSubstr("not a regular file")));
  EXPECT_THAT(linked_temporary.Save(),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr("symbolic link")));
  std::filesystem::remove_all(dir);
  std::filesystem::remove_all(outside);
}

TEST(MetadataStoreTest, LegacyCounterFieldsAreIgnoredOnRead) {
  std::string dir = MakeTempDir("legacy-counters");
  {
    MetadataStore store(dir);
    store.AddInstance("projects/p/instances/i1", "I1", "emulator-config",
                      1000, {}, "");
    store.AddDatabase("projects/p/instances/i1", "db1",
                      "GOOGLE_STANDARD_SQL", {});
    store.UpdateIdCounters(
        "projects/p/instances/i1", "db1",
        {.table_id = 3, .column_id = 5, .change_stream_id = 7});
    GOOGLESQL_ASSERT_OK(store.Save());
  }
  {
    std::ifstream input(dir + "/metadata.json");
    json persisted;
    input >> persisted;
    input.close();
    auto& counters =
        persisted["instances"]["projects/p/instances/i1"]["databases"]["db1"]
                 ["idCounters"];
    ASSERT_EQ(counters["tableId"].get<int64_t>(), 3);
    ASSERT_EQ(counters["columnId"].get<int64_t>(), 5);
    ASSERT_EQ(counters["changeStreamId"].get<int64_t>(), 7);
    counters["sequenceId"] = 11;
    counters["namedSchemaId"] = 13;
    std::ofstream output(dir + "/metadata.json");
    output << persisted.dump(2);
    output.close();
  }
  {
    std::ifstream rewritten(dir + "/metadata.json");
    json persisted;
    rewritten >> persisted;
    const auto& counters =
        persisted["instances"]["projects/p/instances/i1"]["databases"]["db1"]
                 ["idCounters"];
    ASSERT_EQ(counters["tableId"].get<int64_t>(), 3);
    ASSERT_EQ(counters["columnId"].get<int64_t>(), 5);
    ASSERT_EQ(counters["changeStreamId"].get<int64_t>(), 7);
  }

  MetadataStore restored(dir);
  GOOGLESQL_ASSERT_OK(restored.Load());
  const auto restored_instances = restored.instances();
  const auto& counters =
      restored_instances.at("projects/p/instances/i1")
          .databases.at("db1")
          .id_counters;
  EXPECT_EQ(counters.table_id, 3);
  EXPECT_EQ(counters.column_id, 5);
  EXPECT_EQ(counters.change_stream_id, 7);
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, AtomicWriteCleansTmpFile) {
  std::string dir = MakeTempDir("atomic");
  MetadataStore store(dir);
  store.AddInstance("projects/p/instances/i1", "I1", "emulator-config", 1000,
                    {}, "");
  EXPECT_TRUE(store.Save().ok());

  EXPECT_TRUE(std::filesystem::exists(dir + "/metadata.json"));
  EXPECT_FALSE(std::filesystem::exists(dir + "/metadata.json.tmp"));
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, RecoversAtomicTemporaryFileWhenPrimaryIsMissing) {
  std::string dir = MakeTempDir("recover-temporary");
  {
    MetadataStore store(dir);
    store.AddInstance("projects/p/instances/i1", "I1", "emulator-config",
                      1000, {}, "");
    GOOGLESQL_ASSERT_OK(store.Save());
  }
  std::filesystem::rename(dir + "/metadata.json",
                          dir + "/metadata.json.tmp");

  MetadataStore restored(dir);
  GOOGLESQL_ASSERT_OK(restored.Load());
  EXPECT_TRUE(restored.instances().contains("projects/p/instances/i1"));
  EXPECT_TRUE(std::filesystem::exists(dir + "/metadata.json"));
  EXPECT_FALSE(std::filesystem::exists(dir + "/metadata.json.tmp"));
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, StableFileWinsOverStaleTemporaryFile) {
  std::string dir = MakeTempDir("stale-temporary");
  {
    MetadataStore store(dir);
    store.AddInstance("projects/p/instances/i1", "I1", "emulator-config",
                      1000, {}, "");
    GOOGLESQL_ASSERT_OK(store.Save());
  }
  std::ofstream(dir + "/metadata.json.tmp") << "not valid JSON";

  MetadataStore restored(dir);
  GOOGLESQL_ASSERT_OK(restored.Load());
  EXPECT_TRUE(restored.instances().contains("projects/p/instances/i1"));
  EXPECT_FALSE(std::filesystem::exists(dir + "/metadata.json.tmp"));
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, SaveCreatesMissingDataDirectory) {
  std::string dir = MakeTempDir("create-directory") + "/nested";
  MetadataStore store(dir);
  store.SetInstanceConfig("projects/p/instanceConfigs/custom", "AQID");
  GOOGLESQL_ASSERT_OK(store.Save());
  EXPECT_TRUE(std::filesystem::exists(dir + "/metadata.json"));
  std::filesystem::remove_all(std::filesystem::path(dir).parent_path());
}

TEST(MetadataStoreTest, MultipleInstancesAndDatabases) {
  std::string dir = MakeTempDir("multi");
  {
    MetadataStore store(dir);
    store.AddInstance("projects/p/instances/i1", "I1", "cfg", 1000, {}, "");
    store.AddInstance("projects/p/instances/i2", "I2", "cfg", 2000, {}, "");
    store.AddDatabase("projects/p/instances/i1", "db1", "GOOGLE_STANDARD_SQL",
                      {"CREATE TABLE A (K INT64) PRIMARY KEY (K)"});
    store.AddDatabase("projects/p/instances/i1", "db2", "POSTGRESQL",
                      {"CREATE TABLE B (K INT8 PRIMARY KEY)"});
    store.AddDatabase("projects/p/instances/i2", "db3", "GOOGLE_STANDARD_SQL",
                      {});
    EXPECT_TRUE(store.Save().ok());
  }
  {
    MetadataStore store(dir);
    EXPECT_TRUE(store.Load().ok());
    const auto& instances = store.instances();
    EXPECT_EQ(instances.size(), 2);
    EXPECT_EQ(instances.at("projects/p/instances/i1").databases.size(), 2);
    EXPECT_EQ(instances.at("projects/p/instances/i2").databases.size(), 1);
    EXPECT_EQ(
        instances.at("projects/p/instances/i1").databases.at("db2").dialect,
        "POSTGRESQL");
  }
  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, RemoveInstanceAndDatabase) {
  std::string dir = MakeTempDir("remove");
  MetadataStore store(dir);
  store.AddInstance("projects/p/instances/i1", "I1", "cfg", 1000, {}, "");
  store.AddDatabase("projects/p/instances/i1", "db1", "GOOGLE_STANDARD_SQL",
                    {});
  store.AddDatabase("projects/p/instances/i1", "db2", "GOOGLE_STANDARD_SQL",
                    {});

  store.RemoveDatabase("projects/p/instances/i1", "db1");
  EXPECT_TRUE(store.Save().ok());
  EXPECT_TRUE(store.Load().ok());
  EXPECT_EQ(store.instances().at("projects/p/instances/i1").databases.size(),
            1);

  store.RemoveInstance("projects/p/instances/i1");
  EXPECT_TRUE(store.Save().ok());
  EXPECT_TRUE(store.Load().ok());
  EXPECT_TRUE(store.instances().empty());

  std::filesystem::remove_all(dir);
}

TEST(MetadataStoreTest, UpdateDdlAppendsCommittedSchemaBatch) {
  std::string dir = MakeTempDir("ddl");
  MetadataStore store(dir);
  store.AddInstance("projects/p/instances/i1", "I1", "cfg", 1000, {}, "");
  store.AddDatabase("projects/p/instances/i1", "db1", "GOOGLE_STANDARD_SQL",
                    {"CREATE TABLE T1 (K INT64) PRIMARY KEY (K)"},
                    "descriptor-v1");
  store.UpdateDdl("projects/p/instances/i1", "db1",
                  {"CREATE TABLE T2 (K INT64) PRIMARY KEY (K)"},
                  "descriptor-v2");
  EXPECT_TRUE(store.Save().ok());
  EXPECT_TRUE(store.Load().ok());
  const auto instances = store.instances();
  const auto& database =
      instances.at("projects/p/instances/i1").databases.at("db1");
  EXPECT_EQ(database.ddl_statements.size(), 2);
  ASSERT_EQ(database.schema_change_batches.size(), 2);
  EXPECT_EQ(database.schema_change_batches[0].proto_descriptor_bytes,
            "descriptor-v1");
  EXPECT_EQ(database.schema_change_batches[1].proto_descriptor_bytes,
            "descriptor-v2");
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
