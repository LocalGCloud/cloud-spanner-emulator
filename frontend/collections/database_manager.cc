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

#include "frontend/collections/database_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/flags/flag.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "backend/database/database.h"
#include "common/clock.h"
#include "common/errors.h"
#include "common/limits.h"
#include "frontend/common/uris.h"
#include "googlesql/base/status_macros.h"

ABSL_FLAG(int, override_max_databases_per_instance, 100,
          "overrides the allowed maximum number of databases per instance if "
          "the value set is greater than the default limit of Spanner. Not "
          "recommended for use unless there's a very specific need as "
          "overriding this value makes the emulator deviate from limits "
          "in production.");

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

std::vector<std::shared_ptr<Database>> GetDatabasesByInstance(
    const std::map<std::string, std::shared_ptr<Database>>& database_map,
    const std::string& instance_uri) {
  std::string database_uri_prefix = absl::StrCat(instance_uri, "/");
  std::vector<std::shared_ptr<Database>> databases;
  auto itr = database_map.upper_bound(database_uri_prefix);
  while (itr != database_map.end()) {
    if (!absl::StartsWith(itr->first, database_uri_prefix)) {
      break;
    }
    databases.push_back(itr->second);
    ++itr;
  }
  return databases;
}

absl::StatusOr<std::vector<std::filesystem::path>> ChildDirectories(
    const std::filesystem::path& parent) {
  std::error_code error;
  const std::filesystem::file_status parent_status =
      std::filesystem::symlink_status(parent, error);
  if (error == std::errc::no_such_file_or_directory) {
    return std::vector<std::filesystem::path>();
  }
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect persisted data directory ", parent.string(), ": ",
        error.message()));
  }
  if (!std::filesystem::exists(parent_status)) {
    return std::vector<std::filesystem::path>();
  }
  if (std::filesystem::is_symlink(parent_status)) {
    return absl::DataLossError(absl::StrCat(
        "Persisted resource hierarchy contains a symbolic link: ",
        parent.string()));
  }
  if (!std::filesystem::is_directory(parent_status)) {
    return absl::DataLossError(absl::StrCat(
        "Persisted data path is not a readable directory: ",
        parent.string()));
  }

  std::vector<std::filesystem::path> result;
  std::filesystem::directory_iterator entry(parent, error);
  const std::filesystem::directory_iterator end;
  while (!error && entry != end) {
    const bool is_symlink = entry->is_symlink(error);
    if (error) break;
    if (is_symlink) {
      return absl::DataLossError(absl::StrCat(
          "Persisted resource hierarchy contains a symbolic link: ",
          entry->path().string()));
    }
    if (entry->is_directory(error)) {
      result.push_back(entry->path());
    }
    if (error) break;
    entry.increment(error);
  }
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to enumerate persisted data directory ", parent.string(), ": ",
        error.message()));
  }
  return result;
}

absl::Status RejectSymlinkComponents(
    const std::filesystem::path& data_dir,
    const std::filesystem::path& relative_path) {
  std::filesystem::path current = data_dir;
  for (const auto& component : relative_path) {
    current /= component;
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect persistent path ", current.string(), ": ",
          error.message()));
    }
    if (!std::filesystem::exists(status)) continue;
    if (std::filesystem::is_symlink(status)) {
      return absl::DataLossError(absl::StrCat(
          "Persistent database path contains a symbolic link: ",
          current.string()));
    }
  }
  return absl::OkStatus();
}

absl::Status RejectDeletionMarkedRoot(const std::string& data_dir,
                                      const std::string& database_uri) {
  if (data_dir.empty()) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  const std::filesystem::path marker =
      std::filesystem::path(storage_directory).parent_path() /
      ".delete-in-progress";
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(marker, error);
  if (error == std::errc::no_such_file_or_directory) return absl::OkStatus();
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect database deletion marker ", marker.string(), ": ",
        error.message()));
  }
  if (!std::filesystem::exists(status)) return absl::OkStatus();
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return absl::DataLossError(
        absl::StrCat("Invalid database deletion marker ", marker.string()));
  }
  std::ifstream marker_input(marker, std::ios::binary);
  std::string marked_database((std::istreambuf_iterator<char>(marker_input)),
                              std::istreambuf_iterator<char>());
  if (!marker_input.is_open() || marker_input.bad() ||
      marked_database != database_uri) {
    return absl::DataLossError(
        absl::StrCat("Invalid database deletion marker ", marker.string()));
  }
  return absl::FailedPreconditionError(
      absl::StrCat("Database deletion is in progress: ", database_uri));
}

}  // namespace

