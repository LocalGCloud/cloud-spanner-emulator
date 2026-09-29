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

#include "frontend/handlers/backups.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>
#include <utility>
#include "absl/log/absl_log.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "absl/time/civil_time.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "backend/database/database.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/parser/ddl_reserved_words.h"
#include "backend/schema/printer/print_ddl.h"
#include "common/clock.h"
#include "common/config.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/operation_manager.h"
#include "frontend/common/list_filter.h"
#include "frontend/common/uris.h"
#include "frontend/converters/time.h"
#include "frontend/entities/database.h"
#include "frontend/entities/instance.h"
#include "frontend/persistence/backup_catalog.h"
#include "frontend/persistence/metadata_store.h"
#include "frontend/server/handler.h"
#include "google/longrunning/operations.pb.h"
#include "google/iam/v1/policy.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/backup.pb.h"
#include "google/spanner/admin/database/v1/backup_schedule.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "googlesql/base/status_macros.h"

namespace database_api = ::google::spanner::admin::database::v1;
namespace iam_api = ::google::iam::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

absl::Status DeleteBackup(RequestContext* ctx,
                          const database_api::DeleteBackupRequest* request,
                          protobuf_api::Empty* response);

namespace {

constexpr int32_t kMaximumPageSize = 1000;
constexpr absl::Duration kMinimumBackupRetention = absl::Hours(6);
constexpr absl::Duration kMaximumBackupRetention = absl::Hours(24 * 366);
constexpr int kMaximumBackupSchedulesPerDatabase = 4;
// An incremental chain holds a full backup and up to 13 incremental backups,
// and starts over once its full backup is 28 days old.
constexpr int64_t kMaximumIncrementalBackupsPerChain = 13;
constexpr absl::Duration kMaximumBackupChainAge = absl::Hours(24 * 28);

absl::Status ValidateResourceId(const std::string& id,
                                const std::string& description) {
  if (id.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat(description, " must not be empty"));
  }
  if (id.size() > 100) {
    return absl::InvalidArgumentError(
        absl::StrCat(description, " is too long"));
  }
  for (unsigned char ch : id) {
    if (!(std::isalnum(ch) || ch == '-' || ch == '_')) {
      return absl::InvalidArgumentError(
          absl::StrCat(description, " contains an invalid character: ", id));
    }
  }
  return absl::OkStatus();
}

absl::Status ValidateInstance(const std::string& parent, ServerEnv* env) {
  absl::string_view project_id;
  absl::string_view instance_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseInstanceUri(parent, &project_id, &instance_id));
  if (MakeInstanceUri(project_id, instance_id) != parent) {
    return absl::InvalidArgumentError("Instance name must be canonical");
  }
  return env->instance_manager()->GetInstance(parent).status();
}

absl::Status ValidateDatabase(const std::string& name, ServerEnv* env) {
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(
      ParseDatabaseUri(name, &project_id, &instance_id, &database_id));
  if (MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
      name) {
    return absl::InvalidArgumentError("Database name must be canonical");
  }
  return env->database_manager()->GetDatabase(name).status();
}

std::string MakeBackupName(const std::string& parent,
                           const std::string& backup_id) {
  return absl::StrCat(parent, "/backups/", backup_id);
}

std::string MakeBackupScheduleName(const std::string& parent,
                                   const std::string& schedule_id) {
  return absl::StrCat(parent, "/backupSchedules/", schedule_id);
}

absl::Status ValidateBackupName(const std::string& name) {
  const size_t marker = name.rfind("/backups/");
  if (marker == std::string::npos || marker == 0 ||
      marker + std::string("/backups/").size() >= name.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup resource name: ", name));
  }
  const std::string parent = name.substr(0, marker);
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::Status parse_status =
      ParseInstanceUri(parent, &project_id, &instance_id);
  if (!parse_status.ok() ||
      MakeInstanceUri(project_id, instance_id) != parent) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup resource name: ", name));
  }
  return ValidateResourceId(name.substr(marker + 9), "Backup ID");
}

absl::Status ValidateBackupScheduleName(const std::string& name) {
  const size_t marker = name.rfind("/backupSchedules/");
  if (marker == std::string::npos || marker == 0 ||
      marker + std::string("/backupSchedules/").size() >= name.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup schedule resource name: ", name));
  }
  const std::string parent = name.substr(0, marker);
  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  absl::Status parse_status =
      ParseDatabaseUri(parent, &project_id, &instance_id, &database_id);
  if (!parse_status.ok() ||
      MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
          parent) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid backup schedule resource name: ", name));
  }
  const std::string id = name.substr(marker + 17);
  if (id.size() < 2 || id.size() > 60 || id.front() < 'a' ||
      id.front() > 'z' || !std::isalnum(static_cast<unsigned char>(id.back()))) {
    return absl::InvalidArgumentError("Invalid backup schedule ID");
  }
  for (unsigned char ch : id) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '-' || ch == '_')) {
      return absl::InvalidArgumentError("Invalid backup schedule ID");
    }
  }
  return absl::OkStatus();
}

// Validates a CreateBackup, backup schedule, CopyBackup, or RestoreDatabase
// encryption_config. The emulator encrypts nothing, so it reports Google
// default encryption for every type it accepts, and it rejects customer-managed
// encryption because snapshots are stored unencrypted.
template <typename EncryptionConfig>
absl::Status ValidateEncryptionConfig(const EncryptionConfig& config) {
  const bool has_kms_key =
      !config.kms_key_name().empty() || !config.kms_key_names().empty();
  if (config.encryption_type() ==
          EncryptionConfig::ENCRYPTION_TYPE_UNSPECIFIED ||
      !EncryptionConfig::EncryptionType_IsValid(config.encryption_type())) {
    return absl::InvalidArgumentError(
        "encryption_config.encryption_type must be specified");
  }
  if (config.encryption_type() ==
      EncryptionConfig::CUSTOMER_MANAGED_ENCRYPTION) {
    if (!has_kms_key) {
      return absl::InvalidArgumentError(
          "CUSTOMER_MANAGED_ENCRYPTION requires kms_key_name or "
          "kms_key_names");
    }
    return absl::UnimplementedError(
        "CUSTOMER_MANAGED_ENCRYPTION is not supported by the emulator, which "
        "stores backups and databases unencrypted");
  }
  if (has_kms_key) {
    return absl::InvalidArgumentError(
        "kms_key_name and kms_key_names may be set only with "
        "CUSTOMER_MANAGED_ENCRYPTION");
  }
  return absl::OkStatus();
}

database_api::EncryptionInfo GoogleDefaultEncryption() {
  database_api::EncryptionInfo info;
  info.set_encryption_type(
      database_api::EncryptionInfo::GOOGLE_DEFAULT_ENCRYPTION);
  return info;
}

absl::StatusOr<int> ParseCronNumber(std::string_view text, int minimum,
                                    int maximum) {
  int value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value);
  if (text.empty() || parsed.ec != std::errc() ||
      parsed.ptr != text.data() + text.size() || value < minimum ||
      value > maximum) {
    return absl::InvalidArgumentError("Invalid backup schedule cron field");
  }
  return value;
}

template <size_t N>
absl::StatusOr<std::array<bool, N>> ParseCronField(
    std::string_view field, int minimum, int maximum) {
  std::array<bool, N> allowed{};
  for (size_t offset = 0; offset < field.size();) {
    const size_t comma = field.find(',', offset);
    const std::string_view part = field.substr(
        offset, comma == std::string_view::npos ? comma : comma - offset);
    const size_t slash = part.find('/');
    const std::string_view base = part.substr(0, slash);
    int step = 1;
    if (slash != std::string_view::npos) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          step, ParseCronNumber(part.substr(slash + 1), 1, maximum - minimum + 1));
    }
    int first = minimum;
    int last = maximum;
    if (base != "*") {
      const size_t dash = base.find('-');
      GOOGLESQL_ASSIGN_OR_RETURN(first,
                                 ParseCronNumber(base.substr(0, dash), minimum,
                                                 maximum));
      if (dash != std::string_view::npos) {
        GOOGLESQL_ASSIGN_OR_RETURN(last,
                                   ParseCronNumber(base.substr(dash + 1),
                                                   minimum, maximum));
      } else if (slash == std::string_view::npos) {
        last = first;
      }
    }
    if (first > last) {
      return absl::InvalidArgumentError("Invalid backup schedule cron range");
    }
    for (int value = first; value <= last; value += step) allowed[value] = true;
    if (comma == std::string_view::npos) break;
    offset = comma + 1;
    if (offset == field.size()) {
      return absl::InvalidArgumentError("Invalid backup schedule cron list");
    }
  }
  if (std::none_of(allowed.begin(), allowed.end(), [](bool value) {
        return value;
      })) {
    return absl::InvalidArgumentError("Empty backup schedule cron field");
  }
  return allowed;
}

struct ParsedCron {
  int minute = 0;
  std::array<bool, 24> hours{};
  std::array<bool, 32> days{};
  std::array<bool, 13> months{};
  std::array<bool, 8> weekdays{};
  bool all_days = false;
  bool all_weekdays = false;
};

