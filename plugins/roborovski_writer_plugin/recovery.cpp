#include <eosio/roborovski_writer_plugin/recovery.hpp>
#include <eosio/roborovski_writer_plugin/slice_writer.hpp>

#include <fc/io/json.hpp>

#include <boost/crc.hpp>

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>

namespace eosio::roborovski_writer {

namespace {

constexpr size_t ABI_DATA_HEADER_SIZE = 16;
constexpr size_t ABI_INDEX_ENTRY_SIZE = 20;

struct slice_dir {
   uint32_t              num = 0;
   std::filesystem::path path;
};

std::vector<slice_dir> list_slice_dirs(const std::filesystem::path& base) {
   std::vector<slice_dir> dirs;
   if (!std::filesystem::exists(base))
      return dirs;
   for (const auto& entry : std::filesystem::directory_iterator(base)) {
      const auto name = entry.path().filename().string();
      if (!entry.is_directory() || name.rfind("history_", 0) != 0)
         continue;
      unsigned start = 0, end = 0;
      char     tail  = 0;
      if (std::sscanf(name.c_str(), "history_%u-%u%c", &start, &end, &tail) != 2 || start == 0 || end < start)
         throw layout_exception("unexpected slice directory " + name);
      if (end - start + 1 != BLOCKS_PER_SLICE || (start - 1) % BLOCKS_PER_SLICE != 0)
         throw layout_exception("slice directory " + name + " does not match the " +
                                std::to_string(BLOCKS_PER_SLICE) + "-block slice span");
      dirs.push_back({(start - 1) / BLOCKS_PER_SLICE, entry.path()});
   }
   std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.num < b.num; });
   return dirs;
}

std::optional<blocks_index_entry> read_index_entry(const std::filesystem::path& path, uint32_t i) {
   std::ifstream f(path, std::ios::binary);
   if (!f)
      return std::nullopt;
   char buf[INDEX_ENTRY_SIZE];
   f.seekg(4 + static_cast<std::streamoff>(i) * INDEX_ENTRY_SIZE);
   if (!f.read(buf, sizeof(buf)))
      return std::nullopt;
   blocks_index_entry e;
   e.block_num       = static_cast<uint32_t>(get_le(buf, 4));
   e.offset          = get_le(buf + 4, 8);
   e.compressed_size = static_cast<uint32_t>(get_le(buf + 12, 4));
   e.glob_min        = get_le(buf + 16, 8);
   e.glob_max        = get_le(buf + 24, 8);
   return e;
}

uint32_t read_index_count(const std::filesystem::path& path) {
   std::ifstream f(path, std::ios::binary);
   char          buf[4];
   if (!f || !f.read(buf, 4))
      throw io_exception("cannot read the count of " + path.string());
   return static_cast<uint32_t>(get_le(buf, 4));
}

std::optional<std::vector<blocks_index_entry>> load_index(const std::filesystem::path& path) {
   std::ifstream f(path, std::ios::binary);
   if (!f)
      return std::nullopt;
   std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   if (bytes.size() < 4)
      return std::nullopt;
   const uint32_t count = static_cast<uint32_t>(get_le(bytes.data(), 4));
   if (bytes.size() < 4 + static_cast<size_t>(count) * INDEX_ENTRY_SIZE)
      return std::nullopt;
   std::vector<blocks_index_entry> out(count);
   for (uint32_t i = 0; i < count; ++i) {
      const char* e          = bytes.data() + 4 + static_cast<size_t>(i) * INDEX_ENTRY_SIZE;
      out[i].block_num       = static_cast<uint32_t>(get_le(e, 4));
      out[i].offset          = get_le(e + 4, 8);
      out[i].compressed_size = static_cast<uint32_t>(get_le(e + 12, 4));
      out[i].glob_min        = get_le(e + 16, 8);
      out[i].glob_max        = get_le(e + 24, 8);
   }
   return out;
}

void truncate_index_file(const std::filesystem::path& path, uint32_t count) {
   const uint64_t want = 4 + static_cast<uint64_t>(count) * INDEX_ENTRY_SIZE;
   if (file_size_or_zero(path) < want)
      throw data_corruption_exception(path.string() + " holds " + std::to_string(file_size_or_zero(path)) +
                                      " bytes, committed index needs " + std::to_string(want));
   truncate_and_sync(path, want);
   char buf[4];
   put_le(buf, count, 4);
   write_at(path, 0, buf, 4, true);
}

