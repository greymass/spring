#pragma once
#include <eosio/roborovski_writer_plugin/types.hpp>

#include <map>
#include <optional>

namespace eosio::roborovski_writer {

built_block build_block(const signed_block& block, const block_id_type& id,
                        const std::optional<cached_trace>&                 onblock,
                        const std::map<transaction_id_type, cached_trace>& traces);

std::vector<char> encode_block_blob(uint32_t block_num, uint32_t block_slot, const block_id_type& id,
                                    uint64_t glob_min, uint64_t glob_max,
                                    const std::vector<transaction_id_type>& trx_ids,
                                    const std::vector<trx_meta>&            trx_metas,
                                    const std::vector<std::vector<char>>&   data_table,
                                    const std::vector<uint64_t>&            names_table,
                                    const std::vector<uint32_t>&            cat_offsets,
                                    const std::vector<std::vector<char>>&   cat_bytes);

} // namespace eosio::roborovski_writer
