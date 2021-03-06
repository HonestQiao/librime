// vim: set sts=2 sw=2 et:
// encoding: utf-8
//
// Copyleft 2011 RIME Developers
// License: GPLv3
//
// 2011-12-12 GONG Chen <chen.sst@gmail.com>
//
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/scoped_array.hpp>
#include <opencc/opencc.h>
#include <stdint.h>
#include <utf8.h>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/gear/simplifier.h>

static const char *quote_left = "\xe3\x80\x94";  //"\xef\xbc\x88";
static const char *quote_right = "\xe3\x80\x95";  //"\xef\xbc\x89";

namespace rime {

class Opencc {
 public:
  Opencc(const std::string &config_path);
  ~Opencc();
  bool ConvertText(const std::string &text, std::string *simplified, bool *is_single_char);
  
 private:
  opencc_t od_;
};

Opencc::Opencc(const std::string &config_path) {
  LOG(INFO) << "initilizing opencc: " << config_path;
  od_ = opencc_open(config_path.c_str());
  if (od_ == (opencc_t) -1) {
    LOG(ERROR) << "Error opening opencc.";
  }
}

Opencc::~Opencc() {
  if (od_ != (opencc_t) -1) {
    opencc_close(od_);
  }
}

bool Opencc::ConvertText(const std::string &text, std::string *simplified, bool *is_single_char) {
  if (od_ == (opencc_t) -1)
    return false;
  boost::scoped_array<uint32_t> inbuf(new uint32_t[text.length() + 1]);
  uint32_t *end = utf8::unchecked::utf8to32(text.c_str(), text.c_str() + text.length(), inbuf.get());
  *end = L'\0';
  size_t inlen = end - inbuf.get();
  uint32_t *inptr = inbuf.get();
  size_t outlen = inlen * 5;
  boost::scoped_array<uint32_t> outbuf(new uint32_t[outlen + 1]);
  uint32_t *outptr = outbuf.get();
  if (inlen == 1) {
    *is_single_char = true;
    opencc_set_conversion_mode(od_, OPENCC_CONVERSION_LIST_CANDIDATES);
  }
  else {
    *is_single_char = false;
    opencc_set_conversion_mode(od_, OPENCC_CONVERSION_FAST);
  }
  size_t converted = opencc_convert(od_, &inptr, &inlen, &outptr, &outlen);
  if (!converted) {
    LOG(ERROR) << "Error simplifying '" << text << "'.";
    return false;
  }
  *outptr = L'\0';
  boost::scoped_array<char> out_utf8(new char[(outptr - outbuf.get()) * 6 + 1]);
  char *utf8_end = utf8::unchecked::utf32to8(outbuf.get(), outptr, out_utf8.get());
  *utf8_end = '\0';
  *simplified = out_utf8.get();
  return true;
}

// Simplifier

Simplifier::Simplifier(Engine *engine) : Filter(engine),
                                         initialized_(false),
                                         tip_level_(kTipNone) {
  Config *config = engine->schema()->config();
  if (config) {
    std::string tip;
    if (config->GetString("simplifier/tip", &tip)) {
      tip_level_ =
          (tip == "all") ? kTipAll :
          (tip == "char") ? kTipChar : kTipNone;
    }
    config->GetString("simplifier/option_name", &option_name_);
    config->GetString("simplifier/opencc_config", &opencc_config_);
  }
  if (option_name_.empty()) {
    option_name_ = "simplification";  // default switcher option
  }
  if (opencc_config_.empty()) {
    opencc_config_ = "zht2zhs.ini";  // default opencc config file
  }
}

Simplifier::~Simplifier() {
}

void Simplifier::Initialize() {
  initialized_ = true;  // no retry
  boost::filesystem::path opencc_config_path = opencc_config_;
  if (opencc_config_path.is_relative()) {
    boost::filesystem::path user_config_path(Service::instance().deployer().user_data_dir);
    boost::filesystem::path shared_config_path(Service::instance().deployer().shared_data_dir);
    (user_config_path /= "opencc") /= opencc_config_path;
    (shared_config_path /= "opencc") /= opencc_config_path;
    if (boost::filesystem::exists(user_config_path)) {
      opencc_config_path = user_config_path;
    }
    else if (boost::filesystem::exists(shared_config_path)) {
      opencc_config_path = shared_config_path;
    }
  }
  opencc_.reset(new Opencc(opencc_config_path.string()));
}

bool Simplifier::Proceed(CandidateList *recruited,
                         CandidateList *candidates) {
  if (!engine_->context()->get_option(option_name_))  // off
    return true;
  if (!initialized_) Initialize();
  if (!opencc_ || !candidates || candidates->empty())
    return true;
  CandidateList result;
  for (CandidateList::iterator it = candidates->begin();
       it != candidates->end(); ++it) {
    if (!Convert(*it, &result))
      result.push_back(*it);
  }
  candidates->swap(result);
  return true;
}

bool Simplifier::Convert(const shared_ptr<Candidate> &original,
                         CandidateList *result) {
  std::string simplified;
  bool is_single_char = false;
  if (!opencc_->ConvertText(original->text(), &simplified, &is_single_char) ||
      simplified == original->text()) {
    return false;
  }
  if (is_single_char) {
    std::vector<std::string> forms;
    boost::split(forms, simplified, boost::is_any_of(" "));
    for (size_t i = 0; i < forms.size(); ++i) {
      if (forms[i] == original->text()) {
        result->push_back(original);
      }
      else {
        std::string tip;
        if (tip_level_ >= kTipChar) {
          tip = quote_left + original->text() + quote_right;
        }
        result->push_back(
            boost::make_shared<ShadowCandidate>(
                original,
                "zh_simplified",
                forms[i],
                tip));
      }
    }
  }
  else {
    std::string tip;
    if (tip_level_ == kTipAll) {
      tip = quote_left + original->text() + quote_right;
    }
    result->push_back(
        boost::make_shared<ShadowCandidate>(
            original,
            "zh_simplified",
            simplified,
            tip));
  }
  return true;
}

}  // namespace rime
