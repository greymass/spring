#include <eosio/roborovski_writer_plugin/store_format.hpp>

#include <boost/crc.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace eosio::roborovski_writer {

namespace {

constexpr char MANIFEST_MAGIC[8] = {'R', 'B', 'M', 'A', 'N', 'I', 'F', '\0'};
constexpr char BINDING_MAGIC[8]  = {'R', 'B', 'B', 'I', 'N', 'D', '\0', '\0'};
constexpr uint32_t FORMAT_VERSION = 1;

std::string errno_text() { return std::strerror(errno); }

template <size_t N>
void copy_out(char* out, const std::array<uint8_t, N>& a) {
   std::memcpy(out, a.data(), N);
}

template <size_t N>
void copy_in(std::array<uint8_t, N>& a, const char* in) {
   std::memcpy(a.data(), in, N);
}

int open_or_throw(const std::filesystem::path& path, int flags, mode_t mode = 0644) {
   int fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
   if (fd < 0)
      throw io_exception("cannot open " + path.string() + ": " + errno_text());
   return fd;
}

void write_all(int fd, const char* data, size_t size, const std::string& what) {
   while (size > 0) {
      ssize_t n = ::write(fd, data, size);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         throw io_exception("cannot write " + what + ": " + errno_text());
      }
      data += n;
      size -= static_cast<size_t>(n);
   }
}

} // namespace

uint32_t crc32c(const void* data, size_t size) {
   boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true> crc;
   crc.process_bytes(data, size);
   return crc.checksum();
}

void put_le(char* out, uint64_t v, int bytes) {
   for (int i = 0; i < bytes; ++i)
      out[i] = static_cast<char>(v >> (8 * i));
}

uint64_t get_le(const char* in, int bytes) {
   uint64_t v = 0;
   for (int i = 0; i < bytes; ++i)
      v |= static_cast<uint64_t>(static_cast<uint8_t>(in[i])) << (8 * i);
   return v;
}

std::vector<char> manifest::serialize() const {
   std::vector<char> b(MANIFEST_SIZE, 0);
   std::memcpy(b.data(), MANIFEST_MAGIC, 8);
   put_le(b.data() + 8, FORMAT_VERSION, 4);
   copy_out(b.data() + 16, core_uuid);
   copy_out(b.data() + 32, abi_uuid);
   put_le(b.data() + 48, epoch, 8);
   put_le(b.data() + 56, head, 4);
   put_le(b.data() + 60, abi_index_len, 4);
   copy_out(b.data() + 64, head_id);
   put_le(b.data() + 96, slice_data_end, 8);
   put_le(b.data() + 104, slice_index_len, 4);
   put_le(b.data() + 108, abi_floor, 4);
   put_le(b.data() + 112, abi_data_end, 8);
   put_le(b.data() + 124, crc32c(b.data(), 124), 4);
   return b;
}

manifest manifest::parse(const std::vector<char>& b) {
   if (b.size() != MANIFEST_SIZE)
      throw data_corruption_exception("manifest holds " + std::to_string(b.size()) + " bytes, expected " +
                                      std::to_string(MANIFEST_SIZE));
   if (std::memcmp(b.data(), MANIFEST_MAGIC, 8) != 0)
      throw data_corruption_exception("manifest has a bad magic");
   if (get_le(b.data() + 8, 4) != FORMAT_VERSION)
      throw data_corruption_exception("manifest version " + std::to_string(get_le(b.data() + 8, 4)) +
                                      " is not supported by this build");
   if (get_le(b.data() + 124, 4) != crc32c(b.data(), 124))
      throw data_corruption_exception("manifest checksum mismatch");
   manifest m;
   copy_in(m.core_uuid, b.data() + 16);
   copy_in(m.abi_uuid, b.data() + 32);
   m.epoch           = get_le(b.data() + 48, 8);
   m.head            = static_cast<uint32_t>(get_le(b.data() + 56, 4));
   m.abi_index_len   = static_cast<uint32_t>(get_le(b.data() + 60, 4));
   copy_in(m.head_id, b.data() + 64);
   m.slice_data_end  = get_le(b.data() + 96, 8);
   m.slice_index_len = static_cast<uint32_t>(get_le(b.data() + 104, 4));
   m.abi_floor       = static_cast<uint32_t>(get_le(b.data() + 108, 4));
   m.abi_data_end    = get_le(b.data() + 112, 8);
   return m;
}

std::optional<manifest> manifest::read(const std::filesystem::path& core_root) {
   const auto path = manifest_path(core_root);
   if (!std::filesystem::exists(path))
      return std::nullopt;
   return parse(read_file(path));
}

