#include "azookey/learning/TypoLearningStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>

#include "azookey/learning/FileLock.h"

namespace azookey::learning {

namespace {

// --- SQLite memory: every block is overwritten with zeros before it is freed
// (section 12.8 "メモリ上の平文の消去"), including the deserialized database
// and the buffers sqlite3_serialize returns. Each block carries its size in a
// header that keeps malloc's 16-byte alignment.
constexpr size_t kAllocationHeader = 16;

void ZeroBytes(void* data, size_t size) noexcept {
  volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

int ZeroingSize(void* block) {
  if (!block) return 0;
  int size = 0;
  std::memcpy(&size, static_cast<unsigned char*>(block) - kAllocationHeader, sizeof(size));
  return size;
}

void* ZeroingMalloc(int size) {
  if (size <= 0) return nullptr;
  auto* base =
      static_cast<unsigned char*>(std::malloc(kAllocationHeader + static_cast<size_t>(size)));
  if (!base) return nullptr;
  std::memcpy(base, &size, sizeof(size));
  return base + kAllocationHeader;
}

void ZeroingFree(void* block) {
  if (!block) return;
  auto* base = static_cast<unsigned char*>(block) - kAllocationHeader;
  ZeroBytes(base, kAllocationHeader + static_cast<size_t>(ZeroingSize(block)));
  std::free(base);
}

// Never realloc in place: the old block is copied, zeroed and freed, so a
// growing database leaves no plaintext behind in released memory.
void* ZeroingRealloc(void* block, int size) {
  if (!block) return ZeroingMalloc(size);
  if (size <= 0) {
    ZeroingFree(block);
    return nullptr;
  }
  void* fresh = ZeroingMalloc(size);
  if (!fresh) return nullptr;
  std::memcpy(fresh, block, static_cast<size_t>((std::min)(ZeroingSize(block), size)));
  ZeroingFree(block);
  return fresh;
}

int ZeroingRoundup(int size) { return (size + 7) & ~7; }
int ZeroingInit(void*) { return SQLITE_OK; }
void ZeroingShutdown(void*) {}

// sqlite3_config must run once per process before any other SQLite call.
bool ConfigureSqlite() {
  static const bool configured = [] {
    static const sqlite3_mem_methods methods = {
        ZeroingMalloc,  ZeroingFree, ZeroingRealloc,  ZeroingSize,
        ZeroingRoundup, ZeroingInit, ZeroingShutdown, nullptr,
    };
    if (sqlite3_config(SQLITE_CONFIG_MALLOC, &methods) != SQLITE_OK) {
      std::cerr << "TypoLearningStore: SQLite was initialized before its allocator was set\n";
      return false;
    }
    return sqlite3_initialize() == SQLITE_OK;
  }();
  return configured;
}

struct StatementDeleter {
  void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

struct SqliteBuffer {
  unsigned char* data{nullptr};
  sqlite3_int64 size{0};
  SqliteBuffer() = default;
  SqliteBuffer(const SqliteBuffer&) = delete;
  SqliteBuffer& operator=(const SqliteBuffer&) = delete;
  ~SqliteBuffer() { sqlite3_free(data); }
  std::string_view view() const {
    return {reinterpret_cast<const char*>(data), static_cast<size_t>(size)};
  }
};

constexpr const char* kSchemaSql =
    "CREATE TABLE typo_patterns ("
    "  id INTEGER PRIMARY KEY,"
    "  observed_pattern TEXT NOT NULL,"
    "  intended_pattern TEXT NOT NULL,"
    "  typo_type TEXT NOT NULL,"
    "  keyboard_layout TEXT,"
    "  confidence REAL DEFAULT 0.5,"
    "  accept_count INTEGER DEFAULT 0,"
    "  reject_count INTEGER DEFAULT 0,"
    "  last_used_at INTEGER,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE typo_events ("
    "  id INTEGER PRIMARY KEY,"
    "  observed_reading TEXT NOT NULL,"
    "  corrected_reading TEXT NOT NULL,"
    "  selected_surface TEXT,"
    "  typo_type TEXT,"
    "  pattern_id INTEGER,"
    "  app_name TEXT,"
    "  left_context_hash TEXT,"
    "  event_type TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE typo_settings ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE typo_reading_pairs ("
    "  wrong_reading TEXT NOT NULL,"
    "  correct_reading TEXT NOT NULL,"
    "  count INTEGER NOT NULL,"
    "  last_updated_at INTEGER NOT NULL,"
    "  PRIMARY KEY (wrong_reading, correct_reading)"
    ");"
    "PRAGMA user_version = 1;";

// Every column the store reads; a database without them is corrupt.
constexpr const char* kSchemaProbes[] = {
    "SELECT id, observed_pattern, intended_pattern, typo_type, keyboard_layout, confidence, "
    "accept_count, reject_count, last_used_at, created_at, updated_at FROM typo_patterns LIMIT 0",
    "SELECT id, observed_reading, corrected_reading, selected_surface, typo_type, pattern_id, "
    "app_name, left_context_hash, event_type, created_at FROM typo_events LIMIT 0",
    "SELECT key, value, updated_at FROM typo_settings LIMIT 0",
    "SELECT wrong_reading, correct_reading, count, last_updated_at FROM typo_reading_pairs LIMIT 0",
};

constexpr std::string_view kOriginKey = "store_origin";
constexpr std::string_view kEventAccept = "typo_accept";
constexpr std::string_view kEventReject = "typo_reject";
constexpr uint32_t kMaxCount = (std::numeric_limits<uint32_t>::max)();
constexpr size_t kMaxKeyboardLayoutBytes = 16;
constexpr size_t kMaxAppNameBytes = 260;
constexpr size_t kMaxSurfaceBytes = 1024;

sqlite3_int64 ToSql(uint64_t value) {
  return static_cast<sqlite3_int64>(
      (std::min)(value, static_cast<uint64_t>((std::numeric_limits<sqlite3_int64>::max)())));
}

uint64_t FromSql(sqlite3_int64 value) { return value < 0 ? 0 : static_cast<uint64_t>(value); }

uint32_t CountFromSql(sqlite3_int64 value) {
  if (value < 0) return 0;
  return static_cast<uint32_t>((std::min)(static_cast<uint64_t>(value), uint64_t{kMaxCount}));
}

uint32_t SaturatingAdd(uint32_t lhs, uint32_t rhs) {
  return static_cast<uint32_t>((std::min)(uint64_t{lhs} + rhs, uint64_t{kMaxCount}));
}

void BindText(sqlite3_stmt* statement, int index, std::string_view value) {
  sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

void BindOptionalText(sqlite3_stmt* statement, int index, std::string_view value) {
  if (value.empty()) {
    sqlite3_bind_null(statement, index);
  } else {
    BindText(statement, index, value);
  }
}

std::string ColumnText(sqlite3_stmt* statement, int column) {
  const auto* text = sqlite3_column_text(statement, column);
  const int size = sqlite3_column_bytes(statement, column);
  return text ? std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(size))
              : std::string();
}

std::string_view OriginName(TypoStoreOrigin origin) {
  switch (origin) {
    case TypoStoreOrigin::V1Migrated:
      return "v1_migrated";
    case TypoStoreOrigin::Empty:
      return "empty";
    case TypoStoreOrigin::Rebuilt:
      return "rebuilt";
    case TypoStoreOrigin::None:
      break;
  }
  return {};
}

enum class FileState { Missing, Present, Unreadable };

bool PathExists(const std::filesystem::path& path, bool& exists) {
  std::error_code ec;
  exists = std::filesystem::exists(path, ec);
  return !ec;
}

// Reads a stored (encrypted) file as it is on disk.
bool ReadFileBytes(const std::filesystem::path& path, std::string& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}

// Section 12.8 "パス": the state of a logical path from ReadProtectedText. For
// the v1 store, a lone M34 plaintext backup counts as missing.
FileState ReadState(const std::filesystem::path& logical, const ByteCrypto& crypto,
                    std::string& text, bool lone_m34_backup_is_missing) {
  switch (ReadProtectedText(logical, crypto, text)) {
    case ProtectedFileSource::Missing:
      return FileState::Missing;
    case ProtectedFileSource::Encrypted:
    case ProtectedFileSource::Plaintext:
      return FileState::Present;
    case ProtectedFileSource::Error:
      break;
  }
  SecureErase(text);
  if (lone_m34_backup_is_missing) {
    auto m34_backup = logical;
    m34_backup += ".bak";
    bool encrypted = false;
    bool plain = false;
    bool backup = false;
    if (PathExists(EncryptedPathFor(logical), encrypted) && PathExists(logical, plain) &&
        PathExists(m34_backup, backup) && !encrypted && !plain && backup) {
      return FileState::Missing;
    }
  }
  return FileState::Unreadable;
}

// `store` is constructed on the v1 store path. Load finishes the M34
// plaintext migration before the pairs move on.
FileState LoadV1Store(const std::filesystem::path& path, const ByteCrypto& crypto,
                      TypoCorrectionStore& store) {
  std::string text;
  const auto state = ReadState(path, crypto, text, true);
  SecureErase(text);
  if (state != FileState::Present) return state;
  return store.Load() ? FileState::Present : FileState::Unreadable;
}

// Rows the v1 parser cannot read stay in the backup; the readable ones move.
// The backup is never migrated or rewritten here.
FileState LoadV1Backup(const std::filesystem::path& path, const ByteCrypto& crypto,
                       TypoCorrectionStore& store) {
  std::string text;
  const auto state = ReadState(path, crypto, text, false);
  if (state == FileState::Present) store.LoadText(text);
  SecureErase(text);
  return state;
}

// Removes both forms of a logical path (never the M34 plaintext .bak) under
// the lock WriteProtectedText takes.
bool RemoveProtectedFile(const std::filesystem::path& logical) {
  const auto encrypted = EncryptedPathFor(logical);
  FileLockFailure failure;
  auto lock = AcquireExclusiveFileLockForPath(encrypted, failure);
  if (!lock) return false;
  std::error_code ec;
  std::filesystem::remove(encrypted, ec);
  if (ec) return false;
  std::filesystem::remove(logical, ec);
  return !ec;
}

bool IsValidKeyboardLayout(std::string_view layout) {
  if (layout.size() > kMaxKeyboardLayoutBytes) return false;
  return std::all_of(layout.begin(), layout.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
  });
}

bool IsValidContextHash(std::string_view hash) {
  if (hash.empty()) return true;
  return hash.size() == 8 && std::all_of(hash.begin(), hash.end(), [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool HasControlCharacter(std::string_view text) {
  return std::any_of(text.begin(), text.end(),
                     [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; });
}

bool IsValidEventReading(const std::string& reading) {
  if (reading.empty() || HasControlCharacter(reading)) return false;
  return TypoCorrectionStore::Utf8CharLength(reading) <= kTypoCorrectionMaxReadingLength;
}

bool IsValidPatternKey(const TypoPatternRecord& record) {
  return IsValidTypoPattern(record.observed_pattern) &&
         IsValidTypoPattern(record.intended_pattern) && IsKnownTypoType(record.typo_type) &&
         IsValidKeyboardLayout(record.keyboard_layout);
}

// The TSV escaping of the v1 store, for the typo_patterns backup item.
std::string EscapeField(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '#':
        escaped += "\\#";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string UnescapeField(std::string_view value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 == value.size()) {
      unescaped.push_back(value[i]);
      continue;
    }
    const char escaped = value[++i];
    switch (escaped) {
      case '\\':
        unescaped.push_back('\\');
        break;
      case 't':
        unescaped.push_back('\t');
        break;
      case 'n':
        unescaped.push_back('\n');
        break;
      case 'r':
        unescaped.push_back('\r');
        break;
      case '#':
        unescaped.push_back('#');
        break;
      default:
        unescaped.push_back('\\');
        unescaped.push_back(escaped);
        break;
    }
  }
  return unescaped;
}

template <typename T>
bool ParseUnsignedField(std::string_view token, T& value) {
  if (token.empty()) return false;
  T parsed{};
  const char* const last = token.data() + token.size();
  const auto result = std::from_chars(token.data(), last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) return false;
  value = parsed;
  return true;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> fields;
  size_t begin = 0;
  while (true) {
    const size_t tab = line.find('\t', begin);
    fields.push_back(line.substr(begin, tab == std::string_view::npos ? tab : tab - begin));
    if (tab == std::string_view::npos) break;
    begin = tab + 1;
  }
  return fields;
}

}  // namespace

// --- The in-memory database --------------------------------------------------

class TypoLearningStore::Database {
 public:
  enum class OpenResult { Ok, Corrupt, TooNew, Failure };

  explicit Database(sqlite3* db) : db_(db) {}
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ~Database() { sqlite3_close_v2(db_); }

  sqlite3* get() const { return db_; }

  // A connection to an empty in-memory database with the defensive settings.
  static std::unique_ptr<Database> Open() {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(":memory:", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
      sqlite3_close_v2(raw);
      return nullptr;
    }
    auto db = std::make_unique<Database>(raw);
    // Lookaside slots are reused without going through xFree, so they could
    // keep a bound reading until the connection closes; turn them off.
    if (sqlite3_db_config(raw, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 0, 0) != SQLITE_OK) {
      return nullptr;
    }
    int ignored = 0;
    sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, &ignored);
    sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &ignored);
    sqlite3_db_config(raw, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &ignored);
    sqlite3_db_config(raw, SQLITE_DBCONFIG_ENABLE_VIEW, 0, &ignored);
    return db;
  }

  static std::unique_ptr<Database> CreateEmpty() {
    auto db = Open();
    if (!db || !db->ApplyPragmas() || !db->Exec(kSchemaSql)) return nullptr;
    return db;
  }

  // Section 12.8 decisions 3 to 5 on decrypted bytes, which are erased here.
  static OpenResult FromBytes(std::string& bytes, std::unique_ptr<Database>& out,
                              std::string& reason) {
    out.reset();
    if (bytes.empty()) {
      reason = "empty";
      return OpenResult::Corrupt;
    }
    auto db = Open();
    auto* buffer = db ? static_cast<unsigned char*>(sqlite3_malloc64(bytes.size())) : nullptr;
    if (!buffer) {
      SecureErase(bytes);
      return OpenResult::Failure;
    }
    std::memcpy(buffer, bytes.data(), bytes.size());
    const auto size = static_cast<sqlite3_int64>(bytes.size());
    SecureErase(bytes);
    // With FREEONCLOSE, SQLite frees the buffer even when this fails.
    if (sqlite3_deserialize(db->get(), "main", buffer, size, size,
                            SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE) !=
        SQLITE_OK) {
      reason = "deserialize";
      return OpenResult::Corrupt;
    }
    // A buffer that is not a database fails on its first read.
    const auto version = db->QueryInt("PRAGMA user_version");
    if (!version) {
      reason = "deserialize";
      return OpenResult::Corrupt;
    }
    if (*version > kTypoLearningSchemaVersion) return OpenResult::TooNew;
    if (*version < 1) {
      reason = "schema";
      return OpenResult::Corrupt;
    }
    if (!db->ApplyPragmas()) {
      reason = "read";
      return OpenResult::Corrupt;
    }
    if (!db->IntegrityOk()) {
      reason = "integrity";
      return OpenResult::Corrupt;
    }
    for (const char* probe : kSchemaProbes) {
      if (!db->Prepare(probe)) {
        reason = "schema";
        return OpenResult::Corrupt;
      }
    }
    out = std::move(db);
    return OpenResult::Ok;
  }

  bool Exec(const char* sql) const {
    return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  }

  Statement Prepare(std::string_view sql) const {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr) !=
        SQLITE_OK) {
      sqlite3_finalize(statement);
      return Statement{};
    }
    return Statement(statement);
  }

