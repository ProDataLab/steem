#include <fc/container/deque.hpp>
#include <steemit/chain/database.hpp>

#include <steemit/chain/block_summary_object.hpp>
#include <steemit/chain/steem_objects.hpp>
#include <steemit/chain/steem_evaluator.hpp>
#include <steemit/chain/protocol/steem_operations.hpp>
#include <steemit/chain/db_with.hpp>
#include <steemit/chain/exceptions.hpp>
#include <steemit/chain/global_property_object.hpp>
#include <steemit/chain/hardfork.hpp>
#include <steemit/chain/history_object.hpp>
#include <steemit/chain/transaction_evaluation_state.hpp>
#include <steemit/chain/transaction_object.hpp>


#include <graphene/db/flat_index.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>
#include <fc/io/fstream.hpp>

#include <fstream>
#include <functional>
#include <iostream>

namespace steemit { namespace chain {

using boost::container::flat_set;

// C++ requires that static class variables declared and initialized
// in headers must also have a definition in a single source file,
// else linker errors will occur [1].
//
// The purpose of this source file is to collect such definitions in
// a single place.
//
// [1] http://stackoverflow.com/questions/8016780/undefined-reference-to-static-constexpr-char

const uint8_t account_object::space_id;
const uint8_t account_object::type_id;

const uint8_t block_summary_object::space_id;
const uint8_t block_summary_object::type_id;

const uint8_t transaction_object::space_id;
const uint8_t transaction_object::type_id;

const uint8_t witness_object::space_id;
const uint8_t witness_object::type_id;


database::database()
{
   initialize_indexes();
   initialize_evaluators();
}

database::~database()
{
   clear_pending();
}

void database::open( const fc::path& data_dir, uint64_t initial_supply )
{
   try
   {
      object_database::open(data_dir);

      _block_id_to_block.open(data_dir / "database" / "block_num_to_block");

      if( !find(dynamic_global_property_id_type()) )
         init_genesis( initial_supply );

      fc::optional<signed_block> last_block = _block_id_to_block.last();
      if( last_block.valid() )
      {
         _fork_db.start_block( *last_block );
         idump((last_block->id())(last_block->block_num()));
         if( last_block->id() != head_block_id() )
         {
              FC_ASSERT( head_block_num() == 0, "last block ID does not match current chain state",
                         ("last_block->block_num()",last_block->block_num())( "head_block_num", head_block_num()) );
         }
      }
      //idump((head_block_id())(head_block_num()));
   }
   FC_CAPTURE_LOG_AND_RETHROW( (data_dir) )
}

void database::reindex(fc::path data_dir )
{ try {
   ilog( "reindexing blockchain" );
   wipe(data_dir, false);
   open(data_dir);

   auto start = fc::time_point::now();
   auto last_block = _block_id_to_block.last();
   if( !last_block ) {
      elog( "!no last block" );
      edump((last_block));
      return;
   }

   const auto last_block_num = last_block->block_num();

   ilog( "Replaying blocks..." );
   _undo_db.disable();

   for( uint32_t i = 1; i <= last_block_num; ++i )
   {
      if( i % 2000 == 0 ) std::cerr << "   " << double(i*100)/last_block_num << "%   "<<i << " of " <<last_block_num<<"   \n";
      fc::optional< signed_block > block = _block_id_to_block.fetch_by_number(i);
      if( !block.valid() )
      {
         wlog( "Reindexing terminated due to gap:  Block ${i} does not exist!", ("i", i) );
         uint32_t dropped_count = 0;
         while( true )
         {
            fc::optional< block_id_type > last_id = _block_id_to_block.last_id();
            // this can trigger if we attempt to e.g. read a file that has block #2 but no block #1
            if( !last_id.valid() )
               break;
            // we've caught up to the gap
            if( block_header::num_from_id( *last_id ) <= i )
               break;
            _block_id_to_block.remove( *last_id );
            dropped_count++;
         }
         wlog( "Dropped ${n} blocks from after the gap", ("n", dropped_count) );
         break;
      }
      apply_block(*block, skip_witness_signature |
                          skip_transaction_signatures |
                          skip_transaction_dupe_check |
                          skip_tapos_check |
                          skip_witness_schedule_check |
                          skip_authority_check);
   }

   _undo_db.enable();
   auto end = fc::time_point::now();
   ilog( "Done reindexing, elapsed time: ${t} sec", ("t",double((end-start).count())/1000000.0 ) );
} FC_CAPTURE_AND_RETHROW( (data_dir) ) }

void database::wipe(const fc::path& data_dir, bool include_blocks)
{
   ilog("Wiping database", ("include_blocks", include_blocks));
   close();
   object_database::wipe(data_dir);
   if( include_blocks )
      fc::remove_all( data_dir / "database" );
}

void database::close(bool rewind)
{ try {
   if( !_block_id_to_block.is_open() ) return;
   //ilog( "Closing database" );

   // pop all of the blocks that we can given our undo history, this should
   // throw when there is no more undo history to pop
   if( rewind )
   {
      try
      {
         uint32_t cutoff = get_dynamic_global_properties().last_irreversible_block_num;
         //idump((head_block_num()));
         //ilog( "rewinding to last irreversible block number ${c}", ("c",cutoff) );

         clear_pending();
         while( head_block_num() > cutoff )
         {
            block_id_type popped_block_id = head_block_id();
            pop_block();
            _fork_db.remove(popped_block_id); // doesn't throw on missing
            try
            {
               _block_id_to_block.remove(popped_block_id);
            }
            catch (const fc::key_not_found_exception&)
            {
               ilog( "key not found" );
            }
         }
         //idump((head_block_num())(get_dynamic_global_properties().last_irreversible_block_num));
      }
      catch ( const fc::exception& e )
      {
         // ilog( "exception on rewind ${e}", ("e",e.to_detail_string()) );
      }
   }

   //ilog( "Clearing pending state" );
   // Since pop_block() will move tx's in the popped blocks into pending,
   // we have to clear_pending() after we're done popping to get a clean
   // DB state (issue #336).
   clear_pending();

   object_database::flush();
   object_database::close();

   if( _block_id_to_block.is_open() )
      _block_id_to_block.close();

   _fork_db.reset();
} FC_CAPTURE_AND_RETHROW() }

bool database::is_known_block( const block_id_type& id )const
{
   return _fork_db.is_known_block(id) || _block_id_to_block.contains(id);
}
/**
 * Only return true *if* the transaction has not expired or been invalidated. If this
 * method is called with a VERY old transaction we will return false, they should
 * query things by blocks if they are that old.
 */
bool database::is_known_transaction( const transaction_id_type& id )const
{
   const auto& trx_idx = get_index_type<transaction_index>().indices().get<by_trx_id>();
   return trx_idx.find( id ) != trx_idx.end();
}

block_id_type  database::get_block_id_for_num( uint32_t block_num )const
{ try {
   return _block_id_to_block.fetch_block_id( block_num );
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

optional<signed_block> database::fetch_block_by_id( const block_id_type& id )const
{
   auto b = _fork_db.fetch_block( id );
   if( !b )
      return _block_id_to_block.fetch_optional(id);
   return b->data;
}

optional<signed_block> database::fetch_block_by_number( uint32_t num )const
{
   auto results = _fork_db.fetch_block_by_number(num);
   if( results.size() == 1 )
      return results[0]->data;
   else
      return _block_id_to_block.fetch_by_number(num);
   return optional<signed_block>();
}

const signed_transaction& database::get_recent_transaction(const transaction_id_type& trx_id) const
{
   auto& index = get_index_type<transaction_index>().indices().get<by_trx_id>();
   auto itr = index.find(trx_id);
   FC_ASSERT(itr != index.end());
   return itr->trx;
}

std::vector<block_id_type> database::get_block_ids_on_fork(block_id_type head_of_fork) const
{
  pair<fork_database::branch_type, fork_database::branch_type> branches = _fork_db.fetch_branch_from(head_block_id(), head_of_fork);
  if( !((branches.first.back()->previous_id() == branches.second.back()->previous_id())) )
  {
     edump( (head_of_fork)
            (head_block_id())
            (branches.first.size())
            (branches.second.size()) );
     assert(branches.first.back()->previous_id() == branches.second.back()->previous_id());
  }
  std::vector<block_id_type> result;
  for (const item_ptr& fork_block : branches.second)
    result.emplace_back(fork_block->id);
  result.emplace_back(branches.first.back()->previous_id());
  return result;
}

chain_id_type database::get_chain_id() const { return STEEMIT_CHAIN_ID; }

const account_object& database::get_account( const string& name )const
{
   const auto& accounts_by_name = get_index_type<account_index>().indices().get<by_name>();
   auto itr = accounts_by_name.find(name);
   FC_ASSERT(itr != accounts_by_name.end(),
             "Unable to find account '${acct}'. Did you forget to add a record for it?",
             ("acct", name));
   return *itr;
}
const limit_order_object& database::get_limit_order( const string& name, uint16_t orderid )const
{
   const auto& orders_by_account = get_index_type<limit_order_index>().indices().get<by_account>();
   auto itr = orders_by_account.find(boost::make_tuple(name,orderid));
   FC_ASSERT(itr != orders_by_account.end(),
             "Unable to find order '${acct}/${id}'.",
             ("acct", name)("id",orderid));
   return *itr;
}

const witness_object& database::get_witness( const string& name ) const
{
   const auto& witnesses_by_name = get_index_type< witness_index >().indices().get< by_name >();
   auto itr = witnesses_by_name.find( name );
   FC_ASSERT( itr != witnesses_by_name.end(),
              "Unable to find witness account '${wit}'. Did you forget to add a record for it?",
              ( "wit", name ) );
   return *itr;
}
const witness_object* database::find_witness( const string& name ) const
{
   const auto& witnesses_by_name = get_index_type< witness_index >().indices().get< by_name >();
   auto itr = witnesses_by_name.find( name );
   if( itr == witnesses_by_name.end() ) return nullptr;
   return &*itr;
}

const comment_object& database::get_comment( const string& author, const string& permlink )const
{
   const auto& by_permlink_idx = get_index_type< comment_index >().indices().get< by_permlink >();
   auto itr = by_permlink_idx.find( boost::make_tuple( author, permlink ) );
   FC_ASSERT( itr != by_permlink_idx.end() );
   return *itr;
}

void database::pay_fee( const account_object& account, asset fee )
{
   FC_ASSERT( fee.amount >= 0 ); /// NOTE if this fails then validate() on some operation is probably wrong
   if( fee.amount == 0 ) return;

   FC_ASSERT( account.balance >= fee );
   adjust_balance( account, -fee );
   adjust_supply( -fee );
}

void database::update_account_bandwidth( const account_object& a, uint32_t trx_size ) {

   const auto& props = get_dynamic_global_properties();
   if( props.total_vesting_shares.amount > 0 )
   {
      modify( a, [&]( account_object& acnt ) {
          acnt.lifetime_bandwidth += trx_size * STEEMIT_BANDWIDTH_PRECISION;

          auto now = head_block_time();
          auto delta_time = (now - a.last_bandwidth_update).to_seconds();
          uint64_t N = trx_size * STEEMIT_BANDWIDTH_PRECISION;
          if( delta_time >= STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
             acnt.average_bandwidth = N;
          else
          {
             auto old_weight = acnt.average_bandwidth * (STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS - delta_time);
             auto new_weight = delta_time * N;
             acnt.average_bandwidth =  (old_weight + new_weight) / (STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS);
          }

          if( props.total_vesting_shares.amount > 0 )
          {
             FC_ASSERT( a.vesting_shares.amount > 0, "only accounts with a positive vesting balance may transact" );

             fc::uint128 account_vshares(a.vesting_shares.amount.value);
             fc::uint128 total_vshares( props.total_vesting_shares.amount.value );

             fc::uint128 account_average_bandwidth( acnt.average_bandwidth );
             fc::uint128 max_virtual_bandwidth( props.max_virtual_bandwidth );

             FC_ASSERT( (account_vshares * max_virtual_bandwidth) > (account_average_bandwidth * total_vshares),
                        "account exceeded maximum allowed bandwidth per vesting share",
                        ("account_vshares",account_vshares)
                        ("account_average_bandwidth",account_average_bandwidth)
                        ("max_virtual_bandwidth",max_virtual_bandwidth)
                        ("total_vshares",total_vshares) );

          }
          acnt.last_bandwidth_update = now;
      });
   }
}


void database::update_account_market_bandwidth( const account_object& a, uint32_t trx_size ) {

   const auto& props = get_dynamic_global_properties();
   if( props.total_vesting_shares.amount > 0 )
   {
      modify( a, [&]( account_object& acnt ) {
          auto now = head_block_time();
          auto delta_time = (now - a.last_market_bandwidth_update).to_seconds();
          uint64_t N = trx_size * STEEMIT_BANDWIDTH_PRECISION;
          if( delta_time >= STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
             acnt.average_market_bandwidth = N;
          else
          {
             auto old_weight = acnt.average_market_bandwidth * (STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS - delta_time);
             auto new_weight = delta_time * N;
             acnt.average_market_bandwidth =  (old_weight + new_weight) / (STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS);
          }

          if( props.total_vesting_shares.amount > 0 )
          {
             FC_ASSERT( a.vesting_shares.amount > 0, "only accounts with a positive vesting balance may transact" );

             fc::uint128 account_vshares(a.vesting_shares.amount.value);
             fc::uint128 total_vshares( props.total_vesting_shares.amount.value );

             fc::uint128 account_average_bandwidth( acnt.average_market_bandwidth );
             fc::uint128 max_virtual_bandwidth( props.max_virtual_bandwidth / 10 ); /// only 10% of bandwidth can be market
             FC_ASSERT( (account_vshares * max_virtual_bandwidth) > (account_average_bandwidth * total_vshares),
                        "account exceeded maximum allowed market bandwidth per vesting share",
                        ("account_vshares",account_vshares)
                        ("account_average_bandwidth",account_average_bandwidth)
                        ("max_virtual_bandwidth",max_virtual_bandwidth)
                        ("total_vshares",total_vshares) );

          }
          acnt.last_market_bandwidth_update = now;
      });
   }
}





uint32_t database::witness_participation_rate()const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   return uint64_t(STEEMIT_100_PERCENT) * dpo.recent_slots_filled.popcount() / 128;
}

void database::add_checkpoints( const flat_map<uint32_t,block_id_type>& checkpts )
{
   for( const auto& i : checkpts )
      _checkpoints[i.first] = i.second;
}

bool database::before_last_checkpoint()const
{
   return (_checkpoints.size() > 0) && (_checkpoints.rbegin()->first >= head_block_num());
}

bool database::push_block(const signed_block& new_block, uint32_t skip)
{
   bool result;
   detail::with_skip_flags( *this, skip, [&]()
   {
      detail::without_pending_transactions( *this, std::move(_pending_tx),
      [&]()
      {
         result = _push_block(new_block);
      });
   });
   return result;
}

bool database::_push_block(const signed_block& new_block)
{ try {
   uint32_t skip = get_node_properties().skip_flags;
   if( !(skip&skip_fork_db) )
   {
      shared_ptr<fork_item> new_head = _fork_db.push_block(new_block);
      //If the head block from the longest chain does not build off of the current head, we need to switch forks.
      if( new_head->data.previous != head_block_id() )
      {
         //If the newly pushed block is the same height as head, we get head back in new_head
         //Only switch forks if new_head is actually higher than head
         if( new_head->data.block_num() > head_block_num() )
         {
            // wlog( "Switching to fork: ${id}", ("id",new_head->data.id()) );
            auto branches = _fork_db.fetch_branch_from(new_head->data.id(), head_block_id());

            // pop blocks until we hit the forked block
            while( head_block_id() != branches.second.back()->data.previous )
               pop_block();

            // push all blocks on the new fork
            for( auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr )
            {
                // ilog( "pushing blocks from fork ${n} ${id}", ("n",(*ritr)->data.block_num())("id",(*ritr)->data.id()) );
                optional<fc::exception> except;
                try {
                   undo_database::session session = _undo_db.start_undo_session();
                   apply_block( (*ritr)->data, skip );
                   _block_id_to_block.store( (*ritr)->id, (*ritr)->data );
                   session.commit();
                }
                catch ( const fc::exception& e ) { except = e; }
                if( except )
                {
                   // wlog( "exception thrown while switching forks ${e}", ("e",except->to_detail_string() ) );
                   // remove the rest of branches.first from the fork_db, those blocks are invalid
                   while( ritr != branches.first.rend() )
                   {
                      _fork_db.remove( (*ritr)->data.id() );
                      ++ritr;
                   }
                   _fork_db.set_head( branches.second.front() );

                   // pop all blocks from the bad fork
                   while( head_block_id() != branches.second.back()->data.previous )
                      pop_block();

                   // restore all blocks from the good fork
                   for( auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr )
                   {
                      auto session = _undo_db.start_undo_session();
                      apply_block( (*ritr)->data, skip );
                      _block_id_to_block.store( new_block.id(), (*ritr)->data );
                      session.commit();
                   }
                   throw *except;
                }
            }
            return true;
         }
         else return false;
      }
   }

   try {
      auto session = _undo_db.start_undo_session();
      apply_block(new_block, skip);
      _block_id_to_block.store(new_block.id(), new_block);
      session.commit();
   } catch ( const fc::exception& e ) {
      elog("Failed to push new block:\n${e}", ("e", e.to_detail_string()));
      _fork_db.remove(new_block.id());
      throw;
   }

   return false;
} FC_CAPTURE_AND_RETHROW( (new_block) ) }

/**
 * Attempts to push the transaction into the pending queue
 *
 * When called to push a locally generated transaction, set the skip_block_size_check bit on the skip argument. This
 * will allow the transaction to be pushed even if it causes the pending block size to exceed the maximum block size.
 * Although the transaction will probably not propagate further now, as the peers are likely to have their pending
 * queues full as well, it will be kept in the queue to be propagated later when a new block flushes out the pending
 * queues.
 */
void database::push_transaction( const signed_transaction& trx, uint32_t skip )
{ try {
   detail::with_skip_flags( *this, skip, [&]() { _push_transaction( trx ); } );
} FC_CAPTURE_AND_RETHROW( (trx) ) }

void database::_push_transaction( const signed_transaction& trx )
{
   // If this is the first transaction pushed after applying a block, start a new undo session.
   // This allows us to quickly rewind to the clean state of the head block, in case a new block arrives.
   if( !_pending_tx_session.valid() )
      _pending_tx_session = _undo_db.start_undo_session();

   // Create a temporary undo session as a child of _pending_tx_session.
   // The temporary session will be discarded by the destructor if
   // _apply_transaction fails.  If we make it to merge(), we
   // apply the changes.

   auto temp_session = _undo_db.start_undo_session();
   _apply_transaction( trx );
   _pending_tx.push_back( trx );

   notify_changed_objects();
   // The transaction applied successfully. Merge its changes into the pending block session.
   temp_session.merge();

   // notify anyone listening to pending transactions
   on_pending_transaction( trx );
}

signed_block database::generate_block(
   fc::time_point_sec when,
   const string& witness_owner,
   const fc::ecc::private_key& block_signing_private_key,
   uint32_t skip /* = 0 */
   )
{
   signed_block result;
   detail::with_skip_flags( *this, skip, [&]()
   {
      result = _generate_block( when, witness_owner, block_signing_private_key );
   } );
   return result;
}


signed_block database::_generate_block(
   fc::time_point_sec when,
   const string& witness_owner,
   const fc::ecc::private_key& block_signing_private_key
   )
{
   try {
   uint32_t skip = get_node_properties().skip_flags;
   uint32_t slot_num = get_slot_at_time( when );
   FC_ASSERT( slot_num > 0 );
   string scheduled_witness = get_scheduled_witness( slot_num );
   FC_ASSERT( scheduled_witness == witness_owner );

   const auto& witness_obj = get_witness( witness_owner );

   if( !(skip & skip_witness_signature) )
      FC_ASSERT( witness_obj.signing_key == block_signing_private_key.get_public_key() );

   static const size_t max_block_header_size = fc::raw::pack_size( signed_block_header() ) + 4;
   auto maximum_block_size = STEEMIT_MAX_BLOCK_SIZE;
   size_t total_block_size = max_block_header_size;

   signed_block pending_block;

   //
   // The following code throws away existing pending_tx_session and
   // rebuilds it by re-applying pending transactions.
   //
   // This rebuild is necessary because pending transactions' validity
   // and semantics may have changed since they were received, because
   // time-based semantics are evaluated based on the current block
   // time.  These changes can only be reflected in the database when
   // the value of the "when" variable is known, which means we need to
   // re-apply pending transactions in this method.
   //
   _pending_tx_session.reset();
   _pending_tx_session = _undo_db.start_undo_session();

   uint64_t postponed_tx_count = 0;
   // pop pending state (reset to head block state)
   for( const signed_transaction& tx : _pending_tx )
   {
      size_t new_total_size = total_block_size + fc::raw::pack_size( tx );

      // postpone transaction if it would make block too big
      if( new_total_size >= maximum_block_size )
      {
         postponed_tx_count++;
         continue;
      }

      try
      {
         auto temp_session = _undo_db.start_undo_session();
         _apply_transaction( tx );
         temp_session.merge();

         total_block_size += fc::raw::pack_size( tx );
         pending_block.transactions.push_back( tx );
      }
      catch ( const fc::exception& e )
      {
         // Do nothing, transaction will not be re-applied
         wlog( "Transaction was not processed while generating block due to ${e}", ("e", e) );
         wlog( "The transaction was ${t}", ("t", tx) );
      }
   }
   if( postponed_tx_count > 0 )
   {
      wlog( "Postponed ${n} transactions due to block size limit", ("n", postponed_tx_count) );
   }

   _pending_tx_session.reset();

   // We have temporarily broken the invariant that
   // _pending_tx_session is the result of applying _pending_tx, as
   // _pending_tx now consists of the set of postponed transactions.
   // However, the push_block() call below will re-create the
   // _pending_tx_session.

   pending_block.previous = head_block_id();
   pending_block.timestamp = when;
   pending_block.transaction_merkle_root = pending_block.calculate_merkle_root();
   pending_block.witness = witness_owner;

   if( !(skip & skip_witness_signature) )
      pending_block.sign( block_signing_private_key );

   // TODO:  Move this to _push_block() so session is restored.
   if( !(skip & skip_block_size_check) )
   {
      FC_ASSERT( fc::raw::pack_size(pending_block) <= STEEMIT_MAX_BLOCK_SIZE );
   }

   push_block( pending_block, skip );

   return pending_block;
} FC_CAPTURE_AND_RETHROW( (witness_owner) ) }

/**
 * Removes the most recent block from the database and
 * undoes any changes it made.
 */
void database::pop_block()
{ try {
   _pending_tx_session.reset();
   auto head_id = head_block_id();

   /// save the head block so we can recover its transactions
   optional<signed_block> head_block = fetch_block_by_id( head_id );
   STEEMIT_ASSERT( head_block.valid(), pop_empty_chain, "there are no blocks to pop" );

   _fork_db.pop_block();
   _block_id_to_block.remove( head_id );
   pop_undo();

   _popped_tx.insert( _popped_tx.begin(), head_block->transactions.begin(), head_block->transactions.end() );

} FC_CAPTURE_AND_RETHROW() }

void database::clear_pending()
{ try {
   assert( (_pending_tx.size() == 0) || _pending_tx_session.valid() );
   _pending_tx.clear();
   _pending_tx_session.reset();
} FC_CAPTURE_AND_RETHROW() }

void database::push_applied_operation( const operation& op )
{
   operation_object obj;
   obj.trx_id       = _current_trx_id;
   obj.block        = _current_block_num;
   obj.trx_in_block = _current_trx_in_block;
   obj.op_in_trx    = _current_op_in_trx;
   obj.virtual_op   = _current_virtual_op++;
   obj.op           = op;
   on_applied_operation( obj );
}

string database::get_scheduled_witness( uint32_t slot_num )const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);
   uint64_t current_aslot = dpo.current_aslot + slot_num;
   return wso.current_shuffled_witnesses[ current_aslot % wso.current_shuffled_witnesses.size() ];
}

fc::time_point_sec database::get_slot_time(uint32_t slot_num)const
{
   if( slot_num == 0 )
      return fc::time_point_sec();

   auto interval = STEEMIT_BLOCK_INTERVAL;
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   if( head_block_num() == 0 )
   {
      // n.b. first block is at genesis_time plus one block interval
      fc::time_point_sec genesis_time = dpo.time;
      return genesis_time + slot_num * interval;
   }

   int64_t head_block_abs_slot = head_block_time().sec_since_epoch() / interval;
   fc::time_point_sec head_slot_time( head_block_abs_slot * interval );

   // "slot 0" is head_slot_time
   // "slot 1" is head_slot_time,
   //   plus maint interval if head block is a maint block
   //   plus block interval if head block is not a maint block
   return head_slot_time + (slot_num * interval);
}

uint32_t database::get_slot_at_time(fc::time_point_sec when)const
{
   fc::time_point_sec first_slot_time = get_slot_time( 1 );
   if( when < first_slot_time )
      return 0;
   return (when - first_slot_time).to_seconds() / STEEMIT_BLOCK_INTERVAL + 1;
}

/**
 *  Converts STEEM into sbd and adds it to to_account while reducing the STEEM supply
 *  by STEEM and increasing the sbd supply by the specified amount.
 */
asset database::create_sbd( const account_object& to_account, asset steem ) {
try {
  if( steem.amount == 0 )
     return asset(0, SBD_SYMBOL);

  const auto& median_price = get_feed_history().current_median_history;
  if( !median_price.is_null() ) {
     auto sbd = steem * median_price;

     adjust_balance( to_account, sbd );
     adjust_supply( -steem );
     adjust_supply( sbd );

     return sbd;
  } else {
     adjust_balance( to_account, steem );
     return steem;
  }
} FC_CAPTURE_AND_RETHROW( (to_account.name)(steem) ) }

/**
 * @param to_account - the account to receive the new vesting shares
 * @param STEEM - STEEM to be converted to vesting shares
 */
asset database::create_vesting( const account_object& to_account, asset steem ) {
try {
   const auto& cprops = get_dynamic_global_properties();

   /**
    *  The ratio of total_vesting_shares / total_vesting_fund_steem should not
    *  change as the result of the user adding funds
    *
    *  V / C  = (V+Vn) / (C+Cn)
    *
    *  Simplifies to Vn = (V * Cn ) / C
    *
    *  If Cn equals o.amount, then we must solve for Vn to know how many new vesting shares
    *  the user should receive.
    *
    *  128 bit math is requred due to multiplying of 64 bit numbers. This is done in asset and price.
    */
   asset new_vesting = steem * cprops.get_vesting_share_price();

   modify( to_account, [&]( account_object& to ) {
      to.vesting_shares += new_vesting;
   });

   modify( cprops, [&]( dynamic_global_property_object& props ) {
       props.total_vesting_fund_steem += steem;
       props.total_vesting_shares += new_vesting;
   });

   adjust_witness_votes( to_account, new_vesting.amount );

   return new_vesting;
} FC_CAPTURE_AND_RETHROW( (to_account.name)(steem) ) }


fc::sha256 database::get_pow_target()const {
   const auto& dgp = get_dynamic_global_properties();
   fc::sha256 target;
   target._hash[0] = -1;
   target._hash[1] = -1;
   target._hash[2] = -1;
   target._hash[3] = -1;
   target = target >> ((dgp.num_pow_witnesses/4)+4);
   return target;
}

/**
 *
 *  See @ref witness_object::virtual_last_update
 */
void database::update_witness_schedule()
{
      const auto& props = get_dynamic_global_properties();
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);
   //idump((wso.next_shuffle_block_num)(props.num_pow_witnesses) );
   if( (head_block_num() % STEEMIT_MAX_MINERS) == 0 ) //wso.next_shuffle_block_num )
   {
      vector<string> active_witnesses;
      active_witnesses.reserve( STEEMIT_MAX_MINERS );

      fc::uint128 new_virtual_time;

      /// only use vote based scheduling after the first 1M STEEM is created or if there is no POW queued
      if( props.num_pow_witnesses == 0 || head_block_num() > STEEMIT_START_MINER_VOTING_BLOCK )
      {
         const auto& widx = get_index_type<witness_index>().indices().get<by_vote_name>();

         for( auto itr = widx.begin(); itr != widx.end(); ++itr ) {
            if( itr->pow_worker ) continue;
            if( active_witnesses.size() == STEEMIT_MAX_MINERS )
            {
               new_virtual_time = itr->virtual_scheduled_time;
               active_witnesses.push_back(itr->owner);
               break;
            }
            active_witnesses.push_back(itr->owner);
         }

         int count = 0;
         for( auto itr = widx.begin(); itr != widx.end(); ++itr ) {
            modify( *itr, [&]( witness_object& witness ){
              witness.virtual_position        = fc::uint128();
              witness.virtual_scheduled_time  = new_virtual_time + (fc::uint128(STEEMIT_MAX_SHARE_SUPPLY)*1000) / (witness.votes.value+1);
            });
            ++count;
            if( count == active_witnesses.size() )
               break;
         }
      }

      /// Add the next POW witness to the active set if there is one...
      const auto& pow_idx = get_index_type<witness_index>().indices().get<by_pow>();

      auto itr = pow_idx.upper_bound(0);
      /// if there is more than 1 POW witness, then pop the first one from the queue...
      if( props.num_pow_witnesses > STEEMIT_MAX_MINERS ) {
         if( itr != pow_idx.end() ) {
         //   elog( "unscheduling miner ${m} at end of round", ("m",itr->owner) );
            modify( *itr, [&](witness_object& wit ){
               wit.pow_worker = 0;
            });
            modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& obj ){
                obj.num_pow_witnesses--;
            });
         }
      }