absl::Status DatabaseManager::MigrateLegacyStorageDirectories(
    const std::string& data_dir,
    const std::vector<std::string>& database_uris) {
  std::map<std::string, std::vector<std::string>> resources_by_database_id;
  for (const std::string& database_uri : database_uris) {
    absl::string_view project_id;
    absl::string_view instance_id;
    absl::string_view database_id;
    absl::Status parse_status = ParseDatabaseUri(
        database_uri, &project_id, &instance_id, &database_id);
    if (!parse_status.ok() ||
        MakeDatabaseUri(MakeInstanceUri(project_id, instance_id),
                        database_id) != database_uri ||
        !ValidateDatabaseId(database_id).ok()) {
      return absl::DataLossError(
          absl::StrCat("Invalid persisted database resource: ", database_uri));
    }
    resources_by_database_id[std::string(database_id)].push_back(database_uri);
  }

  for (const auto& [database_id, resources] : resources_by_database_id) {
    const std::filesystem::path legacy_root =
        std::filesystem::path(data_dir) / database_id;
    const std::filesystem::path legacy_storage = legacy_root / "storage";
    GOOGLESQL_RETURN_IF_ERROR(RejectSymlinkComponents(
        data_dir, std::filesystem::path(database_id) / "storage"));
    std::error_code error;
    const bool legacy_exists =
        std::filesystem::exists(legacy_storage, error);
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect legacy database storage ",
          legacy_storage.string(), ": ", error.message()));
    }
    if (resources.size() != 1) {
      if (legacy_exists) {
        return absl::DataLossError(absl::StrCat(
            "Legacy database storage ", legacy_storage.string(),
            " is ambiguous for persisted resources: ",
            absl::StrJoin(resources, ", ")));
      }
      continue;
    }

    absl::string_view project_id;
    absl::string_view instance_id;
    absl::string_view parsed_database_id;
    GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
        resources.front(), &project_id, &instance_id, &parsed_database_id));
    GOOGLESQL_ASSIGN_OR_RETURN(
        const std::string scoped_storage_string,
        backend::Database::PersistentStorageDirectory(data_dir,
                                                      resources.front()));
    const std::filesystem::path scoped_storage = scoped_storage_string;
    const std::filesystem::path staged_storage =
        std::filesystem::path(data_dir) / ".database-migrations" /
        std::string(project_id) / std::string(instance_id) /
        std::string(parsed_database_id) / "storage";
    GOOGLESQL_RETURN_IF_ERROR(RejectSymlinkComponents(
        data_dir, std::filesystem::relative(scoped_storage, data_dir)));
    GOOGLESQL_RETURN_IF_ERROR(RejectSymlinkComponents(
        data_dir, std::filesystem::relative(staged_storage, data_dir)));

    const bool scoped_exists =
        std::filesystem::exists(scoped_storage, error);
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect scoped database storage ",
          scoped_storage.string(), " for resource ", resources.front(), ": ",
          error.message()));
    }
    const bool staged_exists =
        std::filesystem::exists(staged_storage, error);
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect staged database migration ",
          staged_storage.string(), " for resource ", resources.front(), ": ",
          error.message()));
    }
    if (scoped_exists) {
      if (legacy_exists || staged_exists) {
        return absl::DataLossError(absl::StrCat(
            "Scoped database storage ", scoped_storage.string(),
            " conflicts with ",
            legacy_exists
                ? absl::StrCat("legacy database storage ",
                               legacy_storage.string())
                : absl::StrCat("staged migration ", staged_storage.string()),
            " for resource ", resources.front()));
      }
      continue;
    }
    if (legacy_exists && staged_exists) {
      return absl::DataLossError(absl::StrCat(
          "Legacy database storage ", legacy_storage.string(),
          " conflicts with staged migration ", staged_storage.string(),
          " for resource ", resources.front()));
    }
    if (!legacy_exists && !staged_exists) {
      continue;
    }

    if (legacy_exists) {
      // Stage only the LevelDB payload, not the legacy root. A database named
      // "projects" would otherwise be moved into its own descendant, while
      // one named "backups" can share its root with the backup namespace.
      std::filesystem::create_directories(staged_storage.parent_path(), error);
      if (error) {
        return absl::DataLossError(absl::StrCat(
            "Failed to create database migration staging directory for ",
            resources.front(), ": ", error.message()));
      }
      std::filesystem::rename(legacy_storage, staged_storage, error);
      if (error) {
        return absl::DataLossError(absl::StrCat(
            "Failed to stage legacy database storage ",
            legacy_storage.string(), " for resource ", resources.front(), ": ",
            error.message()));
      }
      std::filesystem::remove(legacy_root, error);
      error.clear();
    }

    // A staged payload is a recoverable crash point: publishing the directory
    // is an atomic rename within data_dir.
    std::filesystem::create_directories(scoped_storage.parent_path(), error);
    if (!error) {
      std::filesystem::rename(staged_storage, scoped_storage, error);
    }
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to publish migrated database storage ",
          scoped_storage.string(), " for resource ", resources.front(), ": ",
          error.message()));
    }
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::MarkDatabaseMetadataCommitted(
    const std::string& data_dir, const std::string& database_uri) {
  if (data_dir.empty()) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  const std::filesystem::path database_root =
      std::filesystem::path(storage_directory).parent_path();
  const std::filesystem::path marker = database_root / ".metadata-committed";
  const std::filesystem::path temporary_marker =
      database_root / ".metadata-committed.tmp";
  std::error_code error;
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(database_root, error);
  if (error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status)) {
    // Distinguish "the on-disk root simply doesn't exist" -- the specific,
    // previously-confusing mismatch hit when a database's storage directory
    // was removed (by hand, or by a prior --repair_corrupted_databases run)
    // while metadata.json still references it -- from other filesystem
    // errors, and name the database explicitly. See openspec change
    // fix-unique-index-restore-isolation, design.md Decision 4: this used to
    // surface as a generic, unattributed DATA_LOSS that was indistinguishable
    // from other causes and gave operators no path forward.
    if (!std::filesystem::exists(root_status)) {
      return absl::DataLossError(absl::StrCat(
          "Database ", database_uri, " has no on-disk storage at ",
          database_root.string(),
          " but metadata.json still references it. If this database's data "
          "was removed by hand instead of via --repair_corrupted_databases, "
          "restart the emulator with --repair_corrupted_databases to remove "
          "its stale metadata entry; otherwise its data may be recoverable "
          "under the original --data_dir."));
    }
    return absl::DataLossError(absl::StrCat(
        "Persistent database root is unavailable for metadata commit for ",
        database_uri, " at ", database_root.string(),
        error ? absl::StrCat(": ", error.message()) : ""));
  }
  for (const std::filesystem::path& candidate : {marker, temporary_marker}) {
    error.clear();
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) {
      continue;
    }
    if (error || (!std::filesystem::is_regular_file(status) &&
                  status.type() != std::filesystem::file_type::not_found) ||
        std::filesystem::is_symlink(status)) {
      return absl::DataLossError(absl::StrCat(
          "Database metadata marker path is unsafe ", candidate.string(),
          error ? absl::StrCat(": ", error.message()) : ""));
    }
  }
  error.clear();
  if (std::filesystem::is_regular_file(
          std::filesystem::symlink_status(marker, error))) {
    std::ifstream marker_input(marker, std::ios::binary);
    std::string marked_database(
        (std::istreambuf_iterator<char>(marker_input)),
        std::istreambuf_iterator<char>());
    if (!marker_input.is_open() || marker_input.bad() ||
        marked_database != database_uri) {
      return absl::DataLossError(
          absl::StrCat("Invalid database metadata marker ", marker.string()));
    }
    std::filesystem::remove(temporary_marker, error);
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to remove stale database metadata marker ",
          temporary_marker.string(), ": ", error.message()));
    }
    return absl::OkStatus();
  }
  {
    std::ofstream output(temporary_marker, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to create database metadata marker ",
          temporary_marker.string()));
    }
    output << database_uri;
    output.flush();
    if (!output.good()) {
      std::error_code ignored;
      std::filesystem::remove(temporary_marker, ignored);
      return absl::DataLossError(absl::StrCat(
          "Failed to write database metadata marker ",
          temporary_marker.string()));
    }
  }
  std::filesystem::rename(temporary_marker, marker, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary_marker, ignored);
    return absl::DataLossError(absl::StrCat(
        "Failed to publish database metadata marker ", marker.string(), ": ",
        error.message()));
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::MarkDatabaseForDeletion(
    const std::string& data_dir, const std::string& database_uri) {
  if (data_dir.empty()) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  const std::filesystem::path database_root =
      std::filesystem::path(storage_directory).parent_path();
  const std::filesystem::path marker = database_root / ".delete-in-progress";
  std::error_code error;
  if (!std::filesystem::is_directory(database_root, error) || error) {
    return absl::DataLossError(absl::StrCat(
        "Persistent database root is unavailable for deletion ",
        database_root.string(),
        error ? absl::StrCat(": ", error.message()) : ""));
  }
  if (std::filesystem::exists(marker, error)) {
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect database deletion marker ", marker.string(),
          ": ", error.message()));
    }
    std::ifstream marker_input(marker, std::ios::binary);
    std::string marked_database(
        (std::istreambuf_iterator<char>(marker_input)),
        std::istreambuf_iterator<char>());
    if (!marker_input.is_open() || marker_input.bad() ||
        marked_database != database_uri) {
      return absl::DataLossError(absl::StrCat(
          "Invalid database deletion marker ", marker.string()));
    }
    return absl::OkStatus();
  }
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect database deletion marker ", marker.string(), ": ",
        error.message()));
  }

  const std::filesystem::path temporary_marker =
      database_root / ".delete-in-progress.tmp";
  {
    std::ofstream output(temporary_marker, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to create database deletion marker ",
          temporary_marker.string()));
    }
    output << database_uri;
    output.flush();
    if (!output.good()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to write database deletion marker ",
          temporary_marker.string()));
    }
  }
  std::filesystem::rename(temporary_marker, marker, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary_marker, ignored);
    return absl::DataLossError(absl::StrCat(
        "Failed to publish database deletion marker ", marker.string(), ": ",
        error.message()));
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::CancelDatabaseDeletion(
    const std::string& data_dir, const std::string& database_uri) {
  if (data_dir.empty()) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  const std::filesystem::path marker =
      std::filesystem::path(storage_directory).parent_path() /
      ".delete-in-progress";
  std::error_code error;
  std::filesystem::remove(marker, error);
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to cancel database deletion marker ", marker.string(), ": ",
        error.message()));
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::ReconcileDeletedDatabaseDirectories(
    const std::string& data_dir,
    const std::vector<std::string>& database_uris) {
  const std::set<std::string> persisted_databases(database_uris.begin(),
                                                   database_uris.end());
  GOOGLESQL_ASSIGN_OR_RETURN(
      const auto projects,
      ChildDirectories(std::filesystem::path(data_dir) / "projects"));
  for (const std::filesystem::path& project : projects) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        const auto instances, ChildDirectories(project / "instances"));
    for (const std::filesystem::path& instance : instances) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          const auto databases, ChildDirectories(instance / "databases"));
      for (const std::filesystem::path& database_root : databases) {
        const std::filesystem::path marker =
            database_root / ".delete-in-progress";
        std::error_code error;
        const bool has_marker = std::filesystem::exists(marker, error);
        if (error) {
          return absl::DataLossError(absl::StrCat(
              "Failed to inspect database deletion marker ", marker.string(),
              ": ", error.message()));
        }
        if (!has_marker) continue;
        const std::string database_uri = MakeDatabaseUri(
            MakeInstanceUri(project.filename().string(),
                            instance.filename().string()),
            database_root.filename().string());
        std::ifstream marker_input(marker, std::ios::binary);
        std::string marked_database(
            (std::istreambuf_iterator<char>(marker_input)),
            std::istreambuf_iterator<char>());
        if (!marker_input.is_open() || marker_input.bad() ||
            marked_database != database_uri) {
          return absl::DataLossError(absl::StrCat(
              "Invalid database deletion marker ", marker.string()));
        }
        if (persisted_databases.contains(database_uri)) {
          if (!std::filesystem::remove(marker, error) || error) {
            return absl::DataLossError(absl::StrCat(
                "Failed to clear rolled-back database deletion marker ",
                marker.string(), ": ",
                error ? error.message() : "marker was not removed"));
          }
        } else {
          std::filesystem::remove_all(database_root, error);
          if (error) {
            return absl::DataLossError(absl::StrCat(
                "Failed to finish database deletion ",
                database_root.string(), ": ", error.message()));
          }
        }
      }
    }
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::CleanupOrphanedRestoreDirectories(
    const std::string& data_dir,
    const std::vector<std::string>& database_uris) {
  const std::set<std::string> persisted_databases(database_uris.begin(),
                                                   database_uris.end());

  GOOGLESQL_ASSIGN_OR_RETURN(
      const auto projects,
      ChildDirectories(std::filesystem::path(data_dir) / "projects"));
  for (const std::filesystem::path& project : projects) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        const auto instances, ChildDirectories(project / "instances"));
    for (const std::filesystem::path& instance : instances) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          const auto databases, ChildDirectories(instance / "databases"));
      for (const std::filesystem::path& database_root : databases) {
        const std::string database_uri = MakeDatabaseUri(
            MakeInstanceUri(project.filename().string(),
                            instance.filename().string()),
            database_root.filename().string());
        const std::filesystem::path restore_marker =
            database_root / ".restore-in-progress";
        const std::filesystem::path metadata_marker =
            database_root / ".metadata-committed";

        auto validate_marker =
            [&](const std::filesystem::path& marker)
            -> absl::StatusOr<bool> {
          std::error_code error;
          const std::filesystem::file_status status =
              std::filesystem::symlink_status(marker, error);
          if (error == std::errc::no_such_file_or_directory) {
            return false;
          }
          if (error) {
            return absl::DataLossError(absl::StrCat(
                "Failed to inspect database marker ", marker.string(), ": ",
                error.message()));
          }
          if (!std::filesystem::is_regular_file(status) ||
              std::filesystem::is_symlink(status)) {
            return absl::DataLossError(absl::StrCat(
                "Database marker is not a regular file ", marker.string()));
          }
          std::ifstream marker_input(marker, std::ios::binary);
          std::string marked_database(
              (std::istreambuf_iterator<char>(marker_input)),
              std::istreambuf_iterator<char>());
          if (!marker_input.is_open() || marker_input.bad() ||
              marked_database != database_uri) {
            return absl::DataLossError(
                absl::StrCat("Invalid database marker ", marker.string()));
          }
          return true;
        };

        if (persisted_databases.contains(database_uri)) {
          GOOGLESQL_ASSIGN_OR_RETURN(const bool has_restore_marker,
                                     validate_marker(restore_marker));
          GOOGLESQL_ASSIGN_OR_RETURN(const bool has_metadata_marker,
                                     validate_marker(metadata_marker));
          (void)has_restore_marker;
          (void)has_metadata_marker;
          continue;
        }
        GOOGLESQL_ASSIGN_OR_RETURN(const bool has_metadata_marker,
                                   validate_marker(metadata_marker));
        if (has_metadata_marker) {
          return absl::DataLossError(absl::StrCat(
              "Persistent database root has committed data but no metadata: ",
              database_uri));
        }

        // A root without committed metadata is an interrupted create/restore.
        // Removing the root does not follow child symbolic links.
        std::error_code error;
        std::filesystem::remove_all(database_root, error);
        if (error) {
          return absl::DataLossError(absl::StrCat(
              "Failed to remove incomplete database directory ",
              database_root.string(), ": ", error.message()));
        }
      }
    }
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::CompleteRecoveredRestoreDirectories(
    const std::string& data_dir,
    const std::vector<std::string>& database_uris) {
  for (const std::string& database_uri : database_uris) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        const std::string storage_directory,
        backend::Database::PersistentStorageDirectory(data_dir, database_uri));
    const std::filesystem::path database_root =
        std::filesystem::path(storage_directory).parent_path();
    const std::filesystem::path marker =
        database_root / ".restore-in-progress";
    std::error_code error;
    const bool has_marker = std::filesystem::exists(marker, error);
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect restore marker ", marker.string(), ": ",
          error.message()));
    }
    if (!has_marker) continue;

    std::ifstream marker_input(marker, std::ios::binary);
    if (!marker_input.is_open()) {
      return absl::DataLossError(absl::StrCat(
          "Failed to read restore marker for persisted database ",
          database_uri));
    }
    std::string marked_database(
        (std::istreambuf_iterator<char>(marker_input)),
        std::istreambuf_iterator<char>());
    if (marker_input.bad() || marked_database != database_uri) {
      return absl::DataLossError(absl::StrCat(
          "Invalid restore marker for persisted database ", database_uri));
    }
    if (!std::filesystem::remove(marker, error) || error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to clear restore marker ", marker.string(), ": ",
          error ? error.message() : "marker was not removed"));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string>