  std::optional<sqlite3_int64> QueryInt(const char* sql) const {
    auto statement = Prepare(sql);
    if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int64(statement.get(), 0);
  }

  bool Serialize(SqliteBuffer& buffer) const {
    buffer.data = sqlite3_serialize(db_, "main", &buffer.size, 0);
    return buffer.data != nullptr || buffer.size == 0;
  }

  TypoStoreOrigin Origin() const {
    auto statement = Prepare("SELECT value FROM typo_settings WHERE key = ?1");
    if (!statement) return TypoStoreOrigin::None;
    BindText(statement.get(), 1, kOriginKey);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return TypoStoreOrigin::None;
    const auto value = ColumnText(statement.get(), 0);
    for (const auto origin :
         {TypoStoreOrigin::V1Migrated, TypoStoreOrigin::Empty, TypoStoreOrigin::Rebuilt}) {
      if (value == OriginName(origin)) return origin;
    }
    return TypoStoreOrigin::None;
  }

  bool SetOrigin(TypoStoreOrigin origin, uint64_t now) const {
    auto statement = Prepare(
        "INSERT INTO typo_settings (key, value, updated_at) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at");
    if (!statement) return false;
    BindText(statement.get(), 1, kOriginKey);
    BindText(statement.get(), 2, OriginName(origin));
    sqlite3_bind_int64(statement.get(), 3, ToSql(now));
    return sqlite3_step(statement.get()) == SQLITE_DONE;
  }