      /// add all of the pow witnesses to the round until voting takes over, then only add one per round
      itr = pow_idx.upper_bound(0);
      while( itr != pow_idx.end() ) {
       //  ilog( "scheduling miner ${m}", ("m",itr->owner) );
         active_witnesses.push_back( itr->owner );

         /// after we hit 1M STEEM only include one miner per round
         if( head_block_num() > STEEMIT_START_MINER_VOTING_BLOCK ||
             active_witnesses.size() >= STEEMIT_MAX_MINERS )
            break;
         ++itr;
      }
      //wdump((active_witnesses));

      modify( wso, [&]( witness_schedule_object& _wso )
      {
         _wso.current_shuffled_witnesses.clear();
         _wso.current_shuffled_witnesses.reserve( active_witnesses.size() );

         for( const string& w : active_witnesses )
            _wso.current_shuffled_witnesses.push_back( w );

         auto now_hi = uint64_t(head_block_time().sec_since_epoch()) << 32;
         for( uint32_t i = 0; i < _wso.current_shuffled_witnesses.size(); ++i )
         {
            /// High performance random generator
            /// http://xorshift.di.unimi.it/
            uint64_t k = now_hi + uint64_t(i)*2685821657736338717ULL;
            k ^= (k >> 12);
            k ^= (k << 25);
            k ^= (k >> 27);
            k *= 2685821657736338717ULL;

            uint32_t jmax = _wso.current_shuffled_witnesses.size() - i;
            uint32_t j = i + k%jmax;
            std::swap( _wso.current_shuffled_witnesses[i],
                       _wso.current_shuffled_witnesses[j] );
         }

         if( props.num_pow_witnesses == 0 || head_block_num() > STEEMIT_START_MINER_VOTING_BLOCK )
            _wso.current_virtual_time = new_virtual_time;

         _wso.next_shuffle_block_num = head_block_num() + _wso.current_shuffled_witnesses.size();
      });