DatabaseManager::DdlRollbackCheckpointDirectory(
    const std::string& data_dir, const std::string& database_uri,
    const std::string& operation_name) {
  std::string operation_resource;
  absl::string_view operation_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseOperationUri(operation_name, &operation_resource, &operation_id));
  if (operation_resource != database_uri || operation_id.empty() ||
      operation_id == "." || operation_id == ".." ||
      operation_id.find('/') != absl::string_view::npos) {
    return absl::DataLossError(
        absl::StrCat("Invalid durable DDL operation name: ", operation_name));
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  return (std::filesystem::path(storage_directory).parent_path() /
          ".ddl-rollback" / std::string(operation_id) / "storage")
      .string();
}

absl::Status DatabaseManager::RestoreDdlRollbackCheckpoint(
    const std::string& data_dir, const std::string& database_uri,
    const std::string& operation_name) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string checkpoint_directory,
      DdlRollbackCheckpointDirectory(data_dir, database_uri, operation_name));
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory_string,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  const std::filesystem::path checkpoint_directory_path =
      checkpoint_directory;
  const std::filesystem::path storage_directory = storage_directory_string;
  const std::filesystem::path staging_directory =
      storage_directory.parent_path() / ".ddl-restore-staging";
  const std::filesystem::path mutated_directory =
      storage_directory.parent_path() / ".ddl-mutated-storage";

  std::error_code error;
  const std::filesystem::file_status checkpoint_status =
      std::filesystem::symlink_status(checkpoint_directory_path, error);
  if (error || !std::filesystem::is_directory(checkpoint_status) ||
      std::filesystem::is_symlink(checkpoint_status)) {
    return absl::DataLossError(absl::StrCat(
        "Missing or invalid durable DDL rollback checkpoint for ",
        database_uri, error ? absl::StrCat(": ", error.message()) : ""));
  }
  for (std::filesystem::recursive_directory_iterator iterator(
           checkpoint_directory_path, error),
       end;
       iterator != end && !error; iterator.increment(error)) {
    if (iterator->is_symlink(error)) {
      return absl::DataLossError(absl::StrCat(
          "Durable DDL rollback checkpoint contains a symbolic link: ",
          iterator->path().string()));
    }
  }
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect durable DDL rollback checkpoint for ",
        database_uri, ": ", error.message()));
  }

  const bool mutated_exists =
      std::filesystem::exists(mutated_directory, error);
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to inspect interrupted DDL rollback for ", database_uri, ": ",
        error.message()));
  }
  if (mutated_exists) {
    const bool storage_exists =
        std::filesystem::exists(storage_directory, error);
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to inspect persistent storage for ", database_uri, ": ",
          error.message()));
    }
    if (storage_exists) {
      std::filesystem::remove_all(mutated_directory, error);
    } else {
      std::filesystem::rename(mutated_directory, storage_directory, error);
    }
    if (error) {
      return absl::DataLossError(absl::StrCat(
          "Failed to recover interrupted DDL rollback for ", database_uri,
          ": ", error.message()));
    }
  }
  std::filesystem::remove_all(staging_directory, error);
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to clean DDL rollback staging directory for ", database_uri,
        ": ", error.message()));
  }
  std::filesystem::copy(checkpoint_directory_path, staging_directory,
                        std::filesystem::copy_options::recursive, error);
  if (error) {
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging_directory, cleanup_error);
    return absl::DataLossError(absl::StrCat(
        "Failed to stage DDL rollback for ", database_uri, ": ",
        error.message()));
  }
  std::filesystem::rename(storage_directory, mutated_directory, error);
  if (error) {
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging_directory, cleanup_error);
    return absl::DataLossError(absl::StrCat(
        "Failed to preserve mutated storage during DDL rollback for ",
        database_uri, ": ", error.message()));
  }
  std::filesystem::rename(staging_directory, storage_directory, error);
  if (error) {
    std::error_code restore_error;
    std::filesystem::rename(mutated_directory, storage_directory,
                            restore_error);
    return absl::DataLossError(absl::StrCat(
        "Failed to publish DDL rollback for ", database_uri, ": ",
        error.message(),
        restore_error ? absl::StrCat("; failed to restore original storage: ",
                                     restore_error.message())
                      : ""));
  }
  std::filesystem::remove_all(mutated_directory, error);
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "DDL rollback completed but failed to remove mutated storage for ",
        database_uri, ": ", error.message()));
  }
  return absl::OkStatus();
}

