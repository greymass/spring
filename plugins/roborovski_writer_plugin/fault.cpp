#include <eosio/roborovski_writer_plugin/fault.hpp>
#include <eosio/roborovski_writer_plugin/types.hpp>

#include <csignal>
#include <set>

#include <unistd.h>

namespace eosio::roborovski_writer {

namespace {

const std::set<std::string> known_points = {
    "buffer_synced",  "abi_synced",   "core_synced",       "manifest_renamed",
    "manifest_dir_synced", "published_renamed", "segment_reclaimed", "slice_rotated", "abi_initialized",
};

std::optional<fault_spec>                         active;
fc::logger*                                       fault_log = nullptr;
std::function<void(const char*, uint32_t)>        observer;

} // namespace

std::optional<fault_spec> parse_fault_spec(const std::string& text) {
   if (text.empty())
      return std::nullopt;
   const auto first = text.find(':');
   if (first == std::string::npos)
      throw writer_exception("ROBOROVSKI_WRITER_FAULT must be <point>:<kill|pause>[:<block>]");
   fault_spec spec;
   spec.point          = text.substr(0, first);
   const auto second   = text.find(':', first + 1);
   const auto action   = text.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
   if (!known_points.count(spec.point))
      throw writer_exception("ROBOROVSKI_WRITER_FAULT names an unknown point: " + spec.point);
   if (action == "pause")
      spec.pause = true;
   else if (action != "kill")
      throw writer_exception("ROBOROVSKI_WRITER_FAULT action must be kill or pause, not " + action);
   if (second != std::string::npos)
      spec.min_block = static_cast<uint32_t>(std::stoul(text.substr(second + 1)));
   return spec;
}

void configure_fault_hook(const char* env_value, fc::logger& log) {
   fault_log = &log;
   active    = parse_fault_spec(env_value ? env_value : "");
   if (active)
      fc_wlog(log, "ROBOROVSKI_WRITER_FAULT is set: the writer will ${a} at ${p} from block ${b}",
              ("a", active->pause ? "pause" : "be killed")("p", active->point)("b", active->min_block));
}

void set_fault_observer(std::function<void(const char*, uint32_t)> o) { observer = std::move(o); }

void fault_point(const char* point, uint32_t block) {
   if (observer)
      observer(point, block);
   if (!active || active->point != point || block < active->min_block)
      return;
   if (fault_log)
      fc_wlog(*fault_log, "ROBOROVSKI_WRITER_FAULT reached ${p} at block ${b}", ("p", point)("b", block));
   if (active->pause) {
      for (;;)
         ::pause();
   }
   ::kill(::getpid(), SIGKILL);
   for (;;)
      ::pause();
}

} // namespace eosio::roborovski_writer
