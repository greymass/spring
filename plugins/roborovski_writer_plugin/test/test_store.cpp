#include <boost/test/unit_test.hpp>

#include <eosio/roborovski_writer_plugin/fault.hpp>
#include <eosio/roborovski_writer_plugin/store.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

using namespace eosio::roborovski_writer;
namespace fs = std::filesystem;

namespace {

fc::logger store_log;

block_id_type make_id(uint32_t num) {
   block_id_type id;
   std::memset(id.data(), 0x33, 32);
   std::memcpy(id.data(), &num, sizeof(num));
   return id;
}

prepared_block make_block(uint32_t num) {
   std::vector<char> blob(40 + num % 9, static_cast<char>(num));
   std::memcpy(blob.data(), &num, 4);
   const auto id = make_id(num);
   std::memcpy(blob.data() + 8, id.data(), 32);

   prepared_block b;
   b.block_num = num;
   b.id        = id;
   b.parent    = make_id(num - 1);
   b.glob_min  = uint64_t(num) * 10;
   b.glob_max  = uint64_t(num) * 10 + 9;
   b.raw_size  = blob.size();
   b.compressed.resize(ZSTD_compressBound(blob.size()));
   b.compressed.resize(ZSTD_compress(b.compressed.data(), b.compressed.size(), blob.data(), blob.size(), 3));
   if (num % 5 == 0)
      b.abis.push_back({num, uint64_t(num), "{\"version\":\"eosio::abi/1.1\",\"n\":" + std::to_string(num) + "}"});
   return b;
}

void append_range(store& s, uint32_t from, uint32_t to) {
   for (uint32_t n = from; n <= to; ++n)
      s.append(make_block(n));
}

std::vector<char> bytes_of(const fs::path& p) {
   std::ifstream f(p, std::ios::binary);
   return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::vector<char>> history_files(const fs::path& core) {
   std::map<std::string, std::vector<char>> out;
   for (const auto& e : fs::recursive_directory_iterator(core)) {
      const auto name = e.path().filename().string();
      if (e.is_regular_file() &&
          (name == "data.log" || name == "blocks.index" || name == "abis.dat" || name == "abis.index"))
         out[fs::relative(e.path(), core).string()] = bytes_of(e.path());
   }
   return out;
}

struct observer_guard {
   explicit observer_guard(std::function<void(const char*, uint32_t)> f) { set_fault_observer(std::move(f)); }
   ~observer_guard() { set_fault_observer({}); }
};

std::string digest_line(const fs::path& root, const fs::path& file, bool mask) {
   auto       b      = bytes_of(file);
   const auto name   = file.filename().string();
   bool       masked = false;
   if (mask && (name == MANIFEST_NAME || name == PUBLISHED_NAME) && b.size() == MANIFEST_SIZE) {
      std::fill(b.begin() + 16, b.begin() + 48, 0);
      std::fill(b.begin() + 124, b.end(), 0);
      masked = true;
   } else if (mask && name == BINDING_NAME && b.size() == BINDING_SIZE) {
      std::fill(b.begin() + 48, b.begin() + 80, 0);
      std::fill(b.begin() + 92, b.end(), 0);
      masked = true;
   }
   return fc::sha256::hash(b.data(), b.size()).str() + " " + std::to_string(fs::file_size(file)) + " " +
          (masked ? "1" : "0") + " " + fs::relative(file, root).generic_string();
}

std::vector<std::string> digests(const fs::path& root, bool mask) {
   std::vector<std::string> lines;
   for (const auto& e : fs::recursive_directory_iterator(root)) {
      if (e.is_regular_file())
         lines.push_back(digest_line(root, e.path(), mask));
   }
   std::sort(lines.begin(), lines.end());
   return lines;
}

std::vector<std::string> expected_digests(const fs::path& file) {
   std::vector<std::string> lines;
   std::ifstream            f(file);
   for (std::string line; std::getline(f, line);) {
      if (!line.empty())
         lines.push_back(line);
   }
   return lines;
}

} // namespace

BOOST_AUTO_TEST_SUITE(store_tests)

BOOST_AUTO_TEST_CASE(manifest_and_binding_round_trip) {
   manifest m;
   m.core_uuid.fill(1);
   m.abi_uuid.fill(2);
   m.epoch           = 3;
   m.head            = 12345;
   m.head_id.fill(4);
   m.slice_data_end  = 99999;
   m.slice_index_len = 77;
   m.abi_index_len   = 5;
   m.abi_data_end    = 600;
   m.abi_floor       = 11;
   auto bytes        = m.serialize();
   BOOST_TEST(bytes.size() == MANIFEST_SIZE);
   BOOST_TEST(std::string(bytes.data(), 7) == "RBMANIF");
   BOOST_TEST(get_le(bytes.data() + 56, 4) == 12345u);
   BOOST_TEST(get_le(bytes.data() + 108, 4) == 11u);
   BOOST_TEST((manifest::parse(bytes) == m));
   bytes[70] ^= 1;
   BOOST_CHECK_THROW(manifest::parse(bytes), data_corruption_exception);

   binding b;
   b.role = binding_role::abi;
   b.chain_id.fill(9);
   b.core_uuid.fill(1);
   b.abi_uuid.fill(2);
   b.slice_span = BLOCKS_PER_SLICE;
   auto bb      = b.serialize();
   BOOST_TEST(bb.size() == BINDING_SIZE);
   auto back = binding::parse(bb);
   BOOST_TEST((back.role == binding_role::abi));
   BOOST_TEST((back.chain_id == b.chain_id));
   BOOST_TEST(back.slice_span == BLOCKS_PER_SLICE);

   const char* check = "123456789";
   BOOST_TEST(crc32c(check, 9) == 0xE3069283u);
}

BOOST_AUTO_TEST_CASE(fresh_store_is_bound_and_empty) {
   fc::temp_directory tmp;
   const fs::path     core = tmp.path() / "core";
   hash_bytes         chain;
   chain.fill(0xab);
   {
      store s(core, core / "abis", chain, store_log);
      BOOST_TEST(s.opened().adopted);
      BOOST_TEST(s.committed().head == 0u);
      BOOST_TEST(s.committed().epoch == 1u);
   }
   auto cb = binding::read(core);
   auto ab = binding::read(core / "abis");
   BOOST_REQUIRE(cb);
   BOOST_REQUIRE(ab);
   BOOST_TEST((cb->chain_id == chain));
   check_binding_pair(*cb, *ab);
   BOOST_TEST(fs::exists(core / LOCK_NAME));
   BOOST_TEST(fs::exists(core / "abis" / LOCK_NAME));
   BOOST_TEST((bytes_of(published_path(core)) == bytes_of(manifest_path(core))));

   hash_bytes other;
   other.fill(0xcd);
   BOOST_CHECK_THROW(store(core, core / "abis", other, store_log), binding_exception);
}

BOOST_AUTO_TEST_CASE(commit_follows_the_protocol_order) {
   fc::temp_directory tmp;
   const fs::path     core = tmp.path() / "core";
   store              s(core, core / "abis", {}, store_log);
   append_range(s, 2, 40);
   BOOST_TEST(s.commit());
   BOOST_TEST(s.committed().head == 40u);

   const uint64_t committed_end = s.committed().slice_data_end;
   append_range(s, 41, 60);
   std::vector<std::string> seen;
   {
      observer_guard g([&](const char* point, uint32_t) {
         seen.push_back(point);
         const auto m = manifest::read(core);
         BOOST_REQUIRE(m);
         const auto        pv = manifest::parse(bytes_of(published_path(core)));
         const std::string p  = point;
         if (p == "abi_synced" || p == "core_synced") {
            BOOST_TEST(m->head == 40u);
         } else {
            BOOST_TEST(m->head == 60u);
         }
         BOOST_TEST(pv.head == (p == "published_renamed" ? 60u : 40u));
         if (p == "core_synced") {
            const auto data = core / slice_dir_name(0) / "data.log";
            BOOST_TEST(fs::file_size(data) > committed_end);
            BOOST_TEST(get_le(bytes_of(core / slice_dir_name(0) / "blocks.index").data(), 4) == 59u);
         }
      });
      BOOST_TEST(s.commit());
   }
   const std::vector<std::string> want = {"abi_synced", "core_synced", "manifest_renamed", "manifest_dir_synced",
                                          "published_renamed"};
   BOOST_TEST(seen == want, boost::test_tools::per_element());

   const auto m = s.committed();
   BOOST_TEST(m.head == 60u);
   BOOST_TEST(m.slice_index_len == 59u);
   BOOST_TEST(m.slice_data_end == fs::file_size(core / slice_dir_name(0) / "data.log"));
   BOOST_TEST(m.abi_index_len == 12u);
   BOOST_TEST(m.abi_data_end == fs::file_size(core / "abis" / "abis.dat"));
   BOOST_TEST(std::memcmp(m.head_id.data(), make_id(60).data(), 32) == 0);
   BOOST_TEST((manifest::read(core) == m));
   BOOST_TEST((bytes_of(published_path(core)) == bytes_of(manifest_path(core))));
   BOOST_TEST(!s.commit());
}

BOOST_AUTO_TEST_CASE(restart_truncates_to_the_manifest_and_matches_an_uninterrupted_run) {
   fc::temp_directory tmp;
   const fs::path     ref  = tmp.path() / "ref";
   const fs::path     core = tmp.path() / "core";
   {
      store s(ref, ref / "abis", {}, store_log);
      append_range(s, 9990, 10020);
      s.commit();
   }
   {
      store s(core, core / "abis", {}, store_log);
      append_range(s, 9990, 9997);
      s.commit();
      append_range(s, 9998, 10012);
   }
   BOOST_TEST(fs::exists(core / slice_dir_name(1)));
   {
      store s(core, core / "abis", {}, store_log);
      BOOST_TEST(!s.opened().adopted);
      BOOST_TEST(s.opened().core.changed());
      BOOST_TEST(s.opened().core.finalized_reset);
      BOOST_TEST(s.opened().core.removed_slices.size() == 1u);
      BOOST_TEST(s.opened().abi_cut);
      BOOST_TEST(s.written_head() == 9997u);
      BOOST_TEST(!fs::exists(core / slice_dir_name(1)));
      append_range(s, 9998, 10020);
      s.commit();
   }
   const auto got  = history_files(core);
   const auto want = history_files(ref);
   BOOST_REQUIRE(got.size() == want.size());
   for (const auto& [name, bytes] : want) {
      BOOST_TEST_CONTEXT(name) { BOOST_TEST((got.at(name) == bytes)); }
   }
   BOOST_TEST(std::memcmp(read_stored_block_id(core, 10001).data(), make_id(10001).data(), 32) == 0);
   BOOST_TEST(std::memcmp(read_stored_block_id(core, 9990).data(), make_id(9990).data(), 32) == 0);
}

BOOST_AUTO_TEST_CASE(a_second_writer_is_refused_before_it_changes_anything) {
   fc::temp_directory tmp;
   const fs::path     core = tmp.path() / "core";
   const fs::path     abis = tmp.path() / "abis";
   store              first(core, abis, {}, store_log);
   append_range(first, 2, 10);
   first.commit();
   const auto before = bytes_of(manifest_path(core));
   BOOST_CHECK_THROW(store(core, tmp.path() / "other", {}, store_log), lock_held_exception);
   BOOST_CHECK_THROW(store(tmp.path() / "other-core", abis, {}, store_log), lock_held_exception);
   BOOST_TEST((bytes_of(manifest_path(core)) == before));
   append_range(first, 11, 12);
   BOOST_TEST(first.commit());
}

BOOST_AUTO_TEST_CASE(roots_of_different_stores_are_refused) {
   fc::temp_directory tmp;
   { store a(tmp.path() / "a", tmp.path() / "a-abis", {}, store_log); }
   { store b(tmp.path() / "b", tmp.path() / "b-abis", {}, store_log); }
   BOOST_CHECK_THROW(store(tmp.path() / "a", tmp.path() / "b-abis", {}, store_log), binding_exception);
   BOOST_CHECK_THROW(store(tmp.path() / "a", tmp.path() / "a", {}, store_log), layout_exception);
}

BOOST_AUTO_TEST_CASE(abi_floor_is_committed) {
   fc::temp_directory tmp;
   const fs::path     core = tmp.path() / "core";
   {
      store s(core, core / "abis", {}, store_log);
      s.abis().write({500, 42, "{}"});
      s.set_abi_floor(500);
      BOOST_TEST(s.commit());
   }
   store s(core, core / "abis", {}, store_log);
   BOOST_TEST(s.committed().abi_floor == 500u);
   BOOST_TEST(s.committed().abi_index_len == 1u);
   BOOST_TEST(s.committed().head == 0u);
}

BOOST_AUTO_TEST_CASE(shared_crashed_store_fixtures) {
   const fs::path root = fs::path(ROBOROVSKI_FIXTURE_DIR) / "recovery";
   BOOST_REQUIRE(fs::is_directory(root));
   size_t cases = 0;
   for (const auto& entry : fs::directory_iterator(root)) {
      if (!entry.is_directory())
         continue;
      ++cases;
      const auto name = entry.path().filename().string();
      BOOST_TEST_CONTEXT("fixture " << name) {
         fc::temp_directory tmp;
         const fs::path     core = tmp.path() / "core";
         fs::copy(entry.path() / "before" / "core", core, fs::copy_options::recursive);

         const auto  expect  = fc::json::from_file(entry.path() / "expect.json").get_object();
         const auto  outcome = expect["outcome"].as_string();
         const bool  adopted = expect["adopted"].as_bool();
         const auto  head    = expect["head"].as_uint64();

         if (outcome == "recovered") {
            store s(core, core / "abis", {}, store_log);
            BOOST_TEST(s.opened().adopted == adopted);
            BOOST_TEST(s.committed().head == head);
            const auto got  = digests(core, adopted);
            const auto want = expected_digests(entry.path() / "after.sha256");
            BOOST_TEST(got == want, boost::test_tools::per_element());
         } else {
            BOOST_CHECK_THROW(store(core, core / "abis", {}, store_log), writer_exception);
            const auto got  = digests(core, false);
            const auto want = expected_digests(entry.path() / "after.sha256");
            BOOST_TEST(got == want, boost::test_tools::per_element());
         }
      }
   }
   BOOST_TEST(cases == 6u);
}

BOOST_AUTO_TEST_CASE(fault_specs_parse) {
   auto spec = parse_fault_spec("manifest_renamed:pause:120");
   BOOST_REQUIRE(spec);
   BOOST_TEST(spec->point == "manifest_renamed");
   BOOST_TEST(spec->pause);
   BOOST_TEST(spec->min_block == 120u);
   spec = parse_fault_spec("buffer_synced:kill");
   BOOST_REQUIRE(spec);
   BOOST_TEST(!spec->pause);
   BOOST_TEST(!parse_fault_spec(""));
   BOOST_CHECK_THROW(parse_fault_spec("nowhere:kill"), writer_exception);
   BOOST_CHECK_THROW(parse_fault_spec("core_synced:explode"), writer_exception);
   BOOST_CHECK_THROW(parse_fault_spec("core_synced"), writer_exception);
}

BOOST_AUTO_TEST_SUITE_END()
