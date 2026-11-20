#pragma once
#include <eosio/roborovski_writer_plugin/store_format.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace eosio::roborovski_writer {

struct core_bounds {
   uint32_t head            = 0;
   uint64_t slice_data_end  = 0;
   uint32_t slice_index_len = 0;
};

struct truncate_report {
   std::vector<std::string> removed_slices;
   uint64_t                 data_bytes_cut    = 0;
   uint64_t                 index_entries_cut = 0;
   bool                     finalized_reset   = false;

   bool changed() const {
      return !removed_slices.empty() || data_bytes_cut > 0 || index_entries_cut > 0 || finalized_reset;
   }
};

struct adoption {
   core_bounds bounds;
   hash_bytes  head_id{};
};

uint32_t              slice_of(uint32_t block_num);
uint32_t              slice_start(uint32_t slice_num);
uint32_t              slice_max(uint32_t slice_num);
std::filesystem::path slice_path(const std::filesystem::path& core_root, uint32_t slice_num);

// Mirrors appendlog.TruncateToCommitted in roborovski; the shared fixtures keep the two byte-identical.
truncate_report truncate_core_to(const std::filesystem::path& core_root, const core_bounds& b);

// Mirrors abicache.TruncateTo; returns whether anything was cut.
bool truncate_abi_to(const std::filesystem::path& abi_root, uint64_t data_end, uint32_t index_len);

// Mirrors abicache.CommittedBounds for a cache adopted without a manifest.
std::pair<uint64_t, uint32_t> abi_committed_bounds(const std::filesystem::path& abi_root, uint32_t head);

// Mirrors appendlog.FindAdoptionBoundary.
adoption find_adoption_boundary(const std::filesystem::path& core_root);

hash_bytes block_id_from_record(const std::vector<char>& compressed);
hash_bytes read_stored_block_id(const std::filesystem::path& core_root, uint32_t block_num);

} // namespace eosio::roborovski_writer