std::vector<char> read_record(const std::filesystem::path& data_path, uint64_t offset) {
   std::ifstream f(data_path, std::ios::binary);
   if (!f)
      throw io_exception("cannot open " + data_path.string());
   f.seekg(static_cast<std::streamoff>(offset));
   char size_le[4];
   if (!f.read(size_le, 4))
      throw data_corruption_exception("record at " + std::to_string(offset) + " of " + data_path.string() +
                                      " is truncated");
   const uint32_t    size = static_cast<uint32_t>(get_le(size_le, 4));
   std::vector<char> payload(size);
   char              crc_le[4];
   if (!f.read(payload.data(), size) || !f.read(crc_le, 4))
      throw data_corruption_exception("record at " + std::to_string(offset) + " of " + data_path.string() +
                                      " is truncated");
   boost::crc_32_type crc;
   crc.process_bytes(payload.data(), payload.size());
   if (crc.checksum() != static_cast<uint32_t>(get_le(crc_le, 4)))
      throw data_corruption_exception("record at " + std::to_string(offset) + " of " + data_path.string() +
                                      " fails its CRC check");
   return payload;
}

void check_committed_slice(const std::filesystem::path& dir, const core_bounds& b) {
   const auto head = std::to_string(b.head);
   if (b.slice_index_len == 0)
      throw data_corruption_exception("committed head block " + head + ": manifest names an empty index");
   auto entry = read_index_entry(dir / "blocks.index", b.slice_index_len - 1);
   if (!entry)
      throw data_corruption_exception("committed head block " + head + ": index entry " +
                                      std::to_string(b.slice_index_len - 1) + " is missing");
   if (entry->block_num != b.head)
      throw data_corruption_exception("committed head block " + head + ": index entry " +
                                      std::to_string(b.slice_index_len - 1) + " holds block " +
                                      std::to_string(entry->block_num));
   const uint64_t end = entry->offset + RECORD_OVERHEAD + entry->compressed_size;
   if (end != b.slice_data_end)
      throw data_corruption_exception("committed head block " + head + " ends at byte " + std::to_string(end) +
                                      ", manifest says " + std::to_string(b.slice_data_end));
   if (file_size_or_zero(dir / "data.log") < b.slice_data_end)
      throw data_corruption_exception("committed head block " + head + ": data.log holds " +
                                      std::to_string(file_size_or_zero(dir / "data.log")) +
                                      " bytes, manifest commits " + std::to_string(b.slice_data_end));
}

void rewrite_slice_infos(const std::filesystem::path& base, uint32_t head, uint32_t head_slice,
                         const blocks_index_entry& last, bool finalized) {
   const auto              path = base / "slices.json";
   bool                    had  = false;
   std::vector<slice_info> infos;
   if (std::filesystem::exists(path)) {
      auto var = fc::json::from_file(path);
      if (var.is_array()) {
         had = true;
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
            infos.push_back(si);
         }
      } else if (!var.is_null()) {
         throw layout_exception("slices.json is not an array");
      }
   }
   std::vector<slice_info> kept;
   bool                    found = false;
   for (auto info : infos) {
      if (head == 0 || info.slice_num > head_slice)
         continue;
      if (info.slice_num == head_slice) {
         info.end_block = head;
         info.finalized = finalized;
         info.glob_max  = last.glob_max;
         found          = true;
      }
      kept.push_back(info);
   }
   if (head > 0 && !found) {
      slice_info si;
      si.slice_num   = head_slice;
      si.start_block = slice_start(head_slice);
      si.end_block   = head;
      si.max_block   = slice_max(head_slice);
      si.finalized   = finalized;
      si.glob_max    = last.glob_max;
      kept.push_back(si);
      std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.slice_num < b.slice_num; });
      had = true;
   }
   const std::string json = (kept.empty() && !had) ? "null" : slices_json(kept);
   write_file_atomic_durable(path, std::vector<char>(json.begin(), json.end()));
}

} // namespace

uint32_t slice_of(uint32_t block_num) { return block_num == 0 ? 0 : (block_num - 1) / BLOCKS_PER_SLICE; }
uint32_t slice_start(uint32_t slice_num) { return slice_num * BLOCKS_PER_SLICE + 1; }
uint32_t slice_max(uint32_t slice_num) { return (slice_num + 1) * BLOCKS_PER_SLICE; }

std::filesystem::path slice_path(const std::filesystem::path& core_root, uint32_t slice_num) {
   return core_root / slice_dir_name(slice_num);
}