  // Adds a pair, or merges it with an existing one by `merge`.
  enum class PairMerge { Replace, ColumnMax, Add };
  bool UpsertPair(const std::string& wrong, const std::string& correct, uint32_t count,
                  uint64_t last_updated, PairMerge merge) const {
    const char* sql = nullptr;
    switch (merge) {
      case PairMerge::Replace:
        sql =
            "INSERT INTO typo_reading_pairs VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(wrong_reading, correct_reading) DO UPDATE SET "
            "count = excluded.count, last_updated_at = excluded.last_updated_at";
        break;
      case PairMerge::ColumnMax:
        sql =
            "INSERT INTO typo_reading_pairs VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(wrong_reading, correct_reading) DO UPDATE SET "
            "count = MAX(count, excluded.count), "
            "last_updated_at = MAX(last_updated_at, excluded.last_updated_at)";
        break;
      case PairMerge::Add:
        sql =
            "INSERT INTO typo_reading_pairs VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(wrong_reading, correct_reading) DO UPDATE SET "
            "count = MIN(count + excluded.count, 4294967295), "
            "last_updated_at = MAX(last_updated_at, excluded.last_updated_at)";
        break;
    }
    auto statement = Prepare(sql);
    if (!statement) return false;
    BindText(statement.get(), 1, wrong);
    BindText(statement.get(), 2, correct);
    sqlite3_bind_int64(statement.get(), 3, count);
    sqlite3_bind_int64(statement.get(), 4, ToSql(last_updated));
    return sqlite3_step(statement.get()) == SQLITE_DONE;
  }

  std::optional<TypoCorrectionRecord> FindPair(const std::string& wrong,
                                               const std::string& correct) const {
    auto statement = Prepare(
        "SELECT count, last_updated_at FROM typo_reading_pairs "
        "WHERE wrong_reading = ?1 AND correct_reading = ?2");
    if (!statement) return std::nullopt;
    BindText(statement.get(), 1, wrong);
    BindText(statement.get(), 2, correct);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return TypoCorrectionRecord{CountFromSql(sqlite3_column_int64(statement.get(), 0)),
                                FromSql(sqlite3_column_int64(statement.get(), 1))};
  }

  std::vector<TypoCorrectionEntry> Pairs() const {
    std::vector<TypoCorrectionEntry> entries;
    auto statement = Prepare(
        "SELECT wrong_reading, correct_reading, count, last_updated_at FROM typo_reading_pairs "
        "ORDER BY wrong_reading, correct_reading");
    if (!statement) return entries;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
      entries.push_back(TypoCorrectionEntry{
          ColumnText(statement.get(), 0), ColumnText(statement.get(), 1),
          TypoCorrectionRecord{CountFromSql(sqlite3_column_int64(statement.get(), 2)),
                               FromSql(sqlite3_column_int64(statement.get(), 3))}});
    }
    return entries;
  }

  std::optional<sqlite3_int64> CountRows(const char* table) const {
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    return QueryInt(sql.c_str());
  }

  std::vector<TypoPatternRecord> PatternRows(std::optional<int64_t> id) const {
    std::vector<TypoPatternRecord> records;
    auto statement =
        Prepare(id ? "SELECT id, observed_pattern, intended_pattern, typo_type, keyboard_layout, "
                     "confidence, accept_count, reject_count, last_used_at, created_at, updated_at "
                     "FROM typo_patterns WHERE id = ?1"
                   : "SELECT id, observed_pattern, intended_pattern, typo_type, keyboard_layout, "
                     "confidence, accept_count, reject_count, last_used_at, created_at, updated_at "
                     "FROM typo_patterns ORDER BY id");
    if (!statement) return records;
    if (id) sqlite3_bind_int64(statement.get(), 1, *id);
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
      TypoPatternRecord record;
      record.id = sqlite3_column_int64(statement.get(), 0);
      record.observed_pattern = ColumnText(statement.get(), 1);
      record.intended_pattern = ColumnText(statement.get(), 2);
      record.typo_type = ColumnText(statement.get(), 3);
      record.keyboard_layout = ColumnText(statement.get(), 4);
      record.confidence = sqlite3_column_double(statement.get(), 5);
      record.accept_count = CountFromSql(sqlite3_column_int64(statement.get(), 6));
      record.reject_count = CountFromSql(sqlite3_column_int64(statement.get(), 7));
      if (sqlite3_column_type(statement.get(), 8) != SQLITE_NULL) {
        record.last_used_at = FromSql(sqlite3_column_int64(statement.get(), 8));
      }
      record.created_at = FromSql(sqlite3_column_int64(statement.get(), 9));
      record.updated_at = FromSql(sqlite3_column_int64(statement.get(), 10));
      records.push_back(std::move(record));
    }
    return records;
  }

  std::optional<TypoPatternRecord> FindPattern(const TypoPatternRecord& key) const {
    auto statement = Prepare(
        "SELECT id FROM typo_patterns WHERE observed_pattern = ?1 AND intended_pattern = ?2 "
        "AND typo_type = ?3 AND keyboard_layout IS ?4");
    if (!statement) return std::nullopt;
    BindText(statement.get(), 1, key.observed_pattern);
    BindText(statement.get(), 2, key.intended_pattern);
    BindText(statement.get(), 3, key.typo_type);
    BindOptionalText(statement.get(), 4, key.keyboard_layout);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    auto rows = PatternRows(sqlite3_column_int64(statement.get(), 0));
    if (rows.empty()) return std::nullopt;
    return std::move(rows.front());
  }

  // Writes `record` (inserting when its id is 0) with its confidence
  // recomputed at `now`; returns the id.
  std::optional<int64_t> WritePattern(TypoPatternRecord record, uint64_t now) const {
    record.confidence = ComputeTypoConfidence(record.accept_count, record.reject_count,
                                              record.last_used_at, now, false);
    auto statement =
        Prepare(record.id == 0
                    ? "INSERT INTO typo_patterns (observed_pattern, intended_pattern, typo_type, "
                      "keyboard_layout, confidence, accept_count, reject_count, last_used_at, "
                      "created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"
                    : "UPDATE typo_patterns SET observed_pattern = ?1, intended_pattern = ?2, "
                      "typo_type = ?3, keyboard_layout = ?4, confidence = ?5, accept_count = ?6, "
                      "reject_count = ?7, last_used_at = ?8, created_at = ?9, updated_at = ?10 "
                      "WHERE id = ?11");
    if (!statement) return std::nullopt;
    BindText(statement.get(), 1, record.observed_pattern);
    BindText(statement.get(), 2, record.intended_pattern);
    BindText(statement.get(), 3, record.typo_type);
    BindOptionalText(statement.get(), 4, record.keyboard_layout);
    sqlite3_bind_double(statement.get(), 5, record.confidence);
    sqlite3_bind_int64(statement.get(), 6, record.accept_count);
    sqlite3_bind_int64(statement.get(), 7, record.reject_count);
    if (record.last_used_at) {
      sqlite3_bind_int64(statement.get(), 8, ToSql(*record.last_used_at));
    } else {
      sqlite3_bind_null(statement.get(), 8);
    }
    sqlite3_bind_int64(statement.get(), 9, ToSql(record.created_at));
    sqlite3_bind_int64(statement.get(), 10, ToSql(record.updated_at));
    if (record.id != 0) sqlite3_bind_int64(statement.get(), 11, record.id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return std::nullopt;
    return record.id != 0 ? record.id : sqlite3_last_insert_rowid(db_);
  }

  // Section 12.8 "保持上限": oldest rows first, by age and by count.
  bool ApplyRetention(const TypoLearningLimits& limits, uint64_t now) const {
    const uint64_t cutoff = now > limits.max_age_sec ? now - limits.max_age_sec : 0;
    const auto max_rows = static_cast<sqlite3_int64>(
        (std::min)(limits.max_rows, static_cast<size_t>((std::numeric_limits<int>::max)())));
    for (const char* sql : {
             "DELETE FROM typo_events WHERE created_at < ?1",
             "DELETE FROM typo_events WHERE id NOT IN (SELECT id FROM typo_events "
             "ORDER BY created_at DESC, id DESC LIMIT ?2)",
             "DELETE FROM typo_reading_pairs WHERE last_updated_at < ?1",
             "DELETE FROM typo_reading_pairs WHERE rowid NOT IN (SELECT rowid FROM "
             "typo_reading_pairs ORDER BY last_updated_at DESC, rowid DESC LIMIT ?2)",
         }) {
      auto statement = Prepare(sql);
      if (!statement) return false;
      const int cutoff_index = sqlite3_bind_parameter_index(statement.get(), "?1");
      const int rows_index = sqlite3_bind_parameter_index(statement.get(), "?2");
      if (cutoff_index > 0) sqlite3_bind_int64(statement.get(), cutoff_index, ToSql(cutoff));
      if (rows_index > 0) sqlite3_bind_int64(statement.get(), rows_index, max_rows);
      if (sqlite3_step(statement.get()) != SQLITE_DONE) return false;
    }
    return true;
  }

 private:
  bool ApplyPragmas() const {
    // No journal, WAL or temporary file on disk (section 12.8 "同時書き込み"),
    // and deleted rows are overwritten rather than left in free pages.
    return Exec("PRAGMA journal_mode = MEMORY") && Exec("PRAGMA secure_delete = ON");
  }

  bool IntegrityOk() const {
    auto statement = Prepare("PRAGMA integrity_check");
    if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW) return false;
    if (ColumnText(statement.get(), 0) != "ok") return false;
    return sqlite3_step(statement.get()) == SQLITE_DONE;
  }

  sqlite3* db_;
};

