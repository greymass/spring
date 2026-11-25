#include <eosio/roborovski_writer_plugin/abi_writer.hpp>
#include <eosio/roborovski_writer_plugin/store_format.hpp>

namespace eosio::roborovski_writer {

abi_writer::abi_writer(const std::filesystem::path& d)
   : dir(d) {
   std::filesystem::create_directories(dir);

   const auto data_path  = dir / "abis.dat";
   const auto index_path = dir / "abis.index";

   const uint64_t index_size = std::filesystem::exists(index_path) ? std::filesystem::file_size(index_path) : 0;
   if (index_size % INDEX_ENTRY_SIZE != 0)
      throw data_corruption_exception("abis.index size " + std::to_string(index_size) + " is not a multiple of " +
                                      std::to_string(INDEX_ENTRY_SIZE));
   entry_count = index_size / INDEX_ENTRY_SIZE;

   data_file.set_file_path(data_path);
   data_file.open(fc::cfile::create_or_update_rw_mode);
   data_file.seek_end(0);
   data_offset = data_file.tellp();

   index_file.set_file_path(index_path);
   index_file.open(fc::cfile::create_or_update_rw_mode);
   index_file.seek_end(0);
}

abi_writer::~abi_writer() {
   try {
      flush();
   } catch (...) {
   }
}

void abi_writer::write(const abi_record& record) {
   char header[DATA_HEADER_SIZE];
   put_le(header, record.block_num, 4);
   put_le(header + 4, record.contract, 8);
   put_le(header + 12, record.json.size(), 4);

   data_file.write(header, sizeof(header));
   data_file.write(record.json.data(), record.json.size());

   char entry[INDEX_ENTRY_SIZE];
   put_le(entry, record.block_num, 4);
   put_le(entry + 4, record.contract, 8);
   put_le(entry + 12, data_offset, 8);
   index_file.write(entry, sizeof(entry));

   data_offset += DATA_HEADER_SIZE + record.json.size();
   ++entry_count;
}

abi_writer::extent abi_writer::sync() {
   flush();
   sync_fd(data_file.fileno(), (dir / "abis.dat").string());
   sync_fd(index_file.fileno(), (dir / "abis.index").string());
   return {data_offset, static_cast<uint32_t>(entry_count)};
}

void abi_writer::flush() {
   if (data_file.is_open())
      data_file.flush();
   if (index_file.is_open())
      index_file.flush();
}

} // namespace eosio::roborovski_writer
