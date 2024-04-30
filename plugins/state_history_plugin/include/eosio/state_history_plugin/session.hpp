#pragma once
#include <eosio/state_history/compression.hpp>
#include <eosio/state_history/log.hpp>
#include <eosio/state_history/serialization.hpp>
#include <eosio/state_history/types.hpp>

#include <eosio/chain/types.hpp>
#include <eosio/chain/controller.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>


extern const char* const state_history_plugin_abi;

namespace eosio {

using namespace state_history;

class session_base {
public:
   virtual void block_applied(const chain::block_num_type block_num) = 0;

   virtual ~session_base() = default;
};

template<typename SocketType, typename Executor, typename GetBlockID, typename GetBlock, typename OnDone>
class session final : public session_base {
   using coro_throwing_stream = boost::asio::use_awaitable_t<>::as_default_on_t<boost::beast::websocket::stream<SocketType>>;
   using coro_nonthrowing_steadytimer = boost::asio::as_tuple_t<boost::asio::use_awaitable_t<>>::as_default_on_t<boost::asio::steady_timer>;

public:
   session(SocketType&& s, Executor&& st, chain::controller& controller,
              std::optional<state_history_log>& trace_log, std::optional<state_history_log>& chain_state_log, std::optional<state_history_log>& finality_data_log,
              GetBlockID&& get_block_id, GetBlock&& get_block, OnDone&& on_done, fc::logger& logger) :
    strand(std::move(st)), stream(std::move(s)), wake_timer(strand), controller(controller),
    trace_log(trace_log), chain_state_log(chain_state_log), finality_data_log(finality_data_log),
    get_block_id(get_block_id), get_block(get_block), on_done(on_done), logger(logger), remote_endpoint_string(get_remote_endpoint_string()) {
      fc_ilog(logger, "incoming state history connection from ${a}", ("a", remote_endpoint_string));

      boost::asio::co_spawn(strand, read_loop(), [&](std::exception_ptr e) {check_coros_done(e);});
   }

   void block_applied(const chain::block_num_type block_num) {
      //indicates a fork being applied for already-sent blocks; rewind the cursor
      if(block_num < next_block_cursor)
         next_block_cursor = block_num;
      awake_if_idle();
   }

private:
   std::string get_remote_endpoint_string() const {
      try {
         if constexpr(std::is_same_v<SocketType, boost::asio::ip::tcp::socket>)
            return boost::lexical_cast<std::string>(stream.next_layer().remote_endpoint());
         return "UNIX socket";
      } catch (...) {
         return "(unknown)";
      }
   }

   void awake_if_idle() {
      boost::asio::dispatch(strand, [this]() {
         wake_timer.cancel_one();
      });
   }

   void check_coros_done(std::exception_ptr e) {
      //the only exception that should have bubbled out of the coros is a bad_alloc, bubble it up further. No need to bother
      //  with the rest of the cleanup: we'll be shutting down soon anyway due to bad_alloc
      if(e)
         std::rethrow_exception(e);
      //coros always return on the session's strand
      if(--coros_running == 0)
         on_done(this);
   }

   template<typename F>
   void drop_exceptions(F&& f) {
      try{ f(); } catch(...) {}
   }

   template<typename F>
   boost::asio::awaitable<void> readwrite_coro_exception_wrapper(F&& f) {
      coros_running++;

      try {
         co_await f();
      }
      catch(std::bad_alloc&) {
         throw;
      }
      catch(fc::exception& e) {
         if(has_logged_exception.test_and_set() == false)
            fc_ilog(logger, "state history connection from ${a} failed: ${w}", ("a", remote_endpoint_string)("w", e.top_message()));
      }
      catch(boost::system::system_error& e) {
         if(has_logged_exception.test_and_set() == false)
            fc_ilog(logger, "state history connection from ${a} failed: ${w}", ("a", remote_endpoint_string)("w", e.code().message()));
      }
      catch(std::exception& e) {
         if(has_logged_exception.test_and_set() == false)
            fc_ilog(logger, "state history connection from ${a} failed: ${w}", ("a", remote_endpoint_string)("w", e.what()));
      }
      catch(...) {
         if(has_logged_exception.test_and_set() == false)
            fc_ilog(logger, "state history connection from ${a} failed", ("a", remote_endpoint_string));
      }

      drop_exceptions([this](){ stream.next_layer().close(); });
      drop_exceptions([this](){ awake_if_idle();             });
   }

