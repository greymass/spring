#include <boost/test/unit_test.hpp>

#include <eosio/chain/types.hpp>
#include <eosio/resource_feed_plugin/serializer.hpp>
#include <eosio/resource_feed_plugin/types.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>

using namespace eosio::resource_feed;

namespace {

std::vector<char> to_bytes(std::initializer_list<uint8_t> bytes) {
   std::vector<char> out;
   out.reserve(bytes.size());
   for (uint8_t b : bytes)
      out.push_back(static_cast<char>(b));
   return out;
}

std::string to_hex(const std::vector<char>& data) {
   std::ostringstream oss;
   oss << std::hex << std::setfill('0');
   for (char c : data)
      oss << std::setw(2) << (static_cast<unsigned int>(static_cast<uint8_t>(c)));
   return oss.str();
}

uint32_t read_u32_le(const std::vector<char>& d, size_t off) {
   return static_cast<uint32_t>(static_cast<uint8_t>(d[off])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3])) << 24);
}

uint64_t read_u64_le(const std::vector<char>& d, size_t off) {
   uint64_t v = 0;
   for (int i = 7; i >= 0; --i)
      v = (v << 8) | static_cast<uint8_t>(d[off + i]);
   return v;
}

} // namespace

BOOST_AUTO_TEST_SUITE(serializer_tests)

BOOST_AUTO_TEST_CASE(golden_snapshot_end) {
   serializer ser;
   ser.snapshot_end(5);
   auto expected = to_bytes({0x04, 0x00, 0x00, 0x00, 0x07, 0x05, 0x00, 0x00, 0x00});
   BOOST_CHECK_EQUAL_COLLECTIONS(ser.data().begin(), ser.data().end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(golden_block) {
   serializer ser;
   std::array<char, 32> id_bytes{};
   for (int i = 0; i < 32; ++i)
      id_bytes[i] = static_cast<char>(i + 1);
   eosio::chain::block_id_type id;
   std::memcpy(id.data(), id_bytes.data(), 32);

   ser.block(100, id, 1000);

   std::vector<uint8_t> expected_bytes = {0x28, 0x00, 0x00, 0x00, 0x08, 0x64, 0x00, 0x00, 0x00};
   for (int i = 1; i <= 32; ++i)
      expected_bytes.push_back(static_cast<uint8_t>(i));
   expected_bytes.push_back(0xe8);
   expected_bytes.push_back(0x03);
   expected_bytes.push_back(0x00);
   expected_bytes.push_back(0x00);

   std::vector<char> expected;
   expected.reserve(expected_bytes.size());
   for (uint8_t b : expected_bytes)
      expected.push_back(static_cast<char>(b));

   BOOST_CHECK_EQUAL_COLLECTIONS(ser.data().begin(), ser.data().end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(accounts_single_record_layout) {
   serializer ser;
   account_record rec{};
   rec.owner            = 0x0102030405060708ULL;
   rec.net_weight       = 11;
   rec.cpu_weight       = 22;
   rec.ram_bytes        = 33;
   rec.net_last_ordinal = 44;
   rec.net_value_ex     = 55;
   rec.net_consumed     = 66;
   rec.cpu_last_ordinal = 77;
   rec.cpu_value_ex     = 88;
   rec.cpu_consumed     = 99;
   rec.ram_usage        = 111;

   ser.accounts(42, {rec});

   const auto& d = ser.data();
   uint32_t     payload_len = read_u32_le(d, 0);
   BOOST_CHECK_EQUAL(payload_len, 8 + 80);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(d[4]), static_cast<uint8_t>(frame_type::accounts));
   BOOST_CHECK_EQUAL(read_u32_le(d, 5), 42u);
   BOOST_CHECK_EQUAL(read_u32_le(d, 9), 1u);

   size_t rec_off = 13;
   BOOST_CHECK_EQUAL(d.size(), rec_off + 80);
   BOOST_CHECK_EQUAL(read_u64_le(d, rec_off + 0), rec.owner);
   BOOST_CHECK_EQUAL(static_cast<int64_t>(read_u64_le(d, rec_off + 8)), rec.net_weight);
   BOOST_CHECK_EQUAL(static_cast<int64_t>(read_u64_le(d, rec_off + 16)), rec.cpu_weight);
   BOOST_CHECK_EQUAL(static_cast<int64_t>(read_u64_le(d, rec_off + 24)), rec.ram_bytes);
   BOOST_CHECK_EQUAL(read_u32_le(d, rec_off + 32), rec.net_last_ordinal);
   BOOST_CHECK_EQUAL(read_u64_le(d, rec_off + 36), rec.net_value_ex);
   BOOST_CHECK_EQUAL(read_u64_le(d, rec_off + 44), rec.net_consumed);
   BOOST_CHECK_EQUAL(read_u32_le(d, rec_off + 52), rec.cpu_last_ordinal);
   BOOST_CHECK_EQUAL(read_u64_le(d, rec_off + 56), rec.cpu_value_ex);
   BOOST_CHECK_EQUAL(read_u64_le(d, rec_off + 64), rec.cpu_consumed);
   BOOST_CHECK_EQUAL(read_u64_le(d, rec_off + 72), rec.ram_usage);
}

BOOST_AUTO_TEST_CASE(payload_len_hello) {
   serializer ser;
   auto cid = eosio::chain::chain_id_type::empty_chain_id();
   ser.hello(cid, 12345);
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 4u + 2u + 32u + 4u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::hello));
   BOOST_CHECK_EQUAL(ser.data().size(), 5 + 4 + 2 + 32 + 4);
}

BOOST_AUTO_TEST_CASE(payload_len_snapshot_begin) {
   serializer ser;
   ser.snapshot_begin(7);
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 4u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::snapshot_begin));
}

