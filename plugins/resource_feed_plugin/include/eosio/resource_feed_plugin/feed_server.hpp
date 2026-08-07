#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace eosio::resource_feed {

class feed_server {
public:
   feed_server(const std::filesystem::path& socket_path, size_t max_queue_bytes);
   ~feed_server();

   feed_server(const feed_server&)            = delete;
   feed_server& operator=(const feed_server&) = delete;

   void start();
   void stop();

   struct client_view {
      uint64_t id;
      bool     needs_start;
   };

   std::vector<client_view> clients();
   void                     mark_started(uint64_t client_id);
   void                     send(uint64_t client_id, std::vector<char> frames);
   void                     disconnect_all();

private:
   using protocol    = boost::asio::local::stream_protocol;
   using socket_type = protocol::socket;

   struct client {
      explicit client(boost::asio::io_context& ioc)
         : socket(ioc) {}

      socket_type            socket;
      std::vector<char>      queue;
      std::vector<char>      writing;
      std::array<char, 1024> discard{};
      bool                   write_active = false;
      bool                   needs_start  = true;
      bool                   closing      = false;
   };
   using client_ptr = std::shared_ptr<client>;

   void do_accept();
   void start_read(const client_ptr& c, uint64_t id);
   void kick(uint64_t id);
   void do_write(const client_ptr& c, uint64_t id);
   void close_client(uint64_t id);

   std::filesystem::path _socket_path;
   size_t                _max_queue_bytes;

   boost::asio::io_context                                                                _ioc;
   std::optional<protocol::acceptor>                                                      _acceptor;
   std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _work;
   std::thread                                                                            _thread;

   std::mutex                     _mtx;
   std::map<uint64_t, client_ptr> _clients;
   uint64_t                       _next_id = 1;
   bool                           _running = false;
};

} // namespace eosio::resource_feed
