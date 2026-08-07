#include <boost/test/unit_test.hpp>

#include <eosio/resource_feed_plugin/collector.hpp>

#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/resource_limits_private.hpp>
#include <eosio/testing/chainbase_fixture.hpp>
#include <eosio/testing/tester.hpp>

#include <boost/signals2/connection.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::chain::resource_limits;
using namespace eosio::testing;
using namespace eosio::resource_feed;

namespace {

const account_record* find_owner(const block_deltas& d, account_name owner) {
   auto it = std::find_if(d.accounts.begin(), d.accounts.end(),
                          [&](const account_record& r) { return r.owner == owner.to_uint64_t(); });
   return it == d.accounts.end() ? nullptr : &*it;
}

size_t count_owner(const block_deltas& d, account_name owner) {
   return std::count_if(d.accounts.begin(), d.accounts.end(),
                        [&](const account_record& r) { return r.owner == owner.to_uint64_t(); });
}

struct raw_db_fixture : private chainbase_fixture<1024 * 1024>, public resource_limits_manager {
   raw_db_fixture()
   : chainbase_fixture()
   , resource_limits_manager(*chainbase_fixture::_db, [](bool) { return nullptr; }) {
      add_indices();
      initialize_database();
   }

   chainbase::database& db() { return *chainbase_fixture::_db; }
};

std::string quoted_u64(uint64_t v) {
   return "\"" + std::to_string(v) + "\"";
}

std::string quoted_i64(int64_t v) {
   return "\"" + std::to_string(v) + "\"";
}

std::string account_json(const account_record& r) {
   std::ostringstream o;
   o << "{\"owner\": " << quoted_u64(r.owner) << ", \"net_weight\": " << quoted_i64(r.net_weight)
     << ", \"cpu_weight\": " << quoted_i64(r.cpu_weight) << ", \"ram_bytes\": " << quoted_i64(r.ram_bytes)
     << ", \"net_last_ordinal\": " << r.net_last_ordinal << ", \"net_value_ex\": " << quoted_u64(r.net_value_ex)
     << ", \"net_consumed\": " << quoted_u64(r.net_consumed) << ", \"cpu_last_ordinal\": " << r.cpu_last_ordinal
     << ", \"cpu_value_ex\": " << quoted_u64(r.cpu_value_ex) << ", \"cpu_consumed\": " << quoted_u64(r.cpu_consumed)
     << ", \"ram_usage\": " << quoted_u64(r.ram_usage) << "}";
   return o.str();
}

std::string globals_json(const globals_record& g) {
   std::ostringstream o;
   o << "{\"total_net_weight\": " << quoted_u64(g.total_net_weight)
     << ", \"total_cpu_weight\": " << quoted_u64(g.total_cpu_weight)
     << ", \"total_ram_bytes\": " << quoted_u64(g.total_ram_bytes)
     << ", \"virtual_net_limit\": " << quoted_u64(g.virtual_net_limit)
     << ", \"virtual_cpu_limit\": " << quoted_u64(g.virtual_cpu_limit) << "}";
   return o.str();
}

struct chain_fixture : tester {
   chain_fixture() {
      conn = control->accepted_block().connect(
          [this](const block_signal_params&) { blocks.push_back(collect_block_deltas(control->db())); });
   }

   const block_deltas& last() const { return blocks.back(); }

   std::vector<block_deltas> blocks;
   boost::signals2::scoped_connection conn;
};

} // namespace

BOOST_AUTO_TEST_SUITE(collector_tests)

BOOST_FIXTURE_TEST_CASE(new_account_yields_merged_record, chain_fixture) {
   create_account("alice"_n);
   produce_block();

   BOOST_REQUIRE(!blocks.empty());
   const auto& d   = last();
   const auto* rec = find_owner(d, "alice"_n);
   BOOST_REQUIRE(rec != nullptr);
   BOOST_CHECK_EQUAL(rec->net_weight, -1);
   BOOST_CHECK_EQUAL(rec->cpu_weight, -1);
   BOOST_CHECK_EQUAL(rec->ram_bytes, -1);
   BOOST_CHECK_EQUAL(count_owner(d, "alice"_n), 1u);
   BOOST_CHECK(d.removed.empty());
}

