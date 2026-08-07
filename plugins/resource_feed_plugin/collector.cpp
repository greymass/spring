#include <eosio/resource_feed_plugin/collector.hpp>

#include <eosio/chain/resource_limits_private.hpp>

#include <boost/tuple/tuple.hpp>

#include <set>

namespace eosio::resource_feed {

using chain::resource_limits::resource_limits_config_object;
using chain::resource_limits::resource_limits_index;
using chain::resource_limits::resource_limits_object;
using chain::resource_limits::resource_limits_state_index;
using chain::resource_limits::resource_limits_state_object;
using chain::resource_limits::resource_usage_index;
using chain::resource_limits::resource_usage_object;
using chain::resource_limits::by_owner;

namespace {

bool include_delta(const resource_limits_object& old, const resource_limits_object& curr) {
   return old.net_weight != curr.net_weight || //
          old.cpu_weight != curr.cpu_weight || //
          old.ram_bytes != curr.ram_bytes;
}

bool include_delta(const resource_limits_state_object& old, const resource_limits_state_object& curr) {
   return old.total_net_weight != curr.total_net_weight ||   //
          old.total_cpu_weight != curr.total_cpu_weight ||   //
          old.total_ram_bytes != curr.total_ram_bytes ||     //
          old.virtual_net_limit != curr.virtual_net_limit || //
          old.virtual_cpu_limit != curr.virtual_cpu_limit;
}

bool include_delta(const resource_limits_config_object& old, const resource_limits_config_object& curr) {
   return old.account_cpu_usage_average_window != curr.account_cpu_usage_average_window ||
          old.account_net_usage_average_window != curr.account_net_usage_average_window;
}

void read_usage_into(const chainbase::database& db, chain::account_name owner, account_record& rec) {
   const auto* usage = db.find<resource_usage_object, by_owner>(owner);
   if (usage != nullptr) {
      rec.net_last_ordinal = usage->net_usage.last_ordinal;
      rec.net_value_ex     = usage->net_usage.value_ex;
      rec.net_consumed     = usage->net_usage.consumed;
      rec.cpu_last_ordinal = usage->cpu_usage.last_ordinal;
      rec.cpu_value_ex     = usage->cpu_usage.value_ex;
      rec.cpu_consumed     = usage->cpu_usage.consumed;
      rec.ram_usage        = usage->ram_usage;
   }
}

template <typename Index, typename Object, typename Reader>
auto changed_singleton(const chainbase::database& db, Reader reader) -> std::optional<decltype(reader(db))> {
   const auto  undo    = db.get_index<Index>().last_undo_session();
   const auto& curr    = db.get<Object>();
   bool        changed = !undo.new_values.empty();
   for (const auto& old : undo.old_values) {
      if (include_delta(old, curr))
         changed = true;
   }
   if (changed)
      return reader(db);
   return std::nullopt;
}

} // namespace

account_record read_account(const chainbase::database& db, chain::account_name owner) {
   const auto* limits = db.find<resource_limits_object, by_owner>(boost::make_tuple(false, owner));
   if (limits != nullptr)
      return read_account(db, *limits);

   account_record rec{};
   rec.owner      = owner.to_uint64_t();
   rec.net_weight = -1;
   rec.cpu_weight = -1;
   rec.ram_bytes  = -1;
   read_usage_into(db, owner, rec);
   return rec;
}

account_record read_account(const chainbase::database& db, const resource_limits_object& limits) {
   account_record rec{};
   rec.owner      = limits.owner.to_uint64_t();
   rec.net_weight = limits.net_weight;
   rec.cpu_weight = limits.cpu_weight;
   rec.ram_bytes  = limits.ram_bytes;
   read_usage_into(db, limits.owner, rec);
   return rec;
}

globals_record read_globals(const chainbase::database& db) {
   const auto& state = db.get<resource_limits_state_object>();
   return globals_record{state.total_net_weight, state.total_cpu_weight, state.total_ram_bytes, state.virtual_net_limit,
                         state.virtual_cpu_limit};
}

config_record read_config(const chainbase::database& db) {
   const auto& cfg = db.get<resource_limits_config_object>();
   return config_record{cfg.account_cpu_usage_average_window, cfg.account_net_usage_average_window};
}

block_deltas collect_block_deltas(const chainbase::database& db) {
   block_deltas result;

   std::set<chain::account_name> owners;

   const auto& limits_index = db.get_index<resource_limits_index>();
   {
      const auto undo = limits_index.last_undo_session();
      for (const auto& row : undo.new_values)
         owners.insert(row.owner);
      for (const auto& old : undo.old_values) {
         const auto& curr = limits_index.get(old.id);
         if (include_delta(old, curr))
            owners.insert(curr.owner);
      }
      for (const auto& rem : undo.removed_values)
         owners.insert(rem.owner);
   }

   const auto& usage_index = db.get_index<resource_usage_index>();
   {
      const auto undo = usage_index.last_undo_session();
      for (const auto& row : undo.new_values)
         owners.insert(row.owner);
      for (const auto& old : undo.old_values)
         owners.insert(usage_index.get(old.id).owner);
      for (const auto& rem : undo.removed_values)
         owners.insert(rem.owner);
   }

   for (const auto& owner : owners) {
      const auto* limits = db.find<resource_limits_object, by_owner>(boost::make_tuple(false, owner));
      if (limits == nullptr)
         result.removed.push_back(owner.to_uint64_t());
      else
         result.accounts.push_back(read_account(db, *limits));
   }

   result.globals = changed_singleton<resource_limits_state_index, resource_limits_state_object>(db, read_globals);
   result.config =
       changed_singleton<chain::resource_limits::resource_limits_config_index, resource_limits_config_object>(
           db, read_config);

   return result;
}

} // namespace eosio::resource_feed
