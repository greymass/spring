#pragma once
#include <eosio/roborovski_writer_plugin/store_format.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <vector>

namespace eosio::roborovski_writer {

struct prepared_block {
   uint32_t                block_num = 0;
   block_id_type           id;
   block_id_type           parent;
   uint64_t                glob_min     = 0;
   uint64_t                glob_max     = 0;
   uint64_t                action_count = 0;
   uint64_t                raw_size     = 0;
   std::vector<char>       compressed;
   std::vector<abi_record> abis;
};

std::vector<char> serialize_payload(const prepared_block& b);
void              parse_payload(prepared_block& b, const char* data, size_t size);

constexpr size_t BUFFER_SEGMENT_HEADER_SIZE = 64;
constexpr size_t BUFFER_ENTRY_HEADER_SIZE   = 88;
constexpr size_t BUFFER_ENTRY_TRAILER_SIZE  = 16;

struct buffer_scan_report {
   uint32_t segments_removed = 0;
   uint64_t bytes_truncated  = 0;
   uint32_t entries          = 0;
};

// One append-only segment per slice, framed like state history's log; forks append again at the same height.
class buffer_log {
public:
   buffer_log(const std::filesystem::path& dir, const uuid_type& core_uuid, uint64_t epoch);
   ~buffer_log();
   buffer_log(const buffer_log&)            = delete;
   buffer_log& operator=(const buffer_log&) = delete;

   struct entry_ref {
      block_id_type id;
      block_id_type parent;
      uint32_t      segment        = 0;
      uint64_t      payload_offset = 0;
      uint64_t      payload_size   = 0;
   };

   const buffer_scan_report& scan_report() const { return report; }

   entry_ref                append(const prepared_block& block, bool sync);
   std::optional<entry_ref> find(uint32_t block_num, const block_id_type& id) const;
   prepared_block           read(uint32_t block_num, const entry_ref& ref) const;

   // Removes every segment whose heights are all at or below head, and forgets entries at or below it.
   uint32_t discard_through(uint32_t head);

   uint64_t disk_bytes() const;
   size_t   pending_heights() const;
   uint32_t highest() const { return entries.empty() ? 0 : entries.rbegin()->first; }

   static std::filesystem::path segment_path(const std::filesystem::path& dir, uint32_t slice_num);

private:
   struct segment {
      int      fd   = -1;
      uint64_t size = 0;
   };

   segment& open_segment(uint32_t slice_num, bool sync);
   void     scan_segment(uint32_t slice_num, const std::filesystem::path& path);
   void     close_all();

   std::filesystem::path                  dir;
   uuid_type                              core_uuid;
   uint64_t                               epoch;
   std::map<uint32_t, segment>            segments;
   std::multimap<uint32_t, entry_ref>     entries;
   buffer_scan_report                     report;
};

} // namespace eosio::roborovski_writer
