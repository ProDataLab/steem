#include <steemit/chain/database.hpp>
#include <steemit/chain/steem_evaluator.hpp>
#include <steemit/chain/steem_objects.hpp>

#include <fc/uint128.hpp>


namespace steemit { namespace chain {
   using fc::uint128_t;

void witness_update_evaluator::do_apply( const witness_update_operation& o )
{
   const auto&  witness_account = db().get_account( o.owner );

   const auto& by_witness_name_idx = db().get_index_type< witness_index >().indices().get< by_name >();
   auto wit_itr = by_witness_name_idx.find( o.owner );
   if( wit_itr != by_witness_name_idx.end() )
   {
      db().modify( *wit_itr, [&]( witness_object& w ) {
         w.url                = o.url;
         w.signing_key        = o.block_signing_key;
         w.props              = o.props;
      });
   }
   else
   {
      db().pay_fee( witness_account, o.fee );

      db().create< witness_object >( [&]( witness_object& w ) {
         w.owner              = o.owner;
         w.url                = o.url;
         w.signing_key        = o.block_signing_key;
         w.created            = db().head_block_time();
         w.props              = o.props;
      });
   }
}

void account_create_evaluator::do_apply( const account_create_operation& o )
{
   const auto& creator = db().get_account( o.creator );

   const auto& props = db().get_dynamic_global_properties();

   FC_ASSERT( creator.balance >= o.fee, "Isufficient balance to create account", ( "creator.balance", creator.balance )( "required", o.fee ) );

   db().modify( creator, [&]( account_object& c ){
      c.balance -= o.fee;
   });

   const auto& new_account = db().create< account_object >( [&]( account_object& acc )
   {
      acc.name = o.new_account_name;
      acc.owner = o.owner;
      acc.active = o.active;
      acc.posting = o.posting;
      acc.memo_key = o.memo_key;
      acc.json_metadata = o.json_metadata;
      acc.created = props.time;
      acc.last_vote_time = props.time;
   });

   if( o.fee.amount > 0 )
      db().create_vesting( new_account, o.fee );
}


void account_update_evaluator::do_apply( const account_update_operation& o )
{
   db().modify( db().get_account( o.account ), [&]( account_object& acc )
   {
      if( o.owner ) acc.owner = *o.owner;
      if( o.active ) acc.active = *o.active;
      if( o.posting ) acc.posting = *o.posting;
      acc.memo_key = o.memo_key;
      acc.json_metadata = o.json_metadata;
   });
}

void comment_evaluator::do_apply( const comment_operation& o )
{ //try {
   const auto& by_permlink_idx = db().get_index_type< comment_index >().indices().get< by_permlink >();
   auto itr = by_permlink_idx.find( boost::make_tuple( o.author, o.permlink ) );

   const auto& auth = db().get_account( o.author ); /// prove it exists

   auto now = db().head_block_time();
   FC_ASSERT( (now - auth.last_post) > fc::seconds(60), "You may only post once per minute" );
   db().modify( auth, [&]( account_object& a ) {
      a.last_post = now;
   });

   comment_id_type id;


   const comment_object* parent = nullptr;
   if( o.parent_author.size() != 0 ) {
      parent = &db().get_comment( o.parent_author, o.parent_permlink );
      FC_ASSERT( parent->depth < STEEMIT_MAX_COMMENT_DEPTH, "Comment is nested ${x} posts deep, maximum depth is ${y}", ("x",parent->depth)("y",STEEMIT_MAX_COMMENT_DEPTH) );
   }

   if ( itr == by_permlink_idx.end() )
   {
      const auto& new_comment = db().create< comment_object >( [&]( comment_object& com )
      {
         if ( o.parent_author.size() == 0 )
         {
            com.parent_author = "";
            com.parent_permlink = o.parent_permlink;
            com.category = o.parent_permlink;
         }
         else
         {
            com.parent_author = parent->author;
            com.parent_permlink = parent->permlink;
            com.depth = parent->depth + 1;
            com.category = parent->category;
         }

         com.author = o.author;
         com.permlink = o.permlink;
         com.title = o.title;
         com.body = o.body;
         com.json_metadata = o.json_metadata;
         com.last_update = db().head_block_time();
         com.created = com.last_update;
         com.cashout_time  = com.last_update + fc::seconds(STEEMIT_CASHOUT_WINDOW_SECONDS);
      });
      const category_object* cat = db().find_category( new_comment.category );
      if( !cat ) {
         cat = &db().create<category_object>( [&]( category_object& c ){
             c.name = new_comment.category;
             c.discussions = 1;
             c.last_update = db().head_block_time();
         });
      } else {
         db().modify( *cat, [&]( category_object& c ){
             c.discussions++;
             c.last_update = db().head_block_time();
         });
      }

      id = new_comment.id;

      /// TODO: this loop can be skiped for validate-only nodes as it is merely gathering stats for indicies
      while( parent ) {
         db().modify( *parent, [&]( comment_object& p ){
            p.children++;
         });
         if( parent->parent_author.size() )
            parent = &db().get_comment( parent->parent_author, parent->parent_permlink );
         else
            parent = nullptr;
      }
   }
   else
   {
      db().modify( *itr, [&]( comment_object& com )
      {
         if( !parent )
         {
            FC_ASSERT( com.parent_author == "" );
            FC_ASSERT( com.parent_permlink == o.parent_permlink, "The permlink of a comment cannot change" );
         }
         else
         {
            FC_ASSERT( com.parent_author == o.parent_author );
            FC_ASSERT( com.parent_permlink == o.parent_permlink );
         }

         com.title = o.title;
         com.body = o.body;
         com.json_metadata = o.json_metadata;
         com.last_update   = db().head_block_time();
         com.net_rshares   = std::min( com.net_rshares, share_type(0) );
         com.abs_rshares   = 0;
         com.cashout_time  = com.last_update + fc::seconds(STEEMIT_CASHOUT_WINDOW_SECONDS);
      });
   }

} //FC_CAPTURE_LOG_AND_RETHROW( (o) ) }

void transfer_evaluator::do_apply( const transfer_operation& o )
{
   const auto& from_account = db().get_account(o.from);
   const auto& to_account = db().get_account(o.to);

   FC_ASSERT( db().get_balance( from_account, o.amount.symbol ) >= o.amount );
   db().adjust_balance( from_account, -o.amount );
   db().adjust_balance( to_account, o.amount );
}

void transfer_to_vesting_evaluator::do_apply( const transfer_to_vesting_operation& o )
{
   const auto& from_account = db().get_account(o.from);
   const auto& to_account = o.to.size() ? db().get_account(o.to) : from_account;

   FC_ASSERT( db().get_balance( from_account, STEEM_SYMBOL) >= o.amount );
   db().adjust_balance( from_account, -o.amount );
   db().create_vesting( to_account, o.amount );
}

void withdraw_vesting_evaluator::do_apply( const withdraw_vesting_operation& o )
{
    const auto& account = db().get_account( o.account );
    FC_ASSERT( account.vesting_shares >= asset( 0, VESTS_SYMBOL ) );
    FC_ASSERT( account.vesting_shares >= o.vesting_shares );
    db().modify( account, [&]( account_object& a ) {
      a.vesting_withdraw_rate = asset( o.vesting_shares.amount / STEEMIT_VESTING_WITHDRAW_INTERVALS, VESTS_SYMBOL );
      if( a.vesting_withdraw_rate.amount == 0 )
         a.vesting_withdraw_rate.amount = 1;

      a.next_vesting_withdrawal = db().head_block_time() + fc::seconds(STEEMIT_VESTING_WITHDRAW_INTERVAL_SECONDS);
      a.to_withdraw = o.vesting_shares.amount;
      a.withdrawn = 0;
    });
}

void account_witness_proxy_evaluator::do_apply( const account_witness_proxy_operation& o )
{
   const auto& account = db().get_account( o.account );
   FC_ASSERT( account.proxy != o.proxy, "something must change" );

   /// remove all current votes
   db().adjust_witness_votes( account, -account.witness_vote_weight() );

   if( o.proxy.size() ) {
      const auto& new_proxy = db().get_account( o.proxy );
      flat_set<account_id_type> proxy_chain({account.get_id(), new_proxy.get_id()});
      proxy_chain.reserve( STEEMIT_MAX_PROXY_RECURSION_DEPTH + 1 );

      /// check for proxy loops and fail to update the proxy if it would create a loop
      auto cprox = &new_proxy;
      while( cprox->proxy.size() != 0 ) {
         const auto next_proxy = db().get_account( cprox->proxy );
         FC_ASSERT( proxy_chain.insert( next_proxy.get_id() ).second, "Attempt to create a proxy loop" );
         cprox = &next_proxy;
         FC_ASSERT( proxy_chain.size() <= STEEMIT_MAX_PROXY_RECURSION_DEPTH, "Proxy chain is too long" );
      }

      /// clear all individual vote records
      db().clear_witness_votes( account );

      db().modify( account, [&]( account_object& a ) {
          a.proxy = o.proxy;
      });

      /// add all new votes
      db().adjust_witness_votes( account, account.witness_vote_weight() );
   } else { /// we are clearing the proxy which means we simply update the account
      db().modify( account, [&]( account_object& a ) {
          a.proxy = o.proxy;
      });
   }
}


void account_witness_vote_evaluator::do_apply( const account_witness_vote_operation& o )
{
   const auto& voter = db().get_account( o.account );
   FC_ASSERT( voter.proxy.size() == 0, "A proxy is currently set, please clear the proxy before voting for a witness" );

   const auto& witness = db().get_witness( o.witness );

   const auto& by_account_witness_idx = db().get_index_type< witness_vote_index >().indices().get< by_account_witness >();
   auto itr = by_account_witness_idx.find( boost::make_tuple( voter.get_id(), witness.get_id() ) );

   if( itr == by_account_witness_idx.end() ) {
      FC_ASSERT( o.approve, "vote doesn't exist, user must be indicate a desire to approve witness" );
      db().create<witness_vote_object>( [&]( witness_vote_object& v ) {
          v.witness = witness.id;
          v.account = voter.id;
      });
      db().modify( witness, [&]( witness_object& w ) {
          w.votes += voter.witness_vote_weight();
      });
   } else {
      FC_ASSERT( !o.approve, "vote currently exists, user must be indicate a desire to approve witness" );

      db().modify( witness, [&]( witness_object& w ) {
          w.votes -= voter.witness_vote_weight();
      });
      db().remove( *itr );
   }
}

void vote_evaluator::do_apply( const vote_operation& o ) {
   const auto& comment = db().get_comment( o.author, o.permlink );
   const auto& voter   = db().get_account( o.voter );


   auto elapsed_seconds   = (db().head_block_time() - voter.last_vote_time).to_seconds();
   auto regenerated_power = ((STEEMIT_100_PERCENT - voter.voting_power) * elapsed_seconds) /  STEEMIT_VOTE_REGENERATION_SECONDS;
   auto current_power     = std::min( int64_t(voter.voting_power + regenerated_power), int64_t(STEEMIT_100_PERCENT) );
   FC_ASSERT( current_power > 0 );

   int64_t  abs_weight    = abs(o.weight);
   auto     used_power    = (current_power * abs_weight) / STEEMIT_100_PERCENT;
   used_power /= 20; /// a 100% vote means use 5% of voting power which should force users to spread their votes around over 20+ posts

   int64_t abs_rshares    = ((uint128_t(voter.vesting_shares.amount.value) * used_power) / STEEMIT_100_PERCENT).to_uint64();
   int64_t rshares = o.weight < 0 ? -abs_rshares : abs_rshares;

   db().modify( voter, [&]( account_object& a ){
      a.voting_power = current_power - used_power;
      a.last_vote_time = db().head_block_time();
   });

   fc::uint128_t old_rshares = std::max(comment.net_rshares.value, int64_t(0));
   auto old_abs_rshares = comment.abs_rshares.value;

   fc::uint128_t cur_cashout_time_sec = comment.cashout_time.sec_since_epoch();
   fc::uint128_t new_cashout_time_sec = db().head_block_time().sec_since_epoch() + STEEMIT_CASHOUT_WINDOW_SECONDS;
   auto avg_cashout_sec = (cur_cashout_time_sec * old_abs_rshares + new_cashout_time_sec * abs_rshares ) / (comment.abs_rshares.value + abs_rshares );

   FC_ASSERT( abs_rshares > 0 );

   db().modify( comment, [&]( comment_object& c ){
      c.net_rshares += rshares;
      c.abs_rshares += abs_rshares;
      c.cashout_time = fc::time_point_sec( ) + fc::seconds(avg_cashout_sec.to_uint64());
   });

   fc::uint128_t new_rshares = std::max(comment.net_rshares.value, int64_t(0));

   /// square it
   new_rshares *= new_rshares;
   old_rshares *= old_rshares;

   const auto& cprops = db().get_dynamic_global_properties();
   db().modify( cprops, [&]( dynamic_global_property_object& p ){
      p.total_reward_shares2 -= old_rshares;
      p.total_reward_shares2 += new_rshares;
   });

   const auto& cat = db().get_category( comment.category );
   db().modify( cat, [&]( category_object& c ){
      c.abs_rshares += abs_rshares;
      c.last_update = db().head_block_time();
   });

   /** this verifies uniqueness of voter */
   const auto& cvo = db().create<comment_vote_object>( [&]( comment_vote_object& cv ){
       cv.voter   = voter.id;
       cv.comment = comment.id;
       if( rshares > 0 ) {
          u256 rshare256(rshares);
          u256 total256( comment.abs_rshares.value );
          cv.weight  = static_cast<uint64_t>((rshare256 * rshare256 * rshare256) / total256);
       }
   });

   db().modify( comment, [&]( comment_object& c ){
      c.total_vote_weight += cvo.weight;
   });

   db().adjust_rshares2( comment, old_rshares, new_rshares );
}

void custom_evaluator::do_apply( const custom_operation& o ){
   /// TODO..... ??
}


void pow_evaluator::do_apply( const pow_operation& o ) {
   const auto& dgp = db().get_dynamic_global_properties();
   FC_ASSERT( db().head_block_time() > STEEMIT_MINING_TIME, "Mining cannot start until ${t}", ("t",STEEMIT_MINING_TIME) );

   const auto& accounts_by_name = db().get_index_type<account_index>().indices().get<by_name>();
   auto itr = accounts_by_name.find(o.worker_account);
   if(itr == accounts_by_name.end()) {
      db().create< account_object >( [&]( account_object& acc )
      {
         acc.name = o.worker_account;
         acc.owner = authority( 1, o.work.worker, 1);
         acc.active = acc.owner;
         acc.posting = acc.owner;
         acc.memo_key = o.work.worker;
         acc.created = dgp.time;
         acc.last_vote_time = dgp.time;
      });
   }

   const auto& worker_account = db().get_account( o.worker_account ); // verify it exists
   FC_ASSERT( worker_account.active.num_auths() == 1, "miners can only have one key auth" );
   FC_ASSERT( worker_account.active.key_auths.size() == 1, "miners may only have one key auth" );
   FC_ASSERT( worker_account.active.key_auths.begin()->first == o.work.worker, "work must be performed by key that signed the work" );
   FC_ASSERT( o.block_id == db().head_block_id() );

   fc::sha256 target = db().get_pow_target();

   FC_ASSERT( o.work.work < target, "work lacks sufficient difficulty" );

   db().modify( dgp, [&]( dynamic_global_property_object& p ){
      p.total_pow += p.num_pow_witnesses;
      p.num_pow_witnesses++;
   });


   const auto cur_witness = db().find_witness( o.worker_account );
   if( cur_witness ) {
      FC_ASSERT( cur_witness->pow_worker == 0, "this account is already scheduled for pow block production" );
      db().modify(*cur_witness, [&]( witness_object& w ){
          w.props             = o.props;
          w.pow_worker        = dgp.total_pow;
      });
   } else {
      db().create<witness_object>( [&]( witness_object& w ) {
          w.owner             = o.worker_account;
          w.props             = o.props;
          w.signing_key       = o.work.worker;
          w.pow_worker        = dgp.total_pow;
      });
   }
   /// POW reward depends upon whether we are before or after MINER_VOTING kicks in
   asset pow_reward = db().get_pow_reward();
   if( db().head_block_num() < STEEMIT_START_MINER_VOTING_BLOCK )
      pow_reward.amount *= STEEMIT_MAX_MINERS;
   db().adjust_supply( pow_reward, true );

   /// pay the witness that includes this POW
   const auto& inc_witness = db().get_account( dgp.current_witness );
   if( db().head_block_num() < STEEMIT_START_MINER_VOTING_BLOCK )
      db().adjust_balance( inc_witness, pow_reward );
   else
      db().create_vesting( inc_witness, pow_reward );
}

void feed_publish_evaluator::do_apply( const feed_publish_operation& o ) {
  const auto& witness = db().get_witness( o.publisher );
  db().modify( witness, [&]( witness_object& w ){
      w.sbd_exchange_rate = o.exchange_rate;
      w.last_sbd_exchange_update = db().head_block_time();
  });
}

void convert_evaluator::do_apply( const convert_operation& o ) {
  const auto& owner = db().get_account( o.owner );
  FC_ASSERT( db().get_balance( owner, o.amount.symbol ) >= o.amount );

  db().adjust_balance( owner, -o.amount );

  const auto& fhistory = db().get_feed_history();
  FC_ASSERT( !fhistory.current_median_history.is_null() );

  db().create<convert_request_object>( [&]( convert_request_object& obj ){
      obj.owner           = o.owner;
      obj.requestid       = o.requestid;
      obj.amount          = o.amount;
      obj.conversion_date = db().head_block_time() + STEEMIT_CONVERSION_DELAY; // 1 week
  });

}

void limit_order_create_evaluator::do_apply( const limit_order_create_operation& o ) {
   FC_ASSERT( o.expiration > db().head_block_time() );

   const auto& owner = db().get_account( o.owner );

   FC_ASSERT( db().get_balance( owner, o.amount_to_sell.symbol ) >= o.amount_to_sell );

   db().adjust_balance( owner, -o.amount_to_sell );

   const auto& order = db().create<limit_order_object>( [&]( limit_order_object& obj ) {
       obj.created    = db().head_block_time();
       obj.seller     = o.owner;
       obj.orderid    = o.orderid;
       obj.for_sale   = o.amount_to_sell.amount;
       obj.sell_price = o.get_price();
       obj.expiration = o.expiration;
   });

   bool filled = db().apply_order( order );

   if( o.fill_or_kill ) FC_ASSERT( filled );
}

void limit_order_cancel_evaluator::do_apply( const limit_order_cancel_operation& o ) {
   db().cancel_order( db().get_limit_order( o.owner, o.orderid ) );
}

void report_over_production_evaluator::do_apply( const report_over_production_operation& o ) {
   const auto& reporter = db().get_account( o.reporter );
   const auto& violator = db().get_account( o.first_block.witness );
   const auto& witness  = db().get_witness( o.first_block.witness );
   FC_ASSERT( violator.vesting_shares.amount > 0, "violator has no vesting shares, must have already been reported" );
   FC_ASSERT( (db().head_block_num() - o.first_block.block_num()) < STEEMIT_MAX_MINERS, "must report within one round" );
   FC_ASSERT( (db().head_block_num() - witness.last_confirmed_block_num) < STEEMIT_MAX_MINERS, "must report within one round" );
   FC_ASSERT( public_key_type(o.first_block.signee()) == witness.signing_key );

   db().modify( reporter, [&]( account_object& a ){
       a.vesting_shares += violator.vesting_shares;
   });
   db().modify( violator, [&]( account_object& a ){
       a.vesting_shares.amount = 0;
   });
}

} } // steemit::chain