// Parses a schedule's cron text. Full backups must be at least 12 hours
// apart and incremental backups at least 4 hours apart.
absl::StatusOr<ParsedCron> ParseBackupCron(
    const database_api::BackupSchedule& schedule) {
  const std::string& text = schedule.spec().cron_spec().text();
  const bool incremental = schedule.has_incremental_backup_spec();
  const int minimum_hours_apart = incremental ? 4 : 12;
  const absl::Status too_frequent = absl::InvalidArgumentError(absl::StrCat(
      incremental ? "Incremental" : "Full",
      " backup schedules must be at least ", minimum_hours_apart,
      " hours apart"));
  if (text.empty() || text.size() > 128) {
    return absl::InvalidArgumentError("Invalid backup schedule cron text");
  }
  std::istringstream input{std::string(text)};
  std::array<std::string, 5> fields;
  std::string extra;
  for (std::string& field : fields) {
    if (!(input >> field)) {
      return absl::InvalidArgumentError(
          "Backup schedule cron must have five fields");
    }
  }
  if (input >> extra) {
    return absl::InvalidArgumentError(
        "Backup schedule cron must have five fields");
  }
  ParsedCron cron;
  GOOGLESQL_ASSIGN_OR_RETURN(auto minutes,
                             ParseCronField<60>(fields[0], 0, 59));
  if (std::count(minutes.begin(), minutes.end(), true) != 1) {
    return too_frequent;
  }
  cron.minute = std::find(minutes.begin(), minutes.end(), true) -
                minutes.begin();
  GOOGLESQL_ASSIGN_OR_RETURN(cron.hours,
                             ParseCronField<24>(fields[1], 0, 23));
  GOOGLESQL_ASSIGN_OR_RETURN(cron.days,
                             ParseCronField<32>(fields[2], 1, 31));
  GOOGLESQL_ASSIGN_OR_RETURN(cron.months,
                             ParseCronField<13>(fields[3], 1, 12));
  GOOGLESQL_ASSIGN_OR_RETURN(cron.weekdays,
                             ParseCronField<8>(fields[4], 0, 7));
  std::vector<int> hours;
  for (int hour = 0; hour < 24; ++hour) {
    if (cron.hours[hour]) hours.push_back(hour);
  }
  for (size_t index = 0; hours.size() > 1 && index < hours.size(); ++index) {
    const int next = hours[(index + 1) % hours.size()];
    if ((next - hours[index] + 24) % 24 < minimum_hours_apart) {
      return too_frequent;
    }
  }
  cron.all_days = std::all_of(cron.days.begin() + 1, cron.days.end(),
                              [](bool value) { return value; });
  const bool sunday = cron.weekdays[0] || cron.weekdays[7];
  cron.all_weekdays = sunday && std::all_of(
      cron.weekdays.begin() + 1, cron.weekdays.begin() + 7,
      [](bool value) { return value; });
  const bool all_months = std::all_of(
      cron.months.begin() + 1, cron.months.end(),
      [](bool value) { return value; });
  const int selected_days =
      std::count(cron.days.begin() + 1, cron.days.end(), true);
  const int selected_weekdays =
      (sunday ? 1 : 0) + std::count(cron.weekdays.begin() + 1,
                                  cron.weekdays.begin() + 7, true);
  const bool daily = cron.all_days && cron.all_weekdays;
  const bool weekly = cron.all_days && selected_weekdays == 1;
  const bool monthly = selected_days == 1 && cron.all_weekdays;
  if (!all_months || (hours.size() > 1 ? !daily
                                       : !(daily || weekly || monthly))) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Backup schedule cron must be every ", minimum_hours_apart,
        " or more hours, daily, weekly, or monthly"));
  }
  return cron;
}

absl::StatusOr<absl::Time> NextBackupDue(const ParsedCron& cron,
                                          absl::Time after) {
  const absl::TimeZone utc = absl::UTCTimeZone();
  const absl::CivilSecond civil = absl::ToCivilSecond(after, utc);
  absl::CivilDay day(civil.year(), civil.month(), civil.day());
  for (int offset = 0; offset < 366 * 5; ++offset, day += 1) {
    if (!cron.months[day.month()]) continue;
    const std::string weekday_text =
        absl::FormatTime("%w", absl::FromCivil(day, utc), utc);
    const int weekday = weekday_text.front() - '0';
    const bool date_match =
        cron.all_days && cron.all_weekdays
            ? true
            : cron.all_days
                  ? cron.weekdays[weekday] ||
                        (weekday == 0 && cron.weekdays[7])
                  : cron.all_weekdays
                        ? cron.days[day.day()]
                        : cron.days[day.day()] || cron.weekdays[weekday] ||
                              (weekday == 0 && cron.weekdays[7]);
    if (!date_match) continue;
    for (int hour = 0; hour < 24; ++hour) {
      if (!cron.hours[hour]) continue;
      const absl::Time due = absl::FromCivil(
          absl::CivilSecond(day.year(), day.month(), day.day(), hour,
                            cron.minute, 0),
          utc);
      if (due > after) return due;
    }
  }
  return absl::InvalidArgumentError(
      "Backup schedule cron has no future occurrence");
}

absl::Status ValidateBackupSchedule(database_api::BackupSchedule* schedule) {
  if (!schedule->has_spec() || !schedule->spec().has_cron_spec()) {
    return absl::InvalidArgumentError("Backup schedule cron_spec is required");
  }
  if (!schedule->has_full_backup_spec() &&
      !schedule->has_incremental_backup_spec()) {
    return absl::InvalidArgumentError(
        "Backup schedule full_backup_spec or incremental_backup_spec is "
        "required");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const ParsedCron cron, ParseBackupCron(*schedule));
  GOOGLESQL_RETURN_IF_ERROR(
      NextBackupDue(cron, absl::UnixEpoch()).status());
  if (schedule->has_encryption_config()) {
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateEncryptionConfig(schedule->encryption_config()));
  }
  if (!schedule->has_retention_duration()) {
    schedule->mutable_retention_duration()->set_seconds(7 * 24 * 60 * 60);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Duration retention,
      DurationFromProto(schedule->retention_duration()));
  if (retention < kMinimumBackupRetention ||
      retention > kMaximumBackupRetention) {
    return absl::InvalidArgumentError(
        "Backup schedule retention must be between 6 hours and 366 days");
  }
  schedule->mutable_spec()->mutable_cron_spec()->set_time_zone("UTC");
  schedule->mutable_spec()
      ->mutable_cron_spec()
      ->mutable_creation_window()
      ->set_seconds(4 * 60 * 60);
  return absl::OkStatus();
}

absl::StatusOr<int64_t> DirectorySize(const std::string& directory) {
  int64_t bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(directory, error),
       end;
       iterator != end && !error; iterator.increment(error)) {
    if (iterator->is_regular_file(error)) {
      bytes += static_cast<int64_t>(iterator->file_size(error));

    }
  }
  if (error) {
    return absl::InternalError(
        absl::StrCat("Failed to inspect backup snapshot ", directory, ": ",
                     error.message()));
  }
  return bytes;
}

absl::Status ValidateBackupExpiration(absl::Time expire_time,
                                      absl::Time create_time) {
  if (expire_time < create_time + kMinimumBackupRetention ||
      expire_time > create_time + kMaximumBackupRetention) {
    return absl::InvalidArgumentError(
        "Backup expire_time must be between 6 hours and 366 days after "
        "create_time");
  }
  return absl::OkStatus();
}

// Resolves the documented ListBackups filter fields. Field names may omit
// their underscores, as in sizeBytes.
absl::StatusOr<bool> MatchBackupField(const database_api::Backup& backup,
                                      std::string_view filter_field,
                                      std::string_view op,
                                      std::string_view value) {
  std::string field(filter_field);
  field.erase(std::remove(field.begin(), field.end(), '_'), field.end());
  if (field == "name") {
    return CompareString(backup.name(), value, op);
  }
  if (field == "database") {
    return CompareString(backup.database(), value, op);
  }
  if (field == "state") {
    const std::string state =
        backup.state() == database_api::Backup::READY
            ? "READY"
            : backup.state() == database_api::Backup::CREATING
                  ? "CREATING"
                  : "STATE_UNSPECIFIED";
    return CompareString(state, value, op);
  }
  if (field == "backupschedules") {
    if (op == "!=") {
      for (const std::string& schedule : backup.backup_schedules()) {
        if (CompareString(schedule, value, "=")) return false;
      }
      return true;
    }
    for (const std::string& schedule : backup.backup_schedules()) {
      if (CompareString(schedule, value, op)) return true;
    }
    return false;
  }
  if (field == "sizebytes") {
    int64_t size = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, size);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
      return absl::InvalidArgumentError("Invalid backup size filter value");
    }
    return op == ":" ? std::to_string(backup.size_bytes()).find(value) !=
                           std::string::npos
                     : CompareOrdered(backup.size_bytes(), size, op);
  }
  const google::protobuf::Timestamp* timestamp = nullptr;
  if (field == "createtime") timestamp = &backup.create_time();
  if (field == "expiretime") timestamp = &backup.expire_time();
  if (field == "versiontime") timestamp = &backup.version_time();
  if (timestamp != nullptr) {
    absl::Time requested;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full, value, &requested, &error)) {
      return absl::InvalidArgumentError("Invalid backup time filter value");
    }
    GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time actual,
                               TimestampFromProto(*timestamp));
    return op == ":" ?
               absl::FormatTime(absl::RFC3339_full, actual,
                                absl::UTCTimeZone()).find(value) !=
                   std::string::npos
                     : CompareOrdered(actual, requested, op);
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unsupported backup filter field: ", field));
}

