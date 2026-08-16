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
}

TEST(InstanceExtensionsTest, FailedPersistenceRollsBackAdminUpdates) {
  PersistentInstanceConfigDirectory data_dir;
  test::TestEnv env;
  const std::string project = "projects/p";
  const std::string instance_name = project + "/instances/i1";
  const std::string custom_config = project + "/instanceConfigs/custom";
  const std::string built_in_config =
      project + "/instanceConfigs/emulator-config";
  const std::string database_name = instance_name + "/databases/d1";

  instance_api::CreateInstanceConfigRequest config_request;
  config_request.set_parent(project);
  config_request.set_instance_config_id("custom");
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
  const std::string config_name = project + "/instanceConfigs/custom";
  const std::string instance_name = project + "/instances/i1";
  std::string create_operation_name;
  std::string update_operation_name;

  {
    test::TestEnv first;
    instance_api::CreateInstanceConfigRequest create_request;
    create_request.set_parent(project);
    create_request.set_instance_config_id("custom");
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

    update_operation_name = update_operation.name();
    instance_api::GetInstanceConfigRequest get_request;
    get_request.set_name(config_name);
    instance_api::InstanceConfig config;
    grpc::ClientContext get_context;
    status = first.instance_admin_client()->GetInstanceConfig(
        &get_context, get_request, &config);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(config.display_name(), "Custom Two");
    EXPECT_EQ(config.labels().at("env"), "test");

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

    instance_api::ListInstanceConfigsRequest list_configs_request;
    list_configs_request.set_parent(project);
    instance_api::ListInstanceConfigsResponse list_configs_response;
    grpc::ClientContext list_configs_context;
    status = restored.instance_admin_client()->ListInstanceConfigs(
        &list_configs_context, list_configs_request, &list_configs_response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(list_configs_response.instance_configs_size(), 2);
    EXPECT_EQ(list_configs_response.instance_configs(1).name(), config_name);

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
