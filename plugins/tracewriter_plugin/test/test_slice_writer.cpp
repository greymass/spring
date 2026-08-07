#include <boost/test/unit_test.hpp>

#include <eosio/tracewriter_plugin/slice_writer.hpp>
#include <eosio/tracewriter_plugin/types.hpp>

#include <fc/log/logger.hpp>

#include <filesystem>

using namespace eosio::tracewriter;

namespace {
   fc::logger test_log;

   struct test_fixture {
      std::filesystem::path temp_dir;

      test_fixture() {
         temp_dir = std::filesystem::temp_directory_path() / "tracewriter_test";
         std::filesystem::create_directories(temp_dir);
      }

      ~test_fixture() {
         std::error_code ec;
         std::filesystem::remove_all(temp_dir, ec);
      }
   };
}

BOOST_FIXTURE_TEST_SUITE(slice_writer_tests, test_fixture)

BOOST_AUTO_TEST_CASE(test_construction) {
   slice_writer writer(temp_dir, 10000, test_log);
   BOOST_CHECK_EQUAL(writer.get_last_written_block(), 0);
   BOOST_CHECK(writer.get_slice_infos().empty());
}

BOOST_AUTO_TEST_CASE(test_write_single_block) {
   slice_writer writer(temp_dir, 10000, test_log);

   std::vector<char> data = {'t', 'e', 's', 't', ' ', 'd', 'a', 't', 'a'};
   writer.write_block(1, 100, 200, data);

   BOOST_CHECK_EQUAL(writer.get_last_written_block(), 1);
   BOOST_CHECK(writer.has_block(1));
   BOOST_CHECK(!writer.has_block(2));
}

BOOST_AUTO_TEST_CASE(test_write_multiple_blocks) {
   slice_writer writer(temp_dir, 10000, test_log);

   std::vector<char> data = {'t', 'e', 's', 't'};
   writer.write_block(1, 100, 100, data);
   writer.write_block(2, 101, 101, data);
   writer.write_block(3, 102, 102, data);

   BOOST_CHECK_EQUAL(writer.get_last_written_block(), 3);
   BOOST_CHECK(writer.has_block(1));
   BOOST_CHECK(writer.has_block(2));
   BOOST_CHECK(writer.has_block(3));
}

BOOST_AUTO_TEST_CASE(test_get_last_block) {
   slice_writer writer(temp_dir, 10000, test_log);

   BOOST_CHECK_EQUAL(writer.get_last_block(), 0);

   std::vector<char> data = {'t', 'e', 's', 't'};
   writer.write_block(5, 100, 100, data);

   BOOST_CHECK_EQUAL(writer.get_last_block(), 5);
}

BOOST_AUTO_TEST_CASE(test_duplicate_block_rewrite) {
   slice_writer writer(temp_dir, 10000, test_log);

   std::vector<char> data1 = {'f', 'i', 'r', 's', 't'};
   std::vector<char> data2 = {'s', 'e', 'c', 'o', 'n', 'd'};

   writer.write_block(1, 100, 100, data1);
   writer.write_block(1, 100, 100, data2);

   BOOST_CHECK_EQUAL(writer.get_last_written_block(), 1);
   BOOST_CHECK(writer.has_block(1));
}

BOOST_AUTO_TEST_CASE(test_save_and_load_metadata) {
   {
      slice_writer writer(temp_dir, 10000, test_log);

      std::vector<char> data = {'t', 'e', 's', 't'};
      writer.write_block(1, 100, 100, data);
      writer.write_block(2, 101, 101, data);

      writer.save_metadata();
   }

   {
      slice_writer writer(temp_dir, 10000, test_log);

      BOOST_CHECK(writer.has_block(1));
      BOOST_CHECK(writer.has_block(2));
      BOOST_CHECK_EQUAL(writer.get_last_block(), 2);
   }
}

BOOST_AUTO_TEST_CASE(test_slice_boundary) {
   slice_writer writer(temp_dir, 100, test_log);

   std::vector<char> data = {'t', 'e', 's', 't'};

   writer.write_block(99, 100, 100, data);
   writer.write_block(100, 101, 101, data);
   writer.write_block(101, 102, 102, data);

   BOOST_CHECK_EQUAL(writer.get_last_written_block(), 101);
   BOOST_CHECK_EQUAL(writer.get_slice_infos().size(), 1);
}

BOOST_AUTO_TEST_CASE(test_compression) {
   slice_writer writer(temp_dir, 10000, test_log);

   std::vector<char> data(10000, 'x');
   writer.write_block(1, 100, 100, data);

   BOOST_CHECK(writer.get_bytes_compressed() > 0);
   BOOST_CHECK(writer.get_bytes_compressed() < data.size());
}

BOOST_AUTO_TEST_SUITE_END()
