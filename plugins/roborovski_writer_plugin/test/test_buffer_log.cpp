#include <boost/test/unit_test.hpp>

#include <eosio/roborovski_writer_plugin/buffer_log.hpp>
#include <eosio/roborovski_writer_plugin/recovery.hpp>

#include <fc/filesystem.hpp>

#include <cstring>
#include <fstream>

using namespace eosio::roborovski_writer;
namespace fs = std::filesystem;

namespace {

block_id_type make_id(uint32_t num, uint8_t branch) {
   block_id_type id;
   std::memset(id.data(), branch, 32);
   std::memcpy(id.data(), &num, sizeof(num));
   return id;
}

prepared_block make_block(uint32_t num, uint8_t branch = 1) {
   prepared_block b;
   b.block_num    = num;
   b.id           = make_id(num, branch);
   b.parent       = make_id(num - 1, branch);
   b.glob_min     = uint64_t(num) * 10;
   b.glob_max     = uint64_t(num) * 10 + 4;
   b.action_count = 5;
   b.raw_size     = 100 + num % 7;
   b.compressed   = std::vector<char>(20 + num % 13, static_cast<char>(num));
   if (num % 4 == 0)
      b.abis.push_back({num, uint64_t(num) * 3, "{\"version\":\"eosio::abi/1.1\"}"});
   return b;
}

uuid_type uuid_of(uint8_t v) {
   uuid_type u;
   u.fill(v);
   return u;
}

void check_same(const prepared_block& a, const prepared_block& b) {
   BOOST_TEST(a.block_num == b.block_num);
   BOOST_TEST(a.id == b.id);
   BOOST_TEST(a.parent == b.parent);
   BOOST_TEST(a.glob_min == b.glob_min);
   BOOST_TEST(a.glob_max == b.glob_max);
   BOOST_TEST(a.action_count == b.action_count);
   BOOST_TEST(a.raw_size == b.raw_size);
   BOOST_TEST(a.compressed == b.compressed);
   BOOST_REQUIRE(a.abis.size() == b.abis.size());
   for (size_t i = 0; i < a.abis.size(); ++i) {
      BOOST_TEST(a.abis[i].block_num == b.abis[i].block_num);
      BOOST_TEST(a.abis[i].contract == b.abis[i].contract);
      BOOST_TEST(a.abis[i].json == b.abis[i].json);
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(buffer_log_tests)

BOOST_AUTO_TEST_CASE(payload_round_trip) {
   auto b       = make_block(12);
   auto payload = serialize_payload(b);
   prepared_block back;
   back.block_num = b.block_num;
   back.id        = b.id;
   back.parent    = b.parent;
   parse_payload(back, payload.data(), payload.size());
   check_same(b, back);
   payload.push_back(0);
   BOOST_CHECK_THROW(parse_payload(back, payload.data(), payload.size()), data_corruption_exception);
   BOOST_CHECK_THROW(parse_payload(back, payload.data(), 10), data_corruption_exception);
}

BOOST_AUTO_TEST_CASE(entries_survive_reopen_and_forks_share_a_height) {
   fc::temp_directory tmp;
   const fs::path     dir = tmp.path() / "buffer";
   {
      buffer_log log(dir, uuid_of(7), 1);
      for (uint32_t n = 9998; n <= 10003; ++n)
         log.append(make_block(n), true);
      log.append(make_block(10003, 2), true);
      BOOST_TEST(log.pending_heights() == 6u);
   }
   BOOST_TEST(fs::exists(buffer_log::segment_path(dir, 0)));
   BOOST_TEST(fs::exists(buffer_log::segment_path(dir, 1)));

   buffer_log log(dir, uuid_of(7), 1);
   BOOST_TEST(log.scan_report().entries == 7u);
   BOOST_TEST(log.scan_report().bytes_truncated == 0u);
   for (uint32_t n = 9998; n <= 10003; ++n) {
      auto ref = log.find(n, make_id(n, 1));
      BOOST_REQUIRE(ref);
      check_same(log.read(n, *ref), make_block(n));
   }
   auto fork = log.find(10003, make_id(10003, 2));
   BOOST_REQUIRE(fork);
   check_same(log.read(10003, *fork), make_block(10003, 2));
   BOOST_TEST(!log.find(10004, make_id(10004, 1)));
   BOOST_TEST(log.highest() == 10003u);
}

BOOST_AUTO_TEST_CASE(forward_scan_truncates_at_the_first_bad_entry) {
   fc::temp_directory tmp;
   const fs::path     dir = tmp.path() / "buffer";
   uint64_t           good_end = 0;
   {
      buffer_log log(dir, uuid_of(7), 1);
      for (uint32_t n = 2; n <= 6; ++n)
         log.append(make_block(n), false);
      good_end = fs::file_size(buffer_log::segment_path(dir, 0));
      log.append(make_block(7), false);
      log.append(make_block(8), false);
   }
   const auto path = buffer_log::segment_path(dir, 0);
   {
      std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(good_end + BUFFER_ENTRY_HEADER_SIZE + 3));
      char c = 0x5a;
      f.write(&c, 1);
   }
   const auto size = fs::file_size(path);
   {
      buffer_log log(dir, uuid_of(7), 1);
      BOOST_TEST(log.scan_report().entries == 5u);
      BOOST_TEST(log.scan_report().bytes_truncated == size - good_end);
      BOOST_TEST(fs::file_size(path) == good_end);
      BOOST_TEST(!log.find(7, make_id(7, 1)));
      log.append(make_block(7), true);
   }
   fs::resize_file(path, fs::file_size(path) - 1);
   buffer_log log(dir, uuid_of(7), 1);
   BOOST_TEST(log.scan_report().entries == 5u);
   BOOST_TEST(fs::file_size(path) == good_end);
}

BOOST_AUTO_TEST_CASE(torn_segment_header_is_discarded) {
   fc::temp_directory tmp;
   const fs::path     dir = tmp.path() / "buffer";
   fs::create_directories(dir);
   std::ofstream(buffer_log::segment_path(dir, 3), std::ios::binary).write("RBBUF", 5);
   buffer_log log(dir, uuid_of(7), 1);
   BOOST_TEST(log.scan_report().segments_removed == 1u);
   BOOST_TEST(!fs::exists(buffer_log::segment_path(dir, 3)));
}

BOOST_AUTO_TEST_CASE(older_epochs_are_dropped_and_other_stores_refused) {
   fc::temp_directory tmp;
   const fs::path     dir = tmp.path() / "buffer";
   {
      buffer_log log(dir, uuid_of(7), 1);
      log.append(make_block(20), true);
   }
   BOOST_CHECK_THROW(buffer_log(dir, uuid_of(8), 1), binding_exception);
   {
      buffer_log log(dir, uuid_of(7), 2);
      BOOST_TEST(log.scan_report().segments_removed == 1u);
      BOOST_TEST(!log.find(20, make_id(20, 1)));
      log.append(make_block(20), true);
   }
   BOOST_CHECK_THROW(buffer_log(dir, uuid_of(7), 1), data_corruption_exception);
}

BOOST_AUTO_TEST_CASE(discard_removes_whole_committed_segments) {
   fc::temp_directory tmp;
   const fs::path     dir = tmp.path() / "buffer";
   buffer_log         log(dir, uuid_of(7), 1);
   for (uint32_t n = 9995; n <= 10002; ++n)
      log.append(make_block(n), false);
   BOOST_TEST(log.discard_through(9999) == 0u);
   BOOST_TEST(log.pending_heights() == 3u);
   BOOST_TEST(fs::exists(buffer_log::segment_path(dir, 0)));
   BOOST_TEST(log.discard_through(10000) == 1u);
   BOOST_TEST(!fs::exists(buffer_log::segment_path(dir, 0)));
   BOOST_TEST(log.pending_heights() == 2u);
   auto ref = log.find(10002, make_id(10002, 1));
   BOOST_REQUIRE(ref);
   check_same(log.read(10002, *ref), make_block(10002));
}

BOOST_AUTO_TEST_SUITE_END()