BOOST_FIXTURE_TEST_CASE(billed_payer_usage_moves_without_moving_totals, chain_fixture) {
   create_accounts({"alice"_n, "bob"_n});
   produce_block();

   const auto before = read_globals(control->db());

   push_reqauth("alice"_n, "owner");
   produce_block();

   const auto& d   = last();
   const auto* rec = find_owner(d, "alice"_n);
   BOOST_REQUIRE(rec != nullptr);
   BOOST_CHECK_GT(rec->cpu_consumed, 0u);
   BOOST_CHECK_GT(rec->net_consumed, 0u);
   BOOST_CHECK(find_owner(d, "bob"_n) == nullptr);
   BOOST_CHECK(d.removed.empty());
   BOOST_CHECK(!d.config);

   const auto after = read_globals(control->db());
   BOOST_CHECK_EQUAL(after.total_net_weight, before.total_net_weight);
   BOOST_CHECK_EQUAL(after.total_cpu_weight, before.total_cpu_weight);
   BOOST_CHECK_EQUAL(after.total_ram_bytes, before.total_ram_bytes);
   if (d.globals) {
      BOOST_CHECK_EQUAL(d.globals->total_net_weight, before.total_net_weight);
      BOOST_CHECK_EQUAL(d.globals->total_cpu_weight, before.total_cpu_weight);
      BOOST_CHECK_EQUAL(d.globals->total_ram_bytes, before.total_ram_bytes);
   }
}

BOOST_FIXTURE_TEST_CASE(limit_change_yields_account_and_globals, raw_db_fixture) {
   initialize_account("alice"_n, false);

   auto session = db().start_undo_session(true);
   set_account_limits("alice"_n, 8192, 100, 200, false);
   process_account_limit_updates();

   const auto d = collect_block_deltas(db());

   const auto* rec = find_owner(d, "alice"_n);
   BOOST_REQUIRE(rec != nullptr);
   BOOST_CHECK_EQUAL(rec->ram_bytes, 8192);
   BOOST_CHECK_EQUAL(rec->net_weight, 100);
   BOOST_CHECK_EQUAL(rec->cpu_weight, 200);
   BOOST_CHECK_EQUAL(count_owner(d, "alice"_n), 1u);
   BOOST_CHECK(d.removed.empty());

   BOOST_REQUIRE(d.globals.has_value());
   BOOST_CHECK_EQUAL(d.globals->total_net_weight, 100u);
   BOOST_CHECK_EQUAL(d.globals->total_cpu_weight, 200u);
   BOOST_CHECK_EQUAL(d.globals->total_ram_bytes, 8192u);
   BOOST_CHECK(!d.config);
}

BOOST_FIXTURE_TEST_CASE(pending_rows_are_never_emitted, raw_db_fixture) {
   initialize_account("alice"_n, false);

   auto session = db().start_undo_session(true);
   set_account_limits("alice"_n, 4096, 10, 20, false);

   const auto d = collect_block_deltas(db());

   BOOST_CHECK_EQUAL(count_owner(d, "alice"_n), 1u);
   const auto* rec = find_owner(d, "alice"_n);
   BOOST_REQUIRE(rec != nullptr);
   BOOST_CHECK_EQUAL(rec->ram_bytes, -1);
   BOOST_CHECK_EQUAL(rec->net_weight, -1);
   BOOST_CHECK_EQUAL(rec->cpu_weight, -1);
   BOOST_CHECK(d.removed.empty());
   BOOST_CHECK(!d.globals);
}

BOOST_FIXTURE_TEST_CASE(block_average_churn_does_not_yield_globals, raw_db_fixture) {
   auto session = db().start_undo_session(true);

   const auto& state = db().get<resource_limits_state_object>();
   db().modify(state, [](resource_limits_state_object& s) {
      s.average_block_cpu_usage.consumed += 10;
      s.average_block_cpu_usage.value_ex += 10;
      s.average_block_cpu_usage.last_ordinal += 1;
      s.average_block_net_usage.consumed += 10;
      s.pending_cpu_usage += 5;
      s.pending_net_usage += 5;
   });

   const auto d = collect_block_deltas(db());
   BOOST_CHECK(!d.globals);
   BOOST_CHECK(d.accounts.empty());
   BOOST_CHECK(d.removed.empty());
}

