#include <eosio/roborovski_writer_plugin/buffer_log.hpp>
#include <eosio/roborovski_writer_plugin/recovery.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace eosio::roborovski_writer {

namespace {

constexpr char     SEGMENT_MAGIC[8] = {'R', 'B', 'B', 'U', 'F', 'S', 'E', 'G'};
constexpr char     ENTRY_MAGIC[8]   = {'R', 'B', 'B', 'E', 'N', 'T', 'R', 'Y'};
constexpr uint32_t BUFFER_VERSION   = 1;
constexpr uint64_t MAX_PAYLOAD      = 1ull << 32;

class writer {
public:
   void u32(uint64_t v) { put(v, 4); }
   void u64(uint64_t v) { put(v, 8); }
   void bytes(const char* p, size_t n) { out.insert(out.end(), p, p + n); }
   std::vector<char> out;

private:
   void put(uint64_t v, int n) {
      char b[8];
      put_le(b, v, n);
      out.insert(out.end(), b, b + n);
   }
};

class reader {
public:
   reader(const char* p, size_t n)
      : p(p)
      , end(p + n) {}
   uint64_t u32() { return get(4); }
   uint64_t u64() { return get(8); }
   const char* bytes(size_t n) {
      need(n);
      const char* r = p;
      p += n;
      return r;
   }
   bool done() const { return p == end; }

private:
   void need(size_t n) const {
      if (static_cast<size_t>(end - p) < n)
         throw data_corruption_exception("buffer entry payload is truncated");
   }
   uint64_t get(int n) {
      need(n);
      uint64_t v = get_le(p, n);
      p += n;
      return v;
   }
   const char* p;
   const char* end;
};

std::string errno_text() { return std::strerror(errno); }

void pwrite_all(int fd, const char* data, size_t size, uint64_t offset, const std::string& what) {
   while (size > 0) {
      ssize_t n = ::pwrite(fd, data, size, static_cast<off_t>(offset));
      if (n < 0) {
         if (errno == EINTR)
            continue;
         throw io_exception("cannot write " + what + ": " + errno_text());
      }
      data += n;
      size -= static_cast<size_t>(n);
      offset += static_cast<uint64_t>(n);
   }
}

bool pread_all(int fd, char* data, size_t size, uint64_t offset) {
   while (size > 0) {
      ssize_t n = ::pread(fd, data, size, static_cast<off_t>(offset));
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      data += n;
      size -= static_cast<size_t>(n);
      offset += static_cast<uint64_t>(n);
   }
   return true;
}

std::vector<char> segment_header(const uuid_type& core_uuid, uint64_t epoch, uint32_t slice_num) {
   std::vector<char> h(BUFFER_SEGMENT_HEADER_SIZE, 0);
   std::memcpy(h.data(), SEGMENT_MAGIC, 8);
   put_le(h.data() + 8, BUFFER_VERSION, 4);
   std::memcpy(h.data() + 16, core_uuid.data(), 16);
   put_le(h.data() + 32, epoch, 8);
   put_le(h.data() + 40, slice_num, 4);
   put_le(h.data() + 60, crc32c(h.data(), 60), 4);
   return h;
}

} // namespace

std::vector<char> serialize_payload(const prepared_block& b) {
   writer w;
   w.u64(b.glob_min);
   w.u64(b.glob_max);
   w.u64(b.action_count);
   w.u64(b.raw_size);
   w.u32(b.compressed.size());
   w.bytes(b.compressed.data(), b.compressed.size());
   w.u32(b.abis.size());
   for (const auto& a : b.abis) {
      w.u32(a.block_num);
      w.u64(a.contract);
      w.u32(a.json.size());
      w.bytes(a.json.data(), a.json.size());
   }
   return std::move(w.out);
}

void parse_payload(prepared_block& b, const char* data, size_t size) {
   reader r(data, size);
   b.glob_min       = r.u64();
   b.glob_max       = r.u64();
   b.action_count   = r.u64();
   b.raw_size       = r.u64();
   const size_t n   = r.u32();
   const char*  c   = r.bytes(n);
   b.compressed.assign(c, c + n);
   const size_t abi_count = r.u32();
   b.abis.clear();
   for (size_t i = 0; i < abi_count; ++i) {
      abi_record a;
      a.block_num    = static_cast<uint32_t>(r.u32());
      a.contract     = r.u64();
      const size_t m = r.u32();
      const char*  j = r.bytes(m);
      a.json.assign(j, m);
      b.abis.push_back(std::move(a));
   }
   if (!r.done())
      throw data_corruption_exception("buffer entry payload has trailing bytes");
}

