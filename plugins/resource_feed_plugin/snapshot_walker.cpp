#include <eosio/resource_feed_plugin/snapshot_walker.hpp>

#include <eosio/resource_feed_plugin/collector.hpp>

#include <eosio/chain/resource_limits_private.hpp>

#include <boost/tuple/tuple.hpp>

#include <algorithm>

namespace eosio::resource_feed {

using chain::resource_limits::by_owner;
using chain::resource_limits::resource_limits_index;

void snapshot_walker::start(uint32_t block_num) {
   _cursor    = chain::account_name();
   _block_num = block_num;
   _active    = true;
}

std::vector<account_record> snapshot_walker::next_chunk(const chainbase::database& db, size_t max_rows) {
   std::vector<account_record> out;
   if (!_active)
      return out;
   out.reserve(std::min<size_t>(max_rows, 4096));

   const auto& idx = db.get_index<resource_limits_index, by_owner>();
   auto        it  = idx.lower_bound(boost::make_tuple(false, _cursor));
   for (; it != idx.end() && !it->pending && out.size() < max_rows; ++it)
      out.push_back(read_account(db, *it));

   if (it == idx.end() || it->pending)
      _active = false;
   else
      _cursor = it->owner;

   return out;
}

} // namespace eosio::resource_feed