// --- Paths -----------------------------------------------------------------

TypoLearningPaths TypoLearningPathsFor(const std::filesystem::path& v1_store) {
  TypoLearningPaths paths;
  paths.v1_store = v1_store;
  paths.v2_store = v1_store.parent_path() / std::string(kTypoLearningV2FileName);
  auto backup = v1_store.parent_path() / v1_store.stem();
  backup += ".v1";
  backup += v1_store.extension();
  backup += ".bak";
  paths.v1_backup = backup;
  return paths;
}

// --- Store -----------------------------------------------------------------

TypoLearningStore::TypoLearningStore(std::filesystem::path v1_store, const ByteCrypto* crypto,
                                     TypoLearningLimits limits)
    : paths_(TypoLearningPathsFor(v1_store)),
      crypto_(crypto ? crypto : &DpapiCrypto()),
      limits_(limits) {}

TypoLearningStore::~TypoLearningStore() = default;

TypoStoreOrigin TypoLearningStore::origin() const {
  return db_ ? db_->Origin() : TypoStoreOrigin::None;
}

bool TypoLearningStore::StartEmpty(TypoStoreOrigin origin, uint64_t now) {
  db_ = Database::CreateEmpty();
  if (!db_) return false;
  return origin == TypoStoreOrigin::None || db_->SetOrigin(origin, now);
}

void TypoLearningStore::EnterBlocked(TypoLearningLoadResult& result, TypoLearningLoadIssue issue) {
  v1_.reset();
  db_.reset();
  if (issue != TypoLearningLoadIssue::SqliteUnavailable) StartEmpty(TypoStoreOrigin::None, 0);
  mode_ = TypoLearningMode::Blocked;
  result.mode = mode_;
  result.issue = issue;
}

void TypoLearningStore::EnterV1Fallback(std::unique_ptr<TypoCorrectionStore> v1,
                                        TypoLearningLoadResult& result) {
  db_.reset();
  v1_ = std::move(v1);
  mode_ = TypoLearningMode::V1Fallback;
  result.mode = mode_;
  result.issue = TypoLearningLoadIssue::MigrationFailed;
}

void TypoLearningStore::EnterV2(TypoLearningLoadResult& result) {
  v1_.reset();
  mode_ = TypoLearningMode::V2;
  result.mode = mode_;
}

bool TypoLearningStore::WriteDatabase(bool apply_limits, uint64_t now) {
  return db_ && WriteDatabase(*db_, apply_limits, now);
}

bool TypoLearningStore::WriteDatabase(Database& db, bool apply_limits, uint64_t now) const {
  if (apply_limits && db.Origin() != TypoStoreOrigin::None && !db.ApplyRetention(limits_, now)) {
    return false;
  }
  SqliteBuffer buffer;
  if (!db.Serialize(buffer)) return false;
  return WriteProtectedText(paths_.v2_store, buffer.view(), *crypto_);
}

// Migration steps 1 to 5 (section 12.8 "v1 からの移行").
bool TypoLearningStore::MigrateFrom(const TypoCorrectionStore& source, bool from_v1_store,
                                    uint64_t now, TypoLearningLoadResult& result) {
  const auto pairs = source.All();
  bool written = false;
  const auto fail = [&] {
    db_.reset();
    // v2 is not created by a failed migration; an unmarked v2 that cannot be
    // removed is redone by decision 6 on the next load.
    if (written) RemoveProtectedFile(paths_.v2_store);
    return false;
  };
  // 1. Every pair as it is, with no retention limit.
  if (!StartEmpty(TypoStoreOrigin::None, now) || !db_->Exec("BEGIN")) return fail();
  for (const auto& pair : pairs) {
    if (!db_->UpsertPair(pair.wrong_reading, pair.correct_reading, pair.record.count,
                         pair.record.last_updated_epoch_sec, Database::PairMerge::Replace)) {
      db_->Exec("ROLLBACK");
      return fail();
    }
  }
  if (!db_->Exec("COMMIT")) return fail();
  // 2. Save without the mark.
  written = true;
  if (!WriteDatabase(false, now)) return fail();
  // 3. Read it back: decrypt, integrity_check, row count.
  {
    std::string text;
    if (ReadProtectedText(paths_.v2_store, *crypto_, text) != ProtectedFileSource::Encrypted) {
      SecureErase(text);
      return fail();
    }
    std::unique_ptr<Database> readback;
    std::string reason;
    if (Database::FromBytes(text, readback, reason) != Database::OpenResult::Ok) return fail();
    const auto rows = readback->CountRows("typo_reading_pairs");
    if (!rows || static_cast<size_t>(*rows) != pairs.size()) return fail();
  }
  // 4. Mark and save again.
  if (!db_->SetOrigin(TypoStoreOrigin::V1Migrated, now) || !WriteDatabase(false, now)) {
    return fail();
  }
  result.migrated = true;
  result.migrated_pairs = pairs.size();
  std::cerr << "TypoLearningStore: migrated " << pairs.size() << " v1 pairs\n";
  // 5. Move the v1 store aside; a backup source is left in place.
  if (from_v1_store) SettleV1Store(result);
  return true;
}

