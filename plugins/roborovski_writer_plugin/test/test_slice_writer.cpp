#include <boost/test/unit_test.hpp>

#include <eosio/roborovski_writer_plugin/abi_writer.hpp>
#include <eosio/roborovski_writer_plugin/slice_writer.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/filesystem.hpp>

#include <fstream>

using namespace eosio::roborovski_writer;
namespace fs = std::filesystem;

namespace {

fc::logger test_log;

std::vector<char> read_file(const fs::path& p) {
   std::ifstream f(p, std::ios::binary);
   return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string read_text(const fs::path& p) {
   auto b = read_file(p);
   return {b.begin(), b.end()};
}

uint64_t le(const std::vector<char>& b, size_t off, int n) {
   uint64_t v = 0;
   for (int i = 0; i < n; ++i)
      v |= static_cast<uint64_t>(static_cast<uint8_t>(b.at(off + i))) << (8 * i);
   return v;
}

built_block block(uint32_t num, uint64_t glob_min, uint64_t glob_max) {
   built_block b;
   b.block_num = num;
   b.glob_min  = glob_min;
   b.glob_max  = glob_max;
   b.blob      = std::vector<char>(40, static_cast<char>(num & 0xff));
   return b;
}

void write_range(slice_writer& w, uint32_t from, uint32_t to) {
   for (uint32_t n = from; n <= to; ++n)
      w.append(block(n, uint64_t(n) * 10, uint64_t(n) * 10 + 2));
   w.flush();
}

} // namespace

BOOST_AUTO_TEST_SUITE(slice_writer_tests)

BOOST_AUTO_TEST_CASE(header_and_slice_names) {
   auto h = slice_header_bytes(10001, 20000, false);
   const std::vector<uint8_t> expected = {'H', 'W', 'S', 'L', 'I', 'C', 'E', 1,   0x10, 0x27, 0, 0, 0x11, 0x27, 0, 0,
                                          0x20, 0x4e, 0,  0,   0,   0,   0,   0, 0,    0,    0, 0, 0,    0,    0, 0};
   BOOST_TEST(std::vector<uint8_t>(h.begin(), h.end()) == expected, boost::test_tools::per_element());
   BOOST_TEST(slice_dir_name(1) == "history_0000010001-0000020000");

   slice_info si{3, 30001, 30500, 40000, 10000, false, 7, 9};
   BOOST_TEST(slices_json({si}) == "[{\"slice_num\":3,\"start_block\":30001,\"end_block\":30500,\"max_block\":40000,"
                                   "\"blocks_per_slice\":10000,\"finalized\":false,\"glob_min\":7,\"glob_max\":9}]");
   BOOST_TEST(slices_json({}) == "[]");
}

BOOST_AUTO_TEST_CASE(writes_rotates_and_resumes) {
   fc::temp_directory tmp;
   const fs::path     dir = tmp.path();
   {
      slice_writer w(dir, test_log);
      BOOST_TEST(w.empty());
      write_range(w, 2, 10003);
      w.close();
   }

   const auto first = dir / "history_0000000001-0000010000";
   const auto data  = read_file(first / "data.log");
   BOOST_TEST(std::string(data.begin(), data.begin() + 7) == "HWSLICE");
   BOOST_TEST(data[20] == 1);
   BOOST_TEST(read_text(first / "sha256.txt") == fc::sha256::hash(data.data(), data.size()).str() + "\n");

   const auto index = read_file(first / "blocks.index");
   BOOST_REQUIRE(index.size() == 4 + 9999 * 32u);
   BOOST_TEST(le(index, 0, 4) == 9999u);
   BOOST_TEST(le(index, 4, 4) == 2u);
   BOOST_TEST(le(index, 8, 8) == 32u);
   const uint64_t size0 = le(index, 16, 4);
   BOOST_TEST(le(index, 20, 8) == 20u);
   BOOST_TEST(le(index, 28, 8) == 22u);
   BOOST_TEST(le(data, 32, 4) == size0);
   BOOST_TEST(le(index, 4 + 32 + 4, 8) == 32 + 8 + size0);

   const auto second = dir / "history_0000010001-0000020000";
   const auto data2  = read_file(second / "data.log");
   BOOST_TEST(data2[20] == 0);
   BOOST_TEST(!fs::exists(second / "sha256.txt"));
   BOOST_TEST(le(read_file(second / "blocks.index"), 0, 4) == 3u);

   BOOST_TEST(read_text(dir / "slices.json") ==
              "[{\"slice_num\":0,\"start_block\":1,\"end_block\":10000,\"max_block\":10000,\"blocks_per_slice\":10000,"
              "\"finalized\":true,\"glob_min\":20,\"glob_max\":100002},"
              "{\"slice_num\":1,\"start_block\":10001,\"end_block\":10003,\"max_block\":20000,\"blocks_per_slice\":10000,"
              "\"finalized\":false,\"glob_min\":100010,\"glob_max\":100032}]");

   {
      slice_writer w(dir, test_log);
      BOOST_TEST(w.get_last_block() == 10003u);
      BOOST_CHECK_THROW(w.append(block(10005, 1, 1)), data_corruption_exception);
      write_range(w, 10004, 10004);
      w.close();
   }
   BOOST_TEST(le(read_file(second / "blocks.index"), 0, 4) == 4u);
   BOOST_TEST(read_file(second / "data.log").size() == data2.size() + 8 + le(read_file(second / "blocks.index"), 4 + 3 * 32 + 12, 4));
}

BOOST_AUTO_TEST_CASE(snapshot_start_inside_a_slice_finalizes) {
   fc::temp_directory tmp;
   slice_writer       w(tmp.path(), test_log);
   write_range(w, 15000, 20001);
   w.close();
   auto data = read_file(tmp.path() / "history_0000010001-0000020000" / "data.log");
   BOOST_TEST(data[20] == 1);
}

BOOST_AUTO_TEST_CASE(rejects_other_slice_layouts) {
   {
      fc::temp_directory tmp;
      std::ofstream(tmp.path() / "slices.json")
          << "[{\"slice_num\":0,\"start_block\":1,\"end_block\":0,\"max_block\":5000,\"blocks_per_slice\":5000,"
             "\"finalized\":false,\"glob_min\":0,\"glob_max\":0}]";
      fs::create_directories(tmp.path() / "history_0000000001-0000005000");
      BOOST_CHECK_THROW(slice_writer(tmp.path(), test_log), layout_exception);
   }
   {
      fc::temp_directory tmp;
      fs::create_directories(tmp.path() / "history_0000000001-0000010000");
      BOOST_CHECK_THROW(slice_writer(tmp.path(), test_log), layout_exception);
   }
   {
      fc::temp_directory tmp;
      {
         slice_writer w(tmp.path(), test_log);
         write_range(w, 1, 5);
      }
      auto     path = tmp.path() / "history_0000000001-0000010000" / "data.log";
      auto     data = read_file(path);
      data[8]       = 0x20;
      std::ofstream(path, std::ios::binary | std::ios::trunc).write(data.data(), data.size());
      BOOST_CHECK_THROW(slice_writer(tmp.path(), test_log), layout_exception);
   }
}

BOOST_AUTO_TEST_CASE(abi_cache_layout) {
   fc::temp_directory tmp;
   {
      abi_writer w(tmp.path());
      BOOST_TEST(w.empty());
      w.write({5, 0x1122334455667788ull, "{}"});
      w.write({6, 1, ""});
   }
   abi_writer reopened(tmp.path());
   BOOST_TEST(reopened.entries() == 2u);

   auto dat = read_file(tmp.path() / "abis.dat");
   auto idx = read_file(tmp.path() / "abis.index");
   BOOST_REQUIRE(dat.size() == 16 + 2 + 16u);
   BOOST_REQUIRE(idx.size() == 40u);
   BOOST_TEST(le(dat, 0, 4) == 5u);
   BOOST_TEST(le(dat, 4, 8) == 0x1122334455667788ull);
   BOOST_TEST(le(dat, 12, 4) == 2u);
   BOOST_TEST(le(idx, 0, 4) == 5u);
   BOOST_TEST(le(idx, 4, 8) == 0x1122334455667788ull);
   BOOST_TEST(le(idx, 12, 8) == 0u);
   BOOST_TEST(le(idx, 20, 4) == 6u);
   BOOST_TEST(le(idx, 32, 8) == 18u);
   BOOST_TEST(le(dat, 30, 4) == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