absl::Status DatabaseManager::RemoveDdlRollbackCheckpoints(
    const std::string& data_dir, const std::string& database_uri) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(data_dir, database_uri));
  const std::filesystem::path rollback_root =
      std::filesystem::path(storage_directory).parent_path() / ".ddl-rollback";
  std::error_code error;
  std::filesystem::remove_all(rollback_root, error);
  if (error) {
    return absl::DataLossError(absl::StrCat(
        "Failed to remove durable DDL rollback checkpoints for ",
        database_uri, ": ", error.message()));
  }
  return absl::OkStatus();
}

DatabaseManager::Creation::~Creation() {
  if (published_ || manager_ == nullptr) {
    return;
  }
  absl::MutexLock lock(manager_->mu_);
  if (manager_->database_reservations_.erase(database_uri_) > 0) {
    auto count =
        manager_->num_database_reservations_per_instance_.find(instance_uri_);
    if (count !=
        manager_->num_database_reservations_per_instance_.end()) {
      if (--count->second == 0) {
        manager_->num_database_reservations_per_instance_.erase(count);
      }
    }
  }
}

absl::StatusOr<std::shared_ptr<Database>>
DatabaseManager::Creation::Build(
    const backend::SchemaChangeOperation& schema_change_operation,
    const backend::Database::IdCounterValues& id_counters) {
  return Build(schema_change_operation, id_counters, manager_->clock_->Now());
}

