#include <boost/test/unit_test.hpp>

#include <eosio/roborovski_writer_plugin/abi_json.hpp>
#include <eosio/chain/name.hpp>

#include "abi_golden.hpp"

#include <string>

using namespace eosio::roborovski_writer;

namespace {

std::string from_hex(std::string_view h) {
   std::string out;
   for (size_t i = 0; i + 1 < h.size(); i += 2)
      out.push_back(static_cast<char>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
   return out;
}

std::string setabi_data(uint64_t contract, const std::string& abi) {
   std::string out;
   for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<char>(contract >> (8 * i)));
   uint64_t n = abi.size();
   while (n >= 0x80) {
      out.push_back(static_cast<char>((n & 0x7f) | 0x80));
      n >>= 7;
   }
   out.push_back(static_cast<char>(n));
   return out + abi;
}

} // namespace

BOOST_AUTO_TEST_SUITE(abi_json_tests)

BOOST_AUTO_TEST_CASE(matches_go_eosio_output) {
   for (const auto& c : abi_golden_cases) {
      BOOST_TEST_CONTEXT("case " << c.name) {
         const auto bin  = from_hex(c.abi_hex);
         const auto json = abi_binary_to_json(bin.data(), bin.size());
         if (c.json_hex.empty()) {
            BOOST_TEST(!json.has_value());
         } else {
            BOOST_REQUIRE(json.has_value());
            BOOST_TEST(*json == from_hex(c.json_hex));
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(parses_setabi_action_data) {
   const auto  minimal  = from_hex(abi_golden_cases[1].abi_hex);
   const auto  contract = eosio::chain::name("eosio.token").to_uint64_t();
   const auto  data     = setabi_data(contract, minimal);
   const auto  parsed   = parse_setabi(data.data(), data.size());
   BOOST_REQUIRE(parsed.has_value());
   BOOST_TEST(parsed->contract == contract);
   BOOST_TEST(parsed->json == from_hex(abi_golden_cases[1].json_hex));

   const auto cleared = setabi_data(contract, "");
   const auto empty   = parse_setabi(cleared.data(), cleared.size());
   BOOST_REQUIRE(empty.has_value());
   BOOST_TEST(empty->json.empty());

   BOOST_TEST(!parse_setabi(data.data(), 7).has_value());
   BOOST_TEST(!parse_setabi(data.data(), 8).has_value());
   BOOST_TEST(!parse_setabi(data.data(), data.size() - 1).has_value());

   const auto truncated = setabi_data(contract, from_hex(abi_golden_cases[3].abi_hex));
   BOOST_TEST(!parse_setabi(truncated.data(), truncated.size()).has_value());
}

BOOST_AUTO_TEST_CASE(names_render_like_the_chain) {
   for (const char* s : {"", "eosio", "eosio.token", "a", "zzzzzzzzzzzzj", "1.2.3", "accounts.x"}) {
      eosio::chain::name n(s);
      BOOST_TEST(name_to_string(n.to_uint64_t()) == n.to_string());
   }
}

BOOST_AUTO_TEST_SUITE_END()
