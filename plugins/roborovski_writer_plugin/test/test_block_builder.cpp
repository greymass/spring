#include <boost/test/unit_test.hpp>

#include <eosio/roborovski_writer_plugin/abi_json.hpp>
#include <eosio/roborovski_writer_plugin/block_builder.hpp>
#include <eosio/roborovski_writer_plugin/block_serializer.hpp>

#include <eosio/chain/transaction.hpp>
#include <fc/bitutil.hpp>
#include <fc/io/raw.hpp>

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::roborovski_writer;

namespace {

class blob_reader {
public:
   explicit blob_reader(const std::vector<char>& b)
      : data(b) {}

   uint8_t  u8() { return static_cast<uint8_t>(data.at(pos++)); }
   uint64_t le(int n) {
      uint64_t v = 0;
      for (int i = 0; i < n; ++i)
         v |= static_cast<uint64_t>(u8()) << (8 * i);
      return v;
   }
   uint64_t uvarint() {
      uint64_t v = 0;
      for (unsigned s = 0;; s += 7) {
         uint8_t b = u8();
         v |= static_cast<uint64_t>(b & 0x7f) << s;
         if (!(b & 0x80))
            return v;
      }
   }
   std::vector<char> bytes(size_t n) {
      std::vector<char> out(data.begin() + pos, data.begin() + pos + n);
      pos += n;
      return out;
   }
   bool done() const { return pos == data.size(); }

private:
   const std::vector<char>& data;
   size_t                   pos = 0;
};

struct decoded_block {
   uint32_t                         block_num = 0;
   uint32_t                         slot      = 0;
   std::vector<char>                id;
   uint64_t                         glob_max = 0;
   uint64_t                         glob_count = 0;
   std::vector<std::vector<char>>   trx_ids;
   std::vector<uint8_t>             statuses;
   std::vector<std::vector<std::vector<char>>> signatures;
   std::vector<std::vector<char>>   data_table;
   std::vector<uint64_t>            names;
   std::vector<uint64_t>            offsets;
   std::vector<std::vector<char>>   cats;
};

decoded_block decode(const std::vector<char>& blob) {
   blob_reader   r(blob);
   decoded_block d;
   d.block_num  = r.le(4);
   d.slot       = r.le(4);
   d.id         = r.bytes(32);
   d.glob_max   = r.le(8);
   d.glob_count = r.uvarint();
   auto ntrx    = r.uvarint();
   for (uint64_t i = 0; i < ntrx; ++i)
      d.trx_ids.push_back(r.bytes(32));
   for (uint64_t i = 0; i < ntrx; ++i) {
      d.statuses.push_back(r.u8());
      r.le(4);
      r.uvarint();
      r.le(4);
      r.le(2);
      r.le(4);
      std::vector<std::vector<char>> sigs;
      auto                           nsig = r.uvarint();
      for (uint64_t j = 0; j < nsig; ++j)
         sigs.push_back(r.bytes(r.uvarint()));
      d.signatures.push_back(sigs);
   }
   auto ndata = r.uvarint();
   for (uint64_t i = 0; i < ndata; ++i)
      d.data_table.push_back(r.bytes(r.uvarint()));
   auto nnames = r.uvarint();
   for (uint64_t i = 0; i < nnames; ++i)
      d.names.push_back(r.le(8));
   auto ncats = r.uvarint();
   for (uint64_t i = 0; i < ncats; ++i)
      d.offsets.push_back(r.uvarint());
   std::vector<uint64_t> sizes;
   for (uint64_t i = 0; i < ncats; ++i)
      sizes.push_back(r.uvarint());
   for (auto s : sizes)
      d.cats.push_back(r.bytes(s));
   BOOST_TEST(r.done());
   return d;
}

uint64_t cat_global_sequence(const std::vector<char>& cat) {
   std::vector<char> copy = cat;
   blob_reader       r(copy);
   uint8_t           m1 = r.u8();
   if (m1 & (1 << 4))
      r.u8();
   r.uvarint();
   r.uvarint();
   return r.uvarint();
}

action_trace make_action(name receiver, name account, name act_name, uint64_t glob, uint32_t ordinal,
                         uint32_t creator, std::vector<permission_level> auths, bytes data,
                         bool context_free = false) {
   action_trace at;
   at.action_ordinal         = ordinal;
   at.creator_action_ordinal = creator;
   at.closest_unnotified_ancestor_action_ordinal = creator;
   at.receiver                                   = receiver;
   at.act.account                                = account;
   at.act.name                                   = act_name;
   at.act.authorization                          = auths;
   at.act.data                                   = data;
   at.context_free                               = context_free;
   at.elapsed                                    = fc::microseconds(42);
   if (!context_free) {
      action_receipt rcpt;
      rcpt.receiver        = receiver;
      rcpt.global_sequence = glob;
      rcpt.recv_sequence   = glob * 2;
      rcpt.code_sequence   = 1;
      rcpt.abi_sequence    = 1;
      for (const auto& a : auths)
         rcpt.auth_sequence[a.actor] = glob + 100;
      at.receipt = rcpt;
   }
   return at;
}

cached_trace make_trace(transaction_id_type id, std::vector<action_trace> actions) {
   signed_transaction trx;
   trx.expiration       = fc::time_point_sec(1000);
   trx.ref_block_num    = 7;
   trx.ref_block_prefix = 99;
   trx.signatures.emplace_back();
   auto ptrx = std::make_shared<packed_transaction>(std::move(trx), packed_transaction::compression_type::none);

   auto trace         = std::make_shared<transaction_trace>();
   trace->id          = id;
   trace->receipt     = transaction_receipt_header(transaction_receipt_header::executed);
   trace->receipt->cpu_usage_us    = 100;
   trace->receipt->net_usage_words = 12;
   trace->action_traces            = std::move(actions);
   return {trace, ptrx};
}

transaction_id_type trx_id(uint8_t b) {
   transaction_id_type id;
   id.data()[0] = static_cast<char>(b);
   return id;
}

signed_block make_block(uint32_t num, std::vector<transaction_id_type> receipts) {
   signed_block blk;
   blk.previous._hash[0] = fc::endian_reverse_u32(num - 1);
   blk.timestamp         = block_timestamp_type(1695859200);
   for (const auto& id : receipts)
      blk.transactions.emplace_back(id);
   return blk;
}

} // namespace

