#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace eosio::roborovski_writer {

struct parsed_setabi {
   uint64_t    contract = 0;
   std::string json;
};

std::optional<std::string> abi_binary_to_json(const char* data, size_t len);

std::optional<parsed_setabi> parse_setabi(const char* data, size_t len);

std::string go_json_string(const std::string& s);

std::string name_to_string(uint64_t value);

} // namespace eosio::roborovski_writer
