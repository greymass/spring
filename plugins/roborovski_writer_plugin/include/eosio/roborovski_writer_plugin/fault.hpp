#pragma once
#include <fc/log/logger.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace eosio::roborovski_writer {

// ROBOROVSKI_WRITER_FAULT=<point>:<kill|pause>[:<block>] stops the writer at a persistence boundary for the fault gates.
struct fault_spec {
   std::string point;
   bool        pause     = false;
   uint32_t    min_block = 0;
};

std::optional<fault_spec> parse_fault_spec(const std::string& text);

void configure_fault_hook(const char* env_value, fc::logger& log);
void fault_point(const char* point, uint32_t block);
void set_fault_observer(std::function<void(const char* point, uint32_t block)> observer);

} // namespace eosio::roborovski_writer