BOOST_AUTO_TEST_SUITE(block_builder_tests)

BOOST_AUTO_TEST_CASE(matches_trace_ingestion_layout) {
   const name eosio_n("eosio"), alice("alice"), bob("bob"), token("eosio.token"), transfer("transfer"),
       active("active");

   auto onblock = make_trace(trx_id(1), {make_action(eosio_n, eosio_n, name("onblock"), 1000, 1, 0,
                                                     {{eosio_n, active}}, bytes{1, 2, 3})});

   auto abi_bin = std::string("\x0e" "eosio::abi/1.0" "\0\0\0\0\0\0\0", 22);
   bytes setabi;
   auto  acct = alice.to_uint64_t();
   for (int i = 0; i < 8; ++i)
      setabi.push_back(static_cast<char>(acct >> (8 * i)));
   setabi.push_back(static_cast<char>(abi_bin.size()));
   setabi.insert(setabi.end(), abi_bin.begin(), abi_bin.end());

   std::map<transaction_id_type, cached_trace> traces;
   traces[trx_id(2)] = make_trace(
       trx_id(2), {make_action(token, token, transfer, 1003, 1, 0, {{alice, active}}, bytes{9}),
                   make_action(alice, token, transfer, 1001, 2, 1, {{alice, active}}, bytes{9}),
                   make_action(bob, token, transfer, 1002, 3, 1, {{alice, active}}, bytes{9}),
                   make_action(token, token, name("cfa"), 0, 0, 0, {}, bytes{}, true)});
   traces[trx_id(4)] =
       make_trace(trx_id(4), {make_action(eosio_n, eosio_n, name("setabi"), 1004, 1, 0, {{alice, active}}, setabi)});

   auto block = make_block(100, {trx_id(2), trx_id(3), trx_id(4)});
   block_id_type id;
   id.data()[5] = 0x55;

   auto built = build_block(block, id, onblock, traces);
   BOOST_TEST(built.block_num == 100u);
   BOOST_TEST(built.glob_min == 1000u);
   BOOST_TEST(built.glob_max == 1004u);
   BOOST_TEST(built.action_count == 5u);

   auto d = decode(built.blob);
   BOOST_TEST(d.block_num == 100u);
   BOOST_TEST(d.slot == 1695859200u);
   BOOST_TEST(std::equal(d.id.begin(), d.id.end(), id.data()));
   BOOST_TEST(d.glob_max == 1004u);
   BOOST_TEST(d.glob_count == 4u);

   BOOST_REQUIRE(d.trx_ids.size() == 3u);
   BOOST_TEST(d.trx_ids[0][0] == 1);
   BOOST_TEST(d.trx_ids[1][0] == 2);
   BOOST_TEST(d.trx_ids[2][0] == 4);
   BOOST_TEST(d.statuses.size() == 3u);
   BOOST_REQUIRE(d.signatures[1].size() == 1u);
   BOOST_TEST(d.signatures[1][0] == fc::raw::pack(signature_type()));

   std::vector<uint64_t> expected_names = {eosio_n.to_uint64_t(), name("onblock").to_uint64_t(),
                                           active.to_uint64_t(),  token.to_uint64_t(),
                                           transfer.to_uint64_t(), alice.to_uint64_t(),
                                           bob.to_uint64_t(),      name("setabi").to_uint64_t()};
   BOOST_TEST(d.names == expected_names, boost::test_tools::per_element());
   BOOST_TEST(d.data_table.size() == 3u);

   BOOST_REQUIRE(d.cats.size() == 5u);
   for (size_t j = 0; j < d.cats.size(); ++j) {
      BOOST_TEST(cat_global_sequence(d.cats[j]) == 1000u + j);
      BOOST_TEST(d.offsets[j] == 0u);
   }

   BOOST_REQUIRE(built.abis.size() == 1u);
   BOOST_TEST(built.abis[0].block_num == 100u);
   BOOST_TEST(built.abis[0].contract == alice.to_uint64_t());
   BOOST_TEST(built.abis[0].json == *abi_binary_to_json(abi_bin.data(), abi_bin.size()));
}