BOOST_AUTO_TEST_CASE(payload_len_accounts_removed) {
   serializer ser;
   ser.accounts_removed(9, {1, 2, 3});
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 8u + 3u * 8u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::accounts_removed));
}

BOOST_AUTO_TEST_CASE(payload_len_globals) {
   serializer ser;
   globals_record g{1, 2, 3, 4, 5};
   ser.globals(3, g);
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 4u + 5u * 8u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::globals));
}

BOOST_AUTO_TEST_CASE(payload_len_config) {
   serializer ser;
   config_record c{172800, 172800};
   ser.config(3, c);
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 12u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::config));
}

BOOST_AUTO_TEST_CASE(payload_len_snapshot_end) {
   serializer ser;
   ser.snapshot_end(11);
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 4u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::snapshot_end));
}

BOOST_AUTO_TEST_CASE(payload_len_block) {
   serializer ser;
   eosio::chain::block_id_type id;
   ser.block(2, id, 55);
   BOOST_CHECK_EQUAL(read_u32_le(ser.data(), 0), 4u + 32u + 4u);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), static_cast<uint8_t>(frame_type::block));
}

BOOST_AUTO_TEST_CASE(clear_resets_buffer) {
   serializer ser;
   ser.snapshot_end(1);
   BOOST_CHECK(!ser.data().empty());
   ser.clear();
   BOOST_CHECK(ser.data().empty());
}

BOOST_AUTO_TEST_CASE(dump_golden_vectors) {
   serializer ser;

   auto                        cid = eosio::chain::chain_id_type::empty_chain_id();
   std::array<char, 32>        cid_bytes{};
   for (int i = 0; i < 32; ++i)
      cid_bytes[i] = static_cast<char>(0xA0 + i);
   std::memcpy(cid.data(), cid_bytes.data(), 32);

   serializer hello_ser;
   hello_ser.hello(cid, 4242);

   serializer snap_begin_ser;
   snap_begin_ser.snapshot_begin(10);

   account_record r1{};
   r1.owner            = 0x1111111111111111ULL;
   r1.net_weight       = 1000;
   r1.cpu_weight       = 2000;
   r1.ram_bytes        = 3000;
   r1.net_last_ordinal = 100;
   r1.net_value_ex     = 111111;
   r1.net_consumed     = 222222;
   r1.cpu_last_ordinal = 200;
   r1.cpu_value_ex     = 333333;
   r1.cpu_consumed     = 444444;
   r1.ram_usage        = 555555;

   account_record r2{};
   r2.owner            = 0x2222222222222222ULL;
   r2.net_weight       = -500;
   r2.cpu_weight       = -600;
   r2.ram_bytes        = 4096;
   r2.net_last_ordinal = 300;
   r2.net_value_ex     = 666666;
   r2.net_consumed     = 777777;
   r2.cpu_last_ordinal = 400;
   r2.cpu_value_ex     = 888888;
   r2.cpu_consumed     = 999999;
   r2.ram_usage        = 123456;

   serializer accounts_ser;
   accounts_ser.accounts(11, {r1, r2});

   serializer accounts_removed_ser;
   accounts_removed_ser.accounts_removed(12, {0x3333333333333333ULL, 0x4444444444444444ULL});

   globals_record g{5000000, 6000000, 7000000, 8000000, 9000000};
   serializer     globals_ser;
   globals_ser.globals(13, g);

   config_record c{172800, 172800};
   serializer    config_ser;
   config_ser.config(14, c);

   serializer snapshot_end_ser;
   snapshot_end_ser.snapshot_end(15);

   eosio::chain::block_id_type block_id;
   std::array<char, 32>        block_id_bytes{};
   for (int i = 0; i < 32; ++i)
      block_id_bytes[i] = static_cast<char>(i + 1);
   std::memcpy(block_id.data(), block_id_bytes.data(), 32);

   serializer block_ser;
   block_ser.block(100, block_id, 1000);

   struct named_frame {
      std::string        name;
      const std::vector<char>* bytes;
   };

   std::vector<named_frame> frames = {
       {"hello", &hello_ser.data()},
       {"snapshot_begin", &snap_begin_ser.data()},
       {"accounts", &accounts_ser.data()},
       {"accounts_removed", &accounts_removed_ser.data()},
       {"globals", &globals_ser.data()},
       {"config", &config_ser.data()},
       {"snapshot_end", &snapshot_end_ser.data()},
       {"block", &block_ser.data()},
   };

   std::ostringstream json;
   json << "{\"frames\": [";
   for (size_t i = 0; i < frames.size(); ++i) {
      if (i != 0)
         json << ", ";
      json << "{\"name\": \"" << frames[i].name << "\", \"hex\": \"" << to_hex(*frames[i].bytes) << "\"}";
   }
   json << "]}";

   std::ofstream out("resource_feed_vectors.json");
   BOOST_REQUIRE(out.is_open());
   out << json.str();
   out.close();

   for (const auto& f : frames) {
      const auto& d = *f.bytes;
      BOOST_REQUIRE_GE(d.size(), 5u);
      uint32_t payload_len = read_u32_le(d, 0);
      BOOST_CHECK_EQUAL(d.size(), 5u + payload_len);
   }

   BOOST_CHECK_EQUAL(read_u32_le(*frames[2].bytes, 9), 2u);
   BOOST_CHECK_EQUAL(read_u32_le(*frames[3].bytes, 9), 2u);
}

BOOST_AUTO_TEST_SUITE_END()
