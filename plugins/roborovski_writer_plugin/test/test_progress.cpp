#include <boost/test/unit_test.hpp>

#include <eosio/roborovski_writer_plugin/progress.hpp>

using namespace eosio::roborovski_writer;

BOOST_AUTO_TEST_SUITE(progress_tests)

BOOST_AUTO_TEST_CASE(matches_coreindex_sync_line) {
   progress_sample s;
   s.committed    = 25000000;
   s.target       = 50000000;
   s.elapsed_s    = 3.0;
   s.writer_s     = 0.412;
   s.blocks       = 9000;
   s.actions      = 18000;
   s.transactions = 13500;
   s.bytes        = 9000ull * 2048;
   s.glob         = 123456789;
   BOOST_TEST(format_progress(s) == "Block: 25000000 / 50000000 (50.0%)\t3000 BPS | 17.6 MB (2048 B/blk) | "
                                    "2.0 acts/blk, 1.5 txns/blk | glob: 123456789 | writer: 412 of 3000 ms");
}

BOOST_AUTO_TEST_CASE(unknown_target_and_idle_interval) {
   progress_sample s;
   s.committed = 7;
   s.elapsed_s = 3.0;
   BOOST_TEST(format_progress(s) == "Block: 7 / ?\t0 BPS | 0 B (0 B/blk) | 0.0 acts/blk, 0.0 txns/blk | glob: 0 | "
                                    "writer: 0 of 3000 ms");
}

BOOST_AUTO_TEST_CASE(block_log_end_without_a_log) {
   BOOST_TEST(block_log_end("/nonexistent/blocks") == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