BOOST_FIXTURE_TEST_CASE(virtual_limit_change_yields_globals, raw_db_fixture) {
   auto session = db().start_undo_session(true);

   const auto& state = db().get<resource_limits_state_object>();
   db().modify(state, [](resource_limits_state_object& s) { s.virtual_cpu_limit += 1; });

   const auto d = collect_block_deltas(db());
   BOOST_REQUIRE(d.globals.has_value());
   BOOST_CHECK_EQUAL(d.globals->virtual_cpu_limit, read_globals(db()).virtual_cpu_limit);
}

BOOST_FIXTURE_TEST_CASE(config_only_yielded_when_window_changes, raw_db_fixture) {
   const auto defaults = read_config(db());
   BOOST_CHECK_EQUAL(defaults.cpu_window, config::account_cpu_usage_average_window_ms / config::block_interval_ms);
   BOOST_CHECK_EQUAL(defaults.net_window, config::account_net_usage_average_window_ms / config::block_interval_ms);

   auto        session = db().start_undo_session(true);
   const auto& cfg     = db().get<resource_limits_config_object>();
   db().modify(cfg, [](resource_limits_config_object& c) { c.account_cpu_usage_average_window = 1234; });

   const auto d = collect_block_deltas(db());
   BOOST_REQUIRE(d.config.has_value());
   BOOST_CHECK_EQUAL(d.config->cpu_window, 1234u);
   BOOST_CHECK_EQUAL(d.config->net_window, defaults.net_window);
}

BOOST_FIXTURE_TEST_CASE(removed_limits_row_yields_removed_owner, raw_db_fixture) {
   initialize_account("alice"_n, false);

   auto        session = db().start_undo_session(true);
   const auto& limits =
       db().get<resource_limits_object, resource_limits::by_owner>(boost::make_tuple(false, "alice"_n));
   db().remove(limits);

   const auto d = collect_block_deltas(db());
   BOOST_REQUIRE_EQUAL(d.removed.size(), 1u);
   BOOST_CHECK_EQUAL(d.removed[0], "alice"_n.to_uint64_t());
   BOOST_CHECK(find_owner(d, "alice"_n) == nullptr);
}

BOOST_FIXTURE_TEST_CASE(read_account_merges_limits_and_usage, raw_db_fixture) {
   initialize_account("alice"_n, false);
   set_account_limits("alice"_n, 1024, 11, 22, false);
   process_account_limit_updates();

   const auto& usage = db().get<resource_usage_object, resource_limits::by_owner>("alice"_n);
   db().modify(usage, [](resource_usage_object& u) {
      u.cpu_usage.last_ordinal = 1;
      u.cpu_usage.value_ex     = 7;
      u.cpu_usage.consumed     = 1000;
      u.net_usage.last_ordinal = 1;
      u.net_usage.value_ex     = 9;
      u.net_usage.consumed     = 2000;
      u.ram_usage              = 512;
   });

   const auto rec = read_account(db(), "alice"_n);
   BOOST_CHECK_EQUAL(rec.owner, "alice"_n.to_uint64_t());
   BOOST_CHECK_EQUAL(rec.ram_bytes, 1024);
   BOOST_CHECK_EQUAL(rec.net_weight, 11);
   BOOST_CHECK_EQUAL(rec.cpu_weight, 22);
   BOOST_CHECK_EQUAL(rec.cpu_consumed, 1000u);
   BOOST_CHECK_EQUAL(rec.net_consumed, 2000u);
   BOOST_CHECK_EQUAL(rec.cpu_last_ordinal, 1u);
   BOOST_CHECK_EQUAL(rec.net_last_ordinal, 1u);
   BOOST_CHECK_EQUAL(rec.cpu_value_ex, 7u);
   BOOST_CHECK_EQUAL(rec.net_value_ex, 9u);
   BOOST_CHECK_EQUAL(rec.ram_usage, 512u);
}

