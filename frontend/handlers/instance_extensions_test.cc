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

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/strings/escaping.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/persistence/metadata_store.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "grpcpp/client_context.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/test_env.h"

ABSL_DECLARE_FLAG(std::string, data_dir);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

class PersistentInstanceConfigDirectory {
 public:
  PersistentInstanceConfigDirectory()
      : previous_(absl::GetFlag(FLAGS_data_dir)),
        path_((std::filesystem::temp_directory_path() /
               ("spanner-instance-config-" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())))
                  .string()) {
    std::filesystem::create_directories(path_);
    absl::SetFlag(&FLAGS_data_dir, path_);
  }
  const std::string& path() const { return path_; }


  ~PersistentInstanceConfigDirectory() {
    absl::SetFlag(&FLAGS_data_dir, previous_);
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

 private:
  std::string previous_;
  std::string path_;
};

void RestoreInstanceConfigsAndOperations(test::TestEnv* env) {
  MetadataStore* metadata = env->server()->env()->metadata_store();
  ASSERT_NE(metadata, nullptr);
  ASSERT_TRUE(metadata->Load().ok());
  for (const auto& [name, encoded] : metadata->instance_configs()) {
    std::string serialized;
    instance_api::InstanceConfig config;
    ASSERT_TRUE(absl::Base64Unescape(encoded, &serialized));
    ASSERT_TRUE(config.ParseFromString(serialized));
    config.set_name(name);
    ASSERT_TRUE(env->server()->env()->CreateInstanceConfig(config).ok());
  }
  BackupCatalog* catalog = env->server()->env()->backup_catalog();
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->Load().ok());
  for (const auto& operation : catalog->AllOperations()) {
    ASSERT_TRUE(env->server()
                    ->env()
                    ->operation_manager()
                    ->RestoreOperation(operation)
                    .ok());
  }
}

TEST(InstanceExtensionsTest, BuiltInConfigIdIsReserved) {
  test::TestEnv env;
  instance_api::CreateInstanceConfigRequest request;
  request.set_parent("projects/p");
  request.set_instance_config_id("emulator-config");
  request.mutable_instance_config()->set_name(
      "projects/p/instanceConfigs/emulator-config");
  request.mutable_instance_config()->set_display_name("Replacement");
  operations_api::Operation operation;
  grpc::ClientContext context;
  grpc::Status status = env.instance_admin_client()->CreateInstanceConfig(
      &context, request, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);

  instance_api::ListInstanceConfigsRequest list_request;
  list_request.set_parent("projects/p");
  instance_api::ListInstanceConfigsResponse response;
  grpc::ClientContext list_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->ListInstanceConfigs(&list_context, list_request, &response)
                  .ok());
  ASSERT_EQ(response.instance_configs_size(), 1);
  EXPECT_EQ(response.instance_configs(0).name(),
            "projects/p/instanceConfigs/emulator-config");
  EXPECT_EQ(response.instance_configs(0).config_type(),
            instance_api::InstanceConfig::GOOGLE_MANAGED);
}