   //this reads better as a lambda directly inside of read_loop(), but gcc10.x ICEs on capturing this inside a coro
   boost::asio::awaitable<void> read_loop_main_thread(const state_request& req) {
      std::visit(chain::overloaded {
         [this]<typename GetStatusRequestV0orV1, typename = std::enable_if_t<std::is_base_of_v<get_status_request_v0, GetStatusRequestV0orV1>>>(const GetStatusRequestV0orV1&) {
            queued_status_requests.emplace_back(std::is_same_v<GetStatusRequestV0orV1, get_status_request_v1>);
         },
         [this]<typename GetBlocksRequestV0orV1, typename = std::enable_if_t<std::is_base_of_v<get_blocks_request_v0, GetBlocksRequestV0orV1>>>(const GetBlocksRequestV0orV1& gbr) {
            current_blocks_request_v1_finality.reset();
            current_blocks_request = gbr;
            if constexpr(std::is_same_v<GetBlocksRequestV0orV1, get_blocks_request_v1>)
               current_blocks_request_v1_finality = gbr.fetch_finality_data;

            for(const block_position& haveit : current_blocks_request.have_positions) {
               if(current_blocks_request.start_block_num <= haveit.block_num)
                  continue;
               if(const std::optional<chain::block_id_type> id = get_block_id(haveit.block_num); !id || *id != haveit.block_id)
                  current_blocks_request.start_block_num = std::min(current_blocks_request.start_block_num, haveit.block_num);
            }
            current_blocks_request.have_positions.clear();
         },
         [this](const get_blocks_ack_request_v0& gbar0) {
            send_credits += gbar0.num_messages;
         }
      }, req);
      co_return;
   }

   boost::asio::awaitable<void> read_loop() {
      co_await readwrite_coro_exception_wrapper([this]() -> boost::asio::awaitable<void> {
         wake_timer.expires_at(std::chrono::steady_clock::time_point::max());

         if constexpr(std::is_same_v<SocketType, boost::asio::ip::tcp::socket>)
            stream.next_layer().set_option(boost::asio::ip::tcp::no_delay(true));
         stream.next_layer().set_option(boost::asio::socket_base::send_buffer_size(1024*1024));
         stream.write_buffer_bytes(512*1024);

         co_await stream.async_accept();
         co_await stream.async_write(boost::asio::const_buffer(state_history_plugin_abi, strlen(state_history_plugin_abi)));
         stream.binary(true);
         boost::asio::co_spawn(strand, write_loop(), [&](std::exception_ptr e) {check_coros_done(e);});

         while(true) {
            boost::beast::flat_buffer b;
            co_await stream.async_read(b);
            const state_request req = fc::raw::unpack<std::remove_const_t<decltype(req)>>(static_cast<const char*>(b.cdata().data()), b.size());

            //TODO: how can set main thread priority on this?
            co_await boost::asio::co_spawn(app().get_io_service(), read_loop_main_thread(req), boost::asio::use_awaitable);

            awake_if_idle();
         }
      });
   }

   get_status_result_v1 fill_current_status_result() {
      get_status_result_v1 ret;

      ret.head              = {controller.head_block_num(), controller.head_block_id()};
      ret.last_irreversible = {controller.last_irreversible_block_num(), controller.last_irreversible_block_id()};
      ret.chain_id          = controller.get_chain_id();
      if(trace_log)
         std::tie(ret.trace_begin_block, ret.trace_end_block) = trace_log->block_range();
      if(chain_state_log)
         std::tie(ret.chain_state_begin_block, ret.chain_state_end_block) = chain_state_log->block_range();
      if(finality_data_log)
         std::tie(ret.finality_data_begin_block, ret.finality_data_end_block) = finality_data_log->block_range();

      return ret;
   }

