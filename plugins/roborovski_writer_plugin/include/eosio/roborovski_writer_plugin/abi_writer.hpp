#pragma once
#include <eosio/roborovski_writer_plugin/types.hpp>

#include <fc/io/cfile.hpp>

#include <filesystem>
#include <utility>

namespace eosio::roborovski_writer {

class abi_writer {
public:
   static constexpr size_t INDEX_ENTRY_SIZE = 20;
   static constexpr size_t DATA_HEADER_SIZE = 16;

   explicit abi_writer(const std::filesystem::path& dir);
   ~abi_writer();

   abi_writer(const abi_writer&)            = delete;
   abi_writer& operator=(const abi_writer&) = delete;

   bool     empty() const { return entry_count == 0; }
   uint64_t entries() const { return entry_count; }

   void write(const abi_record& record);
   void flush();

   struct extent {
      uint64_t data_end  = 0;
      uint32_t index_len = 0;
   };
   extent sync();

private:
   std::filesystem::path dir;
   fc::cfile             data_file;
   fc::cfile             index_file;
   uint64_t              data_offset = 0;
   uint64_t              entry_count = 0;
};

} // namespace eosio::roborovski_writer