std::filesystem::path buffer_log::segment_path(const std::filesystem::path& dir, uint32_t slice_num) {
   char buf[32];
   std::snprintf(buf, sizeof(buf), "segment-%010u.log", slice_num);
   return dir / buf;
}

buffer_log::buffer_log(const std::filesystem::path& d, const uuid_type& uuid, uint64_t ep)
   : dir(d)
   , core_uuid(uuid)
   , epoch(ep) {
   std::filesystem::create_directories(dir);
   std::vector<std::pair<uint32_t, std::filesystem::path>> found;
   for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const auto name = entry.path().filename().string();
      unsigned   num  = 0;
      char       tail = 0;
      if (!entry.is_regular_file() || std::sscanf(name.c_str(), "segment-%u.lo%c", &num, &tail) != 2 || tail != 'g')
         continue;
      found.emplace_back(num, entry.path());
   }
   try {
      for (const auto& [num, path] : found)
         scan_segment(num, path);
   } catch (...) {
      close_all();
      throw;
   }
}

buffer_log::~buffer_log() { close_all(); }

void buffer_log::close_all() {
   for (auto& [num, seg] : segments) {
      if (seg.fd >= 0)
         ::close(seg.fd);
   }
   segments.clear();
}

void buffer_log::scan_segment(uint32_t slice_num, const std::filesystem::path& path) {
   int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
   if (fd < 0)
      throw io_exception("cannot open " + path.string() + ": " + errno_text());
   const uint64_t size = file_size_or_zero(path);

   std::vector<char> h(BUFFER_SEGMENT_HEADER_SIZE);
   const bool header_ok = size >= h.size() && pread_all(fd, h.data(), h.size(), 0) &&
                          std::memcmp(h.data(), SEGMENT_MAGIC, 8) == 0 &&
                          get_le(h.data() + 60, 4) == crc32c(h.data(), 60);
   if (!header_ok) {
      ::close(fd);
      std::filesystem::remove(path);
      ++report.segments_removed;
      return;
   }
   if (get_le(h.data() + 8, 4) != BUFFER_VERSION) {
      ::close(fd);
      throw data_corruption_exception(path.string() + " has buffer version " + std::to_string(get_le(h.data() + 8, 4)));
   }
   uuid_type seg_uuid;
   std::memcpy(seg_uuid.data(), h.data() + 16, 16);
   const uint64_t seg_epoch = get_le(h.data() + 32, 8);
   if (seg_uuid != core_uuid) {
      ::close(fd);
      throw binding_exception(path.string() + " belongs to store " + format_uuid(seg_uuid) + ", not " +
                              format_uuid(core_uuid));
   }
   if (get_le(h.data() + 40, 4) != slice_num || seg_epoch > epoch) {
      ::close(fd);
      throw data_corruption_exception(path.string() + " does not match its name or the store epoch");
   }
   if (seg_epoch < epoch) {
      ::close(fd);
      std::filesystem::remove(path);
      ++report.segments_removed;
      return;
   }

   uint64_t          pos = BUFFER_SEGMENT_HEADER_SIZE;
   std::vector<char> buf;
   while (pos < size) {
      char hdr[BUFFER_ENTRY_HEADER_SIZE];
      if (size - pos < BUFFER_ENTRY_HEADER_SIZE + BUFFER_ENTRY_TRAILER_SIZE ||
          !pread_all(fd, hdr, sizeof(hdr), pos) || std::memcmp(hdr, ENTRY_MAGIC, 8) != 0)
         break;
      const uint64_t payload_size = get_le(hdr + 80, 8);
      if (payload_size >= MAX_PAYLOAD ||
          size - pos < BUFFER_ENTRY_HEADER_SIZE + payload_size + BUFFER_ENTRY_TRAILER_SIZE)
         break;
      buf.resize(BUFFER_ENTRY_HEADER_SIZE + payload_size + BUFFER_ENTRY_TRAILER_SIZE);
      if (!pread_all(fd, buf.data(), buf.size(), pos))
         break;
      const char* trailer = buf.data() + BUFFER_ENTRY_HEADER_SIZE + payload_size;
      if (get_le(trailer, 4) != crc32c(buf.data(), BUFFER_ENTRY_HEADER_SIZE + payload_size) ||
          get_le(trailer + 8, 8) != pos)
         break;
      const uint32_t block_num = static_cast<uint32_t>(get_le(hdr + 8, 4));
      if (slice_of(block_num) != slice_num)
         break;
      entry_ref ref;
      std::memcpy(ref.id.data(), hdr + 16, 32);
      std::memcpy(ref.parent.data(), hdr + 48, 32);
      ref.segment        = slice_num;
      ref.payload_offset = pos + BUFFER_ENTRY_HEADER_SIZE;
      ref.payload_size   = payload_size;
      entries.emplace(block_num, ref);
      ++report.entries;
      pos += buf.size();
   }
   if (pos < size) {
      if (::ftruncate(fd, static_cast<off_t>(pos)) != 0 || ::fsync(fd) != 0) {
         ::close(fd);
         throw io_exception("cannot truncate " + path.string() + ": " + errno_text());
      }
      report.bytes_truncated += size - pos;
   }
   segments[slice_num] = {fd, pos};
}

