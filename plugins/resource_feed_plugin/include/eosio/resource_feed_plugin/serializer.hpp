#pragma once
#include <eosio/chain/types.hpp>
#include <eosio/resource_feed_plugin/types.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace eosio::resource_feed {

class serializer {
public:
   const std::vector<char>& data() const { return buffer; }
   void                      clear() { buffer.clear(); }

   std::vector<char> take() {
      auto out = std::move(buffer);
      buffer.clear();
      return out;
   }

   void append(const std::vector<char>& frames) { buffer.insert(buffer.end(), frames.begin(), frames.end()); }

   void hello(const chain::chain_id_type& chain_id, uint32_t head_block_num) {
      begin_frame(frame_type::hello);
      write_le(FEED_MAGIC);
      write_le(FEED_VERSION);
      write_bytes(chain_id.data(), 32);
      write_le(head_block_num);
      end_frame();
   }

   void snapshot_begin(uint32_t block_num) {
      begin_frame(frame_type::snapshot_begin);
      write_le(block_num);
      end_frame();
   }

   void accounts(uint32_t block_num, const std::vector<account_record>& records) {
      buffer.reserve(buffer.size() + FRAME_HEADER_SIZE + 8 + records.size() * ACCOUNT_RECORD_SIZE);
      begin_frame(frame_type::accounts);
      write_le(block_num);
      write_le(static_cast<uint32_t>(records.size()));
      for (const auto& r : records)
         write_account_record(r);
      end_frame();
   }

   void accounts_removed(uint32_t block_num, const std::vector<uint64_t>& owners) {
      buffer.reserve(buffer.size() + FRAME_HEADER_SIZE + 8 + owners.size() * 8);
      begin_frame(frame_type::accounts_removed);
      write_le(block_num);
      write_le(static_cast<uint32_t>(owners.size()));
      for (uint64_t owner : owners)
         write_le(owner);
      end_frame();
   }

   void globals(uint32_t block_num, const globals_record& g) {
      begin_frame(frame_type::globals);
      write_le(block_num);
      write_le(g.total_net_weight);
      write_le(g.total_cpu_weight);
      write_le(g.total_ram_bytes);
      write_le(g.virtual_net_limit);
      write_le(g.virtual_cpu_limit);
      end_frame();
   }

   void config(uint32_t block_num, const config_record& c) {
      begin_frame(frame_type::config);
      write_le(block_num);
      write_le(c.cpu_window);
      write_le(c.net_window);
      end_frame();
   }

   void snapshot_end(uint32_t block_num) {
      begin_frame(frame_type::snapshot_end);
      write_le(block_num);
      end_frame();
   }

   void block(uint32_t block_num, const chain::block_id_type& block_id, uint32_t timestamp_slot) {
      begin_frame(frame_type::block);
      write_le(block_num);
      write_bytes(block_id.data(), 32);
      write_le(timestamp_slot);
      end_frame();
   }

private:
   static constexpr size_t FRAME_HEADER_SIZE   = 5;
   static constexpr size_t ACCOUNT_RECORD_SIZE = 80;

   std::vector<char> buffer;
   size_t            frame_start = 0;

   void begin_frame(frame_type type) {
      frame_start = buffer.size();
      buffer.insert(buffer.end(), 4, 0);
      write_le(static_cast<uint8_t>(type));
   }

   void end_frame() {
      const auto payload_len = static_cast<uint32_t>(buffer.size() - frame_start - FRAME_HEADER_SIZE);
      for (size_t i = 0; i < 4; ++i)
         buffer[frame_start + i] = static_cast<char>(payload_len >> (8 * i));
   }

   void write_account_record(const account_record& r) {
      write_le(r.owner);
      write_le(static_cast<uint64_t>(r.net_weight));
      write_le(static_cast<uint64_t>(r.cpu_weight));
      write_le(static_cast<uint64_t>(r.ram_bytes));
      write_le(r.net_last_ordinal);
      write_le(r.net_value_ex);
      write_le(r.net_consumed);
      write_le(r.cpu_last_ordinal);
      write_le(r.cpu_value_ex);
      write_le(r.cpu_consumed);
      write_le(r.ram_usage);
   }

   template <typename T>
   void write_le(T v) {
      static_assert(std::is_unsigned_v<T>, "little-endian writes take unsigned values");
      for (size_t i = 0; i < sizeof(T); ++i)
         buffer.push_back(static_cast<char>(v >> (8 * i)));
   }

   void write_bytes(const char* data, size_t len) { buffer.insert(buffer.end(), data, data + len); }
};

} // namespace eosio::resource_feed