   boost::asio::awaitable<void> write_log_entry(std::optional<locked_decompress_stream>& log_stream, std::optional<state_history_log>& log, chain::block_num_type block_num) {
      uint64_t unpacked_size = 0;

      if(log_stream) //will be unset if either request did not ask for this log entry, or the log isn't enabled
         unpacked_size = log->get_unpacked_entry(block_num, *log_stream); //will return 0 if log does not include the block num asked for

      if(unpacked_size) {
         char buff[1024*1024];
         fc::datastream<char*> ds(buff, sizeof(buff));
         fc::raw::pack(ds, true);
         history_pack_varuint64(ds, unpacked_size);
         co_await stream.async_write_some(false, boost::asio::buffer(buff, ds.tellp()));

         ///TODO: why is there an uncompressed option in the variant?! Shouldn't it always be compressed? was this for old unit tests?
         bio::filtering_istreambuf& decompression_stream = *std::get<std::unique_ptr<bio::filtering_istreambuf>>(log_stream->buf);
         std::streamsize red = 0;
         while((red = bio::read(decompression_stream, buff, sizeof(buff))) != -1) {
            if(red == 0)
               continue;
            co_await stream.async_write_some(false, boost::asio::buffer(buff, red));
         }
      }
      else {
         co_await stream.async_write_some(false, boost::asio::buffer(fc::raw::pack(false)));
      }
   }

   struct block_package {
      get_blocks_result_base blocks_result_base;
      bool is_v1_request = false;
      chain::block_num_type this_block_num = 0; //this shouldn't be needed post log de-mutexing
      std::optional<locked_decompress_stream> trace_stream;
      std::optional<locked_decompress_stream> state_stream;
      std::optional<locked_decompress_stream> finality_stream;
   };

   //this reads better as a lambda directly inside of write_loop(), but gcc10.x ICEs on capturing this inside a coro
   boost::asio::awaitable<void> write_loop_main_thread(std::deque<bool>& status_requests, std::optional<block_package>& block_to_send) {
      status_requests = std::move(queued_status_requests);

      //decide what block -- if any -- to send out
      const chain::block_num_type latest_to_consider = current_blocks_request.irreversible_only ?
                                                       controller.last_irreversible_block_num() : controller.head_block_num();
      if(send_credits && next_block_cursor <= latest_to_consider && next_block_cursor < current_blocks_request.end_block_num) {
         block_to_send.emplace( block_package{
            .blocks_result_base = {
               .head = {controller.head_block_num(), controller.head_block_id()},
               .last_irreversible = {controller.last_irreversible_block_num(), controller.last_irreversible_block_id()}
            },
            .is_v1_request = current_blocks_request_v1_finality.has_value(),
            .this_block_num = next_block_cursor
         });
         if(const std::optional<chain::block_id_type> this_block_id = get_block_id(next_block_cursor)) {
            block_to_send->blocks_result_base.this_block  = {current_blocks_request.start_block_num, *this_block_id};
            if(const std::optional<chain::block_id_type> last_block_id = get_block_id(next_block_cursor - 1))
               block_to_send->blocks_result_base.prev_block = {next_block_cursor - 1, *last_block_id};
            if(chain::signed_block_ptr sbp = get_block(next_block_cursor); sbp && current_blocks_request.fetch_block)
                block_to_send->blocks_result_base.block = fc::raw::pack(*sbp);
            if(current_blocks_request.fetch_traces && trace_log)
               block_to_send->trace_stream.emplace(trace_log->create_locked_decompress_stream());
            if(current_blocks_request.fetch_deltas && chain_state_log)
               block_to_send->state_stream.emplace(chain_state_log->create_locked_decompress_stream());
            if(block_to_send->is_v1_request && *current_blocks_request_v1_finality && finality_data_log)
               block_to_send->finality_stream.emplace(finality_data_log->create_locked_decompress_stream());
         }
         ++next_block_cursor;
         --send_credits;
      }
      co_return;
   }