      /// fetch all witness objects
      vector<const witness_object*> active; active.reserve( wso.current_shuffled_witnesses.size() );
      for( const auto& wname : wso.current_shuffled_witnesses ) { active.push_back(&get_witness(wname)); }

      /// sort them by account_creation_fee
      std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b ){
                 return a->props.account_creation_fee.amount < b->props.account_creation_fee.amount;
                 });

      modify( wso, [&]( witness_schedule_object& _wso ) {
        _wso.median_props.account_creation_fee = active[active.size()/2]->props.account_creation_fee;
      });


      /// sort them by maximum_block_size
      std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b ){
                 return a->props.maximum_block_size < b->props.maximum_block_size;
                 });

      modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& p ){
            p.maximum_block_size = active[active.size()/2]->props.maximum_block_size;
      });


      /// sort them by sbd_interest_rate
      std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b ){
                 return a->props.sbd_interest_rate < b->props.sbd_interest_rate;
                 });

      modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& p ){
            p.sbd_interest_rate = active[active.size()/2]->props.sbd_interest_rate;
      });
   }
}

void database::adjust_witness_votes( const account_object& a, share_type delta, int depth ) {
  if( a.proxy.size() ) {
     const auto& proxy = get_account( a.proxy );

     modify( proxy, [&]( account_object& a ) {
        a.proxied_vsf_votes += delta;
     });

     /// nested proxies are not supported, vote will not propagate
     if( depth > STEEMIT_MAX_PROXY_RECURSION_DEPTH )
        return;

     adjust_witness_votes( proxy, delta, depth + 1 );
  } else {
     const witness_schedule_object& wso = witness_schedule_id_type()(*this);

     const auto& vidx = get_index_type<witness_vote_index>().indices().get<by_account_witness>();
     auto itr = vidx.lower_bound( boost::make_tuple( a.get_id(), witness_id_type() ) );
     while( itr != vidx.end() && itr->account == a.get_id() ) {
        modify( itr->witness(*this), [&]( witness_object& w ){
          w.virtual_position += w.votes.value * (wso.current_virtual_time - w.virtual_last_update);
          w.virtual_last_update = wso.current_virtual_time;
          w.votes += delta;
          w.virtual_scheduled_time = w.virtual_last_update + (1000*STEEMIT_MAX_SHARE_SUPPLY - w.virtual_position)/(w.votes.value+1);
        });
        ++itr;
     }
  }
}

