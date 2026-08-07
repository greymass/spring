#pragma once
#include <eosio/tracewriter_plugin/types.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace eosio::tracewriter {

class block_serializer {
public:
   static constexpr size_t INITIAL_BUFFER_SIZE = 64 * 1024;

   block_serializer() { buffer.reserve(INITIAL_BUFFER_SIZE); }

   void clear() { buffer.clear(); }

   const std::vector<char>& data() const { return buffer; }

   void write_u8(uint8_t v) { buffer.push_back(static_cast<char>(v)); }

   void write_u16_le(uint16_t v) {
      buffer.push_back(static_cast<char>(v));
      buffer.push_back(static_cast<char>(v >> 8));
   }

   void write_u32_le(uint32_t v) {
      buffer.push_back(static_cast<char>(v));
      buffer.push_back(static_cast<char>(v >> 8));
      buffer.push_back(static_cast<char>(v >> 16));
      buffer.push_back(static_cast<char>(v >> 24));
   }

   void write_u64_le(uint64_t v) {
      buffer.push_back(static_cast<char>(v));
      buffer.push_back(static_cast<char>(v >> 8));
      buffer.push_back(static_cast<char>(v >> 16));
      buffer.push_back(static_cast<char>(v >> 24));
      buffer.push_back(static_cast<char>(v >> 32));
      buffer.push_back(static_cast<char>(v >> 40));
      buffer.push_back(static_cast<char>(v >> 48));
      buffer.push_back(static_cast<char>(v >> 56));
   }

   void write_uvarint(uint64_t v) {
      while (v >= 0x80) {
         buffer.push_back(static_cast<char>((v & 0x7F) | 0x80));
         v >>= 7;
      }
      buffer.push_back(static_cast<char>(v));
   }

   void write_varint(int64_t v) {
      uint64_t uv = static_cast<uint64_t>(v) << 1;
      if (v < 0) {
         uv = ~uv;
      }
      write_uvarint(uv);
   }

   void write_bytes(const char* data, size_t len) { buffer.insert(buffer.end(), data, data + len); }

   void write_bytes(const std::vector<char>& data) { buffer.insert(buffer.end(), data.begin(), data.end()); }

   template <size_t N>
   void write_bytes(const std::array<char, N>& arr) {
      buffer.insert(buffer.end(), arr.begin(), arr.end());
   }