std::vector<char> binding::serialize() const {
   std::vector<char> b(BINDING_SIZE, 0);
   std::memcpy(b.data(), BINDING_MAGIC, 8);
   put_le(b.data() + 8, FORMAT_VERSION, 4);
   put_le(b.data() + 12, static_cast<uint32_t>(role), 4);
   copy_out(b.data() + 16, chain_id);
   copy_out(b.data() + 48, core_uuid);
   copy_out(b.data() + 64, abi_uuid);
   put_le(b.data() + 80, slice_span, 4);
   put_le(b.data() + 92, crc32c(b.data(), 92), 4);
   return b;
}

binding binding::parse(const std::vector<char>& b) {
   if (b.size() != BINDING_SIZE)
      throw data_corruption_exception("binding holds " + std::to_string(b.size()) + " bytes, expected " +
                                      std::to_string(BINDING_SIZE));
   if (std::memcmp(b.data(), BINDING_MAGIC, 8) != 0)
      throw data_corruption_exception("binding has a bad magic");
   if (get_le(b.data() + 8, 4) != FORMAT_VERSION)
      throw data_corruption_exception("binding version " + std::to_string(get_le(b.data() + 8, 4)) +
                                      " is not supported by this build");
   if (get_le(b.data() + 92, 4) != crc32c(b.data(), 92))
      throw data_corruption_exception("binding checksum mismatch");
   binding out;
   out.role = static_cast<binding_role>(get_le(b.data() + 12, 4));
   copy_in(out.chain_id, b.data() + 16);
   copy_in(out.core_uuid, b.data() + 48);
   copy_in(out.abi_uuid, b.data() + 64);
   out.slice_span = static_cast<uint32_t>(get_le(b.data() + 80, 4));
   return out;
}

std::optional<binding> binding::read(const std::filesystem::path& root) {
   const auto path = binding_path(root);
   if (!std::filesystem::exists(path))
      return std::nullopt;
   return parse(read_file(path));
}

void binding::write(const std::filesystem::path& root) const { write_file_atomic_durable(binding_path(root), serialize()); }

void check_binding_pair(const binding& core, const binding& abi) {
   if (core.role != binding_role::core)
      throw binding_exception("the core root holds an ABI binding");
   if (abi.role != binding_role::abi)
      throw binding_exception("the ABI root holds a core binding");
   if (core.core_uuid != abi.core_uuid || core.abi_uuid != abi.abi_uuid)
      throw binding_exception("core root is store " + format_uuid(core.core_uuid) + "/" + format_uuid(core.abi_uuid) +
                              ", ABI root is store " + format_uuid(abi.core_uuid) + "/" + format_uuid(abi.abi_uuid));
   if (core.chain_id != abi.chain_id)
      throw binding_exception("core and ABI roots record different chain IDs");
   if (core.slice_span != abi.slice_span)
      throw binding_exception("core slice span " + std::to_string(core.slice_span) + ", ABI slice span " +
                              std::to_string(abi.slice_span));
}

uuid_type new_uuid() {
   std::random_device rd;
   uuid_type          u;
   for (auto& b : u)
      b = static_cast<uint8_t>(rd());
   u[6] = (u[6] & 0x0f) | 0x40;
   u[8] = (u[8] & 0x3f) | 0x80;
   return u;
}

std::string hex(const uint8_t* data, size_t size) {
   static const char* digits = "0123456789abcdef";
   std::string        out;
   out.reserve(size * 2);
   for (size_t i = 0; i < size; ++i) {
      out += digits[data[i] >> 4];
      out += digits[data[i] & 0xf];
   }
   return out;
}