// Migration step 5: the v1 store becomes (or is appended to) the v1 backup,
// encrypted as it is.
bool TypoLearningStore::SettleV1Store(TypoLearningLoadResult& result) {
  const auto pending = [&](bool unreadable) {
    result.v1_store_pending = true;
    if (unreadable) result.v1_side_unreadable = true;
    return false;
  };
  std::string backup_text;
  const auto backup_state = ReadState(paths_.v1_backup, *crypto_, backup_text, false);
  if (backup_state == FileState::Unreadable) return pending(true);

  const auto v1_encrypted = EncryptedPathFor(paths_.v1_store);
  const auto backup_encrypted = EncryptedPathFor(paths_.v1_backup);
  if (backup_state == FileState::Missing) {
    FileLockFailure failure;
    auto v1_lock = AcquireExclusiveFileLockForPath(v1_encrypted, failure);
    auto backup_lock =
        v1_lock ? AcquireExclusiveFileLockForPath(backup_encrypted, failure) : std::nullopt;
    if (!backup_lock) return pending(false);
    // rename replaces an existing target, so the backup is checked again under
    // its lock: one that appeared meanwhile is never overwritten.
    bool appeared = true;
    if (!PathExists(backup_encrypted, appeared) || appeared) return pending(false);
    std::error_code ec;
    std::filesystem::rename(v1_encrypted, backup_encrypted, ec);
    return ec ? pending(false) : true;
  }

  // The backup exists: keep it and append the v1 store's text, so the v1
  // parser (later rows win, later '#' lines are comments) reads the v1 store's
  // values while every unparsable row survives as bytes.
  std::string v1_text;
  if (ReadProtectedText(paths_.v1_store, *crypto_, v1_text) != ProtectedFileSource::Encrypted) {
    SecureErase(v1_text);
    SecureErase(backup_text);
    return pending(false);
  }
  if (!backup_text.empty() && backup_text.back() != '\n') backup_text.push_back('\n');
  backup_text += v1_text;
  SecureErase(v1_text);
  const bool appended = WriteProtectedText(paths_.v1_backup, backup_text, *crypto_);
  SecureErase(backup_text);
  if (!appended || !RemoveProtectedFile(paths_.v1_store)) return pending(false);
  return true;
}

TypoLearningLoadResult TypoLearningStore::Load(uint64_t now_epoch_sec) {
  TypoLearningLoadResult result;
  v1_.reset();
  db_.reset();
  mode_ = TypoLearningMode::Blocked;
  if (!ConfigureSqlite()) {
    EnterBlocked(result, TypoLearningLoadIssue::SqliteUnavailable);
    return result;
  }

  std::string text;
  const auto v2_state = ReadState(paths_.v2_store, *crypto_, text, false);
  if (v2_state == FileState::Unreadable) {
    // Decision 2: neither v2 nor v1 is written.
    EnterBlocked(result, TypoLearningLoadIssue::V2Unreadable);
    return result;
  }
  if (v2_state == FileState::Missing) {
    LoadWithoutV2(now_epoch_sec, result);
    return result;
  }

  std::unique_ptr<Database> loaded;
  std::string reason;
  switch (Database::FromBytes(text, loaded, reason)) {
    case Database::OpenResult::Ok:
      break;
    case Database::OpenResult::TooNew:
      // Decision 4: written by a newer Host; never overwritten.
      EnterBlocked(result, TypoLearningLoadIssue::V2TooNew);
      return result;
    case Database::OpenResult::Corrupt:
      RecoverFromCorruption(now_epoch_sec, reason, result);
      return result;
    case Database::OpenResult::Failure:
      EnterBlocked(result, TypoLearningLoadIssue::SqliteUnavailable);
      return result;
  }
  db_ = std::move(loaded);

  if (db_->Origin() == TypoStoreOrigin::None) {
    RedoUnverifiedMigration(now_epoch_sec, result);
    return result;
  }

  // Decision 7: a v1 store left by an unfinished step 5 (or written by an
  // older Host since) is folded in by column-wise maximum, then moved aside.
  auto v1 = std::make_unique<TypoCorrectionStore>(paths_.v1_store, crypto_);
  const auto v1_state = LoadV1Store(paths_.v1_store, *crypto_, *v1);
  if (v1_state == FileState::Present) {
    bool imported = db_->Exec("BEGIN");
    for (const auto& pair : v1->All()) {
      imported = imported && db_->UpsertPair(pair.wrong_reading, pair.correct_reading,
                                             pair.record.count, pair.record.last_updated_epoch_sec,
                                             Database::PairMerge::ColumnMax);
    }
    imported = imported ? db_->Exec("COMMIT") : (db_->Exec("ROLLBACK"), false);
    if (imported && WriteDatabase(true, now_epoch_sec)) {
      SettleV1Store(result);
    } else {
      result.v1_store_pending = true;
    }
  } else if (v1_state == FileState::Unreadable) {
    result.v1_store_pending = true;
    result.v1_side_unreadable = true;
  } else {
    // Only the backup is left on the v1 side; a reading-pair forget needs it.
    std::string backup_text;
    if (ReadState(paths_.v1_backup, *crypto_, backup_text, false) == FileState::Unreadable) {
      result.v1_side_unreadable = true;
    }
    SecureErase(backup_text);
  }
  EnterV2(result);
  return result;
}

// Decision 1: there is no v2 store.
void TypoLearningStore::LoadWithoutV2(uint64_t now, TypoLearningLoadResult& result) {
  auto v1 = std::make_unique<TypoCorrectionStore>(paths_.v1_store, crypto_);
  switch (LoadV1Store(paths_.v1_store, *crypto_, *v1)) {
    case FileState::Present:
      if (MigrateFrom(*v1, true, now, result)) {
        EnterV2(result);
      } else {
        EnterV1Fallback(std::move(v1), result);
      }
      return;
    case FileState::Unreadable:
      // Creating v2 now would hide a v1 store that may become readable later.
      EnterBlocked(result, TypoLearningLoadIssue::V1StoreUnreadable);
      return;
    case FileState::Missing:
      break;
  }
  TypoCorrectionStore backup(paths_.v1_backup, crypto_);
  switch (LoadV1Backup(paths_.v1_backup, *crypto_, backup)) {
    case FileState::Present:
      // No "v1 のまま読む" here: a new v1 store would be taken for the backup.
      if (MigrateFrom(backup, false, now, result)) {
        EnterV2(result);
      } else {
        EnterBlocked(result, TypoLearningLoadIssue::MigrationFailed);
      }
      return;
    case FileState::Unreadable:
      EnterBlocked(result, TypoLearningLoadIssue::V1BackupUnreadable);
      return;
    case FileState::Missing:
      break;
  }
  if (!StartEmpty(TypoStoreOrigin::Empty, now)) {
    EnterBlocked(result, TypoLearningLoadIssue::SqliteUnavailable);
    return;
  }
  EnterV2(result);
}

// Decisions 3 and 5: copy the encrypted file aside once, then migrate again
// from whichever v1 file exists, or start over empty. Learning continues.
void TypoLearningStore::RecoverFromCorruption(uint64_t now, const std::string& reason,
                                              TypoLearningLoadResult& result) {
  result.corruption_reason = reason;
  std::error_code ec;
  auto stored = EncryptedPathFor(paths_.v2_store);
  if (!std::filesystem::exists(stored, ec)) stored = paths_.v2_store;
  auto copy = stored;
  copy += ".corrupt-" + std::to_string(now);
  bool kept = false;
  bool copy_exists = false;
  if (!ec && PathExists(copy, copy_exists)) {
    if (copy_exists) {
      // A copy from the same second counts only if it holds these bytes; a
      // different corrupt file is never dropped unseen, so saving stops.
      std::string existing;
      std::string current;
      kept = ReadFileBytes(copy, existing) && ReadFileBytes(stored, current) && existing == current;
    } else {
      kept =
          std::filesystem::copy_file(stored, copy, std::filesystem::copy_options::none, ec) && !ec;
    }
  }
  if (!kept) {
    EnterBlocked(result, TypoLearningLoadIssue::CorruptCopyFailed);
    return;
  }
  result.rebuilt_from_corruption = true;
  std::cerr << "TypoLearningStore: v2 store was corrupt (" << reason << "); rebuilding\n";

  TypoCorrectionStore v1(paths_.v1_store, crypto_);
  const auto v1_state = LoadV1Store(paths_.v1_store, *crypto_, v1);
  bool migrated = false;
  if (v1_state == FileState::Present) {
    migrated = MigrateFrom(v1, true, now, result);
  } else {
    if (v1_state == FileState::Unreadable) result.v1_side_unreadable = true;
    TypoCorrectionStore backup(paths_.v1_backup, crypto_);
    const auto backup_state = LoadV1Backup(paths_.v1_backup, *crypto_, backup);
    if (backup_state == FileState::Present) {
      migrated = MigrateFrom(backup, false, now, result);
    } else if (backup_state == FileState::Unreadable) {
      result.v1_side_unreadable = true;
    }
  }
  if (!migrated) {
    if (!StartEmpty(TypoStoreOrigin::Rebuilt, now)) {
      EnterBlocked(result, TypoLearningLoadIssue::SqliteUnavailable);
      return;
    }
    // Replace the corrupt file now, so the next start does not copy it again.
    WriteDatabase(false, now);
  }
  EnterV2(result);
}

