#pragma once

#include <map>
#include <memory>
#include <sstream>

#include <LibISDB/LibISDB.hpp>
#include <LibISDB/EPG/EventInfo.hpp>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <tsduck/tsduck.h>

#include "base.hh"
#include "jsonl_source.hh"
#include "logging.hh"
#include "packet_source.hh"
#include "tsduck_helper.hh"

namespace {

struct EitCollectorOption final {
  SidSet sids;
  SidSet xsids;
  ts::MilliSecond time_limit = 30 * ts::MilliSecPerSec;  // 30s
  bool streaming = false;
  bool use_unicode_symbol = false;
};

struct EitSection {
  uint16_t pid;
  uint16_t sid;
  uint16_t tid;
  uint16_t nid;
  uint16_t tsid;
  uint8_t last_table_id;
  uint8_t section_number;
  uint8_t last_section_number;
  uint8_t segment_last_section_number;
  uint8_t version;
  const uint8_t* events_data;
  size_t events_size;

  static constexpr size_t EIT_PAYLOAD_FIXED_SIZE = 6;
  static constexpr size_t EIT_EVENT_FIXED_SIZE = 12;

  EitSection(const ts::Section& section) {
    const auto* data = section.payload();
    auto size = section.payloadSize();

    pid = section.sourcePID();
    sid = section.tableIdExtension();
    tid = section.tableId();
    nid = ts::GetUInt16(data + 2);
    tsid = ts::GetUInt16(data);
    last_table_id = ts::GetUInt8(data + 5);
    section_number = section.sectionNumber();
    last_section_number = section.lastSectionNumber();
    segment_last_section_number = ts::GetUInt8(data + 4);
    version = section.version();
    events_data = data + EIT_PAYLOAD_FIXED_SIZE;
    events_size = size - EIT_PAYLOAD_FIXED_SIZE;
  }

  inline uint64_t service_triple() const {
    // service triple
    return
        static_cast<uint64_t>(nid)  << 48 |
        static_cast<uint64_t>(tsid) << 32 |
        static_cast<uint32_t>(sid)  << 16;
  }

  inline size_t table_index() const {
    return tid & 0x07;
  }

  inline size_t last_table_index() const {
    return last_table_id & 0x07;
  }

  inline size_t segment_index() const {
    return section_number >> 3;
  }

  inline size_t section_index() const {
    return section_number & 0x07;
  }

  inline size_t last_segment_index() const {
    return last_section_number >> 3;
  }

  inline size_t last_section_index() const {
    return segment_last_section_number & 0x07;
  }

  inline bool IsBasic() const {
    return (tid & 0x0F) < 8;
  }
};

class TableProgress {
 public:
  TableProgress() {
    for (size_t i = 0; i < kNumSections; ++i) {
      section_versions_[i] = 0xFF;  // means no version is stored.
    }
  }

  ~TableProgress() = default;

  void Reset() {
    for (size_t i = 0; i < kNumSegments; ++i) {
      collected_[i] = 0;
      unused_[i] = 0;
    }
    completed_ = false;
  }

  void Unuse() {
    for (size_t i = 0; i < kNumSegments; ++i) {
      unused_[i] = 0xFF;
    }
    completed_ = true;
  }

  void Update(const EitSection& eit) {
    if (!CheckConsistency(eit)) {
      Reset();
    }

    for (auto i = eit.last_segment_index() + 1; i < kNumSegments; ++i) {
      unused_[i] = 0xFF;
    }

    for (auto i = eit.last_section_index() + 1; i < 8; ++i) {
      unused_[eit.segment_index()] |= 1 << i;
    }

    collected_[eit.segment_index()] |= 1 << eit.section_index();

    for (size_t i = eit.section_index(); i <= eit.last_section_index(); ++i) {
      if (section_versions_[i] != 0xFF && section_versions_[i] != eit.version) {
        MIRAKC_ARIB_INFO("  Version changed: sec#{:02X}: {:02d} -> {:02d}",
                         i, section_versions_[i], eit.version);
      }
      section_versions_[i] = eit.version;
    }

    completed_ = CheckCompleted();
  }

