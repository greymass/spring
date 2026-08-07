#include <eosio/resource_feed_plugin/feed_server.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include <fc/log/logger.hpp>

#include <utility>

namespace eosio::resource_feed {

feed_server::feed_server(const std::filesystem::path& socket_path, size_t max_queue_bytes)
   : _socket_path(socket_path)
   , _max_queue_bytes(max_queue_bytes) {}

feed_server::~feed_server() {
   stop();
}

void feed_server::start() {
   if (_running)
      return;

   std::error_code fs_ec;
   std::filesystem::remove(_socket_path, fs_ec);

   _ioc.restart();
   _acceptor.emplace(_ioc, protocol::endpoint(_socket_path.string()));
   _work.emplace(boost::asio::make_work_guard(_ioc));
   _running = true;

   do_accept();
   _thread = std::thread([this] { _ioc.run(); });
}

void feed_server::stop() {
   if (!_running)
      return;
   _running = false;

   boost::asio::post(_ioc, [this] {
      boost::system::error_code ec;
      if (_acceptor)
         _acceptor->close(ec);
      std::vector<uint64_t> ids;
      {
         std::lock_guard<std::mutex> guard(_mtx);
         for (const auto& entry : _clients)
            ids.push_back(entry.first);
      }
      for (uint64_t id : ids)
         close_client(id);
   });

   _work.reset();
   if (_thread.joinable())
      _thread.join();

   _acceptor.reset();
   std::error_code fs_ec;
   std::filesystem::remove(_socket_path, fs_ec);
}

std::vector<feed_server::client_view> feed_server::clients() {
   std::vector<client_view>    out;
   std::lock_guard<std::mutex> guard(_mtx);
   out.reserve(_clients.size());
   for (const auto& [id, c] : _clients) {
      if (!c->closing)
         out.push_back(client_view{id, c->needs_start});
   }
   return out;
}

void feed_server::mark_started(uint64_t client_id) {
   std::lock_guard<std::mutex> guard(_mtx);
   auto                        it = _clients.find(client_id);
   if (it != _clients.end())
      it->second->needs_start = false;
}

void feed_server::send(uint64_t client_id, std::vector<char> frames) {
   if (frames.empty())
      return;

   bool   overflow = false;
   size_t pending  = 0;
   {
      std::lock_guard<std::mutex> guard(_mtx);
      auto                        it = _clients.find(client_id);
      if (it == _clients.end())
         return;
      auto& c = *it->second;
      if (c.closing)
         return;
      if (c.queue.empty())
         c.queue = std::move(frames);
      else
         c.queue.insert(c.queue.end(), frames.begin(), frames.end());
      pending = c.queue.size() + c.writing.size();
      if (pending > _max_queue_bytes) {
         c.closing = true;
         overflow  = true;
      }
   }

   if (overflow)
      wlog("resource feed client ${id} exceeded queue limit (${p} > ${m} bytes); disconnecting",
           ("id", client_id)("p", pending)("m", _max_queue_bytes));

   kick(client_id);
}

void feed_server::disconnect_all() {
   std::vector<uint64_t> ids;
   {
      std::lock_guard<std::mutex> guard(_mtx);
      for (const auto& [id, c] : _clients) {
         c->closing = true;
         ids.push_back(id);
      }
   }
   for (uint64_t id : ids)
      boost::asio::post(_ioc, [this, id] { close_client(id); });
}

void feed_server::do_accept() {
   auto c = std::make_shared<client>(_ioc);
   _acceptor->async_accept(c->socket, [this, c](const boost::system::error_code& ec) {
      if (ec) {
         if (ec == boost::asio::error::operation_aborted)
            return;
         do_accept();
         return;
      }

      uint64_t id = 0;
      {
         std::lock_guard<std::mutex> guard(_mtx);
         id           = _next_id++;
         _clients[id] = c;
      }
      ilog("resource feed client ${id} connected", ("id", id));
      start_read(c, id);
      do_accept();
   });
}

void feed_server::start_read(const client_ptr& c, uint64_t id) {
   c->socket.async_read_some(boost::asio::buffer(c->discard),
                             [this, c, id](const boost::system::error_code& ec, size_t) {
                                if (ec) {
                                   close_client(id);
                                   return;
                                }
                                start_read(c, id);
                             });
}

void feed_server::kick(uint64_t id) {
   boost::asio::post(_ioc, [this, id] {
      client_ptr c;
      bool       closing = false;
      {
         std::lock_guard<std::mutex> guard(_mtx);
         auto                        it = _clients.find(id);
         if (it == _clients.end())
            return;
         c       = it->second;
         closing = c->closing;
      }
      if (closing)
         close_client(id);
      else
         do_write(c, id);
   });
}

void feed_server::do_write(const client_ptr& c, uint64_t id) {
   {
      std::lock_guard<std::mutex> guard(_mtx);
      if (c->write_active || c->queue.empty())
         return;
      c->writing.clear();
      c->writing.swap(c->queue);
      c->write_active = true;
   }

   boost::asio::async_write(c->socket, boost::asio::buffer(c->writing),
                            [this, c, id](const boost::system::error_code& ec, size_t) {
                               bool closing = false;
                               {
                                  std::lock_guard<std::mutex> guard(_mtx);
                                  c->writing.clear();
                                  c->write_active = false;
                                  closing         = c->closing;
                               }
                               if (ec || closing) {
                                  close_client(id);
                                  return;
                               }
                               do_write(c, id);
                            });
}

void feed_server::close_client(uint64_t id) {
   client_ptr c;
   {
      std::lock_guard<std::mutex> guard(_mtx);
      auto                        it = _clients.find(id);
      if (it == _clients.end())
         return;
      c = it->second;
      _clients.erase(it);
   }

   boost::system::error_code ec;
   c->socket.shutdown(socket_type::shutdown_both, ec);
   c->socket.close(ec);
   ilog("resource feed client ${id} disconnected", ("id", id));
}

} // namespace eosio::resource_feed
