#include <boost/test/unit_test.hpp>

#include <eosio/resource_feed_plugin/feed_server.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace eosio::resource_feed;
namespace local = boost::asio::local;

namespace {

struct temp_socket_path {
   std::filesystem::path path;

   temp_socket_path() {
      static std::atomic<int> counter{0};
      path = std::filesystem::path("/tmp") /
             ("rf_feed_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".sock");
      std::error_code ec;
      std::filesystem::remove(path, ec);
   }

   ~temp_socket_path() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
   }
};

std::vector<feed_server::client_view> wait_for_clients(feed_server& server, size_t n) {
   for (int i = 0; i < 1000; ++i) {
      auto snapshot = server.clients();
      if (snapshot.size() == n)
         return snapshot;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   BOOST_FAIL("timed out waiting for " << n << " client(s)");
   return {};
}

void connect_client(boost::asio::io_context& ioc, local::stream_protocol::socket& sock,
                    const std::filesystem::path& path) {
   for (int i = 0; i < 1000; ++i) {
      boost::system::error_code ec;
      sock.connect(local::stream_protocol::endpoint(path.string()), ec);
      if (!ec)
         return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   BOOST_FAIL("timed out connecting to " << path.string());
}

std::vector<char> pattern(size_t len, char seed) {
   std::vector<char> out(len);
   for (size_t i = 0; i < len; ++i)
      out[i] = static_cast<char>(seed + static_cast<char>(i % 61));
   return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(feed_server_tests)

BOOST_AUTO_TEST_CASE(frames_arrive_intact_and_in_order) {
   temp_socket_path sp;
   feed_server      server(sp.path, 4 * 1024 * 1024);
   server.start();

   boost::asio::io_context        ioc;
   local::stream_protocol::socket sock(ioc);
   connect_client(ioc, sock, sp.path);

   const auto view = wait_for_clients(server, 1);
   BOOST_REQUIRE_EQUAL(view.size(), 1u);
   BOOST_CHECK(view[0].needs_start);

   const auto first  = pattern(3, 'a');
   const auto second = pattern(70000, 'q');
   const auto third  = pattern(17, 'z');

   server.send(view[0].id, first);
   server.send(view[0].id, second);
   server.send(view[0].id, third);

   std::vector<char> expected;
   expected.insert(expected.end(), first.begin(), first.end());
   expected.insert(expected.end(), second.begin(), second.end());
   expected.insert(expected.end(), third.begin(), third.end());

   std::vector<char> got(expected.size());
   boost::asio::read(sock, boost::asio::buffer(got));
   BOOST_CHECK(got == expected);

   server.stop();
}

BOOST_AUTO_TEST_CASE(overflowing_client_is_disconnected) {
   temp_socket_path sp;
   feed_server      server(sp.path, 64 * 1024);
   server.start();

   boost::asio::io_context        ioc;
   local::stream_protocol::socket sock(ioc);
   connect_client(ioc, sock, sp.path);

   const auto view = wait_for_clients(server, 1);
   BOOST_REQUIRE_EQUAL(view.size(), 1u);

   const std::vector<char> chunk(64 * 1024, 'y');
   for (int i = 0; i < 256; ++i)
      server.send(view[0].id, chunk);

   wait_for_clients(server, 0);

   boost::system::error_code ec;
   std::vector<char>         buf(8192);
   while (!ec)
      sock.read_some(boost::asio::buffer(buf), ec);
   BOOST_CHECK(ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset);

   server.stop();
}

BOOST_AUTO_TEST_CASE(clients_are_served_independently) {
   temp_socket_path sp;
   feed_server      server(sp.path, 4 * 1024 * 1024);
   server.start();

   boost::asio::io_context        ioc;
   local::stream_protocol::socket sock_a(ioc);
   connect_client(ioc, sock_a, sp.path);
   const auto one = wait_for_clients(server, 1);

   local::stream_protocol::socket sock_b(ioc);
   connect_client(ioc, sock_b, sp.path);
   const auto both = wait_for_clients(server, 2);
   BOOST_REQUIRE_EQUAL(both.size(), 2u);

   const uint64_t id_a = one[0].id;
   const uint64_t id_b = (both[0].id == id_a) ? both[1].id : both[0].id;
   BOOST_REQUIRE_NE(id_a, id_b);

   server.mark_started(id_a);
   for (const auto& c : server.clients()) {
      if (c.id == id_a)
         BOOST_CHECK(!c.needs_start);
      else
         BOOST_CHECK(c.needs_start);
   }

   const auto a_bytes      = pattern(500, 'b');
   const auto direct_bytes = pattern(300, 'd');
   const auto sentinel     = pattern(64, 's');

   server.send(id_a, a_bytes);
   server.send(id_b, direct_bytes);
   server.send(id_a, sentinel);
   server.send(id_b, sentinel);

   std::vector<char> a_expected;
   a_expected.insert(a_expected.end(), a_bytes.begin(), a_bytes.end());
   a_expected.insert(a_expected.end(), sentinel.begin(), sentinel.end());

   std::vector<char> b_expected;
   b_expected.insert(b_expected.end(), direct_bytes.begin(), direct_bytes.end());
   b_expected.insert(b_expected.end(), sentinel.begin(), sentinel.end());

   std::vector<char> a_got(a_expected.size());
   boost::asio::read(sock_a, boost::asio::buffer(a_got));
   BOOST_CHECK(a_got == a_expected);

   std::vector<char> b_got(b_expected.size());
   boost::asio::read(sock_b, boost::asio::buffer(b_got));
   BOOST_CHECK(b_got == b_expected);

   server.stop();
}

BOOST_AUTO_TEST_CASE(disconnect_all_drops_every_client) {
   temp_socket_path sp;
   feed_server      server(sp.path, 4 * 1024 * 1024);
   server.start();

   boost::asio::io_context        ioc;
   local::stream_protocol::socket sock_a(ioc);
   local::stream_protocol::socket sock_b(ioc);
   connect_client(ioc, sock_a, sp.path);
   wait_for_clients(server, 1);
   connect_client(ioc, sock_b, sp.path);
   wait_for_clients(server, 2);

   server.disconnect_all();
   wait_for_clients(server, 0);

   boost::system::error_code ec;
   std::vector<char>         buf(64);
   while (!ec)
      sock_a.read_some(boost::asio::buffer(buf), ec);
   BOOST_CHECK(ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset);

   server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
