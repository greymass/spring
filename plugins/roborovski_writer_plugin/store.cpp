#include <eosio/roborovski_writer_plugin/fault.hpp>
#include <eosio/roborovski_writer_plugin/store.hpp>

#include <chrono>
#include <cstring>

namespace eosio::roborovski_writer {

store::store(const std::filesystem::path& core_dir, const std::filesystem::path& abi_dir, const hash_bytes& chain_id,
             fc::logger& l)
   : log(l) {
   std::filesystem::create_directories(core_dir);
   std::filesystem::create_directories(abi_dir);
   core_root = std::filesystem::canonical(core_dir);
   abi_root  = std::filesystem::canonical(abi_dir);
   if (core_root == abi_root)
      throw layout_exception("core storage and the ABI cache need separate directories (both are " +
                             core_root.string() + ")");

   core_lock = std::make_unique<writer_lock>(core_root);
   abi_lock  = std::make_unique<writer_lock>(abi_root);

   load(chain_id);
   if (have_manifest)
      recover();
   else
      adopt();
   pending_floor = current.abi_floor;

   core = std::make_unique<slice_writer>(core_root, log);
   core->on_rotate = [](uint32_t slice_num) { fault_point("slice_rotated", slice_start(slice_num)); };
   abi  = std::make_unique<abi_writer>(abi_root);

   const auto& tail = core->bounds();
   if (tail.head != current.head || tail.slice_data_end != current.slice_data_end ||
       tail.slice_index_len != current.slice_index_len)
      throw data_corruption_exception("core storage ends at block " + std::to_string(tail.head) +
                                      " after recovery, but the manifest commits block " +
                                      std::to_string(current.head));
   const auto abi_extent = abi->sync();
   if (abi_extent.data_end != current.abi_data_end || abi_extent.index_len != current.abi_index_len)
      throw data_corruption_exception("the ABI cache does not match its committed extent after recovery");
   sync_dir(abi_root);
   std::memcpy(head_id.data(), current.head_id.data(), 32);
   write_file_replace(published_path(core_root), current.serialize());
}

void store::load(const hash_bytes& chain_id) {
   remove_stray_temps(core_root / CONTROL_DIR_NAME, MANIFEST_NAME);
   remove_stray_temps(core_root / CONTROL_DIR_NAME, PUBLISHED_NAME);
   remove_stray_temps(core_root, BINDING_NAME);
   remove_stray_temps(abi_root, BINDING_NAME);

   auto core_b = binding::read(core_root);
   auto abi_b  = binding::read(abi_root);
   auto m      = manifest::read(core_root);

   if (core_b.has_value() != abi_b.has_value())
      throw binding_exception("only one of the core and ABI roots has a binding; they belong to different stores");
   if (m && !core_b)
      throw binding_exception("a manifest exists but the storage roots have no binding");

   if (core_b) {
      check_binding_pair(*core_b, *abi_b);
      if (core_b->slice_span != BLOCKS_PER_SLICE)
         throw binding_exception("store is bound to " + std::to_string(core_b->slice_span) +
                                 "-block slices; the writer requires " + std::to_string(BLOCKS_PER_SLICE));
      if (chain_id != hash_bytes{} && core_b->chain_known() && core_b->chain_id != chain_id)
         throw binding_exception("store is bound to chain " + hex(core_b->chain_id.data(), 32) +
                                 ", nodeos serves chain " + hex(chain_id.data(), 32));
      core_bind = *core_b;
      abi_bind  = *abi_b;
   } else {
      core_bind.role       = binding_role::core;
      core_bind.chain_id   = chain_id;
      core_bind.core_uuid  = new_uuid();
      core_bind.abi_uuid   = new_uuid();
      core_bind.slice_span = BLOCKS_PER_SLICE;
      abi_bind             = core_bind;
      abi_bind.role        = binding_role::abi;
   }
   if (m) {
      if (m->core_uuid != core_bind.core_uuid || m->abi_uuid != core_bind.abi_uuid)
         throw binding_exception("manifest names store " + format_uuid(m->core_uuid) + "/" + format_uuid(m->abi_uuid) +
                                 ", binding names " + format_uuid(core_bind.core_uuid) + "/" +
                                 format_uuid(core_bind.abi_uuid));
      current       = *m;
      have_manifest = true;
   }
   if (core_b && chain_id != hash_bytes{} && !core_bind.chain_known()) {
      core_bind.chain_id = chain_id;
      abi_bind.chain_id  = chain_id;
      core_bind.write(core_root);
      abi_bind.write(abi_root);
      fc_ilog(log, "Recorded chain ID ${c} in the store binding", ("c", hex(chain_id.data(), 32)));
   }
}

void store::recover() {
   report.core = truncate_core_to(core_root, {current.head, current.slice_data_end, current.slice_index_len});
   if (report.core.changed())
      fc_ilog(log, "Recovered core storage to committed block ${b}: removed ${s} uncommitted slices, ${d} data bytes, "
                   "${i} index entries",
              ("b", current.head)("s", report.core.removed_slices.size())("d", report.core.data_bytes_cut)(
                  "i", report.core.index_entries_cut));
   report.abi_cut = truncate_abi_to(abi_root, current.abi_data_end, current.abi_index_len);
   if (report.abi_cut)
      fc_ilog(log, "Recovered the ABI cache to ${n} committed entries", ("n", current.abi_index_len));
}

void store::adopt() {
   const auto a = find_adoption_boundary(core_root);
   truncate_core_to(core_root, a.bounds);
   const auto [abi_end, abi_len] = abi_committed_bounds(abi_root, a.bounds.head);
   truncate_abi_to(abi_root, abi_end, abi_len);

   manifest m;
   m.core_uuid       = core_bind.core_uuid;
   m.abi_uuid        = core_bind.abi_uuid;
   m.epoch           = 1;
   m.head            = a.bounds.head;
   m.slice_data_end  = a.bounds.slice_data_end;
   m.slice_index_len = a.bounds.slice_index_len;
   m.abi_index_len   = abi_len;
   m.abi_data_end    = abi_end;
   if (a.bounds.head > 0)
      m.head_id = a.head_id;

   core_bind.write(core_root);
   abi_bind.write(abi_root);
   write_file_atomic_durable(manifest_path(core_root), m.serialize());
   current       = m;
   have_manifest = true;
   report.adopted = true;
   if (m.head > 0)
      fc_ilog(log, "Adopted existing store at block ${b} (store ${u})", ("b", m.head)("u", format_uuid(m.core_uuid)));
   else
      fc_ilog(log, "Initialized empty store ${u}", ("u", format_uuid(m.core_uuid)));
}

void store::append(const prepared_block& block) {
   for (const auto& rec : block.abis)
      abi->write(rec);
   core->append_compressed(block.block_num, block.compressed, block.glob_min, block.glob_max);
   std::memcpy(head_id.data(), block.id.data(), 32);
}

bool store::commit() {
   const auto start      = std::chrono::steady_clock::now();
   const auto abi_extent = abi->sync();
   fault_point("abi_synced", core->get_last_block());
   const auto tail = core->sync();
   fault_point("core_synced", tail.head);
   fsync_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

   if (tail.head < current.head)
      throw data_corruption_exception("core head " + std::to_string(tail.head) + " is below committed head " +
                                      std::to_string(current.head));
   if (abi_extent.index_len < current.abi_index_len || abi_extent.data_end < current.abi_data_end)
      throw data_corruption_exception("the ABI cache shrank below its committed extent");

   manifest next        = current;
   next.head            = tail.head;
   next.slice_data_end  = tail.slice_data_end;
   next.slice_index_len = tail.slice_index_len;
   next.abi_data_end    = abi_extent.data_end;
   next.abi_index_len   = abi_extent.index_len;
   next.abi_floor       = pending_floor;
   if (tail.head != current.head)
      next.head_id = head_id;
   if (next == current)
      return false;

   atomic_write_steps steps;
   steps.after_rename   = [&] { fault_point("manifest_renamed", next.head); };
   steps.after_dir_sync = [&] { fault_point("manifest_dir_synced", next.head); };
   write_file_atomic_durable(manifest_path(core_root), next.serialize(), steps);
   current = next;
   write_file_replace(published_path(core_root), next.serialize());
   fault_point("published_renamed", next.head);
   return true;
}

void store::close() {
   if (core)
      core->close();
   core.reset();
   abi.reset();
   abi_lock.reset();
   core_lock.reset();
}

} // namespace eosio::roborovski_writer
