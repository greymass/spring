#pragma once
#include <eosio/tracewriter_plugin/types.hpp>

#include <fc/io/cfile.hpp>
#include <fc/log/logger.hpp>

#include <zstd.h>

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace eosio::tracewriter {

class slice_writer {
public:
   static constexpr size_t WRITE_BUFFER_SIZE = 16 * 1024 * 1024;

   slice_writer(const std::filesystem::path& dir, uint32_t blocks_per_slice, fc::logger& log);
   ~slice_writer();

   slice_writer(const slice_writer&)            = delete;
   slice_writer& operator=(const slice_writer&) = delete;

   bool     has_block(uint32_t block_num) const;
   uint32_t get_last_block() const;
   void     handle_duplicate_block(uint32_t block_num);
   void     write_block(uint32_t block_num, uint64_t min_glob, uint64_t max_glob, const std::vector<char>& data);

   uint64_t                        get_bytes_compressed() const { return bytes_compressed_total; }
   uint32_t                        get_last_written_block() const { return last_written_block; }
   const std::vector<slice_info>&  get_slice_infos() const { return slice_infos; }

   void save_metadata();

private:
   void load_existing_state();
   void load_slice_index(uint32_t slice_start, uint32_t slice_max);
   void truncate_last_block(uint32_t block_num);
   void open_slice(uint32_t slice_start, uint32_t slice_max);
   void finalize_current_slice();
   void close_current_slice();
   void write_blocks_index();
   void save_slices_json();
   void flush_write_buffer();
   void buffer_write(const char* data, size_t len);

   std::vector<char> compress_zstd(const std::vector<char>& input);
   uint32_t          compute_crc32(const char* data, size_t len);

   fc::logger&                            log;
   std::filesystem::path                  base_dir;
   uint32_t                               blocks_per_slice;
   uint32_t                               current_slice_num   = UINT32_MAX;
   uint32_t                               current_slice_start = 0;
   uint32_t                               current_slice_max   = 0;
   std::filesystem::path                  current_slice_dir;
   std::filesystem::path                  data_log_path;
   std::filesystem::path                  blocks_idx_path;
   std::unique_ptr<fc::cfile>             data_log_file;
   std::vector<blocks_index_entry>        blocks_index_entries;
   std::map<uint32_t, blocks_index_entry> current_index;
   uint64_t                               data_log_offset = 0;
   std::vector<char>                      write_buffer;

   uint64_t slice_glob_min = std::numeric_limits<uint64_t>::max();
   uint64_t slice_glob_max = 0;

   uint32_t                last_written_block{0};
   uint64_t                bytes_compressed_total{0};
   std::vector<slice_info> slice_infos;

   ZSTD_CCtx* zstd_ctx = nullptr;
};

} // namespace eosio::tracewriter