   boost::asio::awaitable<void> write_loop() {
      co_await readwrite_coro_exception_wrapper([this]() -> boost::asio::awaitable<void> {
         get_status_result_v1 current_status_result;

         while(true) {
            if(!stream.is_open())
               break;

            std::deque<bool>             status_requests;
            std::optional<block_package> block_to_send;

            //write_loop_main_thread() will populate status_requests and block_to_send with what to send this for this iteration of the loop
            ///TODO: How to set main thread priority?
            co_await boost::asio::co_spawn(app().get_io_service(), write_loop_main_thread(status_requests, block_to_send), boost::asio::use_awaitable);

            //if there is nothing to send, go to sleep
            if(status_requests.empty() && !block_to_send) {
               co_await wake_timer.async_wait();
               continue;
            }

            if(status_requests.size())
               current_status_result = fill_current_status_result();

            //send replies to all send status requests first
            for(const bool status_request_is_v1 : status_requests) {
               if(status_request_is_v1 == false) //v0 status request, gets a v0 status result
                  co_await stream.async_write(boost::asio::buffer(fc::raw::pack(state_result((get_status_result_v0)current_status_result))));
               else
                  co_await stream.async_write(boost::asio::buffer(fc::raw::pack(state_result(current_status_result))));
            }

            //and then send the block
            if(block_to_send) {
               const fc::unsigned_int get_blocks_result_variant_index = block_to_send->is_v1_request ?
                                                                        state_result(get_blocks_result_v1()).index() :
                                                                        state_result(get_blocks_result_v0()).index();
               co_await stream.async_write_some(false, boost::asio::buffer(fc::raw::pack(get_blocks_result_variant_index)));
               co_await stream.async_write_some(false, boost::asio::buffer(fc::raw::pack(block_to_send->blocks_result_base)));

               //accessing the _logs here violates the rule that those should only be accessed on the main thread. However, we're
               // only calling get_unpacked_entry() on it which assumes the mutex is held by the locked_decompress_stream. So this is
               // "safe" in some aspects but can deadlock
               co_await write_log_entry(block_to_send->trace_stream, trace_log, block_to_send->this_block_num);
               co_await write_log_entry(block_to_send->state_stream, chain_state_log, block_to_send->this_block_num);
               if(block_to_send->is_v1_request)
                  co_await write_log_entry(block_to_send->finality_stream, finality_data_log, block_to_send->this_block_num);

               co_await stream.async_write_some(true, boost::asio::const_buffer());
            }
         }
      });
   }

private:
   ///these items must only ever be touched by the session's strand
   Executor                          strand;
   coro_throwing_stream              stream;
   coro_nonthrowing_steadytimer      wake_timer;
   unsigned                          coros_running = 0;

   ///these items must only ever be touched on the main thread
   std::deque<bool>                  queued_status_requests;  //false for v0, true for v1

   get_blocks_request_v0             current_blocks_request;
   std::optional<bool>               current_blocks_request_v1_finality; //unset: current request is v0; set means v1; true/false is if finality requested
   //current_blocks_request is modified with the current state; bind some more descriptive names to items frequently used
   uint32_t&                         send_credits = current_blocks_request.max_messages_in_flight;
   chain::block_num_type&            next_block_cursor = current_blocks_request.start_block_num;

   chain::controller&                controller;
   std::optional<state_history_log>& trace_log;
   std::optional<state_history_log>& chain_state_log;
   std::optional<state_history_log>& finality_data_log;

   GetBlockID                        get_block_id;
   GetBlock                          get_block;

   ///these items might be used on either the strand or main thread
   std::atomic_flag                  has_logged_exception;  //TODO, this doesn't get used on anything but the strand now
   OnDone                            on_done;
   fc::logger&                       logger;
   const std::string                 remote_endpoint_string;
};

} // namespace eosio