  void UpdateUnused(const ts::Time& timestamp) {
    size_t segment = ((ts::Time::Fields)(timestamp)).hour / 3;
    for (size_t i = 0; i < segment; ++i) {
      unused_[i] = 0xFF;
    }

    completed_ = CheckCompleted();
  }

  bool CheckCollected(const EitSection& eit) const {
    for (size_t i = eit.section_index(); i < eit.last_section_index(); ++i) {
      if (section_versions_[i] == 0xFF) {
        return false;
      }
      if (section_versions_[i] != eit.version) {
        return false;
      }
    }
    auto mask = 1 << eit.section_index();
    return (collected_[eit.segment_index()] & mask) != 0;
  }

  bool IsCompleted() const {
    return completed_;
  }

  void Show(size_t index) const {
    MIRAKC_ARIB_TRACE("      {}: {:3d}/256", index, CalcProgressCount());
    std::stringstream ss;
    for (size_t i = 0; i < kNumSegments; ++i) {
      ss << '[';
      for (uint8_t j = 0; j < 8; ++j) {
        auto mask = 1 << j;
        if (unused_[i] & mask) {
          ss << '.';
        } else if (collected_[i] & mask) {
          ss << '*';
        } else {
          ss << ' ';
        }
      }
      ss << ']';
      if (i % 8 == 7) {
        MIRAKC_ARIB_TRACE("         {}", ss.str());
        ss.str("");
      }
    }
  }

  size_t CountSections() const {
    size_t n = 0;
    for (size_t i = 0; i < kNumSegments; ++i) {
      for (uint8_t j = 0; j < 8; ++j) {
        auto mask = 1 << j;
        if (collected_[i] & mask) {
          n++;
        }
      }
    }
    return n;
  }

 private:
  static constexpr size_t kNumSections = 256;
  static constexpr size_t kNumSegments = kNumSections / 8;

  bool CheckConsistency(const EitSection& eit) {
    // NOTE:
    //
    // Many implementations processing tables assume that the version number of
    // a table is applied to all sections included in the table.  As a natural
    // consequence, they reset the all sections in the table when the version
    // number in a section of the table is changed.
    //
    // But the definition of the sub table is unclear.  A table consists of a
    // number of sub tables which have a same table_id.  The sub table is
    // defined as a collection of sections having a same version number.  But
    // there is no description about how many sections are included in a sub
    // table.
    //
    // Actually, a TS stream contains a table which includes sections of
    // different versions like below:
    //
    //   Section(00): version(12)
    //   Section(08): version(12)
    //   Section(10): version(11)
    //   Section(18): version(11)
    //   ...
    //
    // In the case above, IsCompleted() never returns true.  As a result,
    // this program never stops.
    //
    // For avoiding the situation above, this method doesn't check whether the
    // current table version number is equal to `eit.version`, and always
    // returns true.
    return true;
  }

  bool CheckCompleted() const {
    for (size_t i = 0; i < kNumSegments; ++i) {
      if ((collected_[i] | unused_[i]) != 0xFF) {
        return false;
      }
    }
    return true;
  }

  int CalcProgressCount() const {
    int count = 0;
    for (size_t i = 0; i < kNumSegments; ++i) {
      auto progress = (collected_[i] | unused_[i]);
      for (uint8_t j = 0; j < 8; ++j) {
        if (progress & (1 << j)) {
          ++count;
        }
      }
    }
    return count;
  }

  // Bitmap
  uint8_t collected_[kNumSegments] = { 0 };
  uint8_t unused_[kNumSegments] = { 0 };
  uint8_t section_versions_[kNumSections];
  bool completed_ = false;

  MIRAKC_ARIB_NON_COPYABLE(TableProgress);
};

class TableGroupProgress {
 public:
  TableGroupProgress() = default;
  ~TableGroupProgress() = default;