truncate_report truncate_core_to(const std::filesystem::path& base, const core_bounds& b) {
   truncate_report report;
   const auto      dirs       = list_slice_dirs(base);
   const uint32_t  head_slice = slice_of(b.head);

   const slice_dir* head = nullptr;
   for (const auto& d : dirs) {
      if (b.head > 0 && d.num == head_slice)
         head = &d;
   }
   if (b.head > 0) {
      if (!head)
         throw data_corruption_exception("committed head block " + std::to_string(b.head) + ": slice " +
                                         std::to_string(head_slice) + " is missing");
      check_committed_slice(head->path, b);
   }

   for (const auto& d : dirs) {
      if (b.head == 0 || d.num > head_slice) {
         std::filesystem::remove_all(d.path);
         report.removed_slices.push_back(d.path.filename().string());
      }
   }

   blocks_index_entry last;
   bool               finalized = false;
   if (head) {
      const auto data_path  = head->path / "data.log";
      const auto index_path = head->path / "blocks.index";
      const auto data_size  = file_size_or_zero(data_path);
      if (data_size > b.slice_data_end) {
         report.data_bytes_cut = data_size - b.slice_data_end;
         truncate_and_sync(data_path, b.slice_data_end);
      }
      const auto     index_size = file_size_or_zero(index_path);
      const uint32_t on_disk    = read_index_count(index_path);
      if (index_size != 4 + static_cast<uint64_t>(b.slice_index_len) * INDEX_ENTRY_SIZE ||
          on_disk != b.slice_index_len) {
         if (on_disk > b.slice_index_len)
            report.index_entries_cut = on_disk - b.slice_index_len;
         truncate_index_file(index_path, b.slice_index_len);
      }
      auto entry = read_index_entry(index_path, b.slice_index_len - 1);
      if (!entry)
         throw data_corruption_exception("cannot read entry " + std::to_string(b.slice_index_len - 1) + " of " +
                                         index_path.string());
      last = *entry;

      std::vector<char> header(SLICE_HEADER_SIZE);
      {
         std::ifstream f(data_path, std::ios::binary);
         if (!f.read(header.data(), header.size()))
            throw layout_exception(data_path.string() + " has no header");
      }
      finalized                = header[20] == 1;
      const uint32_t end_block = static_cast<uint32_t>(get_le(header.data() + 16, 4));
      if (finalized && b.head < end_block) {
         const char flag = 0;
         write_at(data_path, 20, &flag, 1, true);
         std::error_code ec;
         std::filesystem::remove(head->path / "sha256.txt", ec);
         finalized              = false;
         report.finalized_reset = true;
      }
      if (report.changed())
         sync_dir(head->path);
   }
   if (!report.removed_slices.empty())
      sync_dir(base);
   rewrite_slice_infos(base, b.head, head_slice, last, finalized);
   return report;
}

bool truncate_abi_to(const std::filesystem::path& abi_root, uint64_t data_end, uint32_t index_len) {
   const auto     data_path  = abi_root / "abis.dat";
   const auto     index_path = abi_root / "abis.index";
   const uint64_t ds         = file_size_or_zero(data_path);
   const uint64_t is         = file_size_or_zero(index_path);
   const uint64_t want_index = static_cast<uint64_t>(index_len) * ABI_INDEX_ENTRY_SIZE;
   if (ds < data_end || is < want_index)
      throw data_corruption_exception("ABI cache holds " + std::to_string(ds) + " data and " + std::to_string(is) +
                                      " index bytes, manifest commits " + std::to_string(data_end) + " and " +
                                      std::to_string(want_index));
   bool cut = false;
   if (ds > data_end) {
      truncate_and_sync(data_path, data_end);
      cut = true;
   }
   if (is > want_index) {
      truncate_and_sync(index_path, want_index);
      cut = true;
   }
   return cut;
}

std::pair<uint64_t, uint32_t> abi_committed_bounds(const std::filesystem::path& abi_root, uint32_t head) {
   const auto index_path = abi_root / "abis.index";
   if (!std::filesystem::exists(index_path))
      return {0, 0};
   const auto   index = read_file(index_path);
   const size_t n     = index.size() / ABI_INDEX_ENTRY_SIZE;
   size_t       keep  = 0;
   while (keep < n && get_le(index.data() + keep * ABI_INDEX_ENTRY_SIZE, 4) <= head)
      ++keep;
   if (keep == 0)
      return {0, 0};
   const char*    last   = index.data() + (keep - 1) * ABI_INDEX_ENTRY_SIZE;
   const uint64_t offset = get_le(last + 12, 8);
   const auto     data_path = abi_root / "abis.dat";
   std::ifstream  f(data_path, std::ios::binary);
   char           hdr[ABI_DATA_HEADER_SIZE];
   f.seekg(static_cast<std::streamoff>(offset));
   if (!f || !f.read(hdr, sizeof(hdr)))
      throw data_corruption_exception("cannot read ABI record at " + std::to_string(offset));
   if (get_le(hdr, 4) != get_le(last, 4) || get_le(hdr + 4, 8) != get_le(last + 4, 8))
      throw data_corruption_exception("ABI record at " + std::to_string(offset) + " does not match its index entry");
   const uint64_t end = offset + ABI_DATA_HEADER_SIZE + get_le(hdr + 12, 4);
   if (file_size_or_zero(data_path) < end)
      throw data_corruption_exception("ABI record at " + std::to_string(offset) + " runs past the end of abis.dat");
   return {end, static_cast<uint32_t>(keep)};
}

