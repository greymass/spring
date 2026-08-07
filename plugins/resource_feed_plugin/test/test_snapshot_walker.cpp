#include <boost/test/unit_test.hpp>

#include <eosio/resource_feed_plugin/collector.hpp>
#include <eosio/resource_feed_plugin/snapshot_walker.hpp>

#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/resource_limits_private.hpp>
#include <eosio/testing/chainbase_fixture.hpp>
#include <eosio/testing/tester.hpp>

#include <boost/signals2/connection.hpp>
#include <boost/tuple/tuple.hpp>

#include <algorithm>
#include <map>
#include <vector>

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::chain::resource_limits;
using namespace eosio::testing;
using namespace eosio::resource_feed;

namespace {

std::vector<uint64_t> chainbase_owners(const chainbase::database& db) {
   std::vector<uint64_t> out;
   const auto&           idx = db.get_index<resource_limits_index, by_owner>();
   for (auto it = idx.lower_bound(boost::make_tuple(false, account_name())); it != idx.end() && !it->pending; ++it)
      out.push_back(it->owner.to_uint64_t());
   return out;
}

std::vector<uint64_t> drain(snapshot_walker& w, const chainbase::database& db, size_t max_rows, size_t& chunks) {
   std::vector<uint64_t> seen;
   while (w.active()) {
      const auto chunk = w.next_chunk(db, max_rows);
      BOOST_REQUIRE_LE(chunk.size(), max_rows);
      ++chunks;
      BOOST_REQUIRE_LT(chunks, 10000u);
      for (const auto& r : chunk)
         seen.push_back(r.owner);
   }
   return seen;
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

} // namespace

BOOST_AUTO_TEST_SUITE(snapshot_walker_tests)

BOOST_FIXTURE_TEST_CASE(full_walk_returns_every_owner_once_in_name_order, tester) {
   create_accounts({"wa1"_n, "wa2"_n, "wa3"_n, "wa4"_n, "wa5"_n, "wb1"_n, "wb2"_n});
   produce_block();

   snapshot_walker w;
   w.start(control->head().block_num());
   BOOST_CHECK(w.active());
   BOOST_CHECK_EQUAL(w.block_num(), control->head().block_num());

   size_t     chunks = 0;
   const auto seen   = drain(w, control->db(), 3, chunks);

   BOOST_CHECK(!w.active());
   BOOST_CHECK_GT(chunks, 1u);

   const auto expected = chainbase_owners(control->db());
   BOOST_REQUIRE_GT(expected.size(), 7u);
   BOOST_CHECK_EQUAL_COLLECTIONS(seen.begin(), seen.end(), expected.begin(), expected.end());
   BOOST_CHECK(std::is_sorted(seen.begin(), seen.end()));
   BOOST_CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

BOOST_FIXTURE_TEST_CASE(chunk_records_merge_limits_and_usage, tester) {
   create_accounts({"wa1"_n});
   produce_block();
   push_reqauth("wa1"_n, "owner");
   produce_block();

   snapshot_walker w;
   w.start(control->head().block_num());

   std::vector<account_record> all;
   while (w.active()) {
      const auto chunk = w.next_chunk(control->db(), 4);
      all.insert(all.end(), chunk.begin(), chunk.end());
   }

   const auto it = std::find_if(all.begin(), all.end(),
                                [](const account_record& r) { return r.owner == "wa1"_n.to_uint64_t(); });
   BOOST_REQUIRE(it != all.end());

   const auto direct = read_account(control->db(), "wa1"_n);
   BOOST_CHECK_EQUAL(it->net_weight, direct.net_weight);
   BOOST_CHECK_EQUAL(it->cpu_weight, direct.cpu_weight);
   BOOST_CHECK_EQUAL(it->ram_bytes, direct.ram_bytes);
   BOOST_CHECK_EQUAL(it->cpu_consumed, direct.cpu_consumed);
   BOOST_CHECK_EQUAL(it->net_consumed, direct.net_consumed);
   BOOST_CHECK_GT(it->cpu_consumed, 0u);
   BOOST_CHECK_GT(it->ram_usage, 0u);
}

BOOST_FIXTURE_TEST_CASE(accounts_created_behind_the_cursor_are_not_revisited, tester) {
   create_accounts({"wa1"_n, "wa2"_n, "wa3"_n, "wa4"_n, "wa5"_n, "wb1"_n, "wb2"_n});
   produce_block();

   snapshot_walker w;
   w.start(control->head().block_num());

   const auto first = w.next_chunk(control->db(), 3);
   BOOST_REQUIRE_EQUAL(first.size(), 3u);
   BOOST_REQUIRE(w.active());
   BOOST_REQUIRE_LT("aaa"_n.to_uint64_t(), first.back().owner);

   create_accounts({"aaa"_n});
   produce_block();

   size_t     chunks = 0;
   const auto rest   = drain(w, control->db(), 3, chunks);

   BOOST_CHECK(std::find(rest.begin(), rest.end(), "aaa"_n.to_uint64_t()) == rest.end());

   std::vector<uint64_t> seen;
   for (const auto& r : first)
      seen.push_back(r.owner);
   seen.insert(seen.end(), rest.begin(), rest.end());
   BOOST_CHECK(std::is_sorted(seen.begin(), seen.end()));
   BOOST_CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

BOOST_FIXTURE_TEST_CASE(accounts_removed_ahead_of_the_cursor_are_simply_skipped, raw_db_fixture) {
   for (auto owner : {"aaa"_n, "aab"_n, "aac"_n, "aad"_n})
      initialize_account(owner, false);

   snapshot_walker w;
   w.start(1);

   const auto first = w.next_chunk(db(), 2);
   BOOST_REQUIRE_EQUAL(first.size(), 2u);
   BOOST_REQUIRE(w.active());
   BOOST_REQUIRE_LT(first.back().owner, "aad"_n.to_uint64_t());

   const auto& row = db().get<resource_limits_object, by_owner>(boost::make_tuple(false, "aad"_n));
   db().remove(row);

   size_t     chunks = 0;
   const auto rest   = drain(w, db(), 2, chunks);
   BOOST_CHECK(std::find(rest.begin(), rest.end(), "aad"_n.to_uint64_t()) == rest.end());
   BOOST_CHECK_EQUAL(rest.size(), 1u);
   BOOST_CHECK_EQUAL(rest[0], "aac"_n.to_uint64_t());
}

BOOST_FIXTURE_TEST_CASE(pending_rows_are_never_walked, raw_db_fixture) {
   initialize_account("alice"_n, false);
   initialize_account("bob"_n, false);
   set_account_limits("alice"_n, 4096, 10, 20, false);

   snapshot_walker w;
   w.start(1);

   std::vector<uint64_t> seen;
   while (w.active()) {
      for (const auto& r : w.next_chunk(db(), 1))
         seen.push_back(r.owner);
   }

   const auto expected = chainbase_owners(db());
   BOOST_CHECK_EQUAL_COLLECTIONS(seen.begin(), seen.end(), expected.begin(), expected.end());
   BOOST_CHECK_EQUAL(std::count(seen.begin(), seen.end(), "alice"_n.to_uint64_t()), 1);
   BOOST_CHECK_EQUAL(seen.size(), 2u);
}

BOOST_FIXTURE_TEST_CASE(inactive_walker_yields_nothing, raw_db_fixture) {
   initialize_account("alice"_n, false);

   snapshot_walker w;
   BOOST_CHECK(!w.active());
   BOOST_CHECK(w.next_chunk(db(), 10).empty());

   w.start(7);
   BOOST_CHECK_EQUAL(w.block_num(), 7u);
   BOOST_CHECK_EQUAL(w.next_chunk(db(), 10).size(), 1u);
   BOOST_CHECK(!w.active());
   BOOST_CHECK(w.next_chunk(db(), 10).empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

bool same(const account_record& a, const account_record& b) {
   return a.owner == b.owner && a.net_weight == b.net_weight && a.cpu_weight == b.cpu_weight &&
          a.ram_bytes == b.ram_bytes && a.net_last_ordinal == b.net_last_ordinal && a.net_value_ex == b.net_value_ex &&
          a.net_consumed == b.net_consumed && a.cpu_last_ordinal == b.cpu_last_ordinal &&
          a.cpu_value_ex == b.cpu_value_ex && a.cpu_consumed == b.cpu_consumed && a.ram_usage == b.ram_usage;
}

std::map<uint64_t, account_record> chainbase_state(const chainbase::database& db) {
   std::map<uint64_t, account_record> out;
   const auto&                        idx = db.get_index<resource_limits_index, by_owner>();
   for (auto it = idx.lower_bound(boost::make_tuple(false, account_name())); it != idx.end() && !it->pending; ++it)
      out.emplace(it->owner.to_uint64_t(), read_account(db, it->owner));
   return out;
}

struct feed_client : tester {
   explicit feed_client(uint32_t connect_after_blocks, size_t rows = 3)
   : chunk_rows(rows) {
      connect_at = control->head().block_num() + 1 + connect_after_blocks;
      conn       = control->accepted_block().connect(
          [this](const block_signal_params& p) { on_block(std::get<0>(p)->block_num()); });
   }

   void on_block(uint32_t bn) {
      if (bn < connect_at)
         return;

      const auto& db = control->db();
      if (!connected) {
         walker.start(bn);
         connected = true;
      }

      const auto deltas = collect_block_deltas(db);
      for (const auto& r : deltas.accounts)
         state[r.owner] = r;
      for (auto owner : deltas.removed)
         state.erase(owner);

      if (walker.active()) {
         for (const auto& r : walker.next_chunk(db, chunk_rows))
            state[r.owner] = r;
         ++chunks;
         if (!walker.active())
            snapshot_end_block = bn;
      }

      if (snapshot_end_block != 0)
         verify(db, bn);
   }

   void verify(const chainbase::database& db, uint32_t bn) {
      const auto expected = chainbase_state(db);
      BOOST_REQUIRE_MESSAGE(expected.size() == state.size(),
                            "row count diverged at block " << bn << ": chainbase " << expected.size() << " vs client "
                                                           << state.size());
      for (const auto& [owner, rec] : expected) {
         const auto it = state.find(owner);
         BOOST_REQUIRE_MESSAGE(it != state.end(), "missing owner " << name(owner).to_string() << " at block " << bn);
         BOOST_REQUIRE_MESSAGE(same(rec, it->second),
                               "owner " << name(owner).to_string() << " diverged at block " << bn);
      }
      ++verifications;
   }

   size_t                             chunk_rows;
   uint32_t                           connect_at         = 0;
   bool                               connected          = false;
   uint32_t                           snapshot_end_block = 0;
   size_t                             chunks             = 0;
   size_t                             verifications      = 0;
   snapshot_walker                    walker;
   std::map<uint64_t, account_record> state;
   boost::signals2::scoped_connection conn;
};

void drive_churn(feed_client& c) {
   const account_name names[] = {"acca"_n, "accb"_n, "accc"_n, "accd"_n, "acce"_n, "accf"_n,
                                 "accg"_n, "acch"_n, "acci"_n, "accj"_n, "acck"_n, "accl"_n};

   c.produce_block();
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
      c.create_account(names[i]);
      if (i % 3 == 0)
         c.produce_block();
      if (i > 0)
         c.push_reqauth(names[i - 1], "owner");
      c.produce_block();
   }
   for (size_t i = 0; i < 4; ++i) {
      c.push_reqauth(names[i], "owner");
      c.produce_block();
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(snapshot_walker_invariant_tests)

BOOST_AUTO_TEST_CASE(chunks_plus_deltas_reproduce_chainbase_connect_at_first_block) {
   feed_client c(0, 1);
   drive_churn(c);

   BOOST_REQUIRE(c.connected);
   BOOST_REQUIRE_GT(c.snapshot_end_block, 0u);
   BOOST_CHECK_GT(c.chunks, 1u);
   BOOST_CHECK_GT(c.verifications, 5u);
}

BOOST_AUTO_TEST_CASE(chunks_plus_deltas_reproduce_chainbase_connect_mid_history) {
   feed_client c(6);
   drive_churn(c);

   BOOST_REQUIRE(c.connected);
   BOOST_REQUIRE_GT(c.snapshot_end_block, 0u);
   BOOST_CHECK_GT(c.chunks, 1u);
   BOOST_CHECK_GT(c.verifications, 5u);
}

BOOST_AUTO_TEST_CASE(chunks_plus_deltas_reproduce_chainbase_connect_during_heavy_activity) {
   feed_client c(14);
   drive_churn(c);

   BOOST_REQUIRE(c.connected);
   BOOST_REQUIRE_GT(c.snapshot_end_block, 0u);
   BOOST_CHECK_GT(c.chunks, 1u);
   BOOST_CHECK_GT(c.verifications, 1u);
}

BOOST_AUTO_TEST_CASE(whole_snapshot_in_one_chunk_still_reproduces_chainbase) {
   feed_client c(8, 1000);
   drive_churn(c);

   BOOST_REQUIRE(c.connected);
   BOOST_REQUIRE_GT(c.snapshot_end_block, 0u);
   BOOST_CHECK_EQUAL(c.chunks, 1u);
   BOOST_CHECK_GT(c.verifications, 1u);
}

BOOST_AUTO_TEST_SUITE_END()
