// vim: set sts=2 sw=2 et:
// encoding: utf-8
//
// Copyleft 2011 RIME Developers
// License: GPLv3
//
// 2011-11-02 GONG Chen <chen.sst@gmail.com>
//
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <rime_version.h>
#include <rime/common.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/dict/user_db.h>
#include <rime/algo/syllabifier.h>

namespace rime {

// TreeDbAccessor memebers

TreeDbAccessor::TreeDbAccessor(kyotocabinet::DB::Cursor *cursor,
                               const std::string &prefix)
    : cursor_(cursor), prefix_(prefix) {
  Reset();
  if (!prefix.empty())
    Forward(prefix);
}

TreeDbAccessor::~TreeDbAccessor() {
  cursor_.reset();
}

bool TreeDbAccessor::Reset() {
  return cursor_ && cursor_->jump();
}

bool TreeDbAccessor::Forward(const std::string &key) {
  return cursor_ && cursor_->jump(key);
}

bool TreeDbAccessor::Backward(const std::string &key) {
  return cursor_ && cursor_->jump_back(key);
}

bool TreeDbAccessor::GetNextRecord(std::string *key, std::string *value) {
  if (!cursor_ || !key || !value)
    return false;
  return cursor_->get(key, value, true) && boost::starts_with(*key, prefix_);
}

bool TreeDbAccessor::exhausted() {
  std::string key;
  return !cursor_->get_key(&key, false) || !boost::starts_with(key, prefix_);
}

// TreeDb members

TreeDb::TreeDb(const std::string &name) : name_(name), loaded_(false) {
  boost::filesystem::path path(Service::instance().deployer().user_data_dir);
  file_name_ = (path / name).string();
}

void TreeDb::Initialize() {
  db_.reset(new kyotocabinet::TreeDB);
  db_->tune_options(kyotocabinet::TreeDB::TLINEAR | kyotocabinet::TreeDB::TCOMPRESS);
  db_->tune_buckets(10LL * 1000);
  db_->tune_defrag(8);
  db_->tune_page(32768);
}

TreeDb::~TreeDb() {
  if (loaded())
    Close();
}

const shared_ptr<TreeDbAccessor> TreeDb::Query(const std::string &key) {
  if (!loaded())
    return shared_ptr<TreeDbAccessor>();
  kyotocabinet::DB::Cursor *cursor = db_->cursor();  // should be freed by us
  return boost::make_shared<TreeDbAccessor>(cursor, key);
}

bool TreeDb::Fetch(const std::string &key, std::string *value) {
  if (!value || !loaded())
    return false;
  return db_->get(key, value);
}

bool TreeDb::Update(const std::string &key, const std::string &value) {
  if (!loaded()) return false;
  DLOG(INFO) << "update db entry: " << key << " => " << value;
  return db_->set(key, value);
}

bool TreeDb::Erase(const std::string &key) {
  if (!loaded()) return false;
  DLOG(INFO) << "erase db entry: " << key;
  return db_->remove(key);
}

bool TreeDb::Backup() {
  if (!loaded()) return false;
  LOG(INFO) << "backing up db '" << name_ << "'.";
  bool success = db_->dump_snapshot(file_name() + ".snapshot");
  if (!success) {
    LOG(ERROR) << "failed to backup db '" << name_ << "'.";
  }
  return success;
}

bool TreeDb::RecoverFromSnapshot() {
  std::string snapshot_file(file_name() + ".snapshot");
  if (!boost::filesystem::exists(snapshot_file))
    return false;
  LOG(INFO) << "snapshot file exists, trying to recover db '" << name_ << "'.";
  if (loaded()) {
    Close();
  }
  if (Exists()) {
    boost::system::error_code ec;
    boost::filesystem::rename(file_name(), file_name() + ".old", ec);
    if (ec && !Remove()) {
      LOG(ERROR) << "Error removing db file '" << file_name() << "'.";
      return false;
    }
  }
  return Open() && Restore(snapshot_file);
}

bool TreeDb::Restore(const std::string& snapshot_file) {
  if (!loaded()) return false;
  bool success = db_->load_snapshot(snapshot_file);
  if (!success) {
    LOG(ERROR) << "failed to restore db from '" << snapshot_file << "'.";
  }
  return success;
}

bool TreeDb::Exists() const {
  return boost::filesystem::exists(file_name());
}

bool TreeDb::Remove() {
  if (loaded()) {
    LOG(ERROR) << "attempt to remove opened db '" << name_ << "'.";
    return false;
  }
  return boost::filesystem::remove(file_name());
}

bool TreeDb::Open() {
  if (loaded()) return false;
  Initialize();
  loaded_ = db_->open(file_name());
  if (loaded_) {
    std::string db_name;
    if (!Fetch("\x01/db_name", &db_name))
      CreateMetadata();
  }
  else {
    LOG(ERROR) << "Error opening db '" << name_ << "'.";
    if (RecoverFromSnapshot()) {
      LOG(INFO) << "successfully recovered db '" << name_ << "' from snapshot.";
    }
  }
  return loaded_;
}

bool TreeDb::OpenReadOnly() {
  if (loaded()) return false;
  Initialize();
  loaded_ = db_->open(file_name(), kyotocabinet::TreeDB::OREADER);
  if (!loaded_) {
    LOG(ERROR) << "Error opening db '" << name_ << "' read-only.";
  }
  return loaded_;
}

bool TreeDb::Close() {
  if (!loaded()) return false;
  db_->close();
  LOG(INFO) << "closed db '" << name_ << "'.";
  loaded_ = false;
  return true;
}

bool TreeDb::CreateMetadata() {
  LOG(INFO) << "creating metadata for db '" << name_ << "'.";
  std::string rime_version(RIME_VERSION);
  // '\x01' is the meta character
  return db_->set("\x01/db_name", name_) &&
         db_->set("\x01/rime_version", rime_version);
}

// UserDb members

UserDb::UserDb(const std::string &name) : TreeDb(name + ".userdb.kct") {
}

bool UserDb::CreateMetadata() {
  Deployer &deployer(Service::instance().deployer());
  std::string user_id(deployer.user_id);
  // '\x01' is the meta character
  return TreeDb::CreateMetadata() &&
      db_->set("\x01/db_type", "userdb") &&
      db_->set("\x01/user_id", user_id);
}

}  // namespace rime