bool NewerBackup(const database_api::Backup& left,
                 const database_api::Backup& right) {
  if (left.create_time().seconds() != right.create_time().seconds()) {
    return left.create_time().seconds() > right.create_time().seconds();
  }
  if (left.create_time().nanos() != right.create_time().nanos()) {
    return left.create_time().nanos() > right.create_time().nanos();
  }
  return left.name() < right.name();
}

std::string BackupPageToken(std::string_view parent, std::string_view filter,
                            const database_api::Backup& backup) {
  database_api::Backup cursor;
  cursor.set_name(backup.name());
  *cursor.mutable_create_time() = backup.create_time();
  std::string payload(parent);
  payload.push_back('\0');
  payload.append(filter);
  payload.push_back('\0');
  payload.append(cursor.SerializeAsString());
  return absl::Base64Escape(payload);
}

absl::StatusOr<database_api::Backup> ParseBackupPageToken(
    std::string_view token, std::string_view parent, std::string_view filter) {
  if (token.size() > 16 * 1024) {
    return absl::InvalidArgumentError("Backup page_token is too long");
  }
  std::string payload;
  if (!absl::Base64Unescape(token, &payload)) {
    return absl::InvalidArgumentError("Invalid backup page_token");
  }
  const size_t first = payload.find('\0');
  const size_t second = first == std::string::npos
                            ? std::string::npos
                            : payload.find('\0', first + 1);
  if (second == std::string::npos ||
      std::string_view(payload).substr(0, first) != parent ||
      std::string_view(payload).substr(first + 1, second - first - 1) !=
          filter) {
    return absl::InvalidArgumentError(
        "Backup page_token does not match parent and filter");
  }
  database_api::Backup cursor;
  if (!cursor.ParseFromString(payload.substr(second + 1)) ||
      !cursor.has_create_time() ||
      !absl::StartsWith(cursor.name(), absl::StrCat(parent, "/backups/")) ||
      !ValidateBackupName(cursor.name()).ok()) {
    return absl::InvalidArgumentError("Invalid backup page_token");
  }
  GOOGLESQL_RETURN_IF_ERROR(TimestampFromProto(cursor.create_time()).status());
  return cursor;
}

std::string BackupSchedulePageToken(std::string_view parent,
                                    std::string_view name) {
  std::string payload(parent);
  payload.push_back('\0');
  payload.append(name);
  return absl::Base64Escape(payload);
}

absl::StatusOr<std::string> ParseBackupSchedulePageToken(
    std::string_view token, std::string_view parent) {
  if (token.size() > 8192) {
    return absl::InvalidArgumentError("Backup schedule page_token is too long");
  }
  std::string payload;
  if (!absl::Base64Unescape(token, &payload)) {
    return absl::InvalidArgumentError("Invalid backup schedule page_token");
  }
  const size_t separator = payload.find('\0');
  if (separator == std::string::npos ||
      std::string_view(payload).substr(0, separator) != parent) {
    return absl::InvalidArgumentError(
        "Backup schedule page_token does not match parent");
  }
  std::string name = payload.substr(separator + 1);
  if (!absl::StartsWith(name, absl::StrCat(parent, "/backupSchedules/")) ||
      !ValidateBackupScheduleName(name).ok()) {
    return absl::InvalidArgumentError("Invalid backup schedule page_token");
  }
  return name;
}

absl::Status DeleteExpiredBackups(RequestContext* ctx) {
  // ponytail: deleting each expired backup rewrites the catalog; batch only if
  // large local backup catalogs make this sweep expensive.
  for (const auto& entry : ctx->env()->backup_catalog()->AllBackups()) {
    if (!entry.backup.has_expire_time()) continue;
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Time expire_time,
        TimestampFromProto(entry.backup.expire_time()));
    if (expire_time > ctx->env()->clock()->Now()) continue;
    database_api::DeleteBackupRequest request;
    request.set_name(entry.backup.name());
    protobuf_api::Empty response;
    const absl::Status status = DeleteBackup(ctx, &request, &response);
    if (!status.ok() && status.code() != absl::StatusCode::kNotFound) {
      return status;
    }
  }
  return absl::OkStatus();
}

class DirectoryCleanup {
 public:
  explicit DirectoryCleanup(std::filesystem::path path)
      : path_(std::move(path)) {}
  DirectoryCleanup(const DirectoryCleanup&) = delete;
  DirectoryCleanup& operator=(const DirectoryCleanup&) = delete;
  ~DirectoryCleanup() {
    if (armed_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  void Disarm() { armed_ = false; }

 private:
  std::filesystem::path path_;
  bool armed_ = true;
};

absl::Status CopySnapshot(const std::string& source,
                          const std::string& destination) {
  if (!std::filesystem::exists(source)) {
    return absl::DataLossError(
        absl::StrCat("Backup snapshot is missing: ", source));
  }
  if (std::filesystem::exists(destination)) {
    return absl::AlreadyExistsError(
        absl::StrCat("Snapshot destination already exists: ", destination));
  }

  std::error_code error;
  std::filesystem::create_directories(
      std::filesystem::path(destination).parent_path(), error);
  if (error) {
    return absl::InternalError(
        absl::StrCat("Failed to create snapshot parent: ", error.message()));
  }

  static std::atomic<uint64_t> temporary_sequence{0};
  const std::filesystem::path temporary_directory = absl::StrCat(
      destination, ".tmp-",
      std::chrono::steady_clock::now().time_since_epoch().count(), "-",
      temporary_sequence.fetch_add(1));
  std::filesystem::copy(source, temporary_directory,
                        std::filesystem::copy_options::recursive, error);
  if (!error) {
    std::filesystem::rename(temporary_directory, destination, error);
  }
  if (error) {
    const std::string message = error.message();
    std::error_code ignored;
    std::filesystem::remove_all(temporary_directory, ignored);
    std::error_code exists_error;
    if (std::filesystem::exists(destination, exists_error) && !exists_error) {
      return absl::AlreadyExistsError(
          absl::StrCat("Snapshot destination already exists: ", destination));
    }
    return absl::InternalError(
        absl::StrCat("Failed to copy backup snapshot: ", message));
  }
  return absl::OkStatus();
}

std::string OperationName(const std::shared_ptr<Operation>& operation) {
  operations_api::Operation proto;
  operation->ToProto(&proto);
  return proto.name();
}

absl::Status PersistRestoredDatabase(
    ServerEnv* env, const std::string& parent, const std::string& database_id,
    const BackupCatalog::BackupEntry& entry,
    const database_api::Database& restored) {
  MetadataStore* metadata = env->metadata_store();
  if (metadata == nullptr) return absl::OkStatus();
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time create_time,
                             TimestampFromProto(restored.create_time()));
  if (entry.schema_change_batches.empty()) {
    metadata->AddDatabase(
        parent, database_id,
        entry.dialect == database_api::DatabaseDialect::POSTGRESQL
            ? "POSTGRESQL"
            : "GOOGLE_STANDARD_SQL",
        entry.ddl_statements, entry.proto_descriptor_bytes,
        absl::FormatTime(absl::RFC3339_full, create_time,
                         absl::UTCTimeZone()));
  } else {
    const PersistedSchemaChangeBatch& initial =
        entry.schema_change_batches.front();
    metadata->AddDatabase(
        parent, database_id,
        entry.dialect == database_api::DatabaseDialect::POSTGRESQL
            ? "POSTGRESQL"
            : "GOOGLE_STANDARD_SQL",
        initial.statements, initial.proto_descriptor_bytes,
        absl::FormatTime(absl::RFC3339_full, create_time,
                         absl::UTCTimeZone()));
    for (std::size_t index = 1;
         index < entry.schema_change_batches.size(); ++index) {
      const PersistedSchemaChangeBatch& batch =
          entry.schema_change_batches[index];
      metadata->UpdateDdl(parent, database_id, batch.statements,
                          batch.proto_descriptor_bytes,
                          batch.schema_change_timestamp);
    }
  }
  metadata->UpdateIdCounters(
      parent, database_id,
      MetadataStore::IdCounters{
          .table_id = entry.id_counters.table_id,
          .column_id = entry.id_counters.column_id,
          .change_stream_id = entry.id_counters.change_stream_id,
      });
  return absl::OkStatus();
}

// Returns the version_time a CreateBackup request asks for, or nullopt when it
// asks for none. The time must lie between the database's
// earliest_version_time and now. A scheduled backup asks for its cron time,
// which is moved into that range instead of being rejected.
absl::StatusOr<std::optional<absl::Time>> RequestedVersionTime(
    const database_api::Backup& backup, Database* database, Clock* clock,
    bool scheduled) {
  if (!backup.has_version_time()) return std::nullopt;
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time version_time,
                             TimestampFromProto(backup.version_time()));
  database_api::Database database_proto;
  GOOGLESQL_RETURN_IF_ERROR(database->ToProto(&database_proto));
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time earliest_version_time,
      TimestampFromProto(database_proto.earliest_version_time()));
  const absl::Time now = clock->Now();
  if (scheduled) return std::clamp(version_time, earliest_version_time, now);
  if (version_time > now) {
    return absl::InvalidArgumentError(
        "Backup version_time must not be in the future");
  }
  if (version_time < earliest_version_time) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Backup version_time must not be earlier than the database's "
        "earliest_version_time ",
        absl::FormatTime(absl::RFC3339_full, earliest_version_time,
                         absl::UTCTimeZone())));
  }
  return version_time;
}