   void write_compressed_action_trace(const compressed_action_trace& cat) {
      uint8_t  magicmask1 = 0;
      uint8_t  magicmask2 = 0;
      uint32_t auths_len  = static_cast<uint32_t>(cat.auths.size());
      uint32_t delta_len  = static_cast<uint32_t>(cat.account_ram_deltas.size());

      if (cat.contract_name_index == cat.receiver_index)
         magicmask1 |= (1 << 7);
      if (auths_len == 1)
         magicmask1 |= (1 << 6);
      if (delta_len == 0)
         magicmask1 |= (1 << 5);
      if (cat.creator_ao == cat.closest_ua_ao)
         magicmask1 |= (1 << 3);
      if (cat.code_sequence == 1)
         magicmask1 |= (1 << 2);
      if (cat.abi_sequence == cat.code_sequence)
         magicmask1 |= (1 << 1);
      if (cat.action_ordinal == 1)
         magicmask1 |= (1 << 0);

      if (cat.creator_ao == cat.action_ordinal - 1)
         magicmask2 |= (1 << 0);
      if (delta_len == 1)
         magicmask2 |= (1 << 1);
      if (cat.trx_id_index == 0)
         magicmask2 |= (1 << 2);
      if (cat.data_index == 0)
         magicmask2 |= (1 << 3);
      if (auths_len >= 1 && cat.auths[0].account_index == cat.contract_name_index)
         magicmask2 |= (1 << 4);
      if (delta_len >= 1 && auths_len >= 1 && cat.account_ram_deltas[0].account_index == cat.auths[0].account_index)
         magicmask2 |= (1 << 5);
      if (cat.receiver_index == 0)
         magicmask2 |= (1 << 6);
      if (cat.context_free)
         magicmask2 |= (1 << 7);

      if (magicmask2 > 0)
         magicmask1 |= (1 << 4);

      write_u8(magicmask1);
      if (magicmask2 > 0)
         write_u8(magicmask2);

      write_varint(cat.elapsed);
      write_uvarint(cat.recv_sequence);
      write_uvarint(cat.global_sequence);

      if ((magicmask1 & (1 << 0)) == 0)
         write_uvarint(cat.action_ordinal - 2);

      if ((magicmask2 & (1 << 0)) == 0)
         write_uvarint(cat.creator_ao);

      if ((magicmask1 & (1 << 3)) == 0)
         write_uvarint(cat.closest_ua_ao ^ cat.creator_ao);

      if ((magicmask1 & (1 << 2)) == 0)
         write_uvarint(cat.code_sequence - 2);

      if ((magicmask1 & (1 << 1)) == 0)
         write_uvarint(cat.abi_sequence ^ cat.code_sequence);

      if ((magicmask2 & (1 << 6)) == 0)
         write_uvarint(cat.receiver_index);

      if ((magicmask1 & (1 << 7)) == 0)
         write_uvarint(cat.contract_name_index);

      write_uvarint(cat.action_name_index);

      if ((magicmask2 & (1 << 2)) == 0)
         write_uvarint(cat.trx_id_index);

      if ((magicmask2 & (1 << 3)) == 0)
         write_uvarint(cat.data_index);

      if ((magicmask1 & (1 << 6)) == 0)
         write_uvarint(auths_len - 1);

      for (uint32_t i = 0; i < auths_len; ++i) {
         if (i != 0 || ((magicmask2 & (1 << 4)) == 0))
            write_uvarint(cat.auths[i].account_index);
         write_uvarint(cat.auths[i].permission_index);
         write_uvarint(cat.auths[i].seq);
      }

      if ((magicmask1 & (1 << 5)) == 0) {
         if ((magicmask2 & (1 << 1)) == 0)
            write_uvarint(delta_len - 1);
         for (uint32_t i = 0; i < delta_len; ++i) {
            if (i != 0 || ((magicmask2 & (1 << 5)) == 0))
               write_uvarint(cat.account_ram_deltas[i].account_index);
            write_varint(cat.account_ram_deltas[i].delta);
         }
      }
   }

   void write_block_data(const block_data& blk) {
      write_u32_le(blk.block_num);
      write_u32_le(blk.block_time);

      const auto& id_data = blk.producer_block_id.data();
      write_bytes(id_data, 32);

      write_u64_le(blk.max_glob);
      uint64_t glob_count = (blk.max_glob >= blk.min_glob) ? (blk.max_glob - blk.min_glob) : 0;
      write_uvarint(glob_count);

      write_uvarint(blk.trx_ids.size());
      for (const auto& trx_id : blk.trx_ids) {
         write_bytes(trx_id.data(), 32);
      }

      for (const auto& meta : blk.trx_metas) {
         write_u8(meta.status);
         write_u32_le(meta.cpu_usage_us);
         write_uvarint(meta.net_usage_words);
         write_u32_le(meta.expiration);
         write_u16_le(meta.ref_block_num);
         write_u32_le(meta.ref_block_prefix);
         write_uvarint(meta.signatures.size());
         for (const auto& sig : meta.signatures) {
            write_uvarint(sig.size());
            write_bytes(sig);
         }
      }

      write_uvarint(blk.data_table.size());
      for (const auto& data : blk.data_table) {
         write_uvarint(data.size());
         write_bytes(data);
      }

      write_uvarint(blk.names_table.size());
      for (uint64_t name : blk.names_table) {
         write_u64_le(name);
      }
   }

   void write_block_blob(const block_data& blk, const std::vector<compressed_action_trace>& cats,
                         const std::vector<uint32_t>& cat_offsets) {
      write_block_data(blk);

      write_uvarint(cat_offsets.size());
      for (uint32_t offset : cat_offsets) {
         write_uvarint(offset);
      }

      std::vector<std::vector<char>> cat_bytes;
      cat_bytes.reserve(cats.size());
      for (const auto& cat : cats) {
         block_serializer cat_ser;
         cat_ser.write_compressed_action_trace(cat);
         cat_bytes.push_back(cat_ser.data());
      }

      for (const auto& cb : cat_bytes) {
         write_uvarint(cb.size());
      }
      for (const auto& cb : cat_bytes) {
         write_bytes(cb);
      }
   }

private:
   std::vector<char> buffer;
};

} // namespace eosio::tracewriter