  void Update(const EitSection& eit) {
    if (!CheckConsistency(eit)) {
      for (size_t i = 0; i < kNumTables; ++i) {
        tables_[i].Reset();
      }
      for (auto i = eit.last_table_index() + 1; i < kNumTables; ++i) {
        tables_[i].Unuse();
      }
      completed_ = false;
    }

    tables_[eit.table_index()].Update(eit);
    last_table_index_ = eit.last_table_index();

    completed_ = CheckCompleted();
  }

  inline void UpdateUnused(const ts::Time& timestamp) {
    tables_[0].UpdateUnused(timestamp);
    completed_ = CheckCompleted();
  }

  bool CheckCollected(const EitSection& eit) const {
    if (last_table_index_ < 0) {
      return false;
    }
    if (last_table_index_ != eit.last_table_index()) {
      return false;
    }
    return tables_[eit.table_index()].CheckCollected(eit);
  }

  bool IsCompleted() const {
    if (last_table_index_ < 0) {
      return true;
    }
    return completed_;
  }

  void Show(const char* label) const {
    MIRAKC_ARIB_TRACE("    {}: last-table-index({}), ltid-changed({})",
                      label, last_table_index_, last_table_index_change_count_);
    for (size_t i = 0; i < kNumTables; ++i) {
      if (tables_[i].IsCompleted()) {
        continue;
      }
      tables_[i].Show(i);
    }
  }

  size_t CountSections() const {
    size_t n = 0;
    for (size_t i = 0; i < kNumTables; ++i) {
      n += tables_[i].CountSections();
    }
    return n;
  }

 private:
  static constexpr size_t kNumTables = 8;

  bool CheckConsistency(const EitSection& eit) {
    if (last_table_index_ < 0) {
      return false;
    }
    if (last_table_index_ != eit.last_table_index()) {
      MIRAKC_ARIB_INFO("  Last table index changed: {} -> {}",
                       last_table_index_, eit.last_table_index());
      last_table_index_change_count_++;
      return false;
    }
    return true;
  }

  bool CheckCompleted() const {
    for (size_t i = 0; i < kNumTables; ++i) {
      if (!tables_[i].IsCompleted()) {
        return false;
      }
    }
    return true;
  }

  TableProgress tables_[kNumTables];
  int last_table_index_ = -1;
  int last_table_index_change_count_ = 0;
  bool completed_ = false;

  MIRAKC_ARIB_NON_COPYABLE(TableGroupProgress);
};

class ServiceProgress {
 public:
  ServiceProgress() = default;
  ~ServiceProgress() = default;

  void Update(const EitSection& eit) {
    if (eit.IsBasic()) {
      basic_.Update(eit);
    } else {
      extra_.Update(eit);
    }
  }

  inline void UpdateUnused(const ts::Time& timestamp) {
    basic_.UpdateUnused(timestamp);
    extra_.UpdateUnused(timestamp);
  }

  bool CheckCollected(const EitSection& eit) const {
    if (eit.IsBasic()) {
      return basic_.CheckCollected(eit);
    }
    return extra_.CheckCollected(eit);
  }

  bool IsCompleted() const {
    return basic_.IsCompleted() && extra_.IsCompleted();
  }

  void Show(uint32_t id) const {
    MIRAKC_ARIB_TRACE("  {:08X}:", id);
    if (!basic_.IsCompleted()) {
      basic_.Show("basic");
    }
    if (!extra_.IsCompleted()) {
      extra_.Show("extra");
    }
  }

  size_t CountSections() const {
    return basic_.CountSections() + extra_.CountSections();
  }

 private:
  TableGroupProgress basic_;
  TableGroupProgress extra_;

  MIRAKC_ARIB_NON_COPYABLE(ServiceProgress);
};

class CollectProgress {
 public:
  CollectProgress() = default;
  ~CollectProgress() = default;

  void Update(const EitSection& eit) {
    services_[eit.service_triple()].Update(eit);
    completed_ = CheckCompleted();
  }

  void UpdateUnused(const ts::Time& timestamp) {
    for (auto& pair : services_) {
      pair.second.UpdateUnused(timestamp);
    }
  }

  bool CheckCollected(const EitSection& eit) const {
    if (services_.find(eit.service_triple()) == services_.end()) {
      return false;
    }
    return services_.at(eit.service_triple()).CheckCollected(eit);
  }

