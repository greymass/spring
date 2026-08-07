#pragma once
#include <eosio/resource_feed_plugin/types.hpp>

#include <eosio/chain/types.hpp>
#include <chainbase/chainbase.hpp>

#include <optional>
#include <vector>

namespace eosio::chain::resource_limits {
struct resource_limits_object;
}

namespace eosio::resource_feed {

struct block_deltas {
   std::vector<account_record>   accounts;
   std::vector<uint64_t>         removed;
   std::optional<globals_record> globals;
   std::optional<config_record>  config;
};

block_deltas   collect_block_deltas(const chainbase::database& db);
account_record read_account(const chainbase::database& db, chain::account_name owner);
account_record read_account(const chainbase::database& db, const chain::resource_limits::resource_limits_object& limits);
globals_record read_globals(const chainbase::database& db);
config_record  read_config(const chainbase::database& db);

} // namespace eosio::resource_feed