std::string format_uuid(const uuid_type& u) {
   const auto h = hex(u.data(), u.size());
   return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::filesystem::path manifest_path(const std::filesystem::path& core_root) {
   return core_root / CONTROL_DIR_NAME / MANIFEST_NAME;
}

std::filesystem::path published_path(const std::filesystem::path& core_root) {
   return core_root / CONTROL_DIR_NAME / PUBLISHED_NAME;
}

std::filesystem::path binding_path(const std::filesystem::path& root) { return root / BINDING_NAME; }

std::vector<char> read_file(const std::filesystem::path& path) {
   std::ifstream f(path, std::ios::binary);
   if (!f)
      throw io_exception("cannot open " + path.string());
   return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint64_t file_size_or_zero(const std::filesystem::path& path) {
   std::error_code ec;
   auto            size = std::filesystem::file_size(path, ec);
   return ec ? 0 : size;
}

void sync_fd(int fd, const std::string& what) {
#if defined(__linux__)
   int rc = ::fdatasync(fd);
#else
   int rc = ::fsync(fd);
#endif
   if (rc != 0)
      throw io_exception("cannot sync " + what + ": " + errno_text());
}

void sync_file(const std::filesystem::path& path) {
   int fd = open_or_throw(path, O_RDONLY);
   try {
      sync_fd(fd, path.string());
   } catch (...) {
      ::close(fd);
      throw;
   }
   ::close(fd);
}

void sync_dir(const std::filesystem::path& dir) {
   int fd = open_or_throw(dir, O_RDONLY | O_DIRECTORY);
   int rc = ::fsync(fd);
   ::close(fd);
   if (rc != 0)
      throw io_exception("cannot sync directory " + dir.string() + ": " + errno_text());
}

void truncate_and_sync(const std::filesystem::path& path, uint64_t size) {
   int fd = open_or_throw(path, O_RDWR);
   int rc = ::ftruncate(fd, static_cast<off_t>(size));
   if (rc == 0)
      rc = ::fsync(fd);
   ::close(fd);
   if (rc != 0)
      throw io_exception("cannot truncate " + path.string() + ": " + errno_text());
}

void write_at(const std::filesystem::path& path, uint64_t offset, const void* data, size_t size, bool sync) {
   int fd = open_or_throw(path, O_RDWR);
   ssize_t n = ::pwrite(fd, data, size, static_cast<off_t>(offset));
   int rc = (n == static_cast<ssize_t>(size)) ? 0 : -1;
   if (rc == 0 && sync)
      rc = ::fsync(fd);
   ::close(fd);
   if (rc != 0)
      throw io_exception("cannot write " + path.string() + ": " + errno_text());
}

void write_file_atomic_durable(const std::filesystem::path& path, const std::vector<char>& data,
                               const atomic_write_steps& steps) {
   const auto dir = path.parent_path();
   if (!std::filesystem::exists(dir)) {
      std::filesystem::create_directories(dir);
      sync_dir(dir.parent_path());
   }
   auto tmp = dir / ("." + path.filename().string() + ".tmp-" + std::to_string(::getpid()));
   int  fd  = open_or_throw(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   try {
      write_all(fd, data.data(), data.size(), tmp.string());
      if (::fsync(fd) != 0)
         throw io_exception("cannot sync " + tmp.string() + ": " + errno_text());
   } catch (...) {
      ::close(fd);
      std::filesystem::remove(tmp);
      throw;
   }
   ::close(fd);
   std::error_code ec;
   std::filesystem::rename(tmp, path, ec);
   if (ec) {
      std::filesystem::remove(tmp);
      throw io_exception("cannot rename " + tmp.string() + " to " + path.string() + ": " + ec.message());
   }
   if (steps.after_rename)
      steps.after_rename();
   sync_dir(dir);
   if (steps.after_dir_sync)
      steps.after_dir_sync();
}

void write_file_replace(const std::filesystem::path& path, const std::vector<char>& data) {
   auto tmp = path.parent_path() / ("." + path.filename().string() + ".tmp-" + std::to_string(::getpid()));
   int  fd  = open_or_throw(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   try {
      write_all(fd, data.data(), data.size(), tmp.string());
   } catch (...) {
      ::close(fd);
      std::filesystem::remove(tmp);
      throw;
   }
   ::close(fd);
   std::error_code ec;
   std::filesystem::rename(tmp, path, ec);
   if (ec) {
      std::filesystem::remove(tmp);
      throw io_exception("cannot rename " + tmp.string() + " to " + path.string() + ": " + ec.message());
   }
}

void remove_stray_temps(const std::filesystem::path& dir, const std::string& base_name) {
   std::error_code ec;
   if (!std::filesystem::is_directory(dir, ec))
      return;
   const auto prefix = "." + base_name + ".tmp-";
   for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0)
         std::filesystem::remove(entry.path(), ec);
   }
}

uint64_t free_bytes(const std::filesystem::path& dir) {
   struct statvfs st;
   if (::statvfs(dir.c_str(), &st) != 0)
      throw io_exception("cannot read free space of " + dir.string() + ": " + errno_text());
   return static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
}

writer_lock::writer_lock(const std::filesystem::path& root)
   : lock_path(root / LOCK_NAME) {
   std::filesystem::create_directories(root);
   fd = open_or_throw(lock_path, O_RDWR | O_CREAT, 0644);
#if defined(__linux__) && defined(F_OFD_SETLK)
   struct flock lk;
   std::memset(&lk, 0, sizeof(lk));
   lk.l_type   = F_WRLCK;
   lk.l_whence = SEEK_SET;
   int rc      = ::fcntl(fd, F_OFD_SETLK, &lk);
   const bool busy = rc != 0 && (errno == EAGAIN || errno == EACCES);
#else
   int        rc   = ::flock(fd, LOCK_EX | LOCK_NB);
   const bool busy = rc != 0 && errno == EWOULDBLOCK;
#endif
   if (rc != 0) {
      const auto err = errno_text();
      ::close(fd);
      fd = -1;
      if (busy)
         throw lock_held_exception(lock_path.string() + ": another writer holds the lock");
      throw io_exception("cannot lock " + lock_path.string() + ": " + err);
   }
}

writer_lock::~writer_lock() {
   if (fd >= 0)
      ::close(fd);
}

} // namespace eosio::roborovski_writer