absl::StatusOr<std::shared_ptr<Database>>
DatabaseManager::Creation::Build(
    const backend::SchemaChangeOperation& schema_change_operation,
    const backend::Database::IdCounterValues& id_counters,
    absl::Time create_time) {
  if (database_ != nullptr) {
    return absl::FailedPreconditionError(
        "Reserved database has already been built");
  }
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
      database_uri_, &project_id, &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(
      RejectDeletionMarkedRoot(manager_->data_dir_, database_uri_));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<backend::Database> backend_database,
      backend::Database::Create(manager_->clock_, database_id,
                                schema_change_operation, id_counters,
                                database_uri_));
  database_ = std::make_shared<Database>(
      database_uri_, std::move(backend_database), create_time);
  return database_;
}

absl::StatusOr<std::shared_ptr<Database>>
DatabaseManager::Creation::Build(
    const std::vector<backend::SchemaChangeOperation>&
        schema_change_operations,
    const backend::Database::IdCounterValues& id_counters,
    absl::Time create_time) {
  if (database_ != nullptr) {
    return absl::FailedPreconditionError(
        "Reserved database has already been built");
  }
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
      database_uri_, &project_id, &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(
      RejectDeletionMarkedRoot(manager_->data_dir_, database_uri_));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<backend::Database> backend_database,
      backend::Database::Create(manager_->clock_, database_id,
                                schema_change_operations, id_counters,
                                database_uri_));
  database_ = std::make_shared<Database>(
      database_uri_, std::move(backend_database), create_time);
  return database_;
}