BOOST_FIXTURE_TEST_CASE(dump_mirror_math_fixtures, chain_fixture) {
   create_accounts({"alice"_n, "bob"_n, "carol"_n, "dan"_n});
   produce_block();

   auto& mutable_mgr = control->get_mutable_resource_limits_manager();
   mutable_mgr.set_account_limits("alice"_n, 1024 * 1024, 100, 200, false);
   mutable_mgr.set_account_limits("bob"_n, 1024 * 1024, 5000, 9000, false);
   mutable_mgr.set_account_limits("dan"_n, 1024 * 1024, 700, 1300, false);
   mutable_mgr.process_account_limit_updates();
   produce_block();

   push_reqauth("alice"_n, "owner");
   produce_block();

   for (int i = 0; i < 9; ++i) {
      push_reqauth("bob"_n, "owner");
      produce_block();
   }

   push_reqauth("carol"_n, "owner");
   produce_block();

   const auto  globals = read_globals(control->db());
   const auto  cfg     = read_config(control->db());
   const auto& mgr     = control->get_resource_limits_manager();

   BOOST_REQUIRE_GT(globals.total_cpu_weight, 0u);
   BOOST_REQUIRE_GT(globals.total_net_weight, 0u);

   const std::vector<account_name> names{"alice"_n, "bob"_n, "carol"_n, "dan"_n};

   std::vector<std::string> rows;
   for (const auto& n : names) {
      const auto rec = read_account(control->db(), n);
      for (int res = 0; res < 2; ++res) {
         const bool     is_cpu = (res == 0);
         const uint32_t window = is_cpu ? cfg.cpu_window : cfg.net_window;
         const uint32_t last   = is_cpu ? rec.cpu_last_ordinal : rec.net_last_ordinal;

         const std::vector<uint32_t> deltas{0, 1, 999, 100000, window, window + 5000};
         for (uint32_t delta : deltas) {
            const uint32_t slot = last + delta;
            const auto     out =
                is_cpu ? mgr.get_account_cpu_limit_ex(n, std::numeric_limits<uint32_t>::max(),
                                                      block_timestamp_type(slot))
                       : mgr.get_account_net_limit_ex(n, std::numeric_limits<uint32_t>::max(),
                                                      block_timestamp_type(slot));
            BOOST_CHECK(!out.second);

            std::ostringstream row;
            row << "{\"account\": \"" << n.to_string() << "\", \"resource\": \"" << (is_cpu ? "cpu" : "net")
                << "\", \"now_slot\": " << slot << ", \"window\": " << window
                << ", \"cpu_window\": " << cfg.cpu_window << ", \"net_window\": " << cfg.net_window
                << ", \"record\": " << account_json(rec) << ", \"globals\": " << globals_json(globals)
                << ", \"result\": {\"available\": " << quoted_i64(out.first.available)
                << ", \"used\": " << quoted_i64(out.first.used) << ", \"max\": " << quoted_i64(out.first.max)
                << ", \"current_used\": " << quoted_i64(out.first.current_used) << "}}";
            rows.push_back(row.str());
         }
      }
   }

   const auto alice_rec = read_account(control->db(), "alice"_n);
   BOOST_CHECK_EQUAL(alice_rec.cpu_weight, 200);
   BOOST_CHECK_GT(alice_rec.cpu_value_ex, 0u);

   const auto dan_cpu = mgr.get_account_cpu_limit_ex("dan"_n, std::numeric_limits<uint32_t>::max());
   BOOST_CHECK_EQUAL(dan_cpu.first.used, 0);
   BOOST_CHECK_EQUAL(dan_cpu.first.available, dan_cpu.first.max);

   const auto carol_cpu = mgr.get_account_cpu_limit_ex("carol"_n, std::numeric_limits<uint32_t>::max());
   BOOST_CHECK_EQUAL(carol_cpu.first.available, -1);

   const auto bob_cpu = mgr.get_account_cpu_limit_ex("bob"_n, std::numeric_limits<uint32_t>::max());
   BOOST_CHECK_GT(bob_cpu.first.used, 0);

   std::ostringstream json;
   json << "{\"rows\": [";
   for (size_t i = 0; i < rows.size(); ++i) {
      if (i != 0)
         json << ", ";
      json << rows[i];
   }
   json << "]}";

   std::ofstream out("mirror_math_fixtures.json");
   BOOST_REQUIRE(out.is_open());
   out << json.str();
   out.close();

   BOOST_CHECK_EQUAL(rows.size(), names.size() * 2 * 6);
}

BOOST_AUTO_TEST_SUITE_END()
