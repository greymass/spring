#include <eosio/roborovski_writer_plugin/recovery.hpp>
#include <eosio/roborovski_writer_plugin/slice_writer.hpp>
#include <eosio/roborovski_writer_plugin/store_format.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>

#include <boost/crc.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace eosio::roborovski_writer {

namespace {

void write_file_atomic(const std::filesystem::path& path, const std::string& content) {
   auto tmp = path;
   tmp += ".tmp";
   {
      std::ofstream f(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
      if (!f)
         throw io_exception("cannot open " + tmp.string() + " for writing");
      f.write(content.data(), content.size());
      if (!f)
         throw io_exception("cannot write " + tmp.string());
   }
   std::filesystem::rename(tmp, path);
}

std::string file_sha256(const std::filesystem::path& path) {
   std::ifstream f(path, std::ios::binary);
   if (!f)
      throw io_exception("cannot open " + path.string() + " for hashing");
   fc::sha256::encoder enc;
   std::vector<char>   buf(1 << 20);
   while (f) {
      f.read(buf.data(), buf.size());
      if (f.gcount() > 0)
         enc.write(buf.data(), static_cast<uint32_t>(f.gcount()));
   }
   return enc.result().str();
}

std::vector<blocks_index_entry> read_index(const std::filesystem::path& path) {
   std::vector<blocks_index_entry> out;
   if (!std::filesystem::exists(path))
      return out;
   std::ifstream f(path, std::ios::binary);
   std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   if (bytes.size() < 4)
      throw data_corruption_exception(path.string() + " is shorter than its count");
   const uint32_t count = static_cast<uint32_t>(get_le(bytes.data(), 4));
   if (bytes.size() < 4 + static_cast<size_t>(count) * INDEX_ENTRY_SIZE)
      throw data_corruption_exception(path.string() + " holds fewer entries than its count of " +
                                      std::to_string(count));
   out.reserve(count);
   for (uint32_t i = 0; i < count; ++i) {
      const char*        e = bytes.data() + 4 + static_cast<size_t>(i) * INDEX_ENTRY_SIZE;
      blocks_index_entry entry;
      entry.block_num       = static_cast<uint32_t>(get_le(e, 4));
      entry.offset          = get_le(e + 4, 8);
      entry.compressed_size = static_cast<uint32_t>(get_le(e + 12, 4));
      entry.glob_min        = get_le(e + 16, 8);
      entry.glob_max        = get_le(e + 24, 8);
      out.push_back(entry);
   }
   return out;
}

} // namespace

std::string slice_dir_name(uint32_t slice_num) {
   char buf[32];
   std::snprintf(buf, sizeof(buf), "history_%010u-%010u", slice_start(slice_num), slice_max(slice_num));
   return buf;
}

std::string slices_json(const std::vector<slice_info>& infos) {
   std::string out = "[";
   for (size_t i = 0; i < infos.size(); ++i) {
      const auto& s = infos[i];
      if (i)
         out += ',';
      out += "{\"slice_num\":" + std::to_string(s.slice_num) + ",\"start_block\":" + std::to_string(s.start_block) +
             ",\"end_block\":" + std::to_string(s.end_block) + ",\"max_block\":" + std::to_string(s.max_block) +
             ",\"blocks_per_slice\":" + std::to_string(s.blocks_per_slice) +
             ",\"finalized\":" + (s.finalized ? "true" : "false") + ",\"glob_min\":" + std::to_string(s.glob_min) +
             ",\"glob_max\":" + std::to_string(s.glob_max) + "}";
   }
   out += "]";
   return out;
}

std::vector<char> slice_header_bytes(uint32_t start_block, uint32_t max_block, bool finalized) {
   std::vector<char> h(SLICE_HEADER_SIZE, 0);
   std::copy(std::begin(SLICE_MAGIC), std::end(SLICE_MAGIC), h.begin());
   h[7] = static_cast<char>(SLICE_VERSION);
   put_le(h.data() + 8, BLOCKS_PER_SLICE, 4);
   put_le(h.data() + 12, start_block, 4);
   put_le(h.data() + 16, max_block, 4);
   h[20] = finalized ? 1 : 0;
   return h;
}

slice_writer::slice_writer(const std::filesystem::path& dir, fc::logger& l)
   : log(l)
   , base_dir(dir) {
   zstd_ctx = ZSTD_createCCtx();
   if (!zstd_ctx)
      throw compression_exception("failed to create a zstd compression context");

   std::filesystem::create_directories(base_dir);
   validate_layout();
   load_active_slice();
}

slice_writer::~slice_writer() {
   try {
      close();
   } catch (const std::exception& e) {
      fc_elog(log, "closing core storage failed: ${e}", ("e", e.what()));
   }
   if (zstd_ctx)
      ZSTD_freeCCtx(zstd_ctx);
}

void slice_writer::validate_layout() {
   const auto json_path = base_dir / "slices.json";

   std::vector<uint32_t> dirs;
   for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
      const auto name = entry.path().filename().string();
      if (!entry.is_directory() || name.rfind("history_", 0) != 0)
         continue;
      unsigned start = 0, end = 0;
      if (std::sscanf(name.c_str(), "history_%10u-%10u", &start, &end) != 2 || start == 0 || end < start)
         throw layout_exception("unrecognised slice directory " + name);
      if (end - start + 1 != BLOCKS_PER_SLICE || (start - 1) % BLOCKS_PER_SLICE != 0)
         throw layout_exception("slice directory " + name + " does not use " + std::to_string(BLOCKS_PER_SLICE) +
                                "-block slices");
      dirs.push_back(slice_of(start));
   }
   std::sort(dirs.begin(), dirs.end());

