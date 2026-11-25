#pragma once
#include <eosio/roborovski_writer_plugin/types.hpp>

#include <cstdint>
#include <vector>

namespace eosio::roborovski_writer {

class block_serializer {
public:
   void clear() { buffer.clear(); }

   const std::vector<char>& data() const { return buffer; }
   std::vector<char>        take() { return std::move(buffer); }

   void write_u8(uint8_t v) { buffer.push_back(static_cast<char>(v)); }

   void write_u16_le(uint16_t v) {
      for (int i = 0; i < 2; ++i)
         buffer.push_back(static_cast<char>(v >> (8 * i)));
   }

   void write_u32_le(uint32_t v) {
      for (int i = 0; i < 4; ++i)
         buffer.push_back(static_cast<char>(v >> (8 * i)));
   }

   void write_u64_le(uint64_t v) {
      for (int i = 0; i < 8; ++i)
         buffer.push_back(static_cast<char>(v >> (8 * i)));
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
      if (v < 0)
         uv = ~uv;
      write_uvarint(uv);
   }

   void write_bytes(const char* data, size_t len) { buffer.insert(buffer.end(), data, data + len); }

   void write_bytes(const std::vector<char>& data) { buffer.insert(buffer.end(), data.begin(), data.end()); }

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
         write_uvarint(static_cast<uint32_t>(cat.action_ordinal - 2));
      if ((magicmask2 & (1 << 0)) == 0)
         write_uvarint(cat.creator_ao);
      if ((magicmask1 & (1 << 3)) == 0)
         write_uvarint(cat.closest_ua_ao ^ cat.creator_ao);
      if ((magicmask1 & (1 << 2)) == 0)
         write_uvarint(static_cast<uint32_t>(cat.code_sequence - 2));
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
         write_uvarint(static_cast<uint32_t>(auths_len - 1));
      for (uint32_t i = 0; i < auths_len; ++i) {
         if (i != 0 || ((magicmask2 & (1 << 4)) == 0))
            write_uvarint(cat.auths[i].account_index);
         write_uvarint(cat.auths[i].permission_index);
         write_uvarint(cat.auths[i].seq);
      }

      if ((magicmask1 & (1 << 5)) == 0) {
         if ((magicmask2 & (1 << 1)) == 0)
            write_uvarint(static_cast<uint32_t>(delta_len - 1));
         for (uint32_t i = 0; i < delta_len; ++i) {
            if (i != 0 || ((magicmask2 & (1 << 5)) == 0))
               write_uvarint(cat.account_ram_deltas[i].account_index);
            write_varint(cat.account_ram_deltas[i].delta);
         }
      }
   }

private:
   std::vector<char> buffer;
};

} // namespace eosio::roborovski_writer