// Decision 6: a v2 store whose migration was never verified.
void TypoLearningStore::RedoUnverifiedMigration(uint64_t now, TypoLearningLoadResult& result) {
  auto v1 = std::make_unique<TypoCorrectionStore>(paths_.v1_store, crypto_);
  switch (LoadV1Store(paths_.v1_store, *crypto_, *v1)) {
    case FileState::Present:
      db_.reset();
      RemoveProtectedFile(paths_.v2_store);
      if (MigrateFrom(*v1, true, now, result)) {
        EnterV2(result);
      } else {
        EnterV1Fallback(std::move(v1), result);
      }
      return;
    case FileState::Unreadable:
      EnterBlocked(result, TypoLearningLoadIssue::V1StoreUnreadable);
      return;
    case FileState::Missing:
      break;
  }
  TypoCorrectionStore backup(paths_.v1_backup, crypto_);
  switch (LoadV1Backup(paths_.v1_backup, *crypto_, backup)) {
    case FileState::Present:
      if (MigrateFrom(backup, false, now, result)) {
        EnterV2(result);
      } else {
        EnterBlocked(result, TypoLearningLoadIssue::MigrationFailed);
      }
      return;
    case FileState::Unreadable:
      EnterBlocked(result, TypoLearningLoadIssue::V1BackupUnreadable);
      return;
    case FileState::Missing:
      break;
  }
  if (!StartEmpty(TypoStoreOrigin::Rebuilt, now)) {
    EnterBlocked(result, TypoLearningLoadIssue::SqliteUnavailable);
    return;
  }
  WriteDatabase(false, now);
  EnterV2(result);
}

bool TypoLearningStore::Save(uint64_t now_epoch_sec) {
  if (writes_suspended_) return false;
  switch (mode_) {
    case TypoLearningMode::Blocked:
      return false;
    case TypoLearningMode::V1Fallback:
      return v1_ && v1_->Save();
    case TypoLearningMode::V2:
      break;
  }
  return WriteDatabase(true, now_epoch_sec);
}

// --- Reading pairs -----------------------------------------------------------

bool TypoLearningStore::ObserveReadingPair(const std::string& wrong, const std::string& correct,
                                           uint64_t now_epoch_sec) {
  if (writes_suspended_) return false;
  if (mode_ == TypoLearningMode::V1Fallback)
    return v1_ && v1_->Observe(wrong, correct, now_epoch_sec);
  if (!db_ || !TypoCorrectionStore::IsLearnablePair(wrong, correct)) return false;
  auto statement = db_->Prepare(
      "INSERT INTO typo_reading_pairs VALUES (?1, ?2, 1, ?3) "
      "ON CONFLICT(wrong_reading, correct_reading) DO UPDATE SET "
      "count = MIN(count + 1, 4294967295), last_updated_at = excluded.last_updated_at");
  if (!statement) return false;
  BindText(statement.get(), 1, wrong);
  BindText(statement.get(), 2, correct);
  sqlite3_bind_int64(statement.get(), 3, ToSql(now_epoch_sec));
  return sqlite3_step(statement.get()) == SQLITE_DONE;
}

std::optional<std::string> TypoLearningStore::LookupReadingPair(const std::string& wrong,
                                                                uint32_t min_count,
                                                                uint64_t now_epoch_sec) const {
  if (mode_ == TypoLearningMode::V1Fallback) {
    return v1_ ? v1_->Lookup(wrong, min_count, now_epoch_sec) : std::nullopt;
  }
  if (!db_) return std::nullopt;
  // The v1 rules: count at least min_count, not older than the v1 cutoff (a
  // future timestamp counts as current), the highest count, ties to the
  // smaller reading.
  const uint64_t cutoff = now_epoch_sec > kTypoCorrectionMaxRecordAgeSec
                              ? now_epoch_sec - kTypoCorrectionMaxRecordAgeSec
                              : 0;
  auto statement = db_->Prepare(
      "SELECT correct_reading FROM typo_reading_pairs WHERE wrong_reading = ?1 AND count >= ?2 "
      "AND last_updated_at >= ?3 ORDER BY count DESC, correct_reading ASC LIMIT 1");
  if (!statement) return std::nullopt;
  BindText(statement.get(), 1, wrong);
  sqlite3_bind_int64(statement.get(), 2, min_count);
  sqlite3_bind_int64(statement.get(), 3, ToSql(cutoff));
  if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
  return ColumnText(statement.get(), 0);
}

std::vector<TypoCorrectionEntry> TypoLearningStore::ReadingPairs() const {
  if (mode_ == TypoLearningMode::V1Fallback)
    return v1_ ? v1_->All() : std::vector<TypoCorrectionEntry>{};
  return db_ ? db_->Pairs() : std::vector<TypoCorrectionEntry>{};
}

// --- Patterns ----------------------------------------------------------------

std::optional<int64_t> TypoLearningStore::RecordPatternEvent(const TypoPatternEvent& event) {
  if (writes_suspended_ || mode_ == TypoLearningMode::V1Fallback || !db_) return std::nullopt;
  TypoPatternRecord key;
  key.observed_pattern = event.observed_pattern;
  key.intended_pattern = event.intended_pattern;
  key.typo_type = event.typo_type;
  key.keyboard_layout = event.keyboard_layout;
  if (!IsValidPatternKey(key) || !IsValidEventReading(event.observed_reading) ||
      !IsValidEventReading(event.corrected_reading) ||
      event.selected_surface.size() > kMaxSurfaceBytes ||
      HasControlCharacter(event.selected_surface) || event.app_name.size() > kMaxAppNameBytes ||
      HasControlCharacter(event.app_name) || !IsValidContextHash(event.left_context_hash)) {
    return std::nullopt;
  }

  const bool accepted = event.outcome == TypoPatternOutcome::Accept;
  const uint64_t now = event.now_epoch_sec;
  if (!db_->Exec("BEGIN")) return std::nullopt;
  const auto rollback = [&] {
    db_->Exec("ROLLBACK");
    return std::nullopt;
  };
  auto record = db_->FindPattern(key).value_or(key);
  if (record.id == 0) record.created_at = now;
  if (accepted) {
    record.accept_count = SaturatingAdd(record.accept_count, 1);
    // A pattern is "used" when its correction is taken; a rejection must not
    // raise its recency bonus.
    record.last_used_at = now;
  } else {
    record.reject_count = SaturatingAdd(record.reject_count, 1);
  }
  record.updated_at = now;
  const auto id = db_->WritePattern(record, now);
  if (!id) return rollback();

  auto statement = db_->Prepare(
      "INSERT INTO typo_events (observed_reading, corrected_reading, selected_surface, "
      "typo_type, pattern_id, app_name, left_context_hash, event_type, created_at) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
  if (!statement) return rollback();
  BindText(statement.get(), 1, event.observed_reading);
  BindText(statement.get(), 2, event.corrected_reading);
  BindOptionalText(statement.get(), 3, event.selected_surface);
  BindText(statement.get(), 4, event.typo_type);
  sqlite3_bind_int64(statement.get(), 5, *id);
  BindOptionalText(statement.get(), 6, event.app_name);
  BindOptionalText(statement.get(), 7, event.left_context_hash);
  BindText(statement.get(), 8, accepted ? kEventAccept : kEventReject);
  sqlite3_bind_int64(statement.get(), 9, ToSql(now));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) return rollback();
  if (!db_->Exec("COMMIT")) return rollback();
  return id;
}

