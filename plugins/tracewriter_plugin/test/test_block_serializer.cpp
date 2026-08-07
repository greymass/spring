#include <boost/test/unit_test.hpp>

#include <eosio/tracewriter_plugin/block_serializer.hpp>
#include <eosio/tracewriter_plugin/types.hpp>

using namespace eosio::tracewriter;

BOOST_AUTO_TEST_SUITE(block_serializer_tests)

BOOST_AUTO_TEST_CASE(test_write_u8) {
   block_serializer ser;
   ser.write_u8(0x42);
   BOOST_CHECK_EQUAL(ser.data().size(), 1);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 0x42);
}

BOOST_AUTO_TEST_CASE(test_write_u16_le) {
   block_serializer ser;
   ser.write_u16_le(0x1234);
   BOOST_CHECK_EQUAL(ser.data().size(), 2);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 0x34);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[1]), 0x12);
}

BOOST_AUTO_TEST_CASE(test_write_u32_le) {
   block_serializer ser;
   ser.write_u32_le(0x12345678);
   BOOST_CHECK_EQUAL(ser.data().size(), 4);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 0x78);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[1]), 0x56);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[2]), 0x34);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[3]), 0x12);
}

BOOST_AUTO_TEST_CASE(test_write_u64_le) {
   block_serializer ser;
   ser.write_u64_le(0x123456789ABCDEF0ULL);
   BOOST_CHECK_EQUAL(ser.data().size(), 8);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 0xF0);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[1]), 0xDE);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[2]), 0xBC);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[3]), 0x9A);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[4]), 0x78);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[5]), 0x56);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[6]), 0x34);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[7]), 0x12);
}

BOOST_AUTO_TEST_CASE(test_write_uvarint_small) {
   block_serializer ser;
   ser.write_uvarint(127);
   BOOST_CHECK_EQUAL(ser.data().size(), 1);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 127);
}

BOOST_AUTO_TEST_CASE(test_write_uvarint_medium) {
   block_serializer ser;
   ser.write_uvarint(128);
   BOOST_CHECK_EQUAL(ser.data().size(), 2);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 0x80);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[1]), 0x01);
}

BOOST_AUTO_TEST_CASE(test_write_uvarint_large) {
   block_serializer ser;
   ser.write_uvarint(300);
   BOOST_CHECK_EQUAL(ser.data().size(), 2);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 0xAC);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[1]), 0x02);
}

BOOST_AUTO_TEST_CASE(test_write_varint_positive) {
   block_serializer ser;
   ser.write_varint(1);
   BOOST_CHECK_EQUAL(ser.data().size(), 1);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 2);
}

BOOST_AUTO_TEST_CASE(test_write_varint_negative) {
   block_serializer ser;
   ser.write_varint(-1);
   BOOST_CHECK_EQUAL(ser.data().size(), 1);
   BOOST_CHECK_EQUAL(static_cast<uint8_t>(ser.data()[0]), 1);
}

BOOST_AUTO_TEST_CASE(test_clear) {
   block_serializer ser;
   ser.write_u32_le(0x12345678);
   BOOST_CHECK_EQUAL(ser.data().size(), 4);
   ser.clear();
   BOOST_CHECK_EQUAL(ser.data().size(), 0);
}

BOOST_AUTO_TEST_CASE(test_write_bytes) {
   block_serializer ser;
   std::vector<char> data = {'h', 'e', 'l', 'l', 'o'};
   ser.write_bytes(data);
   BOOST_CHECK_EQUAL(ser.data().size(), 5);
   BOOST_CHECK_EQUAL(ser.data()[0], 'h');
   BOOST_CHECK_EQUAL(ser.data()[4], 'o');
}

BOOST_AUTO_TEST_SUITE_END()