hash_bytes block_id_from_record(const std::vector<char>& compressed) {
   hash_bytes id;
   if (compressed.size() < 2 || get_le(compressed.data(), 2) != 0xb528) {
      if (compressed.size() < 40)
         throw data_corruption_exception("block record is shorter than its header");
      std::memcpy(id.data(), compressed.data() + 8, 32);
      return id;
   }
   ZSTD_DStream* ds = ZSTD_createDStream();
   if (!ds)
      throw compression_exception("failed to create a zstd decompression context");
   ZSTD_initDStream(ds);
   char           out_buf[40];
   ZSTD_inBuffer  in{compressed.data(), compressed.size(), 0};
   ZSTD_outBuffer out{out_buf, sizeof(out_buf), 0};
   while (out.pos < out.size && in.pos < in.size) {
      size_t rc = ZSTD_decompressStream(ds, &out, &in);
      if (ZSTD_isError(rc)) {
         ZSTD_freeDStream(ds);
         throw compression_exception(std::string("zstd decompression failed: ") + ZSTD_getErrorName(rc));
      }
      if (rc == 0)
         break;
   }
   ZSTD_freeDStream(ds);
   if (out.pos < out.size)
      throw data_corruption_exception("block record is shorter than its header");
   std::memcpy(id.data(), out_buf + 8, 32);
   return id;
}

hash_bytes read_stored_block_id(const std::filesystem::path& core_root, uint32_t block_num) {
   const auto dir     = slice_path(core_root, slice_of(block_num));
   const auto entries = load_index(dir / "blocks.index");
   if (!entries || entries->empty())
      throw data_corruption_exception("block " + std::to_string(block_num) + " is not indexed in " + dir.string());
   const uint32_t first = entries->front().block_num;
   if (block_num < first || block_num - first >= entries->size() || (*entries)[block_num - first].block_num != block_num)
      throw data_corruption_exception("block " + std::to_string(block_num) + " is not indexed in " + dir.string());
   return block_id_from_record(read_record(dir / "data.log", (*entries)[block_num - first].offset));
}

adoption find_adoption_boundary(const std::filesystem::path& base) {
   adoption   a;
   const auto json_path = base / "slices.json";
   if (std::filesystem::exists(json_path)) {
      auto var = fc::json::from_file(json_path);
      if (var.is_array()) {
         for (const auto& elem : var.get_array()) {
            const auto bps = elem.get_object()["blocks_per_slice"].as_uint64();
            if (bps != 0 && bps != BLOCKS_PER_SLICE)
               throw layout_exception("slices.json records " + std::to_string(bps) +
                                      "-block slices, configured span is " + std::to_string(BLOCKS_PER_SLICE));
         }
      }
   }
   const auto dirs = list_slice_dirs(base);
   for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
      auto entries = load_index(it->path / "blocks.index");
      if (!entries || entries->empty())
         continue;
      for (size_t j = 1; j < entries->size(); ++j) {
         if ((*entries)[j].block_num != (*entries)[j - 1].block_num + 1)
            throw data_corruption_exception("slice " + it->path.filename().string() + " index jumps from block " +
                                            std::to_string((*entries)[j - 1].block_num) + " to " +
                                            std::to_string((*entries)[j].block_num));
      }
      const auto& last = entries->back();
      std::vector<char> record;
      try {
         record = read_record(it->path / "data.log", last.offset);
      } catch (const std::exception& e) {
         throw data_corruption_exception("tail block " + std::to_string(last.block_num) + " fails its check (" +
                                         e.what() + "); repair the store before adopting it");
      }
      a.head_id                = block_id_from_record(record);
      a.bounds.head            = last.block_num;
      a.bounds.slice_index_len = static_cast<uint32_t>(entries->size());
      a.bounds.slice_data_end  = last.offset + RECORD_OVERHEAD + last.compressed_size;
      break;
   }
   return a;
}

} // namespace eosio::roborovski_writer