void database::clear_witness_votes( const account_object& a ) {
  const auto& vidx = get_index_type<witness_vote_index>().indices().get<by_account_witness>();
  auto itr = vidx.lower_bound( boost::make_tuple( a.get_id(), witness_id_type() ) );
  while( itr != vidx.end() && itr->account == a.get_id() ) {
    const auto& current = *itr;
    ++itr;
    remove(current);
  }
}

void database::adjust_rshares2( const comment_object& c, fc::uint128_t old_rshares2, fc::uint128_t new_rshares2 ) {

   modify( c, [&](comment_object& comment ){
      comment.children_rshares2 -= old_rshares2;
      comment.children_rshares2 += new_rshares2;
   });
   if( c.depth ) {
      adjust_rshares2( get_comment( c.parent_author, c.parent_permlink ), old_rshares2, new_rshares2 );
   }
}

void database::process_vesting_withdrawals() {
   const auto& widx = get_index_type<account_index>().indices().get<by_next_vesting_withdrawal>();
   auto current = widx.begin();

   const auto& cprops = get_dynamic_global_properties();

   while( current != widx.end() && current->next_vesting_withdrawal <= head_block_time() ) {
      const auto& cur = *current; ++current;

      share_type withdrawn_vesting;
      if ( cur.to_withdraw - cur.withdrawn < cur.vesting_withdraw_rate.amount )
         withdrawn_vesting = std::min( cur.vesting_shares.amount, cur.to_withdraw % cur.vesting_withdraw_rate.amount ).value;
      else
         withdrawn_vesting = std::min( cur.vesting_shares.amount, cur.vesting_withdraw_rate.amount ).value;

      asset converted_steem = asset( withdrawn_vesting, VESTS_SYMBOL ) * cprops.get_vesting_share_price();

      /// TODO: perhaps we should generate a virtual operation at this point in time
      modify( cur, [&]( account_object& a )
      {
         a.vesting_shares.amount -= withdrawn_vesting;
         a.balance += converted_steem;
         a.withdrawn += withdrawn_vesting;

         if ( a.withdrawn >= a.to_withdraw || a.vesting_shares.amount == 0 )
         {
            a.vesting_withdraw_rate.amount = 0;
            a.next_vesting_withdrawal = fc::time_point_sec::maximum();
         }
         else
         {
            a.next_vesting_withdrawal += fc::seconds( STEEMIT_VESTING_WITHDRAW_INTERVAL_SECONDS );
         }
      });

     push_applied_operation( fill_vesting_withdraw_operation( cur.name, asset( withdrawn_vesting, VESTS_SYMBOL ), converted_steem ) );

     modify( cprops, [&]( dynamic_global_property_object& o ){
        o.total_vesting_fund_steem -= converted_steem;
        o.total_vesting_shares.amount -= withdrawn_vesting;
     });

     if( withdrawn_vesting > 0 )
        adjust_witness_votes( cur, -withdrawn_vesting );
   }
}

void database::adjust_total_payout( const comment_object& cur, const asset& sbd_created )
{
   modify( cur, [&]( comment_object& c ) {
      if( c.total_payout_value.symbol == sbd_created.symbol )
         c.total_payout_value += sbd_created;
   });
}

/**
 *  @recursively pays out parent posts
 */
void database::cashout_comment_helper( const comment_object& cur, const comment_object& origin, asset vesting_steem_reward, asset sbd_reward ) {
   const auto& author = get_account( cur.author );
   if( cur.parent_author.size() ) {
      auto parent_vesting_steem_reward = vesting_steem_reward;
      auto parent_sbd_reward = sbd_reward;
      parent_vesting_steem_reward.amount /= 2;
      parent_sbd_reward.amount /= 2;

      vesting_steem_reward -= parent_vesting_steem_reward;
      sbd_reward -= parent_sbd_reward;

      auto vest_created = create_vesting( author, vesting_steem_reward );
      auto sbd_created  = create_sbd( author, sbd_reward );
      adjust_total_payout( cur, sbd_created + to_sbd( vesting_steem_reward ) );

      push_applied_operation( comment_reward_operation( cur.author, cur.permlink, origin.author, origin.permlink, sbd_created, vest_created ) );

      if( sbd_created > asset( 20, SBD_SYMBOL ) )
      {
         const auto& parent = get_comment( cur.parent_author, cur.parent_permlink );
         sbd_created.amount *= 2; 

         cashout_comment_helper( parent, origin, parent_vesting_steem_reward, parent_sbd_reward );
      } else { 

         const auto& parent_author = get_account(cur.parent_author);
         vest_created = create_vesting( parent_author, vesting_steem_reward );
         sbd_created  = create_sbd( parent_author, sbd_reward );

         push_applied_operation( comment_reward_operation( cur.parent_author, cur.parent_permlink, origin.author, origin.permlink, sbd_created, vest_created ) );
         adjust_total_payout( get_comment( cur.parent_author, cur.parent_permlink ), sbd_created + to_sbd( vesting_steem_reward ) );
      }
   } else { 
      auto vest_created = create_vesting( author, vesting_steem_reward );
      auto sbd_created  = create_sbd( author, sbd_reward );

      push_applied_operation( comment_reward_operation( cur.author, cur.permlink, origin.author, origin.permlink, sbd_created, vest_created ) );
      adjust_total_payout( cur, sbd_created + to_sbd( vesting_steem_reward ) );
   }
}

share_type database::pay_curators( const comment_object& c, share_type max_rewards )
{
   u256 total_weight( c.total_vote_weight );
   share_type unclaimed_rewards = max_rewards;

   const auto& cvidx = get_index_type<comment_vote_index>().indices().get<by_comment_weight_voter>();
   auto itr = cvidx.lower_bound( boost::make_tuple( c.id, uint64_t(-1), account_id_type() ) );
   auto end = cvidx.lower_bound( boost::make_tuple( c.id, uint64_t(0), account_id_type() ) );
   while( itr != end ) {
      u256 weight( itr->weight );
      auto claim = static_cast<uint64_t>((max_rewards.value * weight) / total_weight);
      unclaimed_rewards -= claim;
      auto reward = create_vesting( itr->voter(*this), asset( claim, STEEM_SYMBOL ) );
      push_applied_operation( curate_reward_operation( itr->voter(*this).name, reward, c.author, c.permlink ) );
      ++itr;
   }
   if( max_rewards.value - unclaimed_rewards.value )
      modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& p )
      {
         p.total_reward_fund_steem += unclaimed_rewards;
      });

   return unclaimed_rewards;
}

