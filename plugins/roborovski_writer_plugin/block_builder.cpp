#include <eosio/roborovski_writer_plugin/abi_json.hpp>
#include <eosio/roborovski_writer_plugin/block_builder.hpp>
#include <eosio/roborovski_writer_plugin/block_serializer.hpp>

#include <eosio/chain/config.hpp>

#include <fc/io/raw.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>

namespace eosio::roborovski_writer {

namespace {

class block_tables {
public:
   uint32_t name(uint64_t n) {
      auto [it, inserted] = names_index.try_emplace(n, static_cast<uint32_t>(names.size()));
      if (inserted)
         names.push_back(n);
      return it->second;
   }

   uint32_t data(const char* p, size_t len) {
      std::string key(p, len);
      auto [it, inserted] = data_index.try_emplace(std::move(key), static_cast<uint32_t>(data_table.size()));
      if (inserted)
         data_table.emplace_back(p, p + len);
      return it->second;
   }

   std::vector<uint64_t>          names;
   std::vector<std::vector<char>> data_table;

private:
   std::unordered_map<uint64_t, uint32_t>    names_index;
   std::unordered_map<std::string, uint32_t> data_index;
};

const transaction_id_type& reported_id(const cached_trace& ct) {
   return ct.trace->failed_dtrx_trace ? ct.trace->failed_dtrx_trace->id : ct.trace->id;
}

trx_meta make_trx_meta(const cached_trace& ct) {
   trx_meta meta;
   if (ct.trace->receipt) {
      meta.status          = static_cast<uint8_t>(ct.trace->receipt->status);
      meta.cpu_usage_us    = ct.trace->receipt->cpu_usage_us;
      meta.net_usage_words = ct.trace->receipt->net_usage_words.value;
   }
   const auto& header    = ct.ptrx->get_transaction();
   meta.expiration       = header.expiration.sec_since_epoch();
   meta.ref_block_num    = header.ref_block_num;
   meta.ref_block_prefix = header.ref_block_prefix;
   for (const auto& sig : ct.ptrx->get_signatures())
      meta.signatures.push_back(fc::raw::pack(sig));
   return meta;
}

struct pending_action {
   uint64_t          glob = 0;
   std::vector<char> bytes;
};

} // namespace

std::vector<char> encode_block_blob(uint32_t block_num, uint32_t block_slot, const block_id_type& id,
                                    uint64_t glob_min, uint64_t glob_max,
                                    const std::vector<transaction_id_type>& trx_ids,
                                    const std::vector<trx_meta>&            trx_metas,
                                    const std::vector<std::vector<char>>&   data_table,
                                    const std::vector<uint64_t>&            names_table,
                                    const std::vector<uint32_t>&            cat_offsets,
                                    const std::vector<std::vector<char>>&   cat_bytes) {
   if (trx_ids.size() != trx_metas.size())
      throw data_corruption_exception("block " + std::to_string(block_num) + " has " +
                                      std::to_string(trx_ids.size()) + " transaction IDs but " +
                                      std::to_string(trx_metas.size()) + " metadata records");
   if (cat_offsets.size() != cat_bytes.size())
      throw data_corruption_exception("block " + std::to_string(block_num) + " has mismatched action tables");

   block_serializer s;
   s.write_u32_le(block_num);
   s.write_u32_le(block_slot);
   s.write_bytes(id.data(), 32);
   s.write_u64_le(glob_max);
   s.write_uvarint(glob_max - glob_min);

   s.write_uvarint(trx_ids.size());
   for (const auto& trx_id : trx_ids)
      s.write_bytes(trx_id.data(), 32);

   for (const auto& meta : trx_metas) {
      s.write_u8(meta.status);
      s.write_u32_le(meta.cpu_usage_us);
      s.write_uvarint(meta.net_usage_words);
      s.write_u32_le(meta.expiration);
      s.write_u16_le(meta.ref_block_num);
      s.write_u32_le(meta.ref_block_prefix);
      s.write_uvarint(meta.signatures.size());
      for (const auto& sig : meta.signatures) {
         s.write_uvarint(sig.size());
         s.write_bytes(sig);
      }
   }

   s.write_uvarint(data_table.size());
   for (const auto& d : data_table) {
      s.write_uvarint(d.size());
      s.write_bytes(d);
   }

   s.write_uvarint(names_table.size());
   for (uint64_t n : names_table)
      s.write_u64_le(n);

   s.write_uvarint(cat_offsets.size());
   for (uint32_t off : cat_offsets)
      s.write_uvarint(off);
   for (const auto& cb : cat_bytes)
      s.write_uvarint(cb.size());
   for (const auto& cb : cat_bytes)
      s.write_bytes(cb);

   return s.take();
}

built_block build_block(const signed_block& block, const block_id_type& id,
                        const std::optional<cached_trace>&                 onblock,
                        const std::map<transaction_id_type, cached_trace>& traces) {
   built_block out;
   out.block_num = block.block_num();
   out.id        = id;

   std::vector<const cached_trace*> ordered;
   ordered.reserve(block.transactions.size() + 1);
   if (onblock)
      ordered.push_back(&*onblock);
   for (const auto& r : block.transactions) {
      transaction_id_type trx_id = std::holds_alternative<transaction_id_type>(r.trx)
                                       ? std::get<transaction_id_type>(r.trx)
                                       : std::get<chain::packed_transaction>(r.trx).id();
      auto it = traces.find(trx_id);
      if (it != traces.end())
         ordered.push_back(&it->second);
   }

   std::vector<transaction_id_type>              trx_ids;
   std::vector<trx_meta>                         trx_metas;
   std::map<transaction_id_type, uint32_t>       trx_index;
   trx_ids.reserve(ordered.size());
   trx_metas.reserve(ordered.size());
   for (const auto* ct : ordered) {
      const auto& trx_id = reported_id(*ct);
      trx_index[trx_id]  = static_cast<uint32_t>(trx_ids.size());
      trx_ids.push_back(trx_id);
      trx_metas.push_back(make_trx_meta(*ct));
   }

   block_tables                tables;
   std::vector<pending_action> actions;
   uint64_t                    glob_min = 0;
   uint64_t                    glob_max = 0;

   for (const auto* ct : ordered) {
      const uint32_t trx_id_index = trx_index[reported_id(*ct)];
      for (const auto& at : ct->trace->action_traces) {
         if (at.context_free)
            continue;

         const chain::name receipt_receiver = at.receipt ? at.receipt->receiver : chain::name();
         if (at.receiver != receipt_receiver)
            throw data_corruption_exception("block " + std::to_string(out.block_num) + " action receiver " +
                                            at.receiver.to_string() + " differs from receipt receiver " +
                                            receipt_receiver.to_string());

         compressed_action_trace cat;
         cat.action_ordinal = at.action_ordinal.value;
         cat.creator_ao     = at.creator_action_ordinal.value;
         cat.closest_ua_ao  = at.closest_unnotified_ancestor_action_ordinal.value;
         cat.context_free   = at.context_free;
         cat.elapsed        = at.elapsed.count();
         cat.trx_id_index   = trx_id_index;
         if (at.receipt) {
            cat.global_sequence = at.receipt->global_sequence;
            cat.recv_sequence   = at.receipt->recv_sequence;
            cat.code_sequence   = at.receipt->code_sequence.value;
            cat.abi_sequence    = at.receipt->abi_sequence.value;
         }

         cat.receiver_index      = tables.name(at.receiver.to_uint64_t());
         cat.contract_name_index = tables.name(at.act.account.to_uint64_t());
         cat.action_name_index   = tables.name(at.act.name.to_uint64_t());

         cat.auths.reserve(at.act.authorization.size());
         for (const auto& auth : at.act.authorization) {
            auth_triple t;
            t.account_index    = tables.name(auth.actor.to_uint64_t());
            t.permission_index = tables.name(auth.permission.to_uint64_t());
            if (at.receipt) {
               auto seq = at.receipt->auth_sequence.find(auth.actor);
               if (seq != at.receipt->auth_sequence.end())
                  t.seq = seq->second;
            }
            cat.auths.push_back(t);
         }

         cat.account_ram_deltas.reserve(at.account_ram_deltas.size());
         for (const auto& delta : at.account_ram_deltas)
            cat.account_ram_deltas.push_back({tables.name(delta.account.to_uint64_t()), delta.delta});

         cat.data_index = tables.data(at.act.data.data(), at.act.data.size());

         if (glob_min == 0)
            glob_min = std::numeric_limits<uint64_t>::max();
         glob_min = std::min(glob_min, cat.global_sequence);
         glob_max = std::max(glob_max, cat.global_sequence);

         block_serializer cs;
         cs.write_compressed_action_trace(cat);
         actions.push_back({cat.global_sequence, cs.take()});

         if (at.act.account == chain::config::system_account_name && at.act.name == chain::name("setabi")) {
            if (auto parsed = parse_setabi(at.act.data.data(), at.act.data.size()))
               out.abis.push_back({out.block_num, parsed->contract, std::move(parsed->json)});
         }
      }
   }

   std::stable_sort(actions.begin(), actions.end(),
                    [](const pending_action& a, const pending_action& b) { return a.glob < b.glob; });

   std::vector<uint32_t>          cat_offsets;
   std::vector<std::vector<char>> cat_bytes;
   cat_offsets.reserve(actions.size());
   cat_bytes.reserve(actions.size());
   for (size_t j = 0; j < actions.size(); ++j) {
      cat_offsets.push_back(static_cast<uint32_t>(actions[j].glob - glob_min - j));
      cat_bytes.push_back(std::move(actions[j].bytes));
   }

   out.glob_min     = glob_min;
   out.glob_max     = glob_max;
   out.action_count = actions.size();
   out.blob = encode_block_blob(out.block_num, block.timestamp.slot, id, glob_min, glob_max, trx_ids, trx_metas,
                                tables.data_table, tables.names, cat_offsets, cat_bytes);
   return out;
}

} // namespace eosio::roborovski_writer