BOOST_AUTO_TEST_CASE(failed_deferred_reports_the_deferred_id) {
   auto ct = make_trace(trx_id(7), {make_action(name("eosio"), name("eosio"), name("onerror"), 500, 1, 0, {}, bytes{})});
   auto failed = std::make_shared<transaction_trace>();
   failed->id  = trx_id(8);
   ct.trace->failed_dtrx_trace = failed;

   std::map<transaction_id_type, cached_trace> traces;
   traces[trx_id(8)] = ct;
   auto built = build_block(make_block(5, {trx_id(8)}), block_id_type(), std::nullopt, traces);
   auto d     = decode(built.blob);
   BOOST_REQUIRE(d.trx_ids.size() == 1u);
   BOOST_TEST(d.trx_ids[0][0] == 8);
}

BOOST_AUTO_TEST_CASE(global_sequence_gaps_become_offsets) {
   std::map<transaction_id_type, cached_trace> traces;
   traces[trx_id(2)] = make_trace(trx_id(2), {make_action(name("a"), name("a"), name("x"), 10, 1, 0, {}, bytes{}),
                                              make_action(name("b"), name("a"), name("x"), 15, 2, 1, {}, bytes{}),
                                              make_action(name("c"), name("a"), name("x"), 12, 3, 1, {}, bytes{})});
   auto built = build_block(make_block(9, {trx_id(2)}), block_id_type(), std::nullopt, traces);
   auto d     = decode(built.blob);
   std::vector<uint64_t> expected = {0, 1, 3};
   BOOST_TEST(d.offsets == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(empty_block_has_zero_glob_range) {
   auto built = build_block(make_block(2, {}), block_id_type(), std::nullopt, {});
   BOOST_TEST(built.glob_min == 0u);
   BOOST_TEST(built.glob_max == 0u);
   auto d = decode(built.blob);
   BOOST_TEST(d.trx_ids.empty());
   BOOST_TEST(d.cats.empty());
}

BOOST_AUTO_TEST_CASE(receiver_must_match_receipt) {
   std::map<transaction_id_type, cached_trace> traces;
   auto at = make_action(name("a"), name("a"), name("x"), 10, 1, 0, {}, bytes{});
   at.receipt->receiver = name("b");
   traces[trx_id(2)]    = make_trace(trx_id(2), {at});
   BOOST_CHECK_THROW(build_block(make_block(9, {trx_id(2)}), block_id_type(), std::nullopt, traces),
                     data_corruption_exception);
}

BOOST_AUTO_TEST_CASE(compressed_action_trace_encoding) {
   compressed_action_trace cat;
   cat.global_sequence     = 300;
   cat.elapsed             = -3;
   cat.recv_sequence       = 5;
   cat.action_ordinal      = 3;
   cat.creator_ao          = 1;
   cat.closest_ua_ao       = 1;
   cat.code_sequence       = 4;
   cat.abi_sequence        = 4;
   cat.receiver_index      = 2;
   cat.contract_name_index = 1;
   cat.action_name_index   = 3;
   cat.trx_id_index        = 1;
   cat.data_index          = 2;
   cat.auths               = {{1, 4, 7}, {5, 4, 8}};
   cat.account_ram_deltas  = {{1, -10}};

   block_serializer s;
   s.write_compressed_action_trace(cat);
   const std::vector<uint8_t> expected = {0x1a, 0x32, 0x05, 0x05, 0xac, 0x02, 0x01, 0x01, 0x02, 0x02, 0x01,
                                          0x03, 0x01, 0x02, 0x01, 0x04, 0x07, 0x05, 0x04, 0x08, 0x13};
   std::vector<uint8_t> got(s.data().begin(), s.data().end());
   BOOST_TEST(got == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()