void database::process_comment_cashout() {
   if( head_block_time() < STEEMIT_FIRST_CASHOUT_TIME )
      return;

   const auto& median_price = get_feed_history().current_median_history;

   const auto& cidx = get_index_type<comment_index>().indices().get<by_cashout_time>();
   auto current = cidx.begin();
   //auto end = cidx.lower_bound( head_block_time() );
   while( current != cidx.end() && current->cashout_time <= head_block_time() ) {
      const auto& cur = *current; ++current;
      asset sbd_created(0,SBD_SYMBOL);
      asset vest_created(0,VESTS_SYMBOL);

      const auto& cat = get_category( cur.category );

      if( cur.net_rshares > 0 ) {
         auto reward_tokens = claim_rshare_reward( cur.net_rshares );
         auto curator_rewards = reward_tokens / 2;
         reward_tokens -= curator_rewards;

         if( reward_tokens > 0 ) {
            auto to_sbd     = reward_tokens / 2;
            auto to_vesting = reward_tokens - to_sbd;

            modify( cat, [&]( category_object& c ) {
               c.total_payouts += asset(reward_tokens,STEEM_SYMBOL) * median_price;
            });

            pay_curators( cur, curator_rewards );
            cashout_comment_helper( cur, cur, asset( to_vesting, STEEM_SYMBOL ), asset( to_sbd, STEEM_SYMBOL ) );
         }
         fc::uint128_t old_rshares2(cur.net_rshares.value);
         old_rshares2 *= old_rshares2;

         adjust_rshares2( cur, old_rshares2, 0 );
      }

      modify( cat, [&]( category_object& c ) {
         c.abs_rshares -= cur.abs_rshares;
         c.last_update  = head_block_time();
      });


      modify( cur, [&]( comment_object& c ) {
         if( c.net_rshares > 0 )
             c.net_rshares = 0;
         c.abs_rshares  = 0;
         c.cashout_time = fc::time_point_sec::maximum();
      });

      const auto& vote_idx = get_index_type<comment_vote_index>().indices().get<by_comment_voter>();
      auto vote_itr = vote_idx.lower_bound( comment_id_type(cur.id) );
      while( vote_itr != vote_idx.end() && vote_itr->comment == cur.id ) {
         const auto& cur_vote = *vote_itr;
         ++vote_itr;
         remove(cur_vote);
      }
   }
}

void database::process_funds() {
   const auto& props = get_dynamic_global_properties();

   auto content_reward = get_content_reward();
   auto curate_reward = get_curation_reward();
   auto witness_pay = get_producer_reward();
   auto vesting_reward = content_reward + curate_reward + witness_pay;

   if( props.head_block_number < STEEMIT_START_VESTING_BLOCK )
      vesting_reward.amount = 0;
   else
      vesting_reward.amount.value *= 9;

   modify( props, [&]( dynamic_global_property_object& p ) {
       p.total_vesting_fund_steem += vesting_reward;
       p.total_reward_fund_steem  += content_reward + curate_reward;
       p.current_supply += content_reward + curate_reward + witness_pay + vesting_reward;
       p.virtual_supply += content_reward + curate_reward + witness_pay + vesting_reward;
   });
}

asset database::get_liquidity_reward()const  {
   const auto& props = get_dynamic_global_properties();
   asset percent( (STEEMIT_LIQUIDITY_APR * props.virtual_supply.amount.value * STEEMIT_LIQUIDITY_REWARD_BLOCKS) / (100*STEEMIT_BLOCKS_PER_YEAR), STEEM_SYMBOL);
   return std::max( percent, STEEMIT_MIN_LIQUIDITY_REWARD );
}

asset database::get_content_reward()const  {
   const auto& props = get_dynamic_global_properties();
   asset percent( (STEEMIT_CONTENT_APR * props.virtual_supply.amount.value) / (100*STEEMIT_BLOCKS_PER_YEAR), STEEM_SYMBOL);
   return std::max( percent, STEEMIT_MIN_CONTENT_REWARD );
}

asset database::get_curation_reward()const {
   const auto& props = get_dynamic_global_properties();
   asset percent( (STEEMIT_CURATE_APR * props.virtual_supply.amount.value) / (100*STEEMIT_BLOCKS_PER_YEAR), STEEM_SYMBOL);
   return std::max( percent, STEEMIT_MIN_CURATE_REWARD );
}

asset database::get_producer_reward() {
   const auto& props = get_dynamic_global_properties();
   asset percent( (STEEMIT_PRODUCER_APR * props.virtual_supply.amount.value) / (100*STEEMIT_BLOCKS_PER_YEAR), STEEM_SYMBOL);
   auto pay = std::max( percent, STEEMIT_MIN_PRODUCER_REWARD );
   const auto& witness_account = get_account( props.current_witness );

   /// pay witness in vesting shares
   if( props.head_block_number >= STEEMIT_START_MINER_VOTING_BLOCK || (witness_account.vesting_shares.amount.value == 0) )
      create_vesting( witness_account, pay );
   else
      modify( get_account( witness_account.name), [&]( account_object& a )
      {
         a.balance += pay;
      });

   return pay;
}

asset database::get_pow_reward()const {
   const auto& props = get_dynamic_global_properties();

#if !IS_TEST_NET
   /// 0 block rewards until at least STEEMIT_MAX_MINERS have produced a POW
   if( props.num_pow_witnesses < STEEMIT_MAX_MINERS && props.head_block_number < STEEMIT_START_VESTING_BLOCK )
      return asset( 0, STEEM_SYMBOL );
#endif

   asset percent( (STEEMIT_POW_APR * props.virtual_supply.amount.value) / (100*STEEMIT_BLOCKS_PER_YEAR), STEEM_SYMBOL);
   return std::max( percent, STEEMIT_MIN_POW_REWARD );
}


void database::pay_liquidity_reward() {
   if( (head_block_num() % STEEMIT_LIQUIDITY_REWARD_BLOCKS) == 0 ) {
      const auto& ridx = get_index_type<liquidity_reward_index>().indices().get<by_volume_weight>();
      auto itr = ridx.begin();
      if( itr != ridx.end() && itr->volume_weight() > 0 ) {
         auto reward = get_liquidity_reward();
         adjust_supply( reward, true );
         adjust_balance( itr->owner(*this), reward );
         modify( *itr, [&]( liquidity_reward_balance_object& obj ){
            obj.steem_volume = 0;
            obj.sbd_volume   = 0;
            obj.last_update  = head_block_time();
         });
         push_applied_operation( liquidity_reward_operation( itr->owner( *this ).name, reward ) );
      }
   }
}

void database::process_conversions() {
   auto now = head_block_time();
   const auto& request_by_date = get_index_type<convert_index>().indices().get<by_conversion_date>();
   auto itr = request_by_date.begin();

   const auto& fhistory = get_feed_history();
   if( fhistory.current_median_history.is_null() )
      return;

   asset net_sbd( 0, SBD_SYMBOL );
   asset net_steem( 0, STEEM_SYMBOL );

   while( itr != request_by_date.end() && itr->conversion_date <= now ) {
      const auto& user = get_account( itr->owner );
      auto amount_to_issue = itr->amount * fhistory.current_median_history;

      adjust_balance( user, amount_to_issue );

      net_sbd   += itr->amount;
      net_steem += amount_to_issue;

      push_applied_operation( fill_convert_request_operation ( user.name, itr->requestid, itr->amount, amount_to_issue ) );

      remove( *itr );
      itr = request_by_date.begin();
   }

   const auto& props = get_dynamic_global_properties();
   modify( props, [&]( dynamic_global_property_object& p ){
       p.current_supply += net_steem;
       p.current_sbd_supply -= net_sbd;
       p.virtual_supply += net_steem;
       p.virtual_supply -= net_sbd * get_feed_history().current_median_history;
   });
}

asset database::to_sbd( const asset& steem )const {
   FC_ASSERT( steem.symbol == STEEM_SYMBOL );
   const auto& feed_history = get_feed_history();
   if( feed_history.current_median_history.is_null() )
      return asset( 0, SBD_SYMBOL );

   return steem * feed_history.current_median_history;
}

asset database::to_steem( const asset& sbd )const {
   FC_ASSERT( sbd.symbol == SBD_SYMBOL );
   const auto& feed_history = get_feed_history();
   if( feed_history.current_median_history.is_null() )
      return asset( 0, STEEM_SYMBOL );

   return sbd * feed_history.current_median_history;
}

share_type database::claim_rshare_reward( share_type rshares ) {
   FC_ASSERT( rshares > 0 );

   const auto& props = get_dynamic_global_properties();

   fc::uint128 rs(rshares.value);
   fc::uint128 rf(props.total_reward_fund_steem.amount.value);

   auto rs2 = rs*rs;

   auto payout = (rf * rs2 / props.total_reward_shares2).to_uint64();

   asset sbd_payout_value = to_sbd( asset(payout, STEEM_SYMBOL) );

   if( sbd_payout_value < STEEMIT_MIN_PAYOUT_SBD )
      payout = 0;

   modify( props, [&]( dynamic_global_property_object& p ){
     p.total_reward_shares2 -= rs2;
     p.total_reward_fund_steem.amount -= payout;
   });

   return payout;
}

const dynamic_global_property_object&database::get_dynamic_global_properties() const
{
   return get( dynamic_global_property_id_type() );
}

const node_property_object& database::get_node_properties() const
{
   return _node_property_object;
}

time_point_sec database::head_block_time()const
{
   return get( dynamic_global_property_id_type() ).time;
}

uint32_t database::head_block_num()const
{
   return get( dynamic_global_property_id_type() ).head_block_number;
}

block_id_type database::head_block_id()const
{
   return get( dynamic_global_property_id_type() ).head_block_id;
}

node_property_object& database::node_properties()
{
   return _node_property_object;
}

uint32_t database::last_non_undoable_block_num() const
{
   return head_block_num() - _undo_db.size();
}

void database::initialize_evaluators()
{
    _operation_evaluators.resize(255);

    register_evaluator<vote_evaluator>();
    register_evaluator<comment_evaluator>();
    register_evaluator<transfer_evaluator>();
    register_evaluator<transfer_to_vesting_evaluator>();
    register_evaluator<withdraw_vesting_evaluator>();
    register_evaluator<account_create_evaluator>();
    register_evaluator<account_update_evaluator>();
    register_evaluator<witness_update_evaluator>();
    register_evaluator<account_witness_vote_evaluator>();
    register_evaluator<account_witness_proxy_evaluator>();
    register_evaluator<custom_evaluator>();
    register_evaluator<pow_evaluator>();
    register_evaluator<report_over_production_evaluator>();

    register_evaluator<feed_publish_evaluator>();
    register_evaluator<convert_evaluator>();
    register_evaluator<limit_order_create_evaluator>();
    register_evaluator<limit_order_cancel_evaluator>();
}

void database::initialize_indexes()
{
   reset_indexes();
   _undo_db.set_max_size( STEEMIT_MIN_UNDO_HISTORY );

   //Protocol object indexes
   auto acnt_index = add_index< primary_index<account_index> >();
   acnt_index->add_secondary_index<account_member_index>();

   add_index< primary_index<witness_index> >();
   add_index< primary_index<witness_vote_index> >();
   add_index< primary_index<category_index> >();
   add_index< primary_index<comment_index> >();
   add_index< primary_index<comment_vote_index> >();
   add_index< primary_index<convert_index> >();
   add_index< primary_index<liquidity_reward_index> >();
   add_index< primary_index<limit_order_index> >();

   //Implementation object indexes
   add_index< primary_index<transaction_index                             > >();
   add_index< primary_index<simple_index<dynamic_global_property_object  >> >();
   add_index< primary_index<simple_index<feed_history_object  >> >();
   add_index< primary_index<flat_index<  block_summary_object            >> >();
   add_index< primary_index<simple_index<witness_schedule_object        > > >();
}