   if (!std::filesystem::exists(json_path)) {
      if (!dirs.empty())
         throw layout_exception("core storage at " + base_dir.string() + " has slice directories but no slices.json");
      return;
   }

   auto var = fc::json::from_file(json_path);
   if (var.is_null()) {
      if (!dirs.empty())
         throw layout_exception("slices.json in " + base_dir.string() + " lists no slices but slice directories exist");
      return;
   }
   if (!var.is_array())
      throw layout_exception("slices.json is not an array");
   for (const auto& elem : var.get_array()) {
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
      if (si.blocks_per_slice != BLOCKS_PER_SLICE || si.start_block != slice_start(si.slice_num) ||
          si.max_block != slice_max(si.slice_num))
         throw layout_exception("slices.json declares slice " + std::to_string(si.slice_num) + " as blocks " +
                                std::to_string(si.start_block) + "-" + std::to_string(si.max_block) + " with " +
                                std::to_string(si.blocks_per_slice) + " blocks per slice; the writer requires " +
                                std::to_string(BLOCKS_PER_SLICE));
      infos.push_back(si);
   }
   std::sort(infos.begin(), infos.end(), [](const auto& a, const auto& b) { return a.slice_num < b.slice_num; });

   std::vector<uint32_t> declared;
   for (const auto& si : infos)
      declared.push_back(si.slice_num);
   if (declared != dirs)
      throw layout_exception("slices.json and the slice directories in " + base_dir.string() + " disagree");
   for (size_t i = 0; i + 1 < infos.size(); ++i) {
      if (!infos[i].finalized)
         throw layout_exception("slice " + std::to_string(infos[i].slice_num) +
                                " is not finalized but is not the newest slice");
   }
}

void slice_writer::load_active_slice() {
   if (infos.empty())
      return;
   first_slice_num = infos.front().slice_num;

   const auto& newest = infos.back();
   if (newest.finalized) {
      last_block = newest.end_block;
      load_newest_bounds();
      return;
   }

   active_slice_num = newest.slice_num;
   active_dir       = base_dir / slice_dir_name(active_slice_num);
   const auto data_path = active_dir / "data.log";

   std::vector<char> header(SLICE_HEADER_SIZE);
   {
      std::ifstream f(data_path, std::ios::binary);
      if (!f.read(header.data(), header.size()))
         throw layout_exception(data_path.string() + " has no header");
   }
   if (!std::equal(std::begin(SLICE_MAGIC), std::end(SLICE_MAGIC), header.begin()) ||
       static_cast<uint8_t>(header[7]) != SLICE_VERSION)
      throw layout_exception(data_path.string() + " does not start with an HWSLICE version 1 header");
   if (get_le(header.data() + 8, 4) != BLOCKS_PER_SLICE ||
       get_le(header.data() + 12, 4) != slice_start(active_slice_num) ||
       get_le(header.data() + 16, 4) != slice_max(active_slice_num))
      throw layout_exception(data_path.string() + " header does not match a " + std::to_string(BLOCKS_PER_SLICE) +
                             "-block slice " + std::to_string(active_slice_num));

   entries           = read_index(active_dir / "blocks.index");
   persisted_entries = entries.size();

   data_log = std::make_unique<fc::cfile>();
   data_log->set_file_path(data_path);
   data_log->open(fc::cfile::update_rw_mode);
   data_log->seek_end(0);
   data_offset = data_log->tellp();
   has_active  = true;

   if (!entries.empty()) {
      last_block = entries.back().block_num;
      tail       = {last_block, data_offset, static_cast<uint32_t>(entries.size())};
   } else if (infos.size() > 1) {
      last_block = infos[infos.size() - 2].end_block;
      load_newest_bounds();
   }
}