absl::Status DatabaseManager::Creation::Publish() {
  if (database_ == nullptr) {
    return absl::FailedPreconditionError(
        "Reserved database must be built before publication");
  }
  absl::MutexLock lock(manager_->mu_);
  if (!manager_->database_reservations_.contains(database_uri_)) {
    return absl::InternalError("Database creation reservation was lost");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      RejectDeletionMarkedRoot(manager_->data_dir_, database_uri_));
  if (manager_->database_map_.contains(database_uri_)) {
    return error::DatabaseAlreadyExists(database_uri_);
  }
  manager_->database_map_.emplace(database_uri_, database_);
  manager_->num_databases_per_instance_[instance_uri_] += 1;
  manager_->database_reservations_.erase(database_uri_);
  auto count =
      manager_->num_database_reservations_per_instance_.find(instance_uri_);
  if (count != manager_->num_database_reservations_per_instance_.end() &&
      --count->second == 0) {
    manager_->num_database_reservations_per_instance_.erase(count);
  }
  published_ = true;
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<DatabaseManager::Creation>>
DatabaseManager::ReserveDatabase(const std::string& database_uri) {
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseDatabaseUri(database_uri, &project_id, &instance_id, &database_id));
  GOOGLESQL_RETURN_IF_ERROR(
      RejectDeletionMarkedRoot(data_dir_, database_uri));
  const std::string instance_uri = MakeInstanceUri(project_id, instance_id);

  absl::MutexLock lock(mu_);
  if (database_map_.contains(database_uri) ||
      database_reservations_.contains(database_uri)) {
    return error::DatabaseAlreadyExists(database_uri);
  }
  const int max_databases =
      std::max(limits::kMaxDatabasesPerInstance,
               absl::GetFlag(FLAGS_override_max_databases_per_instance));
  if (num_databases_per_instance_[instance_uri] +
          num_database_reservations_per_instance_[instance_uri] >=
      max_databases) {
    return error::TooManyDatabasesPerInstance(instance_uri);
  }
  database_reservations_.emplace(database_uri, instance_uri);
  num_database_reservations_per_instance_[instance_uri] += 1;
  return std::unique_ptr<Creation>(
      new Creation(this, database_uri, instance_uri));
}

