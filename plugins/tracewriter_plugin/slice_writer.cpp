#include <eosio/tracewriter_plugin/slice_writer.hpp>

#include <fc/io/json.hpp>

#include <boost/crc.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace eosio::tracewriter {

slice_writer::slice_writer(const std::filesystem::path& dir, uint32_t blocks_per_slice, fc::logger& log)
   : log(log)
   , base_dir(dir)
   , blocks_per_slice(blocks_per_slice) {
   write_buffer.reserve(WRITE_BUFFER_SIZE);

   zstd_ctx = ZSTD_createCCtx();
   if (!zstd_ctx) {
      throw compression_exception("Failed to create ZSTD compression context");
   }

   load_existing_state();
}

slice_writer::~slice_writer() {
   close_current_slice();
   if (zstd_ctx) {
      ZSTD_freeCCtx(zstd_ctx);
      zstd_ctx = nullptr;
   }
}

bool slice_writer::has_block(uint32_t block_num) const {
   return current_index.count(block_num) > 0;
}

uint32_t slice_writer::get_last_block() const {
   if (current_index.empty())
      return 0;
   return current_index.rbegin()->first;
}

void slice_writer::handle_duplicate_block(uint32_t block_num) {
   uint32_t last_block = get_last_block();
   if (block_num == last_block) {
      truncate_last_block(block_num);
   } else if (block_num < last_block) {
      throw data_corruption_exception(
          "Tracewriter: block " + std::to_string(block_num) +
          " already exists but is not the last block (last=" + std::to_string(last_block) +
          "). This indicates data inconsistency.");
   }
}

void slice_writer::write_block(uint32_t block_num, uint64_t min_glob, uint64_t max_glob,
                               const std::vector<char>& data) {
   if (has_block(block_num)) {
      handle_duplicate_block(block_num);
   }

   uint32_t block_slice = (block_num - 1) / blocks_per_slice;
   if (block_slice != current_slice_num) {
      if (current_slice_num != UINT32_MAX) {
         finalize_current_slice();
      }
      uint32_t slice_start = block_slice * blocks_per_slice + 1;
      uint32_t slice_max   = (block_slice + 1) * blocks_per_slice;
      open_slice(slice_start, slice_max);
      current_slice_num = block_slice;
   }

   std::vector<char> compressed = compress_zstd(data);

   uint32_t size_le = static_cast<uint32_t>(compressed.size());
   buffer_write(reinterpret_cast<const char*>(&size_le), 4);
   buffer_write(compressed.data(), compressed.size());

   uint32_t crc = compute_crc32(compressed.data(), compressed.size());
   buffer_write(reinterpret_cast<const char*>(&crc), 4);

   blocks_index_entry entry;
   entry.block_num       = block_num;
   entry.offset          = data_log_offset;
   entry.compressed_size = static_cast<uint32_t>(compressed.size());
   entry.glob_min        = min_glob;
   entry.glob_max        = max_glob;
   blocks_index_entries.push_back(entry);
   current_index[block_num] = entry;

   data_log_offset += 4 + compressed.size() + 4;

   if (min_glob > 0 && min_glob != std::numeric_limits<uint64_t>::max()) {
      slice_glob_min = std::min(slice_glob_min, min_glob);
   }
   if (max_glob > 0) {
      slice_glob_max = std::max(slice_glob_max, max_glob);
   }

   last_written_block = block_num;
   bytes_compressed_total += compressed.size();
}

void slice_writer::save_metadata() {
   flush_write_buffer();
   write_blocks_index();
   save_slices_json();
}

void slice_writer::load_existing_state() {
   std::filesystem::path slices_path = base_dir / "slices.json";
   if (std::filesystem::exists(slices_path)) {
      try {
         auto var = fc::json::from_file(slices_path);
         if (var.is_array()) {
            for (const auto& elem : var.get_array()) {
               if (elem.is_object()) {
                  const auto& obj = elem.get_object();
                  slice_info  si;
                  si.slice_num        = obj["slice_num"].as_uint64();
                  si.start_block      = obj["start_block"].as_uint64();
                  si.end_block        = obj["end_block"].as_uint64();
                  si.max_block        = obj["max_block"].as_uint64();
                  si.blocks_per_slice = obj["blocks_per_slice"].as_uint64();
                  si.finalized        = obj["finalized"].as_bool();
                  si.glob_min         = obj["glob_min"].as_uint64();
                  si.glob_max         = obj["glob_max"].as_uint64();
                  slice_infos.push_back(si);
               }
            }
         }
      } catch (const std::exception& e) {
         fc_wlog(log, "Failed to load slices.json, starting fresh: ${e}", ("e", e.what()));
         slice_infos.clear();
      } catch (...) {
         fc_wlog(log, "Failed to load slices.json (unknown error), starting fresh");
         slice_infos.clear();
      }
   }

   if (!slice_infos.empty()) {
      const auto& last_slice = slice_infos.back();
      if (!last_slice.finalized) {
         load_slice_index(last_slice.start_block, last_slice.max_block);
         current_slice_num = last_slice.slice_num;
      } else {
         uint32_t next_slice_start = last_slice.max_block + 1;
         uint32_t next_slice_num   = (next_slice_start - 1) / blocks_per_slice;
         load_slice_index(next_slice_start, (next_slice_num + 1) * blocks_per_slice);
         current_slice_num = next_slice_num;
      }
   }
}