// Returns the persisted schema-change batches committed at or before
// version_time, or nullopt when a later batch has no recorded commit time
// (metadata written by older emulator builds).
std::optional<std::vector<PersistedSchemaChangeBatch>> SchemaChangeBatchesAt(
    const std::vector<PersistedSchemaChangeBatch>& batches,
    absl::Time version_time) {
  std::vector<PersistedSchemaChangeBatch> visible;
  for (const PersistedSchemaChangeBatch& batch : batches) {
    // The first batch creates the database, which predates any version_time.
    if (!visible.empty()) {
      absl::Time committed;
      if (!absl::ParseTime(absl::RFC3339_full, batch.schema_change_timestamp,
                           &committed, nullptr)) {
        return std::nullopt;
      }
      if (committed > version_time) break;
    }
    visible.push_back(batch);
  }
  return visible;
}

// An incremental chain that a scheduled backup extends.
struct ChainLink {
  BackupCatalog::BackupChain chain;
  // version_time of the chain's newest backup.
  absl::Time previous_version_time;
};

// Returns the chain that the schedule's backup at version_time extends, or
// nullopt when the backup starts a new chain: the chain already has 13
// incremental backups, its full backup is 28 days old, or its newest backup
// is gone.
absl::StatusOr<std::optional<ChainLink>> ExtendableChain(
    const BackupCatalog& catalog, const std::string& schedule_name,
    absl::Time version_time) {
  std::optional<BackupCatalog::BackupChain> chain =
      catalog.GetBackupChain(schedule_name);
  if (!chain.has_value() ||
      chain->backup_count > kMaximumIncrementalBackupsPerChain) {
    return std::nullopt;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time oldest_version_time,
                             TimestampFromProto(chain->oldest_version_time));
  if (version_time - oldest_version_time >= kMaximumBackupChainAge) {
    return std::nullopt;
  }
  absl::StatusOr<BackupCatalog::BackupEntry> newest =
      catalog.GetBackup(chain->newest_backup);
  if (absl::IsNotFound(newest.status())) return std::nullopt;
  GOOGLESQL_RETURN_IF_ERROR(newest.status());
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time previous_version_time,
                             TimestampFromProto(newest->backup.version_time()));
  return ChainLink{.chain = *std::move(chain),
                   .previous_version_time = previous_version_time};
}

// Quotes a table name, part by part for a table in a named schema.
std::string QuoteTableName(std::string_view name,
                           database_api::DatabaseDialect dialect) {
  std::vector<std::string> parts;
  for (std::string_view part : absl::StrSplit(name, '.')) {
    if (dialect == database_api::DatabaseDialect::POSTGRESQL) {
      parts.push_back(absl::StrCat(
          "\"", absl::StrReplaceAll(part, {{"\"", "\"\""}}), "\""));
    } else if (backend::ddl::IsReservedWord(part)) {
      parts.push_back(absl::StrCat("`", part, "`"));
    } else {
      parts.emplace_back(part);
    }
  }
  return absl::StrJoin(parts, ".");
}

// Returns the DDL that drops every row deletion policy (TTL) in schema.
std::vector<std::string> DropRowDeletionPolicyStatements(
    const backend::Schema& schema, database_api::DatabaseDialect dialect) {
  std::vector<std::string> statements;
  for (const backend::Table* table : schema.tables()) {
    if (!table->row_deletion_policy().has_value()) continue;
    statements.push_back(absl::StrCat(
        "ALTER TABLE ", QuoteTableName(table->Name(), dialect),
        dialect == database_api::DatabaseDialect::POSTGRESQL
            ? " DROP TTL"
            : " DROP ROW DELETION POLICY"));
  }
  return statements;
}

}  // namespace