absl::StatusOr<std::shared_ptr<Database>> DatabaseManager::CreateDatabase(
    const std::string& database_uri,
    const backend::SchemaChangeOperation& schema_change_operation) {
  return CreateDatabase(database_uri, schema_change_operation,
                        backend::Database::IdCounterValues{});
}

absl::StatusOr<std::shared_ptr<Database>> DatabaseManager::CreateDatabase(
    const std::string& database_uri,
    const backend::SchemaChangeOperation& schema_change_operation,
    const backend::Database::IdCounterValues& id_counters) {
  return CreateDatabase(database_uri, schema_change_operation, id_counters,
                        clock_->Now());
}

absl::StatusOr<std::shared_ptr<Database>> DatabaseManager::CreateDatabase(
    const std::string& database_uri,
    const backend::SchemaChangeOperation& schema_change_operation,
    const backend::Database::IdCounterValues& id_counters,
    absl::Time create_time) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<Creation> creation,
                             ReserveDatabase(database_uri));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      creation->Build(schema_change_operation, id_counters, create_time));
  GOOGLESQL_RETURN_IF_ERROR(creation->Publish());
  return database;
}

absl::StatusOr<std::shared_ptr<Database>> DatabaseManager::GetDatabase(
    const std::string& database_uri) const {
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      GetDatabaseIncludingRecoveryRequired(database_uri));
  if (database->backend()->restore_required()) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Database recovery is required before serving ", database_uri));
  }
  return database;
}

absl::StatusOr<std::shared_ptr<Database>>
DatabaseManager::GetDatabaseIncludingRecoveryRequired(
    const std::string& database_uri) const {
  absl::ReaderMutexLock lock(mu_);
  auto itr = database_map_.find(database_uri);
  if (itr == database_map_.end()) {
    return error::DatabaseNotFound(database_uri);
  }
  return itr->second;
}

absl::Status DatabaseManager::DeleteDatabase(const std::string& database_uri) {
  absl::MutexLock lock(mu_);
  if (database_reservations_.contains(database_uri)) {
    return absl::FailedPreconditionError(
        absl::StrCat("Database creation is in progress: ", database_uri));
  }
  if (database_map_.erase(database_uri) > 0) {
    absl::string_view project_id;
    absl::string_view instance_id;
    absl::string_view database_id;
    GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
        database_uri, &project_id, &instance_id, &database_id));
    std::string instance_uri = MakeInstanceUri(project_id, instance_id);
    num_databases_per_instance_[instance_uri] -= 1;
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::shared_ptr<Database>>>
DatabaseManager::ListDatabases(const std::string& instance_uri) const {
  absl::ReaderMutexLock lock(mu_);
  return GetDatabasesByInstance(database_map_, instance_uri);
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