  bool IsCompleted() const {
    return completed_;
  }

  void Show() const {
    if (IsCompleted()) {
      return;
    }
    MIRAKC_ARIB_TRACE("Progress:");
    for (const auto& pair : services_) {
      if (pair.second.IsCompleted()) {
        continue;
      }
      pair.second.Show(pair.first);
    }
  }

  size_t CountServices() const {
    return services_.size();
  }

  size_t CountSections() const {
    size_t n = 0;
    for (const auto& pair : services_) {
      n += pair.second.CountSections();
    }
    return n;
  }

 private:
  bool CheckCompleted() const {
    for (const auto& pair : services_) {
      if (!pair.second.IsCompleted()) {
        return false;
      }
    }
    return true;
  }

  std::map<uint64_t, ServiceProgress> services_;
  bool completed_ = false;

  MIRAKC_ARIB_NON_COPYABLE(CollectProgress);
};

class EitCollector final : public PacketSink,
                           public JsonlSource,
                           public ts::SectionHandlerInterface,
                           public ts::TableHandlerInterface {
 public:
  explicit EitCollector(const EitCollectorOption& option)
      : option_(option),
        demux_(context_) {
    if (spdlog::default_logger()->should_log(spdlog::level::trace)) {
      EnableShowProgress();
    }

    demux_.setSectionHandler(this);
    demux_.setTableHandler(this);
    demux_.addPID(ts::PID_EIT);
    demux_.addPID(ts::PID_TOT);
  }

  ~EitCollector() override {}

  bool Start() override {
    start_time_ = ts::Time::CurrentUTC();
    return true;
  }

  bool End() override {
    auto elapse = ts::Time::CurrentUTC() - start_time_;
    auto min = elapse / ts::MilliSecPerMin;
    auto sec = (elapse - min * ts::MilliSecPerMin) / ts::MilliSecPerSec;
    auto ms = elapse % ts::MilliSecPerSec;
    MIRAKC_ARIB_INFO(
        "Collected {} services, {} sections, {}:{:02d}.{:03d} elapsed",
        progress_.CountServices(), progress_.CountSections(), min, sec, ms);
    return IsCompleted();
  }

  bool HandlePacket(const ts::TSPacket& packet) override {
    demux_.feedPacket(packet);
    if (IsCompleted()) {
      MIRAKC_ARIB_INFO("Completed");
      return false;
    }
    if (CheckTimeout()) {
      MIRAKC_ARIB_ERROR("Timed out");
      return false;
    }
    return true;
  }

 private:
  void handleSection(ts::SectionDemux&, const ts::Section& section) override {
    if (!section.isValid()) {
      return;
    }

    const auto tid = section.tableId();
    if (tid < ts::TID_EIT_MIN || tid > ts::TID_EIT_MAX) {
      return;
    }
    if (tid == ts::TID_EIT_PF_ACT || tid == ts::TID_EIT_PF_OTH) {
      return;
    }

    if (section.isNext()) {
      return;
    }

    if (section.payloadSize() < EitSection::EIT_PAYLOAD_FIXED_SIZE) {
      return;
    }

    EitSection eit(section);
    if (!option_.sids.IsEmpty() && !option_.sids.Contain(eit.sid)) {
      MIRAKC_ARIB_DEBUG(
          "Ignore SID#{:04X} according to the inclusion list", eit.sid);
      return;
    }
    if (!option_.xsids.IsEmpty() && option_.xsids.Contain(eit.sid)) {
      MIRAKC_ARIB_DEBUG(
          "Ignore SID#{:04X} according to the exclusion list", eit.sid);
      return;
    }
    if (CheckCollected(eit)) {
      return;
    }

    MIRAKC_ARIB_INFO(
        "EIT: onid({:04X}) tsid({:04X}) sid({:04X}) tid({:04X}/{:02X})"
        " sec({:02X}:{:02X}/{:02X}) ver({:02d})",
        eit.nid, eit.tsid, eit.sid, eit.tid, eit.last_table_id,
        eit.section_number, eit.segment_last_section_number,
        eit.last_section_number, eit.version);

    WriteEitSection(eit);
    UpdateProgress(eit);
  }

  void handleTable(ts::SectionDemux&, const ts::BinaryTable& table) override {
    // In ARIB, the timezone of TDT/TOT is JST.
    switch (table.tableId()) {
      case ts::TID_TDT:
        HandleTdt(table);
        break;
      case ts::TID_TOT:
        HandleTot(table);
        break;
      default:
        break;
    }
  }

  inline void HandleTdt(const ts::BinaryTable& table) {
    ts::TDT tdt(context_, table);

    if (!tdt.isValid()) {
      MIRAKC_ARIB_WARN("Broken TDT, skip");
      return;
    }

    MIRAKC_ARIB_INFO("TDT: {}", tdt.utc_time);
    HandleTime(tdt.utc_time);
  }

  inline void HandleTot(const ts::BinaryTable& table) {
    ts::TOT tot(context_, table);

    if (!tot.isValid()) {
      MIRAKC_ARIB_WARN("Broken TOT, skip");
      return;
    }

    MIRAKC_ARIB_INFO("TOT: {}", tot.utc_time);
    HandleTime(tot.utc_time);
  }

  inline void HandleTime(const ts::Time& time) {
    timestamp_ = time;

    progress_.UpdateUnused(timestamp_);

    if (!has_timestamp_) {
      last_updated_ = timestamp_;
      has_timestamp_ = true;
    }
  }

  inline bool CheckCollected(const EitSection& eit) const {
    return progress_.CheckCollected(eit);
  }

  void WriteEitSection(const EitSection& eit) {
    auto json = MakeJsonValue(eit);
    FeedDocument(json);
  }

  rapidjson::Document MakeJsonValue(const EitSection& eit) {
    const auto* data = eit.events_data;
    auto remain = eit.events_size;

    rapidjson::Document eit_json(rapidjson::kObjectType);
    auto& allocator = eit_json.GetAllocator();

    rapidjson::Value events(rapidjson::kArrayType);

    while (remain >= EitSection::EIT_EVENT_FIXED_SIZE) {
      const auto eid = ts::GetUInt16(data);

      ts::Time start_time;
      ts::DecodeMJD(data + 2, 5, start_time);
      start_time -= kJstTzOffset;  // JST -> UTC
      const auto start_time_unix = start_time - ts::Time::UnixEpoch;

      const auto hour = ts::DecodeBCD(data[7]);
      const auto min = ts::DecodeBCD(data[8]);
      const auto sec = ts::DecodeBCD(data[9]);
      const ts::MilliSecond duration =
          hour * ts::MilliSecPerHour + min * ts::MilliSecPerMin +
          sec * ts::MilliSecPerSec;

      const uint8_t running_status = (data[10] >> 5) & 0x07;

      const bool ca_controlled = (data[10] >> 4) & 0x01;

      size_t info_length = ts::GetUInt16(data + 10) & 0x0FFF;
      data += EitSection::EIT_EVENT_FIXED_SIZE;
      remain -= EitSection::EIT_EVENT_FIXED_SIZE;

      LibISDB::DescriptorBlock desc_block;
      desc_block.ParseBlock(data, info_length);

      rapidjson::Value descriptors(rapidjson::kArrayType);

      for (int i = 0; i < desc_block.GetDescriptorCount(); ++i) {
        const auto* dp = desc_block.GetDescriptorByIndex(i);
        if (!dp->IsValid()) {
          continue;
        }
        switch (dp->GetTag()) {
          case LibISDB::ShortEventDescriptor::TAG: {
            const auto* desc =
                static_cast<const LibISDB::ShortEventDescriptor*>(dp);
            auto json = MakeJsonValue(desc, allocator);
            descriptors.PushBack(json, allocator);
            break;
          }
          case LibISDB::ComponentDescriptor::TAG: {
            const auto* desc =
                static_cast<const LibISDB::ComponentDescriptor*>(dp);
            auto json = MakeJsonValue(desc, allocator);
            descriptors.PushBack(json, allocator);
            break;
          }
          case LibISDB::ContentDescriptor::TAG: {
            const auto* desc =
                static_cast<const LibISDB::ContentDescriptor*>(dp);
            auto json = MakeJsonValue(desc, allocator);
            descriptors.PushBack(json, allocator);
            break;
          }
          case LibISDB::AudioComponentDescriptor::TAG: {
            const auto* desc =
                static_cast<const LibISDB::AudioComponentDescriptor*>(dp);
            auto json = MakeJsonValue(desc, allocator);
            descriptors.PushBack(json, allocator);
            break;
          }
          default:
            break;
        }
      }

      {
        auto json = MakeExtendedEventJsonValue(desc_block, allocator);
        if (!json.IsNull()) {
          descriptors.PushBack(json, allocator);
        }
      }

      rapidjson::Value event(rapidjson::kObjectType);
      event.AddMember("eventId", eid, allocator);
      event.AddMember("startTime", start_time_unix, allocator);
      event.AddMember("duration", duration, allocator);
      event.AddMember("scrambled", ca_controlled, allocator);
      event.AddMember("descriptors", descriptors, allocator);

      events.PushBack(event, allocator);

      data += info_length;
      remain -= info_length;
    }

    eit_json.AddMember("originalNetworkId", eit.nid, allocator);
    eit_json.AddMember("transportStreamId", eit.tsid, allocator);
    eit_json.AddMember("serviceId", eit.sid, allocator);
    eit_json.AddMember("tableId", eit.tid, allocator);
    eit_json.AddMember("sectionNumber", eit.section_number, allocator);
    eit_json.AddMember("lastSectionNumber", eit.last_section_number, allocator);
    eit_json.AddMember("segmentLastSectionNumber",
                       eit.segment_last_section_number, allocator);
    eit_json.AddMember("versionNumber", eit.version, allocator);
    eit_json.AddMember("events", events, allocator);

    return eit_json;
  }

  rapidjson::Value MakeJsonValue(
      const LibISDB::ShortEventDescriptor* desc,
      rapidjson::Document::AllocatorType& allocator) const {
    rapidjson::Value json(rapidjson::kObjectType);
    json.AddMember("$type", "ShortEvent", allocator);
    LibISDB::ARIBString event_name;
    desc->GetEventName(&event_name);
    json.AddMember("eventName", DecodeAribString(event_name), allocator);
    LibISDB::ARIBString text;
    desc->GetEventDescription(&text);
    json.AddMember("text", DecodeAribString(text), allocator);
    return json;
  }

  rapidjson::Value MakeJsonValue(
      const LibISDB::ComponentDescriptor* desc,
      rapidjson::Document::AllocatorType& allocator) const {
    rapidjson::Value json(rapidjson::kObjectType);
    json.AddMember("$type", "Component", allocator);
    json.AddMember("streamContent", desc->GetStreamContent(), allocator);
    json.AddMember("componentType", desc->GetComponentType(), allocator);
    return json;
  }

  rapidjson::Value MakeJsonValue(
      const LibISDB::ContentDescriptor* desc,
      rapidjson::Document::AllocatorType& allocator) const {
    rapidjson::Value nibbles(rapidjson::kArrayType);
    for (int i = 0; i < desc->GetNibbleCount(); ++i) {
      LibISDB::ContentDescriptor::NibbleInfo info;
      desc->GetNibble(i, &info);
      rapidjson::Value nibble(rapidjson::kArrayType);
      nibble.PushBack(info.ContentNibbleLevel1, allocator);
      nibble.PushBack(info.ContentNibbleLevel2, allocator);
      nibble.PushBack(info.UserNibble1, allocator);
      nibble.PushBack(info.UserNibble2, allocator);
      nibbles.PushBack(nibble, allocator);
    }
    rapidjson::Value json(rapidjson::kObjectType);
    json.AddMember("$type", "Content", allocator);
    json.AddMember("nibbles", nibbles, allocator);
    return json;
  }

  rapidjson::Value MakeJsonValue(
      const LibISDB::AudioComponentDescriptor* desc,
      rapidjson::Document::AllocatorType& allocator) const {
    rapidjson::Value json(rapidjson::kObjectType);
    json.AddMember("$type", "AudioComponent", allocator);
    json.AddMember("componentType", desc->GetComponentType(), allocator);
    json.AddMember("samplingRate", desc->GetSamplingRate(), allocator);
    return json;
  }

  rapidjson::Value MakeJsonValue(
      const ts::ByteBlock& desc, const ts::ByteBlock& item,
      rapidjson::Document::AllocatorType& allocator) const {
    ts::UString desc_str = context_.fromDVB(desc.data(), desc.size());
    ts::UString item_str = context_.fromDVB(item.data(), item.size());
    rapidjson::Value json(rapidjson::kArrayType);
    json.PushBack(rapidjson::Value().SetString(desc_str.toUTF8(), allocator),
                  allocator);
    json.PushBack(rapidjson::Value().SetString(item_str.toUTF8(), allocator),
                  allocator);
    return json;
  }

  rapidjson::Value MakeJsonValue(
      const LibISDB::String& desc, const LibISDB::String& item,
      rapidjson::Document::AllocatorType& allocator) const {
    rapidjson::Value json(rapidjson::kArrayType);
    json.PushBack(rapidjson::Value().SetString(desc, allocator), allocator);
    json.PushBack(rapidjson::Value().SetString(item, allocator), allocator);
    return json;
  }

  rapidjson::Value MakeExtendedEventJsonValue(
      const LibISDB::DescriptorBlock& desc_block,
      rapidjson::Document::AllocatorType& allocator) const {
    LibISDB::ARIBStringDecoder decoder;
    auto flags = GetAribStringDecodeFlag();
    LibISDB::EventInfo::ExtendedTextInfoList ext_list;
    if (!LibISDB::GetEventExtendedTextList(
            &desc_block, decoder, flags, &ext_list)) {
      return rapidjson::Value();
    }

    rapidjson::Value items(rapidjson::kArrayType);
    for (const auto& ext : ext_list) {
      auto json = MakeJsonValue(ext.Description, ext.Text, allocator);
      items.PushBack(json, allocator);
    }

    rapidjson::Value json(rapidjson::kObjectType);
    json.AddMember("$type", "ExtendedEvent", allocator);
    json.AddMember("items", items, allocator);
    return json;
  }

  void UpdateProgress(const EitSection& eit) {
    last_updated_ = timestamp_;
    progress_.Update(eit);
    if (show_progress_) {
      progress_.Show();
    }
  }

  inline bool IsCompleted() const {
    if (option_.streaming) {
      return false;
    }
    return progress_.IsCompleted();
  }

  inline bool CheckTimeout() const {
    if (option_.streaming) {
      return false;
    }
    auto elapsed = timestamp_ - last_updated_;
    return elapsed >= option_.time_limit;
  }

  inline void EnableShowProgress() {
    show_progress_ = true;
  }

  inline LibISDB::String DecodeAribString(
      const LibISDB::ARIBString& str) const {
    LibISDB::ARIBStringDecoder decoder;
    LibISDB::String utf8;
    decoder.Decode(str, &utf8, GetAribStringDecodeFlag());
    return std::move(utf8);
  }

  inline LibISDB::ARIBStringDecoder::DecodeFlag GetAribStringDecodeFlag()
      const {
    auto flags = LibISDB::ARIBStringDecoder::DecodeFlag::UseCharSize;
    if (option_.use_unicode_symbol) {
      flags |= LibISDB::ARIBStringDecoder::DecodeFlag::UnicodeSymbol;
    }
    return flags;
  }

  const EitCollectorOption option_;
  ts::DuckContext context_;
  ts::SectionDemux demux_;
  bool has_timestamp_ = false;
  ts::Time timestamp_;  // JST
  ts::Time last_updated_;  // JST
  CollectProgress progress_;
  bool show_progress_ = false;
  ts::Time start_time_;  // UTC

  MIRAKC_ARIB_NON_COPYABLE(EitCollector);
};

}  // namespace