absl::Status CreateBackupInternal(
    RequestContext* ctx, const database_api::CreateBackupRequest* request,
    operations_api::Operation* response,
    std::optional<BackupCatalog::ScheduleRun> schedule_run) {
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(ctx));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateResourceId(request->backup_id(), "Backup ID"));
  if (!request->has_backup() || request->backup().database().empty()) {
    return absl::InvalidArgumentError("Backup database must be provided");
  }
  if (request->has_encryption_config()) {
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateEncryptionConfig(request->encryption_config()));
  }
  if (!schedule_run.has_value() && !request->backup().has_expire_time()) {
    return absl::InvalidArgumentError("Backup expire_time must be provided");
  }

  const std::string backup_name =
      MakeBackupName(request->parent(), request->backup_id());
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(backup_name));

  absl::string_view project_id;
  absl::string_view instance_id;
  absl::string_view database_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(
      request->backup().database(), &project_id, &instance_id, &database_id));
  if (MakeInstanceUri(project_id, instance_id) != request->parent() ||
      MakeDatabaseUri(MakeInstanceUri(project_id, instance_id), database_id) !=
          request->backup().database()) {
    return absl::InvalidArgumentError(
        "Backup database must be canonical and belong to the parent instance");
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> database,
      ctx->env()->database_manager()->GetDatabase(
          request->backup().database()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Instance> source_instance,
      ctx->env()->instance_manager()->GetInstance(request->parent()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::optional<absl::Time> version_time,
      RequestedVersionTime(request->backup(), database.get(),
                           ctx->env()->clock(), schedule_run.has_value()));
  BackupCatalog* catalog = ctx->env()->backup_catalog();
  if (!catalog->persistent()) {
    return absl::FailedPreconditionError(
        "Native backups require emulator --data_dir persistent storage");
  }
  auto existing = catalog->GetBackup(backup_name);
  if (existing.ok()) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup already exists: ", backup_name));
  }
  if (existing.status().code() != absl::StatusCode::kNotFound) {
    return existing.status();
  }

  absl::Time expire_time;
  if (!schedule_run.has_value()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        expire_time, TimestampFromProto(request->backup().expire_time()));
    if (expire_time <= ctx->env()->clock()->Now()) {
      return absl::InvalidArgumentError(
          "Backup expire_time must be after the capture time");
    }
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateBackupExpiration(expire_time, ctx->env()->clock()->Now()));
  }

  std::optional<database_api::BackupSchedule> schedule;
  std::optional<ChainLink> chain_link;
  if (schedule_run.has_value()) {
    GOOGLESQL_ASSIGN_OR_RETURN(schedule,
                               catalog->GetBackupSchedule(schedule_run->name));
    if (schedule->has_incremental_backup_spec()) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          chain_link,
          ExtendableChain(*catalog, schedule_run->name,
                          version_time.value_or(ctx->env()->clock()->Now())));
    }
  }

  absl::MutexLock schema_change_lock(&database->schema_change_mutex());
  const std::string snapshot_directory =
      catalog->SnapshotDirectory(backup_name);
  GOOGLESQL_RETURN_IF_ERROR(catalog->PrepareSnapshot(backup_name));
  const std::filesystem::path snapshot_root =
      std::filesystem::path(snapshot_directory).parent_path();
  DirectoryCleanup snapshot_cleanup(snapshot_root);
  DirectoryCleanup intent_cleanup(snapshot_root.string() + ".creating");
  GOOGLESQL_ASSIGN_OR_RETURN(
      const backend::Database::BackupCheckpoint checkpoint,
      database->backend()->CreateBackupCheckpoint(
          snapshot_directory, version_time.value_or(absl::InfiniteFuture()),
          chain_link.has_value() ? chain_link->previous_version_time
                                 : absl::InfiniteFuture()));

  if (schedule.has_value()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        const absl::Duration retention,
        DurationFromProto(schedule->retention_duration()));
    expire_time = checkpoint.capture_time + retention;
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, checkpoint.capture_time));
  GOOGLESQL_ASSIGN_OR_RETURN(auto capture_timestamp,
                             TimestampToProto(checkpoint.capture_time));

  BackupCatalog::BackupEntry entry;
  entry.backup = request->backup();
  entry.backup.clear_backup_schedules();
  if (schedule_run.has_value()) {
    GOOGLESQL_ASSIGN_OR_RETURN(*entry.backup.mutable_expire_time(),
                               TimestampToProto(expire_time));
    entry.backup.add_backup_schedules(schedule_run->name);
  }
  entry.backup.set_name(backup_name);
  entry.backup.set_database_dialect(database->backend()->dialect());
  entry.backup.set_state(database_api::Backup::READY);
  *entry.backup.mutable_create_time() = capture_timestamp;
  if (version_time.has_value()) {
    GOOGLESQL_ASSIGN_OR_RETURN(*entry.backup.mutable_version_time(),
                               TimestampToProto(*version_time));
  } else {
    *entry.backup.mutable_version_time() = capture_timestamp;
  }
  *entry.backup.mutable_encryption_info() = GoogleDefaultEncryption();
  entry.backup.clear_encryption_information();
  GOOGLESQL_ASSIGN_OR_RETURN(int64_t size_bytes,
                             DirectorySize(snapshot_directory));
  entry.backup.set_size_bytes(size_bytes);
  // Every snapshot is a full copy. An incremental backup still reports only
  // the bytes of the versions committed since its chain's previous backup as
  // exclusive to it, as production does.
  const int64_t exclusive_size_bytes =
      chain_link.has_value() ? std::min(checkpoint.changed_bytes, size_bytes)
                             : size_bytes;
  entry.backup.set_exclusive_size_bytes(exclusive_size_bytes);
  entry.backup.set_freeable_size_bytes(exclusive_size_bytes);
  entry.backup.clear_incremental_backup_chain_id();
  if (chain_link.has_value()) {
    entry.backup.set_incremental_backup_chain_id(chain_link->chain.id);
    *entry.backup.mutable_oldest_version_time() =
        chain_link->chain.oldest_version_time;
  } else {
    if (schedule.has_value() && schedule->has_incremental_backup_spec()) {
      entry.backup.set_incremental_backup_chain_id(request->backup_id());
    }
    *entry.backup.mutable_oldest_version_time() = entry.backup.version_time();
  }
  const backend::Schema* schema = checkpoint.schema;
  GOOGLESQL_ASSIGN_OR_RETURN(entry.proto_descriptor_bytes,
                             schema->proto_bundle()->GetProtoDescriptorBytes());

  bool found_persisted_ddl = false;
  if (auto* metadata = ctx->env()->metadata_store(); metadata != nullptr) {
    auto persisted_instances = metadata->instances();
    auto instance_it = persisted_instances.find(request->parent());
    if (instance_it != persisted_instances.end()) {
      auto database_it =
          instance_it->second.databases.find(std::string(database_id));
      if (database_it != instance_it->second.databases.end()) {
        const std::vector<PersistedSchemaChangeBatch>& persisted =
            database_it->second.schema_change_batches;
        std::optional<std::vector<PersistedSchemaChangeBatch>> batches =
            SchemaChangeBatchesAt(
                persisted, version_time.value_or(absl::InfiniteFuture()));
        // Batches without commit times are all visible when no later schema
        // exists.
        if (!batches.has_value() &&
            schema == database->backend()->GetLatestSchema()) {
          batches = persisted;
        }
        if (batches.has_value()) {
          entry.schema_change_batches = *std::move(batches);
          for (const PersistedSchemaChangeBatch& batch :
               entry.schema_change_batches) {
            entry.ddl_statements.insert(entry.ddl_statements.end(),
                                        batch.statements.begin(),
                                        batch.statements.end());
          }
          found_persisted_ddl = true;
        }
      }
    }
  }
  if (!found_persisted_ddl) {
    GOOGLESQL_ASSIGN_OR_RETURN(entry.ddl_statements,
                               backend::PrintDDLStatements(schema));
    entry.schema_change_batches.push_back(
        {.statements = entry.ddl_statements,
         .proto_descriptor_bytes = entry.proto_descriptor_bytes});
  }
  entry.dialect = database->backend()->dialect();
  const backend::Database::IdCounterValues counters =
      database->backend()->GetIdCounterValues();
  entry.id_counters.table_id = counters.table_id;
  entry.id_counters.column_id = counters.column_id;
  entry.id_counters.change_stream_id = counters.change_stream_id;
  instance_api::Instance source_instance_proto;
  source_instance->ToProto(&source_instance_proto);
  entry.source_instance_config = source_instance_proto.config();

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          backup_name, OperationManager::kAutoGeneratedId));
  entry.operation_name = OperationName(operation);
  operation->SetResponse(entry.backup);
  operations_api::Operation persisted_operation;
  operation->ToProto(&persisted_operation);
  absl::Status catalog_status =
      catalog->CreateBackup(entry, persisted_operation, schedule_run);
  if (!catalog_status.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(entry.operation_name);
    return catalog_status;
  }

  snapshot_cleanup.Disarm();
  *response = std::move(persisted_operation);
  return absl::OkStatus();
}

absl::Status CreateBackup(RequestContext* ctx,
                          const database_api::CreateBackupRequest* request,
                          operations_api::Operation* response) {
  return CreateBackupInternal(ctx, request, response, std::nullopt);
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CreateBackup);

absl::Status RunDueBackupSchedules(ServerEnv* env, absl::Time now) {
  grpc::ServerContext grpc_context;
  RequestContext ctx(env, &grpc_context);
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(&ctx));
  absl::Status first_error;
  for (const auto& entry : env->backup_catalog()->AllBackupSchedules()) {
    const auto& schedule = entry.schedule;
    auto cron = ParseBackupCron(schedule);
    if (!cron.ok()) {
      if (first_error.ok()) first_error = cron.status();
      continue;
    }
    int64_t due_seconds = entry.next_due_seconds;
    if (due_seconds == 0) {
      absl::Time origin = now;
      if (schedule.has_update_time()) {
        auto parsed = TimestampFromProto(schedule.update_time());
        if (!parsed.ok()) {
          if (first_error.ok()) first_error = parsed.status();
          continue;
        }
        origin = *parsed;
      }
      auto initial_due = NextBackupDue(*cron, origin);
      if (!initial_due.ok()) {
        if (first_error.ok()) first_error = initial_due.status();
        continue;
      }
      due_seconds = absl::ToUnixSeconds(*initial_due);
      absl::Status initialized = env->backup_catalog()->AdvanceBackupSchedule(
          schedule.name(), 0, due_seconds, schedule.SerializeAsString());
      if (!initialized.ok()) {
        if (initialized.code() != absl::StatusCode::kAborted &&
            first_error.ok()) {
          first_error = initialized;
        }
        continue;
      }
    }
    const absl::Time due = absl::FromUnixSeconds(due_seconds);
    if (now < due) continue;
    auto following = NextBackupDue(*cron, due);
    if (!following.ok()) {
      if (first_error.ok()) first_error = following.status();
      continue;
    }
    if (now > due + absl::Hours(4)) {
      auto future = NextBackupDue(*cron, now);
      if (!future.ok()) {
        if (first_error.ok()) first_error = future.status();
        continue;
      }
      absl::Status skipped = env->backup_catalog()->AdvanceBackupSchedule(
          schedule.name(), due_seconds, absl::ToUnixSeconds(*future),
          schedule.SerializeAsString());
      if (!skipped.ok() && skipped.code() != absl::StatusCode::kAborted &&
          first_error.ok()) {
        first_error = skipped;
      }
      continue;
    }
    absl::string_view project_id;
    absl::string_view instance_id;
    absl::string_view database_id;
    const std::string database_name =
        schedule.name().substr(0, schedule.name().rfind("/backupSchedules/"));
    absl::Status parsed_name = ParseDatabaseUri(
        database_name, &project_id, &instance_id, &database_id);
    if (!parsed_name.ok()) {
      if (first_error.ok()) first_error = parsed_name;
      continue;
    }
    const std::string parent = MakeInstanceUri(project_id, instance_id);
    database_api::CreateBackupRequest request;
    request.set_parent(parent);
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : schedule.name()) {
      hash = (hash ^ ch) * 1099511628211ULL;
    }
    request.set_backup_id(
        absl::StrCat("scheduled-", absl::Hex(hash), "-", due_seconds));
    request.mutable_backup()->set_database(
        MakeDatabaseUri(parent, database_id));
    // A scheduled backup holds the database as of its cron time.
    request.mutable_backup()->mutable_version_time()->set_seconds(due_seconds);
    operations_api::Operation operation;
    absl::Status status = CreateBackupInternal(
        &ctx, &request, &operation,
        BackupCatalog::ScheduleRun{
            schedule.name(), due_seconds, absl::ToUnixSeconds(*following),
            schedule.SerializeAsString()});
    if (!status.ok() && status.code() != absl::StatusCode::kAborted &&
        first_error.ok()) {
      first_error = status;
    }
  }
  return first_error;
}

absl::Status GetBackup(RequestContext* ctx,
                       const database_api::GetBackupRequest* request,
                       database_api::Backup* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->name()));
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(ctx));
  GOOGLESQL_ASSIGN_OR_RETURN(
      BackupCatalog::BackupEntry entry,
      ctx->env()->backup_catalog()->GetBackup(request->name()));
  *response = entry.backup;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetBackup);

