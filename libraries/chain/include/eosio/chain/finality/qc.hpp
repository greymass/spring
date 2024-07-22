#pragma once

#include <eosio/chain/finality/finality_core.hpp>
#include <eosio/chain/finality/finalizer_policy.hpp>
#include <eosio/chain/finality/vote_message.hpp>
#include <eosio/chain/block_timestamp.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>
#include <fc/bitutil.hpp>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace eosio::chain {

   using bls_public_key          = fc::crypto::blslib::bls_public_key;
   using bls_signature           = fc::crypto::blslib::bls_signature;
   using bls_aggregate_signature = fc::crypto::blslib::bls_aggregate_signature;
   using bls_private_key         = fc::crypto::blslib::bls_private_key;

   using vote_bitset   = fc::dynamic_bitset;
   using bls_key_map_t = std::map<bls_public_key, bls_private_key>;

   constexpr std::array weak_bls_sig_postfix = { 'W', 'E', 'A', 'K' };
   using weak_digest_t = std::array<uint8_t, sizeof(digest_type) + weak_bls_sig_postfix.size()>;

   inline weak_digest_t create_weak_digest(const digest_type& digest) {
      weak_digest_t res;
      std::memcpy(res.begin(), digest.data(), digest.data_size());
      std::memcpy(res.begin() + digest.data_size(), weak_bls_sig_postfix.data(), weak_bls_sig_postfix.size());
      return res;
   }

   enum class vote_status {
      success,
      duplicate,             // duplicate vote, expected as votes arrive on multiple connections
      unknown_public_key,    // public key is invalid, indicates invalid vote
      invalid_signature,     // signature is invalid, indicates invalid vote
      unknown_block,         // block not available, possibly less than LIB, or too far in the future
      max_exceeded           // received too many votes for a connection
   };

   enum class has_vote_status_t {
      voted,
      not_voted,
      irrelevant_finalizer
   };

   struct qc_sig_t {
      bool is_weak()   const { return !!weak_votes; }
      bool is_strong() const { return !weak_votes; }

      std::optional<vote_bitset> strong_votes;
      std::optional<vote_bitset> weak_votes;
      bls_aggregate_signature    sig;

      // called from net threads
      void verify(const finalizer_policy_ptr& fin_policy, const digest_type& strong_digest, const weak_digest_t& weak_digest) const;
   };

   struct qc_t {
      uint32_t                  block_num{0};
      // signatures corresponding to the active finalizer policy
      qc_sig_t                  active_policy_sig;
      // signatures corresponding to the pending finalizer policy if there is one
      std::optional<qc_sig_t>   pending_policy_sig;

      bool is_strong() const {
         return active_policy_sig.is_strong() && (!pending_policy_sig || pending_policy_sig->is_strong());
      }

      bool is_weak() const {
         return active_policy_sig.is_weak() || (pending_policy_sig && pending_policy_sig->is_weak());
      }

      qc_claim_t to_qc_claim() const {
         return { .block_num = block_num, .is_strong_qc = is_strong() };
      }
   };

   struct qc_data_t {
      std::optional<qc_t> qc;  // Comes either from traversing branch from parent and calling get_best_qc()
                               // or from an incoming block extension.
      qc_claim_t qc_claim;     // describes the above qc_t. In rare cases (bootstrap, starting from snapshot,
                               // disaster recovery), we may not have a qc_t so we use the `lib` block_num
                               // and specify `weak`.
   };

   /**
    * All public methods are thread-safe.
    * Used for incorporating votes into a qc signature.
    * "open" in that it allows new votes to be added at any time.
    */
   class open_qc_sig_t {
   public:
      enum class state_t {
         unrestricted,  // No quorum reached yet, still possible to achieve any state.
         restricted,    // Enough `weak` votes received to know it is impossible to reach the `strong` state.
         weak_achieved, // Enough `weak` + `strong` votes for a valid `weak` QC, still possible to reach the `strong` state.
         weak_final,    // Enough `weak` + `strong` votes for a valid `weak` QC, `strong` not possible anymore.
         strong         // Enough `strong` votes to have a valid `strong` QC
      };

      struct votes_t : fc::reflect_init {
      private:
         friend struct fc::reflector<votes_t>;
         friend struct fc::reflector_init_visitor<votes_t>;
         friend struct fc::has_reflector_init<votes_t>;
         friend class open_qc_sig_t;

         vote_bitset                    bitset;
         bls_aggregate_signature        sig;
         std::vector<std::atomic<bool>> processed; // avoid locking mutex for _bitset duplicate check

         void reflector_init();
      public:
         explicit votes_t(size_t num_finalizers)
            : bitset(num_finalizers)
            , processed(num_finalizers) {}

         // thread safe
         bool has_voted(size_t index) const;

         vote_status add_vote(size_t index, const bls_signature& sig);

         template<class CB>
         void visit_bitset(const CB& cb) const {
            for (size_t i = 0; i < bitset.size(); ++i) {
               if (bitset[i])
                  cb(i);
            }
         }
      };

      open_qc_sig_t();

      explicit open_qc_sig_t(size_t num_finalizers, uint64_t quorum, uint64_t max_weak_sum_before_weak_final);

      bool is_quorum_met() const;
      static bool is_quorum_met(state_t s) {
         return s == state_t::strong || s == state_t::weak_achieved || s == state_t::weak_final;
      }

      vote_status add_vote(uint32_t connection_id,
                           block_num_type block_num,
                           bool strong,
                           size_t index,
                           const bls_signature& sig,
                           uint64_t weight);

      bool has_voted(size_t index) const;
      bool has_voted(bool strong, size_t index) const;

      // for debugging, thread safe
      template<class CB>
      void visit_votes(const CB& cb) const {
         std::lock_guard g(*_mtx);
         strong_votes.visit_bitset([&](size_t idx) { cb(idx, true); });
         weak_votes.visit_bitset([&](size_t idx)   { cb(idx, false); });
      }

      state_t state() const { std::lock_guard g(*_mtx); return pending_state; };

      std::optional<qc_sig_t> get_best_qc() const;
      void set_received_qc_sig(const qc_sig_t& qc);
      bool received_qc_sig_is_strong() const;
   private:
      friend struct fc::reflector<open_qc_sig_t>;
      friend class qc_chain;
      std::unique_ptr<std::mutex> _mtx;
      std::optional<qc_sig_t> received_qc_sig; // best qc_t received from the network inside block extension
      uint64_t             quorum {0};
      uint64_t             max_weak_sum_before_weak_final {0}; // max weak sum before becoming weak_final
      state_t              pending_state { state_t::unrestricted };
      uint64_t             strong_sum {0}; // accumulated sum of strong votes so far
      uint64_t             weak_sum {0}; // accumulated sum of weak votes so far
      votes_t              weak_votes {0};
      votes_t              strong_votes {0};

      // called by add_vote, already protected by mutex
      vote_status add_strong_vote(size_t index,
                                  const bls_signature& sig,
                                  uint64_t weight);

      // called by add_vote, already protected by mutex
      vote_status add_weak_vote(size_t index,
                                const bls_signature& sig,
                                uint64_t weight);

      bool is_quorum_met_no_lock() const;
      qc_sig_t to_valid_qc_sig() const;
   };

   // finalizer authority of strong, weak, or missing votes
   struct qc_vote_metrics_t {
      std::set<finalizer_authority_ptr> strong_votes;
      std::set<finalizer_authority_ptr> weak_votes;
      std::set<finalizer_authority_ptr> missing_votes;
   };

   /**
    * All public methods are thread-safe, pending_policy_sig optional set at construction time.
    */
   class open_qc_t {
   public:
      open_qc_t(const finalizer_policy_ptr& active_finalizer_policy,
                const finalizer_policy_ptr& pending_finalizer_policy)
         : active_finalizer_policy(active_finalizer_policy)
         , pending_finalizer_policy(pending_finalizer_policy)
         , active_policy_sig{open_qc_sig_t{active_finalizer_policy->finalizers.size(),
                                              active_finalizer_policy->threshold,
                                              active_finalizer_policy->max_weak_sum_before_weak_final()}}
         , pending_policy_sig{!pending_finalizer_policy
                                 ? std::optional<open_qc_sig_t>{}
                                 : std::optional<open_qc_sig_t>{std::in_place,
                                                                   pending_finalizer_policy->finalizers.size(),
                                                                   pending_finalizer_policy->threshold,
                                                                   pending_finalizer_policy->max_weak_sum_before_weak_final()}}
      {}

      open_qc_t() = default;

      std::optional<qc_t> get_best_qc(block_num_type block_num) const;
      // verify qc against active and pending policy
      void verify_qc(const qc_t& qc, const digest_type& strong_digest, const weak_digest_t& weak_digest) const;
      qc_vote_metrics_t vote_metrics(const qc_t& qc) const;
      // return qc missing vote's finalizers
      std::set<finalizer_authority_ptr> missing_votes(const qc_t& qc) const;
      void set_received_qc(const qc_t& qc);
      bool received_qc_is_strong() const;
      vote_status aggregate_vote(uint32_t connection_id, const vote_message& vote,
                                 block_num_type block_num, std::span<const uint8_t> finalizer_digest);
      has_vote_status_t has_voted(const bls_public_key& key) const;
      bool is_quorum_met() const;

   private:
      friend struct fc::reflector<open_qc_t>;
      finalizer_policy_ptr         active_finalizer_policy;  // not modified after construction
      finalizer_policy_ptr         pending_finalizer_policy; // not modified after construction
      open_qc_sig_t                active_policy_sig;
      std::optional<open_qc_sig_t> pending_policy_sig;
   };

} //eosio::chain


FC_REFLECT_ENUM(eosio::chain::vote_status, (success)(duplicate)(unknown_public_key)(invalid_signature)(unknown_block)(max_exceeded))
FC_REFLECT(eosio::chain::qc_sig_t, (strong_votes)(weak_votes)(sig));
FC_REFLECT(eosio::chain::open_qc_sig_t, (received_qc_sig)(quorum)(max_weak_sum_before_weak_final)(pending_state)(strong_sum)(weak_sum)(weak_votes)(strong_votes));
FC_REFLECT(eosio::chain::open_qc_t, (active_finalizer_policy)(pending_finalizer_policy)(active_policy_sig)(pending_policy_sig));
FC_REFLECT_ENUM(eosio::chain::open_qc_sig_t::state_t, (unrestricted)(restricted)(weak_achieved)(weak_final)(strong));
FC_REFLECT(eosio::chain::open_qc_sig_t::votes_t, (bitset)(sig));
FC_REFLECT(eosio::chain::qc_t, (block_num)(active_policy_sig)(pending_policy_sig));
