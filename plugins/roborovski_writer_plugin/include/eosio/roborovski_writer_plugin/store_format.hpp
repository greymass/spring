#pragma once
#include <eosio/roborovski_writer_plugin/types.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace eosio::roborovski_writer {

using uuid_type  = std::array<uint8_t, 16>;
using hash_bytes = std::array<uint8_t, 32>;

uint32_t crc32c(const void* data, size_t size);

void     put_le(char* out, uint64_t v, int bytes);
uint64_t get_le(const char* in, int bytes);

constexpr size_t MANIFEST_SIZE = 128;
constexpr size_t BINDING_SIZE  = 96;

inline const std::string CONTROL_DIR_NAME = "control";
inline const std::string MANIFEST_NAME    = "manifest";
inline const std::string BINDING_NAME     = "store.binding";
inline const std::string LOCK_NAME        = "writer.lock";

struct manifest {
   uuid_type  core_uuid{};
   uuid_type  abi_uuid{};
   uint64_t   epoch           = 0;
   uint32_t   head            = 0;
   hash_bytes head_id{};
   uint64_t   slice_data_end  = 0;
   uint32_t   slice_index_len = 0;
   uint32_t   abi_index_len   = 0;
   uint64_t   abi_data_end    = 0;
   uint32_t   abi_floor       = 0;

   std::vector<char>              serialize() const;
   static manifest                parse(const std::vector<char>& bytes);
   static std::optional<manifest> read(const std::filesystem::path& core_root);
   bool                           operator==(const manifest&) const = default;
};

enum class binding_role : uint32_t { core = 1, abi = 2 };

struct binding {
   binding_role role = binding_role::core;
   hash_bytes   chain_id{};
   uuid_type    core_uuid{};
   uuid_type    abi_uuid{};
   uint32_t     slice_span = 0;

   bool chain_known() const { return chain_id != hash_bytes{}; }

   std::vector<char>             serialize() const;
   static binding                parse(const std::vector<char>& bytes);
   static std::optional<binding> read(const std::filesystem::path& root);
   void                          write(const std::filesystem::path& root) const;
};

void check_binding_pair(const binding& core, const binding& abi);

uuid_type   new_uuid();
std::string format_uuid(const uuid_type& u);
std::string hex(const uint8_t* data, size_t size);

std::filesystem::path manifest_path(const std::filesystem::path& core_root);
std::filesystem::path binding_path(const std::filesystem::path& root);

std::vector<char> read_file(const std::filesystem::path& path);
uint64_t          file_size_or_zero(const std::filesystem::path& path);
void              sync_file(const std::filesystem::path& path);
void              sync_dir(const std::filesystem::path& dir);
void              sync_fd(int fd, const std::string& what);
void              truncate_and_sync(const std::filesystem::path& path, uint64_t size);
void              write_at(const std::filesystem::path& path, uint64_t offset, const void* data, size_t size, bool sync);

struct atomic_write_steps {
   std::function<void()> after_rename;
   std::function<void()> after_dir_sync;
};
void write_file_atomic_durable(const std::filesystem::path& path, const std::vector<char>& data,
                               const atomic_write_steps& steps = {});
void remove_stray_temps(const std::filesystem::path& dir, const std::string& base_name);

uint64_t free_bytes(const std::filesystem::path& dir);

class writer_lock {
public:
   explicit writer_lock(const std::filesystem::path& root);
   ~writer_lock();
   writer_lock(const writer_lock&)            = delete;
   writer_lock& operator=(const writer_lock&) = delete;

   const std::filesystem::path& path() const { return lock_path; }

private:
   std::filesystem::path lock_path;
   int                   fd = -1;
};

class lock_held_exception : public writer_exception {
public:
   using writer_exception::writer_exception;
};

class binding_exception : public writer_exception {
public:
   using writer_exception::writer_exception;
};

} // namespace eosio::roborovski_writer