absl::Status ListBackups(RequestContext* ctx,
                         const database_api::ListBackupsRequest* request,
                         database_api::ListBackupsResponse* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_ASSIGN_OR_RETURN(const ListFilter filter,
                             ListFilter::Parse(request->filter()));
  const auto matches = [&filter](const database_api::Backup& backup) {
    return filter.Matches([&backup](std::string_view field,
                                    std::string_view op,
                                    std::string_view value) {
      return MatchBackupField(backup, field, op, value);
    });
  };
  GOOGLESQL_RETURN_IF_ERROR(matches(database_api::Backup()).status());
  std::optional<database_api::Backup> cursor;
  if (!request->page_token().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        cursor, ParseBackupPageToken(request->page_token(), request->parent(),
                                     request->filter()));
  }
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(ctx));
  int32_t page_size = request->page_size();
  if (page_size <= 0 || page_size > kMaximumPageSize) {
    page_size = kMaximumPageSize;
  }
  std::vector<BackupCatalog::BackupEntry> backups =
      ctx->env()->backup_catalog()->ListBackups(request->parent());
  std::sort(backups.begin(), backups.end(), [](const auto& left, const auto& right) {
    return NewerBackup(left.backup, right.backup);
  });
  for (const auto& entry : backups) {
    if (cursor.has_value() && !NewerBackup(*cursor, entry.backup)) continue;
    GOOGLESQL_ASSIGN_OR_RETURN(const bool matched, matches(entry.backup));
    if (!matched) continue;
    if (response->backups_size() >= page_size) {
      response->set_next_page_token(BackupPageToken(
          request->parent(), request->filter(),
          response->backups(response->backups_size() - 1)));
      break;
    }
    *response->add_backups() = entry.backup;
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListBackups);

absl::Status UpdateBackup(RequestContext* ctx,
                          const database_api::UpdateBackupRequest* request,
                          database_api::Backup* response) {
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(ctx));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_backup()) {
    return absl::InvalidArgumentError("Backup must be provided");
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->backup().name()));
  if (request->update_mask().paths().empty()) {
    return absl::InvalidArgumentError("Backup update_mask must be provided");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      BackupCatalog::BackupEntry entry,
      ctx->env()->backup_catalog()->GetBackup(request->backup().name()));
  for (const std::string& path : request->update_mask().paths()) {
    if (path != "expire_time") {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported backup update field: ", path));
    }
  }
  if (!request->backup().has_expire_time()) {
    return absl::InvalidArgumentError("Backup expire_time must be provided");
  }
  *entry.backup.mutable_expire_time() = request->backup().expire_time();
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time create_time,
      TimestampFromProto(entry.backup.create_time()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      const absl::Time expire_time,
      TimestampFromProto(entry.backup.expire_time()));
  if (expire_time <= ctx->env()->clock()->Now()) {
    return absl::InvalidArgumentError(
        "Backup expire_time must be in the future");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, create_time));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->UpdateBackup(entry.backup));
  *response = entry.backup;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, UpdateBackup);

absl::Status DeleteBackup(RequestContext* ctx,
                          const database_api::DeleteBackupRequest* request,
                          protobuf_api::Empty* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->name()));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->GetBackup(request->name()).status());

  MetadataStore* metadata = ctx->env()->metadata_store();
  const auto previous_metadata_policy =
      metadata == nullptr ? std::optional<iam_api::Policy>()
                          : metadata->GetIamPolicy(request->name());

  if (metadata != nullptr) {
    metadata->RemoveIamPolicy(request->name());
    metadata->SetPendingBackupDeletion(request->name());
    absl::Status metadata_status = metadata->Save();
    if (!metadata_status.ok()) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_metadata_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_metadata_policy);
      }
      return metadata_status;
    }
  }

  absl::Status delete_status =
      ctx->env()->backup_catalog()->DeleteBackup(request->name());
  if (!delete_status.ok()) {
    if (metadata != nullptr) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_metadata_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_metadata_policy);
      }
      absl::Status rollback_status = metadata->Save();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            delete_status.message(),
            "; failed to roll back backup deletion intent: ",
            rollback_status.message()));
      }
    }
    return delete_status;
  }

  ctx->env()->RemoveIamPolicies(request->name());
  if (metadata != nullptr) {
    metadata->RemovePendingBackupDeletion(request->name());
    GOOGLESQL_RETURN_IF_ERROR(metadata->Save());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, DeleteBackup);