void slice_writer::load_slice_index(uint32_t slice_start, uint32_t slice_max) {
   std::ostringstream ss;
   ss << "history_" << std::setfill('0') << std::setw(10) << slice_start << "-" << std::setfill('0') << std::setw(10)
      << slice_max;
   std::filesystem::path slice_dir = base_dir / ss.str();
   std::filesystem::path idx_path  = slice_dir / "blocks.index";

   current_index.clear();

   if (!std::filesystem::exists(idx_path)) {
      return;
   }

   fc::cfile idx_file;
   idx_file.set_file_path(idx_path);
   idx_file.open("rb");

   uint32_t count = 0;
   idx_file.read(reinterpret_cast<char*>(&count), 4);

   for (uint32_t i = 0; i < count; ++i) {
      blocks_index_entry entry;
      idx_file.read(reinterpret_cast<char*>(&entry.block_num), 4);
      idx_file.read(reinterpret_cast<char*>(&entry.offset), 8);
      idx_file.read(reinterpret_cast<char*>(&entry.compressed_size), 4);
      idx_file.read(reinterpret_cast<char*>(&entry.glob_min), 8);
      idx_file.read(reinterpret_cast<char*>(&entry.glob_max), 8);
      current_index[entry.block_num] = entry;
      blocks_index_entries.push_back(entry);

      if (entry.glob_min > 0 && entry.glob_min != std::numeric_limits<uint64_t>::max()) {
         slice_glob_min = std::min(slice_glob_min, entry.glob_min);
      }
      if (entry.glob_max > 0) {
         slice_glob_max = std::max(slice_glob_max, entry.glob_max);
      }

      data_log_offset    = entry.offset + 4 + entry.compressed_size + 4;
      last_written_block = entry.block_num;
   }

   idx_file.close();

   current_slice_start = slice_start;
   current_slice_max   = slice_max;
   current_slice_dir   = slice_dir;
   data_log_path       = slice_dir / "data.log";
   blocks_idx_path     = slice_dir / "blocks.index";

   data_log_file = std::make_unique<fc::cfile>();
   data_log_file->set_file_path(data_log_path);
   data_log_file->open(fc::cfile::create_or_update_rw_mode);
   data_log_file->seek_end(0);
}

void slice_writer::truncate_last_block(uint32_t block_num) {
   auto it = current_index.find(block_num);
   if (it == current_index.end())
      return;

   uint64_t truncate_offset = it->second.offset;

   flush_write_buffer();
   if (data_log_file && data_log_file->is_open()) {
      data_log_file->close();
   }

   std::filesystem::resize_file(data_log_path, truncate_offset);

   current_index.erase(it);
   if (!blocks_index_entries.empty() && blocks_index_entries.back().block_num == block_num) {
      blocks_index_entries.pop_back();
   }

   data_log_offset = truncate_offset;
   data_log_file   = std::make_unique<fc::cfile>();
   data_log_file->set_file_path(data_log_path);
   data_log_file->open(fc::cfile::create_or_update_rw_mode);
   data_log_file->seek_end(0);

   fc_ilog(log, "Truncated and will rewrite block ${b}", ("b", block_num));
}

void slice_writer::open_slice(uint32_t slice_start, uint32_t slice_max) {
   close_current_slice();

   current_slice_start = slice_start;
   current_slice_max   = slice_max;
   blocks_index_entries.clear();
   blocks_index_entries.reserve(blocks_per_slice);
   current_index.clear();

   std::ostringstream ss;
   ss << "history_" << std::setfill('0') << std::setw(10) << slice_start << "-" << std::setfill('0') << std::setw(10)
      << slice_max;
   current_slice_dir = base_dir / ss.str();
   std::filesystem::create_directories(current_slice_dir);

   data_log_path   = current_slice_dir / "data.log";
   blocks_idx_path = current_slice_dir / "blocks.index";

   data_log_file = std::make_unique<fc::cfile>();
   data_log_file->set_file_path(data_log_path);
   data_log_file->open(fc::cfile::truncate_rw_mode);

   slice_header header;
   header.start_block      = slice_start;
   header.max_block        = slice_max;
   header.blocks_per_slice = blocks_per_slice;

   data_log_file->write(reinterpret_cast<const char*>(&header), sizeof(header));
   data_log_file->flush();
   data_log_offset = sizeof(header);

   slice_glob_min = std::numeric_limits<uint64_t>::max();
   slice_glob_max = 0;
}

