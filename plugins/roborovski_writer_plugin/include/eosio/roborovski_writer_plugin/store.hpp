#pragma once
#include <eosio/roborovski_writer_plugin/abi_writer.hpp>
#include <eosio/roborovski_writer_plugin/buffer_log.hpp>
#include <eosio/roborovski_writer_plugin/recovery.hpp>
#include <eosio/roborovski_writer_plugin/slice_writer.hpp>
#include <eosio/roborovski_writer_plugin/store_format.hpp>

#include <fc/log/logger.hpp>

#include <filesystem>
#include <memory>

namespace eosio::roborovski_writer {

struct open_report {
   bool            adopted = false;
   truncate_report core;
   bool            abi_cut = false;
};

// Owns core storage and the ABI cache for the life of the process and publishes commits through the manifest.
class store {
public:
   store(const std::filesystem::path& core_root, const std::filesystem::path& abi_root, const hash_bytes& chain_id,
         fc::logger& log);

   const manifest&    committed() const { return current; }
   const binding&     core_binding() const { return core_bind; }
   const open_report& opened() const { return report; }

   uint32_t   written_head() const { return core->get_last_block(); }
   hash_bytes written_head_id() const { return head_id; }

   abi_writer& abis() { return *abi; }
   void        set_abi_floor(uint32_t floor) { pending_floor = floor; }

   void append(const prepared_block& block);

   // ABI sync, core sync, manifest replace; false when nothing changed since the last commit.
   bool commit();

   double last_fsync_seconds() const { return fsync_seconds; }

   void close();

private:
   void load(const hash_bytes& chain_id);
   void adopt();
   void recover();

   fc::logger&                  log;
   std::filesystem::path        core_root;
   std::filesystem::path        abi_root;
   std::unique_ptr<writer_lock> core_lock;
   std::unique_ptr<writer_lock> abi_lock;
   binding                      core_bind;
   binding                      abi_bind;
   manifest                     current;
   bool                         have_manifest = false;
   open_report                  report;

   std::unique_ptr<slice_writer> core;
   std::unique_ptr<abi_writer>   abi;
   hash_bytes                    head_id{};
   uint32_t                      pending_floor = 0;
   double                        fsync_seconds = 0;
};

} // namespace eosio::roborovski_writer