void slice_writer::load_newest_bounds() {
   for (auto it = infos.rbegin(); it != infos.rend(); ++it) {
      auto idx = read_index(base_dir / slice_dir_name(it->slice_num) / "blocks.index");
      if (idx.empty())
         continue;
      const auto& e = idx.back();
      tail          = {e.block_num, e.offset + RECORD_OVERHEAD + e.compressed_size, static_cast<uint32_t>(idx.size())};
      return;
   }
}

void slice_writer::open_slice(uint32_t slice_num) {
   active_slice_num = slice_num;
   active_dir       = base_dir / slice_dir_name(slice_num);
   std::filesystem::create_directories(active_dir);
   root_changed = true;
   unsynced_dirs.insert(active_dir);

   data_log = std::make_unique<fc::cfile>();
   data_log->set_file_path(active_dir / "data.log");
   data_log->open(fc::cfile::truncate_rw_mode);
   const auto header = slice_header_bytes(slice_start(slice_num), slice_max(slice_num), false);
   data_log->write(header.data(), header.size());
   data_log->flush();
   data_offset = SLICE_HEADER_SIZE;

   entries.clear();
   entries.reserve(BLOCKS_PER_SLICE);
   persisted_entries = 0;
   has_active        = true;

   {
      fc::cfile idx;
      idx.set_file_path(active_dir / "blocks.index");
      idx.open(fc::cfile::truncate_rw_mode);
      char count[4] = {0, 0, 0, 0};
      idx.write(count, 4);
      idx.flush();
   }

   if (infos.empty())
      first_slice_num = slice_num;
   slice_info si;
   si.slice_num   = slice_num;
   si.start_block = slice_start(slice_num);
   si.max_block   = slice_max(slice_num);
   infos.push_back(si);
   save_slices_json();
}

void slice_writer::append(const built_block& block) {
   append_compressed(block.block_num, compress(block.blob), block.glob_min, block.glob_max);
}

void slice_writer::append_compressed(uint32_t block_num, const std::vector<char>& compressed, uint64_t glob_min,
                                     uint64_t glob_max) {
   if (last_block != 0 && block_num != last_block + 1)
      throw data_corruption_exception("core storage ends at block " + std::to_string(last_block) +
                                      " and cannot accept block " + std::to_string(block_num));

   const uint32_t target  = slice_of(block_num);
   bool           rotated = false;
   if (has_active && target != active_slice_num) {
      finalize_active_slice();
      rotated = true;
   }
   if (!has_active)
      open_slice(target);
   if (rotated && on_rotate)
      on_rotate(target);

   boost::crc_32_type crc;
   crc.process_bytes(compressed.data(), compressed.size());

   char size_le[4];
   char crc_le[4];
   put_le(size_le, compressed.size(), 4);
   put_le(crc_le, crc.checksum(), 4);
   data_log->write(size_le, 4);
   data_log->write(compressed.data(), compressed.size());
   data_log->write(crc_le, 4);

   blocks_index_entry entry;
   entry.block_num       = block_num;
   entry.offset          = data_offset;
   entry.compressed_size = static_cast<uint32_t>(compressed.size());
   entry.glob_min        = glob_min;
   entry.glob_max        = glob_max;
   entries.push_back(entry);

   data_offset      += RECORD_OVERHEAD + compressed.size();
   bytes_compressed += compressed.size();
   last_block        = block_num;
   tail              = {block_num, data_offset, static_cast<uint32_t>(entries.size())};
   unsynced_dirs.insert(active_dir);
}

slice_bounds slice_writer::sync() {
   if (has_active) {
      data_log->flush();
      sync_fd(data_log->fileno(), (active_dir / "data.log").string());
      write_index();
   }
   for (const auto& dir : unsynced_dirs) {
      for (const char* name : {"data.log", "blocks.index"}) {
         if (std::filesystem::exists(dir / name))
            sync_file(dir / name);
      }
      sync_dir(dir);
   }
   unsynced_dirs.clear();
   if (root_changed) {
      sync_dir(base_dir);
      root_changed = false;
   }
   return tail;
}