absl::Status CopyBackup(RequestContext* ctx,
                        const database_api::CopyBackupRequest* request,
                        operations_api::Operation* response) {
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(ctx));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateResourceId(request->backup_id(), "Backup ID"));
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->source_backup()));
  if (!request->has_expire_time()) {
    return absl::InvalidArgumentError("Copied backup expire_time is required");
  }
  if (request->has_encryption_config()) {
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateEncryptionConfig(request->encryption_config()));
  }

  BackupCatalog* catalog = ctx->env()->backup_catalog();
  const std::string name =
      MakeBackupName(request->parent(), request->backup_id());
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(name));
  auto existing = catalog->GetBackup(name);
  if (existing.ok()) {
    return absl::AlreadyExistsError(
        absl::StrCat("Backup already exists: ", name));
  }
  if (existing.status().code() != absl::StatusCode::kNotFound) {
    return existing.status();
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<BackupCatalog::SnapshotLease> source_lease,
      catalog->AcquireSnapshot(request->source_backup()));
  BackupCatalog::BackupEntry source = source_lease->entry();
  if (source.backup.state() != database_api::Backup::READY) {
    return absl::FailedPreconditionError(
        "Source backup must be in READY state");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      absl::Time source_create_time,
      TimestampFromProto(source.backup.create_time()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      absl::Time expire_time, TimestampFromProto(request->expire_time()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupExpiration(expire_time, source_create_time));
  if (expire_time < ctx->env()->clock()->Now() + kMinimumBackupRetention) {
    return absl::InvalidArgumentError(
        "Copied backup expire_time must be at least 6 hours from now");
  }

  const std::string destination_directory = catalog->SnapshotDirectory(name);
  GOOGLESQL_RETURN_IF_ERROR(catalog->PrepareSnapshot(name));
  const std::filesystem::path destination_root =
      std::filesystem::path(destination_directory).parent_path();
  DirectoryCleanup destination_cleanup(destination_root);
  DirectoryCleanup intent_cleanup(destination_root.string() + ".creating");
  GOOGLESQL_RETURN_IF_ERROR(CopySnapshot(
      source_lease->snapshot_directory(), destination_directory));
  source_lease.reset();

  BackupCatalog::BackupEntry copy = source;
  copy.backup.set_name(name);
  copy.backup.clear_backup_schedules();
  *copy.backup.mutable_expire_time() = request->expire_time();
  // The copy is a full snapshot outside any incremental chain.
  copy.backup.clear_incremental_backup_chain_id();
  *copy.backup.mutable_oldest_version_time() = copy.backup.version_time();
  copy.backup.set_exclusive_size_bytes(copy.backup.size_bytes());
  copy.backup.set_freeable_size_bytes(copy.backup.size_bytes());
  *copy.backup.mutable_encryption_info() = GoogleDefaultEncryption();
  copy.backup.clear_encryption_information();
  GOOGLESQL_ASSIGN_OR_RETURN(*copy.backup.mutable_create_time(),
                             TimestampToProto(ctx->env()->clock()->Now()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          name, OperationManager::kAutoGeneratedId));
  copy.operation_name = OperationName(operation);
  operation->SetResponse(copy.backup);
  operations_api::Operation persisted_operation;
  operation->ToProto(&persisted_operation);
  absl::Status status = catalog->CreateBackup(copy, persisted_operation);
  if (!status.ok()) {
    ctx->env()->operation_manager()->DeleteOperation(copy.operation_name);
    return status;
  }

  destination_cleanup.Disarm();
  *response = std::move(persisted_operation);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CopyBackup);

absl::Status RestoreDatabase(
    RequestContext* ctx, const database_api::RestoreDatabaseRequest* request,
    operations_api::Operation* response) {
  GOOGLESQL_RETURN_IF_ERROR(DeleteExpiredBackups(ctx));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateInstance(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabaseId(request->database_id()));
  if (!request->has_backup()) {
    return absl::InvalidArgumentError("Restore backup source is required");
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupName(request->backup()));
  if (request->has_encryption_config()) {
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateEncryptionConfig(request->encryption_config()));
  }
  MetadataStore* metadata = ctx->env()->metadata_store();
  if (metadata == nullptr) {
    return absl::FailedPreconditionError(
        "Native restore requires persistent metadata storage");
  }

  BackupCatalog* catalog = ctx->env()->backup_catalog();
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<BackupCatalog::SnapshotLease> source_lease,
      catalog->AcquireSnapshot(request->backup()));
  BackupCatalog::BackupEntry entry = source_lease->entry();
  if (entry.backup.state() != database_api::Backup::READY) {
    return absl::FailedPreconditionError(
        "Restore source backup must be in READY state");
  }

  absl::string_view target_project_id;
  absl::string_view target_instance_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceUri(
      request->parent(), &target_project_id, &target_instance_id));
  const size_t backup_marker = entry.backup.name().rfind("/backups/");
  if (backup_marker == std::string::npos) {
    return absl::DataLossError("Backup has an invalid resource name");
  }
  const std::string backup_parent =
      entry.backup.name().substr(0, backup_marker);
  absl::string_view backup_project_id;
  absl::string_view backup_instance_id;
  GOOGLESQL_RETURN_IF_ERROR(ParseInstanceUri(
      backup_parent, &backup_project_id, &backup_instance_id));
  if (target_project_id != backup_project_id) {
    return absl::InvalidArgumentError(
        "Restored database must be in the same project as the backup");
  }

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Instance> target_instance,
      ctx->env()->instance_manager()->GetInstance(request->parent()));
  instance_api::Instance target_instance_proto;
  target_instance->ToProto(&target_instance_proto);
  if (entry.source_instance_config.empty()) {
    return absl::DataLossError(
        "Backup is missing its source instance configuration");
  }
  if (target_instance_proto.config() != entry.source_instance_config) {
    return absl::FailedPreconditionError(
        "Restore destination instance configuration does not match the "
        "source backup configuration");
  }

  const std::string database_uri =
      MakeDatabaseUri(request->parent(), request->database_id());
  GOOGLESQL_ASSIGN_OR_RETURN(
      const std::string storage_directory,
      backend::Database::PersistentStorageDirectory(config::data_dir(),
                                                    database_uri));
  const std::filesystem::path database_root =
      std::filesystem::path(storage_directory).parent_path();
  std::optional<DirectoryCleanup> database_cleanup;
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<DatabaseManager::Creation> creation,
      ctx->env()->database_manager()->ReserveDatabase(database_uri));
  const std::filesystem::path staging_root =
      database_root.string() + ".restoring";
  const std::filesystem::path restore_marker =
      database_root / ".restore-in-progress";
  std::error_code filesystem_error;
  if (std::filesystem::exists(database_root, filesystem_error)) {
    if (filesystem_error) {
      return absl::InternalError(absl::StrCat(
          "Failed to inspect restore destination: ",
          filesystem_error.message()));
    }
    const bool owned_incomplete_restore =
        std::filesystem::exists(restore_marker, filesystem_error);
    if (filesystem_error) {
      return absl::InternalError(absl::StrCat(
          "Failed to inspect restore ownership marker: ",
          filesystem_error.message()));
    }
    if (!owned_incomplete_restore) {
      return absl::AlreadyExistsError(
          absl::StrCat("Restore destination storage already exists: ",
                       database_uri));
    }
    std::filesystem::remove_all(database_root, filesystem_error);
    if (filesystem_error) {
      return absl::InternalError(absl::StrCat(
          "Failed to recover incomplete restore destination: ",
          filesystem_error.message()));
    }
  } else if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to inspect restore destination: ",
        filesystem_error.message()));
  }

  std::filesystem::remove_all(staging_root, filesystem_error);
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to clean incomplete restore staging directory: ",
        filesystem_error.message()));
  }
  std::filesystem::create_directories(staging_root, filesystem_error);
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to create restore staging directory: ",
        filesystem_error.message()));
  }
  DirectoryCleanup staging_cleanup(staging_root);
  {
    std::ofstream marker(staging_root / ".restore-in-progress");
    marker << database_uri;
    if (!marker) {
      return absl::InternalError(
          "Failed to write restore ownership marker");
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(CopySnapshot(
      source_lease->snapshot_directory(),
      (staging_root / "storage").string()));
  source_lease.reset();

  std::filesystem::create_directories(database_root.parent_path(),
                                      filesystem_error);
  if (!filesystem_error) {
    std::filesystem::rename(staging_root, database_root, filesystem_error);
  }
  if (filesystem_error) {
    return absl::InternalError(absl::StrCat(
        "Failed to publish restore snapshot: ", filesystem_error.message()));
  }
  staging_cleanup.Disarm();
  database_cleanup.emplace(database_root);

  backend::Database::IdCounterValues counters{
      .table_id = entry.id_counters.table_id,
      .column_id = entry.id_counters.column_id,
      .change_stream_id = entry.id_counters.change_stream_id,
  };
  std::vector<backend::SchemaChangeOperation> schema_change_operations;
  if (entry.schema_change_batches.empty()) {
    schema_change_operations.push_back(
        {.statements = entry.ddl_statements,
         .proto_descriptor_bytes = entry.proto_descriptor_bytes,
         .database_dialect = entry.dialect,
         .replaying_committed_ddl = true});
  } else {
    for (const auto& batch : entry.schema_change_batches) {
      schema_change_operations.push_back(
          {.statements = batch.statements,
           .proto_descriptor_bytes = batch.proto_descriptor_bytes,
           .database_dialect = entry.dialect,
           .replaying_committed_ddl = true});
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Database> restored,
      creation->Build(schema_change_operations, counters,
                      ctx->env()->clock()->Now()));
  // Like production, a restored database has no row deletion policies. The
  // drop is replayed after the backup's own schema changes on restart.
  const std::vector<std::string> drop_ttl_statements =
      DropRowDeletionPolicyStatements(*restored->backend()->GetLatestSchema(),
                                      entry.dialect);
  if (!drop_ttl_statements.empty()) {
    int successful_statements = 0;
    absl::Time commit_timestamp;
    absl::Status backfill_status;
    GOOGLESQL_RETURN_IF_ERROR(restored->backend()->UpdateSchema(
        {.statements = drop_ttl_statements, .database_dialect = entry.dialect},
        &successful_statements, &commit_timestamp, &backfill_status));
    GOOGLESQL_RETURN_IF_ERROR(backfill_status);
    if (entry.schema_change_batches.empty()) {
      entry.schema_change_batches.push_back(
          {.statements = entry.ddl_statements,
           .proto_descriptor_bytes = entry.proto_descriptor_bytes});
    }
    entry.schema_change_batches.push_back(
        {.statements = drop_ttl_statements,
         .schema_change_timestamp = absl::FormatTime(
             absl::RFC3339_full, commit_timestamp, absl::UTCTimeZone())});
  }
  database_api::Database restored_proto;
  GOOGLESQL_RETURN_IF_ERROR(restored->ToProto(&restored_proto));

  GOOGLESQL_ASSIGN_OR_RETURN(
      std::shared_ptr<Operation> operation,
      ctx->env()->operation_manager()->CreateOperation(
          database_uri, OperationManager::kAutoGeneratedId));
  operation->SetResponse(restored_proto);
  operations_api::Operation persisted_operation;
  operation->ToProto(&persisted_operation);
  bool operation_persisted = false;
  bool metadata_persisted = false;
  auto rollback = [&](const absl::Status& failure) {
    absl::Status operation_rollback = absl::OkStatus();
    if (operation_persisted) {
      operation_rollback =
          catalog->DeleteOperation(persisted_operation.name());
    }
    metadata->RemovePendingOperation(persisted_operation.name());
    metadata->RemoveDatabase(request->parent(), request->database_id());
    absl::Status metadata_rollback = absl::OkStatus();
    if (metadata_persisted) {
      metadata_rollback = metadata->Save();
    }
    if (!metadata_rollback.ok()) {
      // The durable metadata still names the restored database and pending
      // terminal operation. Preserve both the root and restore marker so a
      // restart can reconcile them, and restore the same record in memory so
      // a later metadata save cannot erase the only durable recovery path.
      absl::Status metadata_restore = PersistRestoredDatabase(
          ctx->env(), request->parent(), request->database_id(), entry,
          restored_proto);
      metadata->SetPendingOperation(persisted_operation);
      database_cleanup->Disarm();
      return absl::DataLossError(absl::StrCat(
          failure.message(), "; failed to roll back restored state; metadata: ",
          metadata_rollback.message(),
          metadata_restore.ok()
              ? ""
              : absl::StrCat("; failed to restore in-memory metadata: ",
                             metadata_restore.message()),
          "; restart required to reconcile durable restore"));
    }
    if (!operation_rollback.ok()) {
      // The catalog still contains the terminal operation. Preserve the
      // matching database metadata and root so startup can replay that
      // operation instead of leaving it attached to a deleted snapshot.
      absl::Status metadata_restore = PersistRestoredDatabase(
          ctx->env(), request->parent(), request->database_id(), entry,
          restored_proto);
      metadata->SetPendingOperation(persisted_operation);
      absl::Status recovery_save = metadata->Save();
      database_cleanup->Disarm();
      return absl::DataLossError(absl::StrCat(
          failure.message(), "; failed to roll back restored operation: ",
          operation_rollback.message(),
          metadata_restore.ok()
              ? ""
              : absl::StrCat("; failed to restore in-memory metadata: ",
                             metadata_restore.message()),
          recovery_save.ok()
              ? "; restart required to reconcile durable restore"
              : absl::StrCat("; failed to persist restore recovery state: ",
                             recovery_save.message())));
    }
    ctx->env()->operation_manager()->DeleteOperation(
        persisted_operation.name());
    restored.reset();
    return failure;
  };

  GOOGLESQL_RETURN_IF_ERROR(PersistRestoredDatabase(
      ctx->env(), request->parent(), request->database_id(), entry,
      restored_proto));
  metadata->SetPendingOperation(persisted_operation);
  absl::Status metadata_status = metadata->Save();
  if (!metadata_status.ok()) {
    return rollback(metadata_status);
  }
  metadata_persisted = true;
  absl::Status metadata_marker_status =
      DatabaseManager::MarkDatabaseMetadataCommitted(config::data_dir(),
                                                     database_uri);
  if (!metadata_marker_status.ok()) {
    return rollback(metadata_marker_status);
  }
  absl::Status operation_status =
      catalog->SaveOperation(persisted_operation);
  if (!operation_status.ok()) {
    return rollback(operation_status);
  }
  operation_persisted = true;

  absl::Status publish_status = creation->Publish();
  if (!publish_status.ok()) {
    return rollback(publish_status);
  }
  database_cleanup->Disarm();
  metadata->RemovePendingOperation(persisted_operation.name());
  absl::Status journal_status = metadata->Save();
  if (!journal_status.ok()) {
    // The durable pending record is intentionally safe to replay: the catalog
    // already contains the exact terminal operation.
    ABSL_LOG(WARNING) << "Failed to clear promoted restore operation journal "
                      << persisted_operation.name() << ": " << journal_status;
  }
  std::filesystem::remove(restore_marker, filesystem_error);
  if (filesystem_error) {
    ABSL_LOG(WARNING) << "Failed to remove completed restore marker "
                      << restore_marker << ": " << filesystem_error.message();
  }
  *response = std::move(persisted_operation);
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, RestoreDatabase);

absl::Status CreateBackupSchedule(
    RequestContext* ctx,
    const database_api::CreateBackupScheduleRequest* request,
    database_api::BackupSchedule* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabase(request->parent(), ctx->env()));
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateResourceId(request->backup_schedule_id(), "Backup schedule ID"));
  if (!request->has_backup_schedule()) {
    return absl::InvalidArgumentError("Backup schedule must be provided");
  }
  database_api::BackupSchedule schedule = request->backup_schedule();
  schedule.set_name(
      MakeBackupScheduleName(request->parent(), request->backup_schedule_id()));
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupScheduleName(schedule.name()));
  if (!ctx->env()->backup_catalog()->persistent()) {
    return absl::FailedPreconditionError(
        "Backup schedules require emulator --data_dir persistent storage");
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupSchedule(&schedule));
  if (ctx->env()->backup_catalog()->ListBackupSchedules(request->parent())
          .size() >= kMaximumBackupSchedulesPerDatabase) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "Database ", request->parent(), " already has the maximum of ",
        kMaximumBackupSchedulesPerDatabase, " backup schedules"));
  }
  const absl::Time now = ctx->env()->clock()->Now();
  GOOGLESQL_ASSIGN_OR_RETURN(const ParsedCron cron, ParseBackupCron(schedule));
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time next_due,
                             NextBackupDue(cron, now));
  GOOGLESQL_ASSIGN_OR_RETURN(*schedule.mutable_update_time(),
                             TimestampToProto(now));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->CreateBackupSchedule(
          schedule, absl::ToUnixSeconds(next_due)));
  *response = schedule;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, CreateBackupSchedule);