std::vector<TypoPatternRecord> TypoLearningStore::Patterns() const {
  return db_ && mode_ != TypoLearningMode::V1Fallback ? db_->PatternRows(std::nullopt)
                                                      : std::vector<TypoPatternRecord>{};
}

std::optional<std::string> TypoLearningStore::DominantApp(int64_t pattern_id) const {
  if (!db_ || mode_ == TypoLearningMode::V1Fallback) return std::nullopt;
  auto statement = db_->Prepare(
      "SELECT app_name, COUNT(*) AS accepts FROM typo_events WHERE pattern_id = ?1 "
      "AND event_type = ?2 AND app_name IS NOT NULL AND app_name <> '' "
      "GROUP BY app_name ORDER BY accepts DESC, app_name ASC LIMIT 2");
  if (!statement) return std::nullopt;
  sqlite3_bind_int64(statement.get(), 1, pattern_id);
  BindText(statement.get(), 2, kEventAccept);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
  auto app = ColumnText(statement.get(), 0);
  const auto accepts = sqlite3_column_int64(statement.get(), 1);
  if (sqlite3_step(statement.get()) == SQLITE_ROW &&
      sqlite3_column_int64(statement.get(), 1) == accepts) {
    return std::nullopt;
  }
  return app;
}

std::optional<double> TypoLearningStore::PatternConfidence(int64_t pattern_id,
                                                           std::string_view current_app,
                                                           bool app_aware_learning,
                                                           uint64_t now_epoch_sec) const {
  if (!db_ || mode_ == TypoLearningMode::V1Fallback) return std::nullopt;
  const auto rows = db_->PatternRows(pattern_id);
  if (rows.empty()) return std::nullopt;
  bool app_bonus = false;
  if (app_aware_learning && !current_app.empty()) {
    const auto dominant = DominantApp(pattern_id);
    app_bonus = dominant && *dominant == current_app;
  }
  const auto& row = rows.front();
  return ComputeTypoConfidence(row.accept_count, row.reject_count, row.last_used_at, now_epoch_sec,
                               app_bonus);
}

size_t TypoLearningStore::EventCount() const {
  if (!db_ || mode_ == TypoLearningMode::V1Fallback) return 0;
  return static_cast<size_t>(db_->CountRows("typo_events").value_or(0));
}

// --- Deletion ----------------------------------------------------------------

// Removes the pair's rows from the v1 backup, then from the v1 store if it is
// still there, keeping every other byte. Both are read before either is
// written, so an unreadable one fails the forget with no file changed.
bool TypoLearningStore::RemovePairFromV1Side(const std::string& wrong, const std::string& correct,
                                             bool& removed) {
  removed = false;
  struct Side {
    std::filesystem::path path;
    std::string rewritten;
    bool removed{false};
  };
  std::vector<Side> sides;
  const auto erase_all = [&] {
    for (auto& side : sides) SecureErase(side.rewritten);
  };
  for (const auto& [path, v1_rule] :
       {std::pair{paths_.v1_backup, false}, std::pair{paths_.v1_store, true}}) {
    std::string text;
    const auto state = ReadState(path, *crypto_, text, v1_rule);
    if (state == FileState::Missing) continue;
    if (state == FileState::Unreadable) {
      erase_all();
      return false;
    }
    Side side{path, {}, false};
    side.rewritten = TypoCorrectionStore::WithoutPairRows(text, wrong, correct, side.removed);
    SecureErase(text);
    sides.push_back(std::move(side));
  }
  bool ok = true;
  for (auto& side : sides) {
    if (ok && side.removed) {
      ok = WriteProtectedText(side.path, side.rewritten, *crypto_);
      removed = removed || ok;
    }
  }
  erase_all();
  return ok;
}

bool TypoLearningStore::RemoveV1Files() {
  return RemoveProtectedFile(paths_.v1_backup) && RemoveProtectedFile(paths_.v1_store);
}

// A separate copy of the published database, so a change is published only
// after it has been saved.
std::unique_ptr<TypoLearningStore::Database> TypoLearningStore::CopyDatabase() const {
  SqliteBuffer buffer;
  if (!db_ || !db_->Serialize(buffer)) return nullptr;
  std::string bytes(buffer.view());
  std::unique_ptr<Database> copy;
  std::string reason;
  // FromBytes erases `bytes`.
  if (Database::FromBytes(bytes, copy, reason) != Database::OpenResult::Ok) return nullptr;
  return copy;
}

// Runs `change` on a copy of the database. When it changed anything (or
// `always_save`), the copy is VACUUMed and saved, and replaces the published
// database only once the save succeeded; any failure leaves the published
// contents as they were (section 12.8 "学習の削除").
bool TypoLearningStore::CommitDeletion(uint64_t now, bool always_save, bool& changed,
                                       const std::function<bool(Database&, bool&)>& change) {
  changed = false;
  auto copy = CopyDatabase();
  if (!copy || !copy->Exec("BEGIN")) return false;
  if (!change(*copy, changed) || !copy->Exec("COMMIT")) {
    copy->Exec("ROLLBACK");
    return false;
  }
  if (!changed && !always_save) return true;
  if (!copy->Exec("VACUUM") || !WriteDatabase(*copy, true, now)) return false;
  db_ = std::move(copy);
  return true;
}

TypoForgetResult TypoLearningStore::ForgetReadingPair(const std::string& wrong,
                                                      const std::string& correct,
                                                      uint64_t now_epoch_sec) {
  if (writes_suspended_ || mode_ == TypoLearningMode::Blocked) return TypoForgetResult::Failed;
  bool removed_side = false;
  if (mode_ == TypoLearningMode::V1Fallback) {
    // The backup first, then the v1 store (section 12.8 "v1 のまま読む").
    {
      std::string text;
      const auto state = ReadState(paths_.v1_backup, *crypto_, text, false);
      if (state == FileState::Unreadable) return TypoForgetResult::Failed;
      if (state == FileState::Present) {
        auto rewritten = TypoCorrectionStore::WithoutPairRows(text, wrong, correct, removed_side);
        SecureErase(text);
        const bool ok = !removed_side || WriteProtectedText(paths_.v1_backup, rewritten, *crypto_);
        SecureErase(rewritten);
        if (!ok) return TypoForgetResult::Failed;
      }
    }
    const auto before = v1_->All();
    if (!v1_->Remove(wrong, correct)) {
      return removed_side ? TypoForgetResult::Removed : TypoForgetResult::NotFound;
    }
    if (!v1_->Save()) {
      for (const auto& entry : before) v1_->Put(entry);
      return TypoForgetResult::Failed;
    }
    return TypoForgetResult::Removed;
  }

  if (!RemovePairFromV1Side(wrong, correct, removed_side)) return TypoForgetResult::Failed;
  bool changed = false;
  const bool ok = CommitDeletion(now_epoch_sec, false, changed, [&](Database& db, bool& any) {
    for (const char* sql : {
             "DELETE FROM typo_reading_pairs WHERE wrong_reading = ?1 AND correct_reading = ?2",
             "DELETE FROM typo_events WHERE observed_reading = ?1 AND corrected_reading = ?2",
         }) {
      auto statement = db.Prepare(sql);
      if (!statement) return false;
      BindText(statement.get(), 1, wrong);
      BindText(statement.get(), 2, correct);
      if (sqlite3_step(statement.get()) != SQLITE_DONE) return false;
      any = any || sqlite3_changes(db.get()) > 0;
    }
    return true;
  });
  if (!ok) return TypoForgetResult::Failed;
  return changed || removed_side ? TypoForgetResult::Removed : TypoForgetResult::NotFound;
}