void slice_writer::flush() {
   if (!has_active)
      return;
   data_log->flush();
   write_index();
}

void slice_writer::close() {
   if (!has_active)
      return;
   flush();
   update_active_info();
   save_slices_json();
   data_log->close();
   data_log.reset();
   has_active = false;
}

void slice_writer::write_index() {
   if (persisted_entries == entries.size())
      return;
   fc::cfile idx;
   idx.set_file_path(active_dir / "blocks.index");
   idx.open(fc::cfile::update_rw_mode);
   idx.seek(4 + persisted_entries * INDEX_ENTRY_SIZE);
   std::vector<char> buf((entries.size() - persisted_entries) * INDEX_ENTRY_SIZE);
   char*             p = buf.data();
   for (size_t i = persisted_entries; i < entries.size(); ++i, p += INDEX_ENTRY_SIZE) {
      const auto& e = entries[i];
      put_le(p, e.block_num, 4);
      put_le(p + 4, e.offset, 8);
      put_le(p + 12, e.compressed_size, 4);
      put_le(p + 16, e.glob_min, 8);
      put_le(p + 24, e.glob_max, 8);
   }
   idx.write(buf.data(), buf.size());
   idx.flush();
   char count[4];
   put_le(count, entries.size(), 4);
   idx.seek(0);
   idx.write(count, 4);
   idx.flush();
   idx.close();
   persisted_entries = entries.size();
}

void slice_writer::check_complete() const {
   if (entries.empty())
      throw data_corruption_exception("slice " + std::to_string(active_slice_num) + " has no blocks to finalize");
   const uint32_t start = slice_start(active_slice_num);
   const uint32_t first = entries.front().block_num;
   const bool     first_ok =
       first == start || (start == 1 && first == 2) || (active_slice_num == first_slice_num && first > start);
   if (!first_ok)
      throw data_corruption_exception("slice " + std::to_string(active_slice_num) + " starts at block " +
                                      std::to_string(first) + " instead of " + std::to_string(start));
   for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].block_num != first + i)
         throw data_corruption_exception("slice " + std::to_string(active_slice_num) + " has a gap at block " +
                                         std::to_string(first + i));
   }
   if (entries.back().block_num != slice_max(active_slice_num))
      throw data_corruption_exception("slice " + std::to_string(active_slice_num) + " ends at block " +
                                      std::to_string(entries.back().block_num));
}

void slice_writer::finalize_active_slice() {
   check_complete();
   flush();
   data_log->close();
   data_log.reset();

   const auto data_path = active_dir / "data.log";
   {
      fc::cfile f;
      f.set_file_path(data_path);
      f.open(fc::cfile::update_rw_mode);
      f.seek(20);
      char flag = 1;
      f.write(&flag, 1);
      f.flush();
      f.close();
   }
   write_file_atomic(active_dir / "sha256.txt", file_sha256(data_path) + "\n");

   update_active_info();
   infos.back().finalized = true;
   save_slices_json();
   has_active = false;

   fc_ilog(log, "Finalized slice ${n} (blocks ${s}-${e})",
           ("n", active_slice_num)("s", entries.front().block_num)("e", entries.back().block_num));
}

void slice_writer::update_active_info() {
   if (!has_active || infos.empty() || infos.back().slice_num != active_slice_num)
      return;
   auto& si     = infos.back();
   si.end_block = entries.empty() ? si.end_block : entries.back().block_num;
   si.glob_min  = 0;
   si.glob_max  = 0;
   if (!entries.empty()) {
      si.glob_min = entries.front().glob_min;
      si.glob_max = entries.front().glob_max;
      for (const auto& e : entries) {
         si.glob_min = std::min(si.glob_min, e.glob_min);
         si.glob_max = std::max(si.glob_max, e.glob_max);
      }
   }
}

void slice_writer::save_slices_json() { write_file_atomic(base_dir / "slices.json", slices_json(infos)); }

std::vector<char> slice_writer::compress(const std::vector<char>& input) {
   std::vector<char> output(ZSTD_compressBound(input.size()));
   size_t n = ZSTD_compressCCtx(zstd_ctx, output.data(), output.size(), input.data(), input.size(), ZSTD_LEVEL);
   if (ZSTD_isError(n))
      throw compression_exception(std::string("zstd compression failed: ") + ZSTD_getErrorName(n));
   output.resize(n);
   return output;
}

} // namespace eosio::roborovski_writer