void database::init_genesis( uint64_t init_supply )
{ try {
   _undo_db.disable();
   struct auth_inhibitor {
      auth_inhibitor(database& db) : db(db), old_flags(db.node_properties().skip_flags)
      { db.node_properties().skip_flags |= skip_authority_check; }
      ~auth_inhibitor()
      { db.node_properties().skip_flags = old_flags; }
   private:
      database& db;
      uint32_t old_flags;
   } inhibitor(*this);

   transaction_evaluation_state genesis_eval_state(this);

   flat_index<block_summary_object>& bsi = get_mutable_index_type< flat_index<block_summary_object> >();
   bsi.resize(0xffff+1);

   // Create blockchain accounts
   public_key_type      init_public_key(STEEMIT_INIT_PUBLIC_KEY);

   create<account_object>([this](account_object& a) {
       a.name = STEEMIT_MINER_ACCOUNT;
       a.owner.weight_threshold = 1;
       a.active.weight_threshold = 1;
   });
   create<account_object>([this](account_object& a) {
       a.name = STEEMIT_NULL_ACCOUNT;
       a.owner.weight_threshold = 1;
       a.active.weight_threshold = 1;
   });
   create<account_object>([this](account_object& a) {
       a.name = STEEMIT_TEMP_ACCOUNT;
       a.owner.weight_threshold = 0;
       a.active.weight_threshold = 0;
   });

   for( int i = 0; i < STEEMIT_NUM_INIT_MINERS; ++i ) {
      create<account_object>([&](account_object& a) {
         a.name = STEEMIT_INIT_MINER_NAME + ( i ? fc::to_string( i ) : std::string() );
         a.owner.weight_threshold = 1;
         a.owner.add_authority( init_public_key, 1 );
         a.active  = a.owner;
         a.posting = a.active;
         a.memo_key = init_public_key;
         a.balance  = asset( i ? 0 : init_supply, STEEM_SYMBOL );
      });

      create<witness_object>([&](witness_object& w ) {
         w.owner        = STEEMIT_INIT_MINER_NAME + ( i ? fc::to_string(i) : std::string() );
         w.signing_key  = init_public_key;
      });
   }

   create<dynamic_global_property_object>([&](dynamic_global_property_object& p) {
      p.current_witness = STEEMIT_INIT_MINER_NAME;
      p.time = STEEMIT_GENESIS_TIME;
      p.recent_slots_filled = fc::uint128::max_value();
      p.current_supply = asset( init_supply, STEEM_SYMBOL );
      p.virtual_supply = p.current_supply;
      p.maximum_block_size = STEEMIT_MAX_BLOCK_SIZE;
   });

   // Nothing to do
   create<feed_history_object>([&](feed_history_object& o) {});
   create<block_summary_object>([&](block_summary_object&) {});

   // Create witness scheduler
   create<witness_schedule_object>([&]( witness_schedule_object& wso )
   {
      wso.current_shuffled_witnesses.push_back(STEEMIT_INIT_MINER_NAME);
   });

   _undo_db.enable();
} FC_CAPTURE_AND_RETHROW() }


void database::validate_transaction( const signed_transaction& trx )
{
   auto session = _undo_db.start_undo_session();
   _apply_transaction( trx );
}

void database::notify_changed_objects()
{ try {
   if( _undo_db.enabled() )
   {
      const auto& head_undo = _undo_db.head();
      vector<object_id_type> changed_ids;  changed_ids.reserve(head_undo.old_values.size());
      for( const auto& item : head_undo.old_values ) changed_ids.push_back(item.first);
      for( const auto& item : head_undo.new_ids ) changed_ids.push_back(item);
      vector<const object*> removed;
      removed.reserve( head_undo.removed.size() );
      for( const auto& item : head_undo.removed )
      {
         changed_ids.push_back( item.first );
         removed.emplace_back( item.second.get() );
      }
      changed_objects(changed_ids);
   }
} FC_CAPTURE_AND_RETHROW() }

//////////////////// private methods ////////////////////

void database::apply_block( const signed_block& next_block, uint32_t skip )
{
   auto block_num = next_block.block_num();
   if( _checkpoints.size() && _checkpoints.rbegin()->second != block_id_type() )
   {
      auto itr = _checkpoints.find( block_num );
      if( itr != _checkpoints.end() )
         FC_ASSERT( next_block.id() == itr->second, "Block did not match checkpoint", ("checkpoint",*itr)("block_id",next_block.id()) );

      if( _checkpoints.rbegin()->first >= block_num )
         skip = ~0;// WE CAN SKIP ALMOST EVERYTHING
   }

   detail::with_skip_flags( *this, skip, [&]()
   {
      _apply_block( next_block );
   } );

   /// check invariants
   validate();
   return;
}

void database::_apply_block( const signed_block& next_block )
{ try {
   uint32_t next_block_num = next_block.block_num();
   uint32_t skip = get_node_properties().skip_flags;

   FC_ASSERT( (skip & skip_merkle_check) || next_block.transaction_merkle_root == next_block.calculate_merkle_root(), "", ("next_block.transaction_merkle_root",next_block.transaction_merkle_root)("calc",next_block.calculate_merkle_root())("next_block",next_block)("id",next_block.id()) );

   const witness_object& signing_witness = validate_block_header(skip, next_block);

   _current_block_num    = next_block_num;
   _current_trx_in_block = 0;

   /// modify current witness so transaction evaluators can know who included the transaction,
   /// this is mostly for POW operations which must pay the current_witness
   modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& dgp ){
      dgp.current_witness = next_block.witness;
   });

   for( const auto& trx : next_block.transactions )
   {
      _current_trx_id = trx.id();
      /* We do not need to push the undo state for each transaction
       * because they either all apply and are valid or the
       * entire block fails to apply.  We only need an "undo" state
       * for transactions when validating broadcast transactions or
       * when building a block.
       */
      apply_transaction( trx, skip );
      ++_current_trx_in_block;
   }

   update_global_dynamic_data(next_block);
   update_signing_witness(signing_witness, next_block);

   update_last_irreversible_block();

   create_block_summary(next_block);
   clear_expired_transactions();
   clear_expired_orders();
   update_witness_schedule();

   update_median_feed();

   process_funds();
   process_conversions();
   process_comment_cashout();
   process_vesting_withdrawals();
   pay_liquidity_reward();

   // notify observers that the block has been applied
   applied_block( next_block ); //emit

   notify_changed_objects();
} //FC_CAPTURE_AND_RETHROW( (next_block.block_num()) )  }
FC_LOG_AND_RETHROW() }
const feed_history_object& database::get_feed_history()const {
   return feed_history_id_type()(*this);
}
const witness_schedule_object& database::get_witness_schedule_object()const {
   return witness_schedule_id_type()(*this);
}

void database::update_median_feed() {
try {
   if( (head_block_num() % STEEMIT_FEED_INTERVAL_BLOCKS) != 0 )
      return;

   auto now = head_block_time();
   const witness_schedule_object& wso = get_witness_schedule_object();
   vector<price> feeds; feeds.reserve( wso.current_shuffled_witnesses.size() );
   for( const auto& w : wso.current_shuffled_witnesses ) {
      const auto& wit = get_witness(w);
      if( wit.last_sbd_exchange_update < now + STEEMIT_MAX_FEED_AGE &&
          !wit.sbd_exchange_rate.is_null() )
      {
         feeds.push_back( wit.sbd_exchange_rate );
      }
   }

   if( feeds.size() >= STEEMIT_MIN_FEEDS ) {
      std::sort( feeds.begin(), feeds.end() );
      auto median_feed = feeds[feeds.size()/2];

      modify( get_feed_history(), [&]( feed_history_object& fho ){
           fho.price_history.push_back( median_feed );
           if( fho.price_history.size() > STEEMIT_FEED_HISTORY_WINDOW )
               fho.price_history.pop_front();

           if( fho.price_history.size() ) {
              std::deque<price> copy = fho.price_history;
              std::sort( copy.begin(), copy.end() ); /// todo: use nth_item
              fho.current_median_history = copy[copy.size()/2];
           }
      });
   }
} FC_CAPTURE_AND_RETHROW() }

void database::apply_transaction(const signed_transaction& trx, uint32_t skip)
{
   detail::with_skip_flags( *this, skip, [&]() { _apply_transaction(trx); });
}

void database::_apply_transaction(const signed_transaction& trx)
{ try {
   uint32_t skip = get_node_properties().skip_flags;

   if( true || !(skip&skip_validate) )   /* issue #505 explains why this skip_flag is disabled */
      trx.validate();

   auto& trx_idx = get_mutable_index_type<transaction_index>();
   const chain_id_type& chain_id = STEEMIT_CHAIN_ID;
   auto trx_id = trx.id();
   // idump((trx_id)(skip&skip_transaction_dupe_check));
   FC_ASSERT( (skip & skip_transaction_dupe_check) ||
              trx_idx.indices().get<by_trx_id>().find(trx_id) == trx_idx.indices().get<by_trx_id>().end() );
   transaction_evaluation_state eval_state(this);
   eval_state._trx = &trx;

   if( !(skip & (skip_transaction_signatures | skip_authority_check) ) )
   {
      auto get_active  = [&]( const string& name ) { return &get_account(name).active; };
      auto get_owner   = [&]( const string& name ) { return &get_account(name).owner;  };
      auto get_posting = [&]( const string& name ) { return &get_account(name).posting;  };

      trx.verify_authority( chain_id, get_active, get_owner, get_posting, STEEMIT_MAX_SIG_CHECK_DEPTH );

   }
   flat_set<string> required; vector<authority> other;
   trx.get_required_authorities( required, required, required, other );

   auto trx_size = fc::raw::pack_size(trx);

   for( const auto& auth : required ) {
      update_account_bandwidth( get_account(auth), trx_size );
      for( const auto& op : trx.operations ) {
         if( is_market_operation( op ) )
         {
            update_account_market_bandwidth( get_account(auth), trx_size );
            break;
         }
      }
   }



   //Skip all manner of expiration and TaPoS checking if we're on block 1; It's impossible that the transaction is
   //expired, and TaPoS makes no sense as no blocks exist.
   if( BOOST_LIKELY(head_block_num() > 0) )
   {
      if( !(skip & skip_tapos_check) )
      {
         const auto& tapos_block_summary = block_summary_id_type( trx.ref_block_num )(*this);
         //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
         FC_ASSERT( trx.ref_block_prefix == tapos_block_summary.block_id._hash[1],
                    "", ("trx.ref_block_prefix", trx.ref_block_prefix)
                    ("tapos_block_summary",tapos_block_summary.block_id._hash[1]));
      }

      fc::time_point_sec now = head_block_time();

      FC_ASSERT( trx.expiration <= now + fc::seconds(STEEMIT_MAX_TIME_UNTIL_EXPIRATION), "",
                 ("trx.expiration",trx.expiration)("now",now)("max_til_exp",STEEMIT_MAX_TIME_UNTIL_EXPIRATION));
      FC_ASSERT( now <= trx.expiration, "", ("now",now)("trx.exp",trx.expiration) );
   }

   //Insert transaction into unique transactions database.
   if( !(skip & skip_transaction_dupe_check) )
   {
      create<transaction_object>([&](transaction_object& transaction) {
         transaction.trx_id = trx_id;
         transaction.trx = trx;
      });
   }

   //Finally process the operations
   _current_op_in_trx = 0;
   for( const auto& op : trx.operations )
   {
      apply_operation(eval_state, op);
      ++_current_op_in_trx;
   }

} FC_CAPTURE_AND_RETHROW( (trx) ) }

