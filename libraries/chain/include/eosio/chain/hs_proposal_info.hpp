#pragma once

#include <eosio/chain/types.hpp>

namespace eosio::chain {

   struct hs_proposal_info_t {
      uint32_t last_qc_block_height {0};  ///< The block height of the most recent ancestor block that has a QC justification
      bool     is_last_qc_strong {false}; ///< whether the QC for the block referenced by last_qc_block_height is strong or weak.
   };

   using hs_proposal_info_ptr = std::shared_ptr<hs_proposal_info_t>;

   /**
    * Block Header Extension Compatibility
    */
   struct hs_proposal_info_extension : hs_proposal_info_t {
      static constexpr uint16_t extension_id()   { return 3; } 
      static constexpr bool     enforce_unique() { return true; }
   };

} /// eosio::chain

FC_REFLECT( eosio::chain::hs_proposal_info_t, (last_qc_block_height)(is_last_qc_strong) )
FC_REFLECT_DERIVED( eosio::chain::hs_proposal_info_extension, (eosio::chain::hs_proposal_info_t), )