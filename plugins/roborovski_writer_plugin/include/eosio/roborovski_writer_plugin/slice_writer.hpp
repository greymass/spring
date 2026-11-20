#pragma once
#include <eosio/roborovski_writer_plugin/types.hpp>

#include <fc/io/cfile.hpp>
#include <fc/log/logger.hpp>

#include <zstd.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace eosio::roborovski_writer {

std::string slice_dir_name(uint32_t slice_num);
std::string slices_json(const std::vector<slice_info>& infos);
std::vector<char> slice_header_bytes(uint32_t start_block, uint32_t max_block, bool finalized);

class slice_writer {
public:
   slice_writer(const std::filesystem::path& dir, fc::logger& log);
   ~slice_writer();

   slice_writer(const slice_writer&)            = delete;
   slice_writer& operator=(const slice_writer&) = delete;

   bool     empty() const { return last_block == 0; }
   uint32_t get_last_block() const { return last_block; }
   uint64_t get_bytes_compressed() const { return bytes_compressed; }
   const std::vector<slice_info>& get_slice_infos() const { return infos; }

   void append(const built_block& block);
   void flush();
   void close();

private:
   void validate_layout();
   void load_active_slice();
   void open_slice(uint32_t slice_num);
   void finalize_active_slice();
   void check_complete() const;
   void write_index();
   void save_slices_json();
   void update_active_info();

   std::vector<char> compress(const std::vector<char>& input);

   fc::logger&           log;
   std::filesystem::path base_dir;

   std::vector<slice_info> infos;

   bool                            has_active = false;
   uint32_t                        active_slice_num = 0;
   std::filesystem::path           active_dir;
   std::unique_ptr<fc::cfile>      data_log;
   uint64_t                        data_offset = 0;
   std::vector<blocks_index_entry> entries;
   size_t                          persisted_entries = 0;

   uint32_t last_block       = 0;
   uint32_t first_slice_num  = 0;
   uint64_t bytes_compressed = 0;

   ZSTD_CCtx* zstd_ctx = nullptr;
};

} // namespace eosio::roborovski_writer