absl::Status GetBackupSchedule(
    RequestContext* ctx, const database_api::GetBackupScheduleRequest* request,
    database_api::BackupSchedule* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupScheduleName(request->name()));
  GOOGLESQL_ASSIGN_OR_RETURN(
      *response,
      ctx->env()->backup_catalog()->GetBackupSchedule(request->name()));
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, GetBackupSchedule);

absl::Status ListBackupSchedules(
    RequestContext* ctx,
    const database_api::ListBackupSchedulesRequest* request,
    database_api::ListBackupSchedulesResponse* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateDatabase(request->parent(), ctx->env()));
  int32_t page_size = request->page_size();
  if (page_size <= 0 || page_size > kMaximumPageSize) {
    page_size = kMaximumPageSize;
  }
  std::string cursor;
  if (!request->page_token().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        cursor, ParseBackupSchedulePageToken(request->page_token(),
                                             request->parent()));
  }
  for (const auto& schedule :
       ctx->env()->backup_catalog()->ListBackupSchedules(request->parent())) {
    if (!cursor.empty() && schedule.name() < cursor) {
      continue;
    }
    if (response->backup_schedules_size() >= page_size) {
      response->set_next_page_token(
          BackupSchedulePageToken(request->parent(), schedule.name()));
      break;
    }
    *response->add_backup_schedules() = schedule;
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, ListBackupSchedules);

absl::Status UpdateBackupSchedule(
    RequestContext* ctx,
    const database_api::UpdateBackupScheduleRequest* request,
    database_api::BackupSchedule* response) {
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  if (!request->has_backup_schedule()) {
    return absl::InvalidArgumentError("Backup schedule must be provided");
  }
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateBackupScheduleName(request->backup_schedule().name()));
  if (request->update_mask().paths().empty()) {
    return absl::InvalidArgumentError(
        "Backup schedule update_mask must be provided");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(database_api::BackupSchedule current,
                             ctx->env()->backup_catalog()->GetBackupSchedule(
                                 request->backup_schedule().name()));
  const database_api::BackupSchedule& requested = request->backup_schedule();
  bool spec_changed = false;
  for (const std::string& path : request->update_mask().paths()) {
    if (path == "spec" && requested.has_spec()) {
      *current.mutable_spec() = requested.spec();
      spec_changed = true;
    } else if ((path == "spec.cron_spec" ||
                path == "spec.cron_spec.text") &&
               requested.has_spec() && requested.spec().has_cron_spec()) {
      *current.mutable_spec()->mutable_cron_spec() =
          requested.spec().cron_spec();
      spec_changed = true;
    } else if (path == "retention_duration" &&
               requested.has_retention_duration()) {
      *current.mutable_retention_duration() = requested.retention_duration();
    } else if (path == "encryption_config" &&
               requested.has_encryption_config()) {
      *current.mutable_encryption_config() = requested.encryption_config();
    } else if (path == "full_backup_spec" && requested.has_full_backup_spec()) {
      *current.mutable_full_backup_spec() = requested.full_backup_spec();
    } else if (path == "incremental_backup_spec" &&
               requested.has_incremental_backup_spec()) {
      *current.mutable_incremental_backup_spec() =
          requested.incremental_backup_spec();
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported backup schedule update field: ", path));
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupSchedule(&current));
  const absl::Time now = ctx->env()->clock()->Now();
  std::optional<int64_t> next_due;
  if (spec_changed) {
    GOOGLESQL_ASSIGN_OR_RETURN(const ParsedCron cron, ParseBackupCron(current));
    GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time due,
                               NextBackupDue(cron, now));
    next_due = absl::ToUnixSeconds(due);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(*current.mutable_update_time(),
                             TimestampToProto(now));
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->UpdateBackupSchedule(current, next_due));
  *response = current;
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, UpdateBackupSchedule);

absl::Status DeleteBackupSchedule(
    RequestContext* ctx,
    const database_api::DeleteBackupScheduleRequest* request,
    protobuf_api::Empty* response) {
  GOOGLESQL_RETURN_IF_ERROR(ValidateBackupScheduleName(request->name()));
  absl::MutexLock admin_transaction_lock(
      &ctx->env()->admin_transaction_mutex());
  GOOGLESQL_RETURN_IF_ERROR(
      ctx->env()->backup_catalog()->GetBackupSchedule(request->name()).status());

  MetadataStore* metadata = ctx->env()->metadata_store();
  const std::optional<iam_api::Policy> previous_persisted_policy =
      metadata == nullptr ? std::nullopt
                          : metadata->GetIamPolicy(request->name());
  if (metadata != nullptr) {
    metadata->RemoveIamPolicy(request->name());
    metadata->SetPendingBackupDeletion(request->name());
    absl::Status metadata_status = metadata->Save();
    if (!metadata_status.ok()) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_persisted_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_persisted_policy);
      }
      return metadata_status;
    }
  }

  absl::Status delete_status =
      ctx->env()->backup_catalog()->DeleteBackupSchedule(request->name());
  if (!delete_status.ok()) {
    if (metadata != nullptr) {
      metadata->RemovePendingBackupDeletion(request->name());
      if (previous_persisted_policy.has_value()) {
        metadata->SetIamPolicy(request->name(), *previous_persisted_policy);
      }
      absl::Status rollback_status = metadata->Save();
      if (!rollback_status.ok()) {
        return absl::DataLossError(absl::StrCat(
            delete_status.message(),
            "; failed to roll back backup schedule deletion intent: ",
            rollback_status.message()));
      }
    }
    return delete_status;
  }

  ctx->env()->RemoveIamPolicy(request->name());
  if (metadata != nullptr) {
    metadata->RemovePendingBackupDeletion(request->name());
    GOOGLESQL_RETURN_IF_ERROR(metadata->Save());
  }
  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(DatabaseAdmin, DeleteBackupSchedule);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