TypoForgetResult TypoLearningStore::ForgetPattern(const std::string& observed_pattern,
                                                  const std::string& intended_pattern,
                                                  uint64_t now_epoch_sec) {
  if (writes_suspended_ || mode_ == TypoLearningMode::Blocked) return TypoForgetResult::Failed;
  // "v1 のまま読む" keeps no patterns.
  if (mode_ == TypoLearningMode::V1Fallback) return TypoForgetResult::NotFound;
  bool changed = false;
  const bool ok = CommitDeletion(now_epoch_sec, false, changed, [&](Database& db, bool& any) {
    for (const char* sql : {
             "DELETE FROM typo_events WHERE pattern_id IN (SELECT id FROM typo_patterns "
             "WHERE observed_pattern = ?1 AND intended_pattern = ?2)",
             "DELETE FROM typo_patterns WHERE observed_pattern = ?1 AND intended_pattern = ?2",
         }) {
      auto statement = db.Prepare(sql);
      if (!statement) return false;
      BindText(statement.get(), 1, observed_pattern);
      BindText(statement.get(), 2, intended_pattern);
      if (sqlite3_step(statement.get()) != SQLITE_DONE) return false;
      any = any || sqlite3_changes(db.get()) > 0;
    }
    return true;
  });
  if (!ok) return TypoForgetResult::Failed;
  return changed ? TypoForgetResult::Removed : TypoForgetResult::NotFound;
}

bool TypoLearningStore::ResetAll(uint64_t now_epoch_sec) {
  // 1. A store that refuses to save cannot be emptied.
  if (writes_suspended_ || mode_ == TypoLearningMode::Blocked) return false;
  if (mode_ == TypoLearningMode::V1Fallback) {
    // The backup first, then the v1 store emptied and saved.
    if (!RemoveProtectedFile(paths_.v1_backup)) return false;
    const auto before = v1_->All();
    v1_->Reset();
    if (!v1_->Save()) {
      for (const auto& entry : before) v1_->Put(entry);
      return false;
    }
    return true;
  }
  // 2. The v1 files go first, so a later rebuild cannot bring the rows back.
  if (!RemoveV1Files()) return false;
  // 3. Empty a copy of the learned tables (typo_settings is not learning) and
  // save it; the published contents stay until the save succeeds.
  bool changed = false;
  return CommitDeletion(now_epoch_sec, true, changed, [](Database& db, bool& any) {
    any = true;
    return db.Exec("DELETE FROM typo_patterns") && db.Exec("DELETE FROM typo_events") &&
           db.Exec("DELETE FROM typo_reading_pairs");
  });
}

// --- Backup items ------------------------------------------------------------

std::string TypoLearningStore::SerializeReadingPairsText() const {
  if (mode_ == TypoLearningMode::V1Fallback) return v1_ ? v1_->SerializeText() : std::string();
  TypoCorrectionStore pairs(std::filesystem::path{}, crypto_);
  for (const auto& entry : ReadingPairs()) pairs.Put(entry);
  return pairs.SerializeText();
}

std::string TypoLearningStore::SerializePatternsText() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << kTypoPatternsEscapedTsvHeader << '\n';
  for (const auto& record : Patterns()) {
    out << EscapeField(record.observed_pattern) << '\t' << EscapeField(record.intended_pattern)
        << '\t' << EscapeField(record.typo_type) << '\t' << EscapeField(record.keyboard_layout)
        << '\t' << record.accept_count << '\t' << record.reject_count << '\t';
    if (record.last_used_at) out << *record.last_used_at;
    out << '\t' << record.created_at << '\t' << record.updated_at << '\n';
  }
  return out.str();
}

bool TypoLearningStore::ParsePatternsText(std::string_view text,
                                          std::vector<TypoPatternRecord>& patterns) {
  patterns.clear();
  bool all_valid = true;
  bool first_line = true;
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t newline = text.find('\n', begin);
    const size_t end = newline == std::string_view::npos ? text.size() : newline;
    std::string_view line = text.substr(begin, end - begin);
    begin = newline == std::string_view::npos ? text.size() : newline + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (first_line) {
      first_line = false;
      // The item is always written with its header; without one the fields
      // cannot be unescaped reliably.
      if (line != kTypoPatternsEscapedTsvHeader) return false;
      continue;
    }
    if (line.empty() || line.front() == '#') continue;
    const auto fields = SplitTabs(line);
    TypoPatternRecord record;
    uint64_t last_used = 0;
    const bool parsed = fields.size() == 9 && ParseUnsignedField(fields[4], record.accept_count) &&
                        ParseUnsignedField(fields[5], record.reject_count) &&
                        (fields[6].empty() || ParseUnsignedField(fields[6], last_used)) &&
                        ParseUnsignedField(fields[7], record.created_at) &&
                        ParseUnsignedField(fields[8], record.updated_at);
    if (parsed) {
      record.observed_pattern = UnescapeField(fields[0]);
      record.intended_pattern = UnescapeField(fields[1]);
      record.typo_type = UnescapeField(fields[2]);
      record.keyboard_layout = UnescapeField(fields[3]);
      if (!fields[6].empty()) record.last_used_at = last_used;
    }
    if (!parsed || !IsValidPatternKey(record)) {
      all_valid = false;
      continue;
    }
    patterns.push_back(std::move(record));
  }
  return all_valid && !first_line;
}

ImportCounts TypoLearningStore::MergeReadingPairs(const TypoCorrectionStore& other,
                                                  ImportConflictPolicy policy) {
  if (mode_ == TypoLearningMode::V1Fallback) return v1_->Merge(other, policy);
  ImportCounts counts;
  if (!db_) return counts;
  for (const auto& entry : other.All()) {
    const auto existing = db_->FindPair(entry.wrong_reading, entry.correct_reading);
    if (!existing) {
      if (db_->UpsertPair(entry.wrong_reading, entry.correct_reading, entry.record.count,
                          entry.record.last_updated_epoch_sec, Database::PairMerge::Replace)) {
        ++counts.imported;
      }
      continue;
    }
    ++counts.conflicts;
    switch (policy) {
      case ImportConflictPolicy::Merge:
        if (db_->UpsertPair(entry.wrong_reading, entry.correct_reading, entry.record.count,
                            entry.record.last_updated_epoch_sec, Database::PairMerge::Add)) {
          ++counts.imported;
        }
        break;
      case ImportConflictPolicy::Overwrite:
        if (db_->UpsertPair(entry.wrong_reading, entry.correct_reading, entry.record.count,
                            entry.record.last_updated_epoch_sec, Database::PairMerge::Replace)) {
          ++counts.imported;
        }
        break;
      case ImportConflictPolicy::KeepBoth:
        ++counts.skipped;
        break;
    }
  }
  return counts;
}

ImportCounts TypoLearningStore::MergePatterns(const std::vector<TypoPatternRecord>& patterns,
                                              ImportConflictPolicy policy, uint64_t now_epoch_sec) {
  ImportCounts counts;
  if (mode_ == TypoLearningMode::V1Fallback || !db_) {
    counts.skipped = patterns.size();
    return counts;
  }
  for (const auto& incoming : patterns) {
    if (!IsValidPatternKey(incoming)) {
      ++counts.skipped;
      continue;
    }
    auto existing = db_->FindPattern(incoming);
    if (!existing) {
      auto record = incoming;
      record.id = 0;
      if (db_->WritePattern(record, now_epoch_sec)) ++counts.imported;
      continue;
    }
    ++counts.conflicts;
    switch (policy) {
      case ImportConflictPolicy::Merge: {
        existing->accept_count = SaturatingAdd(existing->accept_count, incoming.accept_count);
        existing->reject_count = SaturatingAdd(existing->reject_count, incoming.reject_count);
        if (incoming.last_used_at &&
            (!existing->last_used_at || *incoming.last_used_at > *existing->last_used_at)) {
          existing->last_used_at = incoming.last_used_at;
        }
        existing->created_at = (std::min)(existing->created_at, incoming.created_at);
        existing->updated_at = (std::max)(existing->updated_at, incoming.updated_at);
        if (db_->WritePattern(*existing, now_epoch_sec)) ++counts.imported;
        break;
      }
      case ImportConflictPolicy::Overwrite: {
        auto record = incoming;
        record.id = existing->id;
        if (db_->WritePattern(record, now_epoch_sec)) ++counts.imported;
        break;
      }
      case ImportConflictPolicy::KeepBoth:
        ++counts.skipped;
        break;
    }
  }
  return counts;
}

}  // namespace azookey::learning