TEST(InstanceExtensionsTest, ConfigValidationPaginationAndReferences) {
  test::TestEnv env;
  const std::string project = "projects/p";
  const std::string base = project + "/instanceConfigs/emulator-config";
  auto make_request = [&](const std::string& id) {
    instance_api::CreateInstanceConfigRequest request;
    request.set_parent(project);
    request.set_instance_config_id(id);
    request.mutable_instance_config()->set_name(project + "/instanceConfigs/" + id);
    request.mutable_instance_config()->set_base_config(base);
    request.mutable_instance_config()->set_display_name(id);
    return request;
  };
  auto create = [&](const instance_api::CreateInstanceConfigRequest& request) {
    operations_api::Operation operation;
    grpc::ClientContext context;
    return env.instance_admin_client()->CreateInstanceConfig(
        &context, request, &operation);
  };

  auto request = make_request("custom");
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request = make_request("custom-bad_");
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request = make_request("custom-alpha");
  request.mutable_instance_config()->clear_name();
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request = make_request("custom-alpha");
  request.mutable_instance_config()->set_name(
      "projects/other/instanceConfigs/custom-alpha");
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request = make_request("custom-alpha");
  request.mutable_instance_config()->set_base_config(
      "projects/other/instanceConfigs/emulator-config");
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  request = make_request("custom-alpha");
  ASSERT_TRUE(create(request).ok());
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::ALREADY_EXISTS);
  request = make_request("custom-zeta");
  ASSERT_TRUE(create(request).ok());
  request = make_request("custom-preview");
  request.set_validate_only(true);
  ASSERT_TRUE(create(request).ok());
  instance_api::GetInstanceConfigRequest get_request;
  get_request.set_name(project + "/instanceConfigs/custom-preview");
  instance_api::InstanceConfig config;
  grpc::ClientContext get_context;
  EXPECT_EQ(env.instance_admin_client()
                ->GetInstanceConfig(&get_context, get_request, &config)
                .error_code(),
            grpc::StatusCode::NOT_FOUND);
  request = make_request("custom-alpha");
  request.set_validate_only(true);
  EXPECT_EQ(create(request).error_code(), grpc::StatusCode::ALREADY_EXISTS);

  instance_api::ListInstanceConfigsRequest list_request;
  list_request.set_parent(project);
  list_request.set_page_size(1);
  for (const std::string& expected : {"custom-alpha", "custom-zeta",
                                      "emulator-config"}) {
    instance_api::ListInstanceConfigsResponse page;
    grpc::ClientContext context;
    ASSERT_TRUE(env.instance_admin_client()
                    ->ListInstanceConfigs(&context, list_request, &page)
                    .ok());
    ASSERT_EQ(page.instance_configs_size(), 1);
    EXPECT_EQ(page.instance_configs(0).name(),
              project + "/instanceConfigs/" + expected);
    list_request.set_page_token(page.next_page_token());
    if (expected != "emulator-config") {
      EXPECT_FALSE(page.next_page_token().empty());
    } else {
      EXPECT_TRUE(page.next_page_token().empty());
    }
  }
  list_request.set_page_token("projects/other/instanceConfigs/custom-zeta");
  instance_api::ListInstanceConfigsResponse page;
  grpc::ClientContext list_context;
  EXPECT_EQ(env.instance_admin_client()
                ->ListInstanceConfigs(&list_context, list_request, &page)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  instance_api::UpdateInstanceConfigRequest update;
  update.mutable_instance_config()->set_name(
      project + "/instanceConfigs/custom-alpha");
  update.mutable_instance_config()->set_display_name("Unexpected");
  update.mutable_update_mask()->add_paths("displayName");
  operations_api::Operation operation;
  for (const std::string& path : {"displayName", "base_config", "replicas"}) {
    update.mutable_update_mask()->set_paths(0, path);
    grpc::ClientContext context;
    EXPECT_EQ(env.instance_admin_client()
                  ->UpdateInstanceConfig(&context, update, &operation)
                  .error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
  }
  update.mutable_update_mask()->set_paths(0, "labels");
  (*update.mutable_instance_config()->mutable_labels())["Upper"] = "bad";
  grpc::ClientContext labels_context;
  EXPECT_EQ(env.instance_admin_client()
                ->UpdateInstanceConfig(&labels_context, update, &operation)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  BackupCatalog::BackupEntry backup;
  backup.backup.set_name(project + "/instances/i1/backups/b1");
  backup.backup.set_state(database_api::Backup::CREATING);
  backup.source_instance_config = project + "/instanceConfigs/custom-alpha";
  backup.operation_name = backup.backup.name() + "/operations/create";
  operations_api::Operation backup_operation;
  backup_operation.set_name(backup.operation_name);
  backup_operation.set_done(true);
  ASSERT_TRUE(env.server()
                  ->env()
                  ->backup_catalog()
                  ->CreateBackup(backup, backup_operation)
                  .ok());
  instance_api::DeleteInstanceConfigRequest delete_request;
  delete_request.set_name(backup.source_instance_config);
  protobuf_api::Empty empty;
  grpc::ClientContext delete_context;
  EXPECT_EQ(env.instance_admin_client()
                ->DeleteInstanceConfig(&delete_context, delete_request, &empty)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  ASSERT_TRUE(
      env.server()->env()->backup_catalog()->DeleteBackup(backup.backup.name())
          .ok());
  grpc::ClientContext delete_after_backup_context;
  EXPECT_TRUE(env.instance_admin_client()
                  ->DeleteInstanceConfig(&delete_after_backup_context,
                                         delete_request, &empty)
                  .ok());
}

TEST(InstanceExtensionsTest, ConfigEtagGuardsUpdatesAndDeletes) {
  test::TestEnv env;
  const std::string config_name = "projects/p/instanceConfigs/custom-etag";
  instance_api::CreateInstanceConfigRequest create;
  create.set_parent("projects/p");
  create.set_instance_config_id("custom-etag");
  create.mutable_instance_config()->set_name(config_name);
  create.mutable_instance_config()->set_base_config(
      "projects/p/instanceConfigs/emulator-config");
  create.mutable_instance_config()->set_display_name("First");
  operations_api::Operation operation;
  grpc::ClientContext create_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->CreateInstanceConfig(&create_context, create, &operation)
                  .ok());
  instance_api::InstanceConfig created;
  ASSERT_TRUE(operation.response().UnpackTo(&created));
  ASSERT_FALSE(created.etag().empty());

  instance_api::UpdateInstanceConfigRequest update;
  update.mutable_instance_config()->set_name(config_name);
  update.mutable_instance_config()->set_etag(created.etag());
  update.mutable_instance_config()->set_display_name("Second");
  update.mutable_update_mask()->add_paths("display_name");
  grpc::ClientContext update_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->UpdateInstanceConfig(&update_context, update, &operation)
                  .ok());
  instance_api::InstanceConfig updated;
  ASSERT_TRUE(operation.response().UnpackTo(&updated));
  EXPECT_EQ(updated.display_name(), "Second");
  ASSERT_FALSE(updated.etag().empty());
  EXPECT_NE(updated.etag(), created.etag());

  update.mutable_instance_config()->set_display_name("Stale");
  grpc::ClientContext stale_update_context;
  EXPECT_EQ(env.instance_admin_client()
                ->UpdateInstanceConfig(&stale_update_context, update, &operation)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  instance_api::GetInstanceConfigRequest get;
  get.set_name(config_name);
  instance_api::InstanceConfig current;
  grpc::ClientContext get_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->GetInstanceConfig(&get_context, get, &current)
                  .ok());
  EXPECT_EQ(current.display_name(), "Second");
  EXPECT_EQ(current.etag(), updated.etag());

  instance_api::DeleteInstanceConfigRequest remove;
  remove.set_name(config_name);
  remove.set_etag(created.etag());
  protobuf_api::Empty empty;
  grpc::ClientContext stale_delete_context;
  EXPECT_EQ(env.instance_admin_client()
                ->DeleteInstanceConfig(&stale_delete_context, remove, &empty)
                .error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
  remove.set_etag(updated.etag());
  grpc::ClientContext delete_context;
  EXPECT_TRUE(env.instance_admin_client()
                  ->DeleteInstanceConfig(&delete_context, remove, &empty)
                  .ok());
}

TEST(InstanceExtensionsTest, FailedPersistenceRollsBackAdminUpdates) {
  PersistentInstanceConfigDirectory data_dir;
  test::TestEnv env;
  const std::string project = "projects/p";
  const std::string instance_name = project + "/instances/i1";
  const std::string custom_config = project + "/instanceConfigs/custom-test";
  const std::string built_in_config =
      project + "/instanceConfigs/emulator-config";
  const std::string database_name = instance_name + "/databases/d1";

  instance_api::CreateInstanceConfigRequest config_request;
  config_request.set_parent(project);
  config_request.set_instance_config_id("custom-test");
  config_request.mutable_instance_config()->set_name(custom_config);
  config_request.mutable_instance_config()->set_base_config(built_in_config);
  config_request.mutable_instance_config()->set_display_name("Custom");
  operations_api::Operation operation;
  grpc::ClientContext config_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->CreateInstanceConfig(&config_context, config_request,
                                         &operation)
                  .ok());

  instance_api::CreateInstanceRequest instance_request;
  instance_request.set_parent(project);
  instance_request.set_instance_id("i1");
  instance_request.mutable_instance()->set_config(built_in_config);
  instance_request.mutable_instance()->set_display_name("Original");
  instance_request.mutable_instance()->set_node_count(1);
  grpc::ClientContext instance_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->CreateInstance(&instance_context, instance_request,
                                   &operation)
                  .ok());

  database_api::CreateDatabaseRequest database_request;
  database_request.set_parent(instance_name);
  database_request.set_create_statement("CREATE DATABASE `d1`");
  grpc::ClientContext database_context;
  ASSERT_TRUE(env.database_admin_client()
                  ->CreateDatabase(&database_context, database_request,
                                   &operation)
                  .ok());

  ASSERT_TRUE(
      std::filesystem::create_directory(data_dir.path() + "/metadata.json.tmp"));

  instance_api::MoveInstanceRequest move_request;
  move_request.set_name(instance_name);
  move_request.set_target_config(custom_config);
  grpc::ClientContext move_context;
  grpc::Status status = env.instance_admin_client()->MoveInstance(
      &move_context, move_request, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  instance_api::UpdateInstanceRequest update_instance;
  update_instance.mutable_instance()->set_name(instance_name);
  update_instance.mutable_instance()->set_display_name("Replacement");
  update_instance.mutable_field_mask()->add_paths("display_name");
  grpc::ClientContext update_instance_context;
  status = env.instance_admin_client()->UpdateInstance(
      &update_instance_context, update_instance, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  database_api::UpdateDatabaseRequest update_database;
  update_database.mutable_database()->set_name(database_name);
  update_database.mutable_database()->set_enable_drop_protection(true);
  update_database.mutable_update_mask()->add_paths("enable_drop_protection");
  grpc::ClientContext update_database_context;
  status = env.database_admin_client()->UpdateDatabase(
      &update_database_context, update_database, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  instance_api::GetInstanceRequest get_instance;
  get_instance.set_name(instance_name);
  instance_api::Instance instance;
  grpc::ClientContext get_instance_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->GetInstance(&get_instance_context, get_instance, &instance)
                  .ok());
  EXPECT_EQ(instance.config(), built_in_config);
  EXPECT_EQ(instance.display_name(), "Original");

  database_api::GetDatabaseRequest get_database;
  get_database.set_name(database_name);
  database_api::Database database;
  grpc::ClientContext get_database_context;
  ASSERT_TRUE(env.database_admin_client()
                  ->GetDatabase(&get_database_context, get_database, &database)
                  .ok());
  EXPECT_FALSE(database.enable_drop_protection());

  const auto persisted = env.server()->env()->metadata_store()->instances();
  EXPECT_EQ(persisted.at(instance_name).config, built_in_config);
  EXPECT_EQ(persisted.at(instance_name).display_name, "Original");
  EXPECT_FALSE(
      persisted.at(instance_name).databases.at("d1").enable_drop_protection);
}

TEST(InstanceExtensionsTest, ConfigLifecyclePersistenceMoveAndOperationList) {
  PersistentInstanceConfigDirectory data_dir;
  const std::string project = "projects/p";
  const std::string config_name = project + "/instanceConfigs/custom-test";
  const std::string base_config =
      project + "/instanceConfigs/emulator-config";
  const std::string instance_name = project + "/instances/i1";
  std::string create_operation_name;
  std::string update_operation_name;
  std::string update_etag;

  {
    test::TestEnv first;
    instance_api::CreateInstanceConfigRequest create_request;
    create_request.set_parent(project);
    create_request.set_instance_config_id("custom-test");
    create_request.mutable_instance_config()->set_name(config_name);
    create_request.mutable_instance_config()->set_base_config(base_config);
    create_request.mutable_instance_config()->set_display_name("Custom One");
    (*create_request.mutable_instance_config()->mutable_labels())["env"] =
        "test";
    operations_api::Operation create_operation;
    grpc::ClientContext create_context;
    grpc::Status status = first.instance_admin_client()->CreateInstanceConfig(
        &create_context, create_request, &create_operation);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(create_operation.done());
    create_operation_name = create_operation.name();
    instance_api::InstanceConfig created;
    ASSERT_TRUE(create_operation.response().UnpackTo(&created));
    EXPECT_EQ(created.name(), config_name);
    EXPECT_EQ(created.display_name(), "Custom One");
    EXPECT_EQ(created.config_type(), instance_api::InstanceConfig::USER_MANAGED);
    EXPECT_EQ(created.state(), instance_api::InstanceConfig::READY);
    ASSERT_FALSE(created.etag().empty());
    instance_api::CreateInstanceConfigMetadata create_metadata;
    ASSERT_TRUE(create_operation.metadata().UnpackTo(&create_metadata));
    EXPECT_EQ(create_metadata.instance_config().name(), config_name);
    EXPECT_EQ(create_metadata.progress().progress_percent(), 100);
    EXPECT_TRUE(create_metadata.progress().has_start_time());
    EXPECT_TRUE(create_metadata.progress().has_end_time());

    instance_api::UpdateInstanceConfigRequest update_request;
    update_request.mutable_instance_config()->set_name(config_name);
    update_request.mutable_instance_config()->set_display_name("Custom Two");
    update_request.mutable_update_mask()->add_paths("display_name");
    operations_api::Operation update_operation;
    grpc::ClientContext update_context;
    status = first.instance_admin_client()->UpdateInstanceConfig(
        &update_context, update_request, &update_operation);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(update_operation.done());
    instance_api::UpdateInstanceConfigMetadata update_metadata;
    ASSERT_TRUE(update_operation.metadata().UnpackTo(&update_metadata));
    EXPECT_EQ(update_metadata.instance_config().display_name(), "Custom Two");
    EXPECT_EQ(update_metadata.progress().progress_percent(), 100);
    EXPECT_TRUE(update_metadata.progress().has_start_time());
    EXPECT_TRUE(update_metadata.progress().has_end_time());

    update_operation_name = update_operation.name();
    instance_api::InstanceConfig updated_config;
    ASSERT_TRUE(update_operation.response().UnpackTo(&updated_config));
    update_etag = updated_config.etag();
    ASSERT_FALSE(update_etag.empty());
    EXPECT_NE(update_etag, created.etag());
    instance_api::GetInstanceConfigRequest get_request;
    get_request.set_name(config_name);
    instance_api::InstanceConfig config;
    grpc::ClientContext get_context;
    status = first.instance_admin_client()->GetInstanceConfig(
        &get_context, get_request, &config);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(config.display_name(), "Custom Two");
    EXPECT_EQ(config.etag(), update_etag);
    EXPECT_EQ(config.labels().at("env"), "test");
    EXPECT_EQ(config.config_type(), instance_api::InstanceConfig::USER_MANAGED);

    ASSERT_TRUE(first.server()
                    ->env()
                    ->operation_manager()
                    ->CreateOperation(
                        project + "0/instanceConfigs/foreign", "create")
                    .ok());

    instance_api::ListInstanceConfigOperationsRequest list_request;
    list_request.set_parent(project);
    list_request.set_page_size(1);
    instance_api::ListInstanceConfigOperationsResponse list_response;
    grpc::ClientContext list_context;
    status = first.instance_admin_client()->ListInstanceConfigOperations(
        &list_context, list_request, &list_response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(list_response.operations_size(), 1);
    EXPECT_NE(list_response.operations(0).name().find("/instanceConfigs/"),
              std::string::npos);
    ASSERT_FALSE(list_response.next_page_token().empty());

    list_request.set_page_token(list_response.next_page_token());
    list_response.Clear();
    grpc::ClientContext list_next_context;
    status = first.instance_admin_client()->ListInstanceConfigOperations(
        &list_next_context, list_request, &list_response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(list_response.operations_size(), 1);
    EXPECT_NE(list_response.operations(0).name().find("/instanceConfigs/"),
              std::string::npos);
    EXPECT_TRUE(list_response.next_page_token().empty());

    list_request.set_page_token(
        "projects/other/instanceConfigs/custom-test/operations/create");
    grpc::ClientContext foreign_token_context;
    EXPECT_EQ(first.instance_admin_client()
                  ->ListInstanceConfigOperations(&foreign_token_context,
                                                 list_request, &list_response)
                  .error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    list_request.clear_page_token();
    list_request.set_page_size(100);
    list_request.set_filter("done:false");
    list_response.Clear();
    grpc::ClientContext incomplete_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&incomplete_context,
                                                   list_request, &list_response)
                    .ok());
    EXPECT_EQ(list_response.operations_size(), 0);

    list_request.set_filter(
        "metadata.@type=type.googleapis.com/google.spanner.admin.instance.v1."
        "CreateInstanceConfigMetadata");
    list_response.Clear();
    grpc::ClientContext metadata_filter_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&metadata_filter_context,
                                                   list_request, &list_response)
                    .ok());
    ASSERT_EQ(list_response.operations_size(), 1);
    EXPECT_EQ(list_response.operations(0).name(), create_operation_name);

    list_request.set_filter("name=" + create_operation_name);
    list_response.Clear();
    grpc::ClientContext name_filter_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&name_filter_context,
                                                   list_request, &list_response)
                    .ok());
    ASSERT_EQ(list_response.operations_size(), 1);
    EXPECT_EQ(list_response.operations(0).name(), create_operation_name);

    list_request.set_filter("done:true AND error:*");
    list_response.Clear();
    grpc::ClientContext error_filter_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&error_filter_context,
                                                   list_request, &list_response)
                    .ok());
    EXPECT_EQ(list_response.operations_size(), 0);

    // Metadata paths match by reflection, ignoring case.
    list_request.set_filter(
        "metadata.instance_config.name:CUSTOM-TEST "
        "metadata.progress.progress_percent = 100");
    list_response.Clear();
    grpc::ClientContext metadata_path_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&metadata_path_context,
                                                   list_request, &list_response)
                    .ok());
    EXPECT_EQ(list_response.operations_size(), 2);

    // OR binds tighter than AND: (C OR done) AND E, not C OR (done AND E).
    list_request.set_filter(
        "metadata.instance_config.display_name = \"custom two\" OR done:true "
        "AND metadata.instance_config.display_name:ONE");
    list_response.Clear();
    grpc::ClientContext precedence_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&precedence_context,
                                                   list_request, &list_response)
                    .ok());
    ASSERT_EQ(list_response.operations_size(), 1);
    EXPECT_EQ(list_response.operations(0).name(), create_operation_name);

    list_request.set_filter(
        "(metadata.@type=type.googleapis.com/"
        "google.spanner.admin.instance.v1.CreateInstanceConfigMetadata) AND "
        "(metadata.instance_config.name:custom-test) AND "
        "(metadata.progress.start_time < \"2021-03-28T14:50:00Z\") AND "
        "(error:*)");
    list_response.Clear();
    grpc::ClientContext documented_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&documented_context,
                                                   list_request, &list_response)
                    .ok());
    EXPECT_EQ(list_response.operations_size(), 0);

    list_request.set_filter("metadata:*");
    list_response.Clear();
    grpc::ClientContext presence_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&presence_context,
                                                   list_request, &list_response)
                    .ok());
    EXPECT_EQ(list_response.operations_size(), 2);

    list_request.set_filter("unknown:field");
    grpc::ClientContext unsupported_filter_context;
    EXPECT_EQ(first.instance_admin_client()
                  ->ListInstanceConfigOperations(&unsupported_filter_context,
                                                 list_request, &list_response)
                  .error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    list_request.set_filter("done:true");
    list_request.set_page_size(1);
    list_response.Clear();
    grpc::ClientContext filtered_page_context;
    ASSERT_TRUE(first.instance_admin_client()
                    ->ListInstanceConfigOperations(&filtered_page_context,
                                                   list_request, &list_response)
                    .ok());
    ASSERT_FALSE(list_response.next_page_token().empty());
    list_request.set_page_token(list_response.next_page_token());
    list_request.set_filter("done:false");
    grpc::ClientContext changed_filter_context;
    EXPECT_EQ(first.instance_admin_client()
                  ->ListInstanceConfigOperations(&changed_filter_context,
                                                 list_request, &list_response)
                  .error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    list_request.clear_filter();
    list_request.clear_page_token();

    ASSERT_TRUE(first.server()
                    ->env()
                    ->operation_manager()
                    ->CreateOperation(
                        project +
                            "/instances/i1/databases/instanceConfigs",
                        "create")
                    .ok());
    list_request.clear_page_token();
    list_request.set_page_size(100);
    list_response.Clear();
    grpc::ClientContext reserved_word_context;
    status = first.instance_admin_client()->ListInstanceConfigOperations(
        &reserved_word_context, list_request, &list_response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(list_response.operations_size(), 2);
  }

  {
    test::TestEnv restored;
    RestoreInstanceConfigsAndOperations(&restored);

    instance_api::GetInstanceConfigRequest get_config_request;
    get_config_request.set_name(config_name);
    instance_api::InstanceConfig config;
    grpc::ClientContext get_config_context;
    grpc::Status status = restored.instance_admin_client()->GetInstanceConfig(
        &get_config_context, get_config_request, &config);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(config.display_name(), "Custom Two");
    EXPECT_EQ(config.etag(), update_etag);
    EXPECT_EQ(config.labels().at("env"), "test");

    instance_api::ListInstanceConfigOperationsRequest restored_list_request;
    restored_list_request.set_parent(project);
    instance_api::ListInstanceConfigOperationsResponse restored_list_response;
    grpc::ClientContext restored_list_context;
    status = restored.instance_admin_client()->ListInstanceConfigOperations(
        &restored_list_context, restored_list_request, &restored_list_response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(restored_list_response.operations_size(), 2);
    EXPECT_EQ(restored_list_response.operations(0).name(),
              create_operation_name);
    EXPECT_EQ(restored_list_response.operations(1).name(),
              update_operation_name);
    instance_api::CreateInstanceConfigMetadata restored_create_metadata;
    instance_api::UpdateInstanceConfigMetadata restored_update_metadata;
    EXPECT_TRUE(restored_list_response.operations(0).metadata().UnpackTo(
        &restored_create_metadata));
    EXPECT_TRUE(restored_list_response.operations(1).metadata().UnpackTo(
        &restored_update_metadata));

    instance_api::ListInstanceConfigsRequest list_configs_request;
    list_configs_request.set_parent(project);
    instance_api::ListInstanceConfigsResponse list_configs_response;
    grpc::ClientContext list_configs_context;
    status = restored.instance_admin_client()->ListInstanceConfigs(
        &list_configs_context, list_configs_request, &list_configs_response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(list_configs_response.instance_configs_size(), 2);
    EXPECT_EQ(list_configs_response.instance_configs(0).name(), config_name);

    instance_api::CreateInstanceRequest create_instance;
    create_instance.set_parent(project);
    create_instance.set_instance_id("i1");
    create_instance.mutable_instance()->set_config("emulator-config");
    operations_api::Operation instance_operation;
    grpc::ClientContext create_instance_context;
    status = restored.instance_admin_client()->CreateInstance(
        &create_instance_context, create_instance, &instance_operation);
    ASSERT_TRUE(status.ok()) << status.error_message();

    instance_api::MoveInstanceRequest move_request;
    move_request.set_name(instance_name);
    move_request.set_target_config(config_name);
    operations_api::Operation move_operation;
    grpc::ClientContext move_context;
    status = restored.instance_admin_client()->MoveInstance(
        &move_context, move_request, &move_operation);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(move_operation.done());
    instance_api::MoveInstanceMetadata move_metadata;
    ASSERT_TRUE(move_operation.metadata().UnpackTo(&move_metadata));
    EXPECT_EQ(move_metadata.target_config(), config_name);
    EXPECT_EQ(move_metadata.progress().progress_percent(), 100);
    EXPECT_TRUE(move_metadata.progress().has_start_time());
    EXPECT_TRUE(move_metadata.progress().has_end_time());
    instance_api::Instance moved_instance;
    ASSERT_TRUE(move_operation.response().UnpackTo(&moved_instance));
    EXPECT_EQ(moved_instance.config(), config_name);

    instance_api::GetInstanceRequest get_instance_request;
    get_instance_request.set_name(instance_name);
    instance_api::Instance instance;
    grpc::ClientContext get_instance_context;
    status = restored.instance_admin_client()->GetInstance(
        &get_instance_context, get_instance_request, &instance);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(instance.config(), config_name);
    EXPECT_EQ(restored.server()
                  ->env()
                  ->metadata_store()
                  ->instances()
                  .at(instance_name)
                  .config,
              config_name);

    instance_api::DeleteInstanceConfigRequest delete_request;
    delete_request.set_name(config_name);
    protobuf_api::Empty empty;
    grpc::ClientContext in_use_delete_context;
    status = restored.instance_admin_client()->DeleteInstanceConfig(
        &in_use_delete_context, delete_request, &empty);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    move_request.set_target_config(
        project + "/instanceConfigs/emulator-config");
    BackupCatalog::BackupEntry backup;
    backup.backup.set_name(instance_name + "/backups/b1");
    backup.backup.set_state(database_api::Backup::CREATING);
    backup.source_instance_config = config_name;
    backup.operation_name = backup.backup.name() + "/operations/create";
    operations_api::Operation backup_operation;
    backup_operation.set_name(backup.operation_name);
    backup_operation.set_done(true);
    ASSERT_TRUE(std::filesystem::create_directories(
        std::filesystem::path(restored.server()
                                  ->env()
                                  ->backup_catalog()
                                  ->SnapshotDirectory(backup.backup.name()))
            .parent_path()));
    ASSERT_TRUE(restored.server()
                    ->env()
                    ->backup_catalog()
                    ->CreateBackup(backup, backup_operation)
                    .ok());
    grpc::ClientContext backed_up_context;
    status = restored.instance_admin_client()->MoveInstance(
        &backed_up_context, move_request, &move_operation);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    ASSERT_TRUE(restored.server()
                    ->env()
                    ->backup_catalog()
                    ->DeleteBackup(backup.backup.name())
                    .ok());

    grpc::ClientContext move_back_context;
    status = restored.instance_admin_client()->MoveInstance(
        &move_back_context, move_request, &move_operation);
    ASSERT_TRUE(status.ok()) << status.error_message();

    grpc::ClientContext delete_context;
    status = restored.instance_admin_client()->DeleteInstanceConfig(
        &delete_context, delete_request, &empty);
    ASSERT_TRUE(status.ok()) << status.error_message();

    grpc::ClientContext missing_context;
    status = restored.instance_admin_client()->GetInstanceConfig(
        &missing_context, get_config_request, &config);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_FALSE(restored.server()
                     ->env()
                     ->metadata_store()
                     ->instance_configs()
                     .contains(config_name));
  }
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