void slice_writer::finalize_current_slice() {
   flush_write_buffer();
   if (data_log_file && data_log_file->is_open()) {
      data_log_file->close();
   }
   write_blocks_index();

   slice_info info;
   info.slice_num        = (current_slice_start - 1) / blocks_per_slice;
   info.start_block      = current_slice_start;
   info.end_block        = current_slice_max;
   info.max_block        = current_slice_max;
   info.blocks_per_slice = blocks_per_slice;
   info.finalized        = true;
   info.glob_min         = (slice_glob_min == std::numeric_limits<uint64_t>::max()) ? 0 : slice_glob_min;
   info.glob_max         = slice_glob_max;
   slice_infos.push_back(info);

   save_slices_json();
   current_index.clear();
}

void slice_writer::close_current_slice() {
   flush_write_buffer();
   if (data_log_file && data_log_file->is_open()) {
      data_log_file->close();
   }
   write_blocks_index();
}

void slice_writer::write_blocks_index() {
   if (blocks_index_entries.empty())
      return;

   fc::cfile idx_file;
   idx_file.set_file_path(blocks_idx_path);
   idx_file.open(fc::cfile::truncate_rw_mode);

   uint32_t count = static_cast<uint32_t>(blocks_index_entries.size());
   idx_file.write(reinterpret_cast<const char*>(&count), 4);

   for (const auto& entry : blocks_index_entries) {
      idx_file.write(reinterpret_cast<const char*>(&entry.block_num), 4);
      idx_file.write(reinterpret_cast<const char*>(&entry.offset), 8);
      idx_file.write(reinterpret_cast<const char*>(&entry.compressed_size), 4);
      idx_file.write(reinterpret_cast<const char*>(&entry.glob_min), 8);
      idx_file.write(reinterpret_cast<const char*>(&entry.glob_max), 8);
   }
   idx_file.flush();
   idx_file.close();
}

void slice_writer::save_slices_json() {
   std::filesystem::path slices_path = base_dir / "slices.json";
   std::ofstream         file(slices_path, std::ios::out | std::ios::trunc);
   if (!file.is_open()) {
      fc_elog(log, "Failed to open ${path} for writing", ("path", slices_path.string()));
      return;
   }

   file << "[";
   size_t count = 0;
   for (size_t i = 0; i < slice_infos.size(); ++i) {
      const auto& s = slice_infos[i];
      if (count > 0)
         file << ",";
      file << "{\"slice_num\":" << s.slice_num << ",\"start_block\":" << s.start_block
           << ",\"end_block\":" << s.end_block << ",\"max_block\":" << s.max_block
           << ",\"blocks_per_slice\":" << s.blocks_per_slice << ",\"finalized\":" << (s.finalized ? "true" : "false")
           << ",\"glob_min\":" << s.glob_min << ",\"glob_max\":" << s.glob_max << "}";
      count++;
   }

   if (current_slice_num != UINT32_MAX && !blocks_index_entries.empty()) {
      bool already_in_list = false;
      for (const auto& s : slice_infos) {
         if (s.slice_num == current_slice_num) {
            already_in_list = true;
            break;
         }
      }
      if (!already_in_list) {
         if (count > 0)
            file << ",";
         uint64_t glob_min = (slice_glob_min == std::numeric_limits<uint64_t>::max()) ? 0 : slice_glob_min;
         file << "{\"slice_num\":" << current_slice_num << ",\"start_block\":" << current_slice_start
              << ",\"end_block\":" << current_slice_max << ",\"max_block\":" << current_slice_max
              << ",\"blocks_per_slice\":" << blocks_per_slice << ",\"finalized\":false"
              << ",\"glob_min\":" << glob_min << ",\"glob_max\":" << slice_glob_max << "}";
      }
   }

   file << "]";
   file.close();
}

std::vector<char> slice_writer::compress_zstd(const std::vector<char>& input) {
   size_t            max_size = ZSTD_compressBound(input.size());
   std::vector<char> output(max_size);
   size_t compressed_size = ZSTD_compressCCtx(zstd_ctx, output.data(), max_size, input.data(), input.size(), 3);
   if (ZSTD_isError(compressed_size)) {
      throw compression_exception(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
   }
   output.resize(compressed_size);
   return output;
}

uint32_t slice_writer::compute_crc32(const char* data, size_t len) {
   boost::crc_32_type crc;
   crc.process_bytes(data, len);
   return crc.checksum();
}

void slice_writer::flush_write_buffer() {
   if (write_buffer.empty() || !data_log_file || !data_log_file->is_open())
      return;
   data_log_file->write(write_buffer.data(), write_buffer.size());
   data_log_file->flush();
   write_buffer.clear();
}

void slice_writer::buffer_write(const char* data, size_t len) {
   if (write_buffer.size() + len > WRITE_BUFFER_SIZE) {
      flush_write_buffer();
   }
   write_buffer.insert(write_buffer.end(), data, data + len);
}

} // namespace eosio::tracewriter