buffer_log::segment& buffer_log::open_segment(uint32_t slice_num, bool sync) {
   auto it = segments.find(slice_num);
   if (it != segments.end())
      return it->second;
   const auto path = segment_path(dir, slice_num);
   int        fd   = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
   if (fd < 0)
      throw io_exception("cannot create " + path.string() + ": " + errno_text());
   const auto h = segment_header(core_uuid, epoch, slice_num);
   try {
      pwrite_all(fd, h.data(), h.size(), 0, path.string());
      if (sync) {
         sync_fd(fd, path.string());
         sync_dir(dir);
      }
   } catch (...) {
      ::close(fd);
      throw;
   }
   return segments[slice_num] = {fd, h.size()};
}

buffer_log::entry_ref buffer_log::append(const prepared_block& block, bool sync) {
   const uint32_t slice_num = slice_of(block.block_num);
   auto&          seg       = open_segment(slice_num, sync);
   const auto     payload   = serialize_payload(block);

   std::vector<char> buf(BUFFER_ENTRY_HEADER_SIZE + payload.size() + BUFFER_ENTRY_TRAILER_SIZE, 0);
   std::memcpy(buf.data(), ENTRY_MAGIC, 8);
   put_le(buf.data() + 8, block.block_num, 4);
   std::memcpy(buf.data() + 16, block.id.data(), 32);
   std::memcpy(buf.data() + 48, block.parent.data(), 32);
   put_le(buf.data() + 80, payload.size(), 8);
   std::memcpy(buf.data() + BUFFER_ENTRY_HEADER_SIZE, payload.data(), payload.size());
   char* trailer = buf.data() + BUFFER_ENTRY_HEADER_SIZE + payload.size();
   put_le(trailer, crc32c(buf.data(), BUFFER_ENTRY_HEADER_SIZE + payload.size()), 4);
   put_le(trailer + 8, seg.size, 8);

   const auto what = segment_path(dir, slice_num).string();
   pwrite_all(seg.fd, buf.data(), buf.size(), seg.size, what);
   if (sync)
      sync_fd(seg.fd, what);

   entry_ref ref;
   ref.id             = block.id;
   ref.parent         = block.parent;
   ref.segment        = slice_num;
   ref.payload_offset = seg.size + BUFFER_ENTRY_HEADER_SIZE;
   ref.payload_size   = payload.size();
   seg.size += buf.size();
   entries.emplace(block.block_num, ref);
   return ref;
}

std::optional<buffer_log::entry_ref> buffer_log::find(uint32_t block_num, const block_id_type& id) const {
   std::optional<entry_ref> found;
   auto [lo, hi] = entries.equal_range(block_num);
   for (auto it = lo; it != hi; ++it) {
      if (it->second.id == id)
         found = it->second;
   }
   return found;
}

prepared_block buffer_log::read(uint32_t block_num, const entry_ref& ref) const {
   auto it = segments.find(ref.segment);
   if (it == segments.end())
      throw data_corruption_exception("buffer segment " + std::to_string(ref.segment) + " is not open");
   std::vector<char> payload(ref.payload_size);
   if (!pread_all(it->second.fd, payload.data(), payload.size(), ref.payload_offset))
      throw io_exception("cannot read buffer entry for block " + std::to_string(block_num));
   prepared_block b;
   b.block_num = block_num;
   b.id        = ref.id;
   b.parent    = ref.parent;
   parse_payload(b, payload.data(), payload.size());
   return b;
}

uint32_t buffer_log::discard_through(uint32_t head) {
   entries.erase(entries.begin(), entries.upper_bound(head));
   uint32_t removed = 0;
   for (auto it = segments.begin(); it != segments.end();) {
      if (slice_max(it->first) > head) {
         ++it;
         continue;
      }
      ::close(it->second.fd);
      std::filesystem::remove(segment_path(dir, it->first));
      it = segments.erase(it);
      ++removed;
   }
   return removed;
}

uint64_t buffer_log::disk_bytes() const {
   uint64_t total = 0;
   for (const auto& [num, seg] : segments)
      total += seg.size;
   return total;
}

size_t buffer_log::pending_heights() const {
   size_t n = 0;
   for (auto it = entries.begin(); it != entries.end(); it = entries.upper_bound(it->first))
      ++n;
   return n;
}

} // namespace eosio::roborovski_writer
