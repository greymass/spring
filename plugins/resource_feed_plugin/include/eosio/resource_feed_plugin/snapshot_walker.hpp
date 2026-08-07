#pragma once
#include <eosio/resource_feed_plugin/types.hpp>

#include <eosio/chain/types.hpp>
#include <chainbase/chainbase.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eosio::resource_feed {

class snapshot_walker {
public:
   void start(uint32_t block_num);
   bool active() const { return _active; }

   uint32_t block_num() const { return _block_num; }

   std::vector<account_record> next_chunk(const chainbase::database& db, size_t max_rows);

private:
   chain::account_name _cursor;
   uint32_t            _block_num = 0;
   bool                _active    = false;
};

} // namespace eosio::resource_feed