void database::apply_operation(transaction_evaluation_state& eval_state, const operation& op)
{ try {
   int i_which = op.which();
   uint64_t u_which = uint64_t( i_which );
   if( i_which < 0 )
      assert( "Negative operation tag" && false );
   if( u_which >= _operation_evaluators.size() )
      assert( "No registered evaluator for this operation" && false );
   unique_ptr<op_evaluator>& eval = _operation_evaluators[ u_which ];
   if( !eval )
      assert( "No registered evaluator for this operation" && false );
   push_applied_operation( op );
   eval->evaluate( eval_state, op, true );
} FC_CAPTURE_AND_RETHROW(  ) }

const witness_object& database::validate_block_header( uint32_t skip, const signed_block& next_block )const
{
   FC_ASSERT( head_block_id() == next_block.previous, "", ("head_block_id",head_block_id())("next.prev",next_block.previous) );
   FC_ASSERT( head_block_time() < next_block.timestamp, "", ("head_block_time",head_block_time())("next",next_block.timestamp)("blocknum",next_block.block_num()) );
   const witness_object& witness = get_witness( next_block.witness ); //(*this);

   if( !(skip&skip_witness_signature) )
      FC_ASSERT( next_block.validate_signee( witness.signing_key ) );

   if( !(skip&skip_witness_schedule_check) )
   {
      uint32_t slot_num = get_slot_at_time( next_block.timestamp );
      FC_ASSERT( slot_num > 0 );

      string scheduled_witness = get_scheduled_witness( slot_num );

      FC_ASSERT( witness.owner == scheduled_witness, "Witness produced block at wrong time",
                 ("block witness",next_block.witness)("scheduled",scheduled_witness)("slot_num",slot_num) );
   }

   return witness;
}

void database::create_block_summary(const signed_block& next_block)
{
   block_summary_id_type sid(next_block.block_num() & 0xffff );
   modify( sid(*this), [&](block_summary_object& p) {
         p.block_id = next_block.id();
   });
}

void database::update_global_dynamic_data( const signed_block& b )
{
   auto block_size = fc::raw::pack_size(b);
   const dynamic_global_property_object& _dgp =
      dynamic_global_property_id_type(0)(*this);

   uint32_t missed_blocks = 0;
   if( head_block_time() != fc::time_point_sec() ) {
      missed_blocks = get_slot_at_time( b.timestamp );
      assert( missed_blocks != 0 );
      missed_blocks--;
      for( uint32_t i = 0; i < missed_blocks; ++i ) {
         const auto& witness_missed = get_witness( get_scheduled_witness( i+1 ) );
         if(  witness_missed.owner != b.witness ) {
            modify( witness_missed, [&]( witness_object& w ) {
              w.total_missed++;
            });
         }
      }
   }

   // dynamic global properties updating
   modify( _dgp, [&]( dynamic_global_property_object& dgp ){
      dgp.head_block_number = b.block_num();
      dgp.head_block_id = b.id();
      dgp.time = b.timestamp;
      dgp.virtual_supply = dgp.current_supply
         + ( get_feed_history().current_median_history.is_null() ? asset( 0, STEEM_SYMBOL ) : dgp.current_sbd_supply * get_feed_history().current_median_history );
      dgp.recent_slots_filled = (
           (dgp.recent_slots_filled << 1)
           + 1) << missed_blocks;
      dgp.current_aslot += missed_blocks+1;
      dgp.average_block_size =  (99 * dgp.average_block_size + block_size)/100;

      if( dgp.head_block_number % 20 == 0 ) {
         if( dgp.average_block_size > dgp.maximum_block_size/2 )
         {
           dgp.current_reserve_ratio /= 2; 
         } else { 
           dgp.current_reserve_ratio++;
         }
      }
      dgp.max_virtual_bandwidth = (dgp.maximum_block_size * dgp.current_reserve_ratio *
                                  STEEMIT_BANDWIDTH_PRECISION * STEEMIT_BANDWIDTH_AVERAGE_WINDOW_SECONDS) / STEEMIT_BLOCK_INTERVAL;
   });

   if( !(get_node_properties().skip_flags & skip_undo_history_check) )
   {
      STEEMIT_ASSERT( _dgp.head_block_number - _dgp.last_irreversible_block_num  < STEEMIT_MAX_UNDO_HISTORY, undo_database_exception,
                 "The database does not have enough undo history to support a blockchain with so many missed blocks. "
                 "Please add a checkpoint if you would like to continue applying blocks beyond this point.",
                 ("last_irreversible_block_num",_dgp.last_irreversible_block_num)("head", _dgp.head_block_number)
                 ("max_undo",STEEMIT_MAX_UNDO_HISTORY) );
   }

   _undo_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
   _fork_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
}

void database::update_signing_witness(const witness_object& signing_witness, const signed_block& new_block)
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   uint64_t new_block_aslot = dpo.current_aslot + get_slot_at_time( new_block.timestamp );

   modify( signing_witness, [&]( witness_object& _wit )
   {
      _wit.last_aslot = new_block_aslot;
      _wit.last_confirmed_block_num = new_block.block_num();
   } );
}

void database::update_last_irreversible_block()
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   /**
    * Prior to voting taking over, we must be more conservative...
    *
    */
   if( head_block_num() < STEEMIT_START_MINER_VOTING_BLOCK )
   {
      modify( dpo, [&]( dynamic_global_property_object& _dpo )
      {
         if ( head_block_num() > STEEMIT_MAX_MINERS )
            _dpo.last_irreversible_block_num = head_block_num() - STEEMIT_MAX_MINERS;
      } );
      return;
   }

   const witness_schedule_object& wso = witness_schedule_id_type()(*this);

   vector< const witness_object* > wit_objs;
   wit_objs.reserve( wso.current_shuffled_witnesses.size() );
   for( const string& wid : wso.current_shuffled_witnesses )
      wit_objs.push_back( &get_witness(wid) );

   static_assert( STEEMIT_IRREVERSIBLE_THRESHOLD > 0, "irreversible threshold must be nonzero" );

   // 1 1 1 2 2 2 2 2 2 2 -> 2     .7*10 = 7
   // 1 1 1 1 1 1 1 2 2 2 -> 1
   // 3 3 3 3 3 3 3 3 3 3 -> 3

   size_t offset = ((STEEMIT_100_PERCENT - STEEMIT_IRREVERSIBLE_THRESHOLD) * wit_objs.size() / STEEMIT_100_PERCENT);

   std::nth_element( wit_objs.begin(), wit_objs.begin() + offset, wit_objs.end(),
      []( const witness_object* a, const witness_object* b )
      {
         return a->last_confirmed_block_num < b->last_confirmed_block_num;
      } );

   uint32_t new_last_irreversible_block_num = wit_objs[offset]->last_confirmed_block_num;

   if( new_last_irreversible_block_num > dpo.last_irreversible_block_num )
   {
      modify( dpo, [&]( dynamic_global_property_object& _dpo )
      {
         _dpo.last_irreversible_block_num = new_last_irreversible_block_num;
      } );
   }
}


bool database::apply_order( const limit_order_object& new_order_object )
{
   auto order_id = new_order_object.id;

   const auto& limit_price_idx = get_index_type<limit_order_index>().indices().get<by_price>();

   auto max_price = ~new_order_object.sell_price;
   auto limit_itr = limit_price_idx.lower_bound(max_price.max());
   auto limit_end = limit_price_idx.upper_bound(max_price);

   bool finished = false;
   while( !finished && limit_itr != limit_end )
   {
      auto old_limit_itr = limit_itr;
      ++limit_itr;
      // match returns 2 when only the old order was fully filled. In this case, we keep matching; otherwise, we stop.
      finished = ( match(new_order_object, *old_limit_itr, old_limit_itr->sell_price) & 0x1 );
   }

   return find_object(order_id) == nullptr;
}

int database::match( const limit_order_object& new_order, const limit_order_object& old_order, const price& match_price )
{
   assert( new_order.sell_price.quote.symbol == old_order.sell_price.base.symbol );
   assert( new_order.sell_price.base.symbol  == old_order.sell_price.quote.symbol );
   assert( new_order.for_sale > 0 && old_order.for_sale > 0 );
   assert( match_price.quote.symbol == new_order.sell_price.base.symbol );
   assert( match_price.base.symbol == old_order.sell_price.base.symbol );

   auto new_order_for_sale = new_order.amount_for_sale();
   auto old_order_for_sale = old_order.amount_for_sale();

   asset new_order_pays, new_order_receives, old_order_pays, old_order_receives;

   if( new_order_for_sale <= old_order_for_sale * match_price )
   {
      old_order_receives = new_order_for_sale;
      new_order_receives  = new_order_for_sale * match_price;
   }
   else
   {
      //This line once read: assert( old_order_for_sale < new_order_for_sale * match_price );
      //This assert is not always true -- see trade_amount_equals_zero in operation_tests.cpp
      //Although new_order_for_sale is greater than old_order_for_sale * match_price, old_order_for_sale == new_order_for_sale * match_price
      //Removing the assert seems to be safe -- apparently no asset is created or destroyed.
      new_order_receives = old_order_for_sale;
      old_order_receives = old_order_for_sale * match_price;
   }

   old_order_pays = new_order_receives;
   new_order_pays  = old_order_receives;

   assert( new_order_pays == new_order.amount_for_sale() ||
           old_order_pays == old_order.amount_for_sale() );

   auto age = head_block_time() - old_order.created;
   if( age >= STEEMIT_MIN_LIQUIDITY_REWARD_PERIOD_SEC )
   {
      if( old_order_receives.symbol == STEEM_SYMBOL )
      {
         adjust_liquidity_reward( get_account( old_order.seller ), old_order_receives, false );
         adjust_liquidity_reward( get_account( new_order.seller ), -old_order_receives, false );
      }
      else
      {
         adjust_liquidity_reward( get_account( old_order.seller ), new_order_receives, true );
         adjust_liquidity_reward( get_account( new_order.seller ), -new_order_receives, true );
      }
   }

   int result = 0;
   result |= fill_order( new_order, new_order_pays, new_order_receives );
   result |= fill_order( old_order, old_order_pays, old_order_receives ) << 1;
   assert( result != 0 );
   return result;
}


void database::adjust_liquidity_reward( const account_object& owner, const asset& volume, bool is_sdb ) {
   const auto& ridx = get_index_type<liquidity_reward_index>().indices().get<by_owner>();
   auto itr = ridx.find( owner.id );
   if( itr != ridx.end() ) {
      modify<liquidity_reward_balance_object>( *itr, [&]( liquidity_reward_balance_object& r ){
         if( head_block_time() - r.last_update >= STEEMIT_LIQUIDITY_TIMEOUT_SEC )
         {
            r.sbd_volume = 0;
            r.steem_volume = 0;
         }

         if( is_sdb )
            r.sbd_volume += volume.amount.value;
         else
            r.steem_volume += volume.amount.value;
         r.last_update = head_block_time();
      });
   }
   else {
      create<liquidity_reward_balance_object>( [&](liquidity_reward_balance_object& r ){
         r.owner = owner.id;
         if( is_sdb )
            r.sbd_volume = volume.amount.value;
         else
            r.steem_volume = volume.amount.value;
         r.last_update = head_block_time();
      });
   }

}


bool database::fill_order( const limit_order_object& order, const asset& pays, const asset& receives )
{ try {
   FC_ASSERT( order.amount_for_sale().symbol == pays.symbol );
   FC_ASSERT( pays.symbol != receives.symbol );

   const account_object& seller = get_account( order.seller );

   adjust_balance( seller, receives );

   push_applied_operation( fill_order_operation( order.seller, order.orderid, pays, receives ) );

   if( pays == order.amount_for_sale() )
   {
      remove( order );
      return true;
   }
   else
   {
      modify( order, [&]( limit_order_object& b ) {
                             b.for_sale -= pays.amount;
                          });
      /**
       *  There are times when the AMOUNT_FOR_SALE * SALE_PRICE == 0 which means that we
       *  have hit the limit where the seller is asking for nothing in return.  When this
       *  happens we must refund any balance back to the seller, it is too small to be
       *  sold at the sale price.
       */
      if( order.amount_to_receive().amount == 0 )
      {
         cancel_order(order);
         return true;
      }
      return false;
   }
} FC_CAPTURE_AND_RETHROW( (order)(pays)(receives) ) }

void database::cancel_order( const limit_order_object& order ) {
   adjust_balance( get_account(order.seller), order.amount_for_sale() );
   remove(order);
}


void database::clear_expired_transactions()
{
   //Look for expired transactions in the deduplication list, and remove them.
   //Transactions must have expired by at least two forking windows in order to be removed.
   auto& transaction_idx = static_cast<transaction_index&>(get_mutable_index(implementation_ids, impl_transaction_object_type));
   const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
   while( (!dedupe_index.empty()) && (head_block_time() > dedupe_index.rbegin()->trx.expiration) )
      transaction_idx.remove(*dedupe_index.rbegin());
}

void database::clear_expired_orders() {
   auto now = head_block_time();
   const auto& orders_by_exp = get_index_type<limit_order_index>().indices().get<by_expiration>();
   auto itr = orders_by_exp.begin();
   while( itr != orders_by_exp.end() && itr->expiration < now ) {
      cancel_order( *itr );
      itr = orders_by_exp.begin();
   }
}

void database::adjust_balance( const account_object& a, const asset& delta )
{
   modify( a, [&]( account_object& acnt ){
      switch( delta.symbol ) {
         case STEEM_SYMBOL:
            acnt.balance += delta;
            break;
         case SBD_SYMBOL:
            if( a.sbd_seconds_last_update != head_block_time() )
            {
               acnt.sbd_seconds += fc::uint128_t(a.sbd_balance.amount.value) * (head_block_time() - a.sbd_seconds_last_update).to_seconds();
               acnt.sbd_seconds_last_update = head_block_time();

               if( acnt.sbd_seconds > 0 &&
                   (acnt.sbd_seconds_last_update - acnt.sbd_last_interest_payment).to_seconds() > STEEMIT_SBD_INTEREST_COMPOUND_INTERVAL_SEC )
               {
                  auto interest = acnt.sbd_seconds / STEEMIT_SECONDS_PER_YEAR;
                  interest *= get_dynamic_global_properties().sbd_interest_rate;
                  interest /= STEEMIT_100_PERCENT;
                  asset interest_paid(interest.to_uint64(), SBD_SYMBOL);
                  acnt.sbd_balance += interest_paid;
                  acnt.sbd_seconds = 0;
                  acnt.sbd_last_interest_payment = head_block_time();
                  push_applied_operation( interest_operation( a.name, interest_paid ) );

                  modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& props)
                  {
                     props.current_sbd_supply += interest_paid;
                     props.virtual_supply += interest_paid * get_feed_history().current_median_history;
                  });
               }
            }
            acnt.sbd_balance += delta;
            break;
         default:
            FC_ASSERT( !"invalid symbol" );
      }
   });
}

void database::adjust_supply( const asset& delta, bool adjust_vesting ) {

   const auto& props = get_dynamic_global_properties();
   if( props.head_block_number < STEEMIT_BLOCKS_PER_DAY*7 )
      adjust_vesting = false;

   modify( props, [&]( dynamic_global_property_object& props ){
      switch( delta.symbol ) {
         case STEEM_SYMBOL:
         {
            asset new_vesting( (adjust_vesting && delta.amount > 0) ? delta.amount * 9 : 0, STEEM_SYMBOL );
            props.current_supply += delta + new_vesting;
            props.virtual_supply += delta + new_vesting;
            props.total_vesting_fund_steem += new_vesting;
            assert( props.current_supply.amount.value >= 0 );
            break;
         }
         case SBD_SYMBOL:
            props.current_sbd_supply += delta;
            props.virtual_supply += delta * get_feed_history().current_median_history;
            assert( props.current_sbd_supply.amount.value >= 0 );
            break;
         default:
            FC_ASSERT( !"invalid symbol" );
      }
   });
}


asset database::get_balance( const account_object& a, asset_symbol_type symbol )const
{
   switch( symbol ) {
      case STEEM_SYMBOL:
         return a.balance;
      case SBD_SYMBOL:
         return a.sbd_balance;
      default:
         FC_ASSERT( !"invalid symbol" );
   }
}

/**
 * Verifies all supply invariantes check out
 */
void database::validate( )const
{
   const auto& db = *this;
   try
   {
     // const auto& account_idx = get_index_type< account_index >().indices().get< by_id >();
      const auto& account_idx = get_index_type<account_index>().indices().get<by_name>();
      asset total_supply = asset( 0, STEEM_SYMBOL );
      asset total_sbd = asset( 0, SBD_SYMBOL );
      asset total_vesting = asset( 0, VESTS_SYMBOL );
      share_type total_vsf_votes = share_type( 0 );

      for( auto itr = account_idx.begin(); itr != account_idx.end(); itr++ )
      {
         total_supply += itr->balance;
         total_sbd += itr->sbd_balance;
         total_vesting += itr->vesting_shares;
         total_vsf_votes += itr->proxy == STEEMIT_PROXY_TO_SELF_ACCOUNT ? itr->proxied_vsf_votes + itr->vesting_shares.amount : 0;
      }

      const auto& convert_request_idx = db.get_index_type< convert_index >().indices();

      for( auto itr = convert_request_idx.begin(); itr != convert_request_idx.end(); itr++ )
      {
         if( itr->amount.symbol == STEEM_SYMBOL )
            total_supply += itr->amount;
         else if( itr->amount.symbol == SBD_SYMBOL )
            total_sbd += itr->amount;
         else
            FC_ASSERT( !"Encountered illegal symbol in convert_request_object" );
      }

      const auto& limit_order_idx = db.get_index_type< limit_order_index >().indices();

      for( auto itr = limit_order_idx.begin(); itr != limit_order_idx.end(); itr++ )
      {
         if( itr->sell_price.base.symbol == STEEM_SYMBOL )
         {
            total_supply += asset( itr->for_sale, STEEM_SYMBOL );
         }
         else if ( itr->sell_price.base.symbol == SBD_SYMBOL )
         {
            total_sbd += asset( itr->for_sale, SBD_SYMBOL );
         }
      }

      auto gpo = db.get_dynamic_global_properties();

      total_supply += gpo.total_vesting_fund_steem
         + gpo.total_reward_fund_steem;

      FC_ASSERT( gpo.current_supply.amount.value == total_supply.amount.value );
      FC_ASSERT( gpo.current_sbd_supply.amount.value == total_sbd.amount.value );
      FC_ASSERT( gpo.total_vesting_shares == total_vesting );
      FC_ASSERT( gpo.total_vesting_shares.amount == total_vsf_votes );
      FC_ASSERT( gpo.virtual_supply >= gpo.current_supply );
      if ( !db.get_feed_history().current_median_history.is_null() )
         FC_ASSERT( gpo.current_sbd_supply * db.get_feed_history().current_median_history + gpo.current_supply
            == gpo.virtual_supply );
   }
   FC_LOG_AND_RETHROW();
}

const category_object* database::find_category( const string& name )const
{
   const auto& idx = get_index_type<category_index>().indices().get<by_name>();
   auto itr = idx.find(name);
   if( itr != idx.end() )
      return &*itr;
   return nullptr;
}
const category_object& database::get_category( const string& name )const{
   auto cat = find_category( name );
   FC_ASSERT( cat != nullptr, "Unable to find category ${c}", ("c",name) );
   return *cat;

}

} } //steemit::chain
