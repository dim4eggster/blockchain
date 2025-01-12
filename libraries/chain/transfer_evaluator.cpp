/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/chain/transfer_evaluator.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/settings_object.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/is_authorized_asset.hpp>

namespace graphene { namespace chain {

void_result transfer_evaluator::do_evaluate( const transfer_operation& op )
{ try {

   const database& d = db();

   from_account_ptr = &op.from(d);
   to_account_ptr = &op.to(d);
   const account_object& from_account = *from_account_ptr;
   const account_object& to_account = *to_account_ptr;

   const asset_object& asset_type = op.amount.asset_id(d);

   const asset_object* fee_asset_type_ptr = d.find(asset_type.params.fee_paying_asset);
   // many unit tests don't have an edc-asset
   const asset_object& fee_asset_type = fee_asset_type_ptr ? *fee_asset_type_ptr : CORE_ASSET(d);

   asset_dyn_data_ptr = &asset_type.dynamic_asset_data_id(d);

   settings_ptr = &(const chain::settings_object&)(d.get(settings_id_type(0)));
   const chain::settings_object& settings = *settings_ptr;

   try {

      GRAPHENE_ASSERT(
         is_authorized_asset( d, from_account, asset_type ),
         transfer_from_account_not_whitelisted,
         "'from' account ${from} is not whitelisted for asset ${asset}",
         ("from",op.from)
         ("asset",op.amount.asset_id)
      );
      GRAPHENE_ASSERT(
         is_authorized_asset( d, to_account, asset_type ),
         transfer_to_account_not_whitelisted,
         "'to' account ${to} is not whitelisted for asset ${asset}",
         ("to",op.to)
         ("asset",op.amount.asset_id)
      );
      GRAPHENE_ASSERT(
         not_restricted_account( d, from_account, directionality_type::payer),
         transfer_from_account_restricted,
         "'from' account ${from} is restricted by committee",
         ("from",op.from)
      );
      GRAPHENE_ASSERT(
         not_restricted_account( d, to_account, directionality_type::receiver),
         transfer_to_account_restricted,
         "'to' account ${to} is restricted by committee",
         ("to",op.to)
      );

      if (asset_type.is_transfer_restricted())
      {
         GRAPHENE_ASSERT(
            from_account.id == asset_type.issuer || to_account.id == asset_type.issuer,
            transfer_restricted_transfer_asset,
            "Asset ${asset} has transfer_restricted flag enabled",
            ("asset", op.amount.asset_id)
          );
      }

      if (d.head_block_time() > HARDFORK_627_TIME)
      {
         // EDC daily limit
         if ((op.amount.asset_id == EDC_ASSET) && from_account.edc_limit_transfers_enabled)
         {
            if ((d.head_block_time() < HARDFORK_636_TIME) || !to_account.burning_mode_enabled)
            {
               bool limit_is_valid = true;
               share_type max_amount = (from_account.edc_transfers_max_amount > 0) ? from_account.edc_transfers_max_amount : settings.edc_transfers_daily_limit;

               if (d.head_block_time() > HARDFORK_631_TIME) {
                  limit_is_valid = max_amount >= (from_account.edc_transfers_amount_counter + op.amount.amount);
               }
               else {
                  limit_is_valid = max_amount > (from_account.edc_transfers_amount_counter + op.amount.amount);
               }
               FC_ASSERT(limit_is_valid
                        , "Daily transfers limit exceeded. Current transfers counter value: ${a} (+op.amount)"
                        , ("a", from_account.edc_transfers_amount_counter.value) );
            }
         }

         int64_t fee_percent = 0;

         if (d.head_block_time() > HARDFORK_628_TIME)
         {
            if ( (d.head_block_time() >= HARDFORK_636_TIME)
                 && (fee_asset_type.get_id() == EDC_ASSET)
                 && (from_account.rank > e_account_rank::_default)
                 // don't take fee from burning operation
                 && !to_account.burning_mode_enabled) {
               fee_percent = d.get_account_fee_edc_percent_by_rank(from_account);
            }
            else if ((d.head_block_time() < HARDFORK_636_TIME) || !to_account.burning_mode_enabled)
            {
               optional<chain::settings_fee> fee = d.get_custom_fee(settings.transfer_fees, fee_asset_type.get_id());
               fee_percent = (fee) ? fee->percent : 0;
            }
         }
         else
         {
            optional<chain::settings_fee> fee = d.get_custom_fee(settings.transfer_fees, asset_type.get_id());
            fee_percent = (fee) ? fee->percent : 0;
         }

         if (fee_percent > 0)
         {
            custom_fee = std::round(op.amount.amount.value * d.get_percent(fee_percent));

            bool balance_is_valid = d.get_balance(from_account, asset_type).amount >= (op.amount.amount + custom_fee);
            FC_ASSERT(balance_is_valid,
                      "Insufficient Balance: ${balance}, unable to transfer '${total_transfer}' from account '${a}' to '${t}'. Custom fee: ${fee}",
                      ("a", from_account.name)("t", to_account.name)("total_transfer", d.to_pretty_string(op.amount))
                      ("fee", d.to_pretty_string(asset(custom_fee, op.amount.asset_id)))
                      ("balance", d.to_pretty_string(d.get_balance(from_account, asset_type))));
         }

         if (custom_fee > 0) {
            FC_ASSERT(op.fee.amount >= custom_fee, "Wrong fee amount (${fee}) sent. Custom fee: ${cfee}", ("fee", op.fee.amount)("custom fee", custom_fee) );
         }

         if (fee_percent == 0)
         {
            // check only amounts (without fees)
            bool balance_is_valid = d.get_balance(from_account, asset_type).amount >= op.amount.amount;
            FC_ASSERT(balance_is_valid,
                      "Insufficient Balance: ${balance}, unable to transfer '${total_transfer}' from account '${a}' to '${t}'",
                      ("a", from_account.name)("t", to_account.name)("total_transfer", d.to_pretty_string(op.amount))(
                      "balance", d.to_pretty_string(d.get_balance(from_account, asset_type))));
         }
      }

      // see also 'asset_reserve_operation'
      if (to_account.burning_mode_enabled)
      {
         FC_ASSERT(!asset_type.is_market_issued(), "Cannot reserve (burn) ${sym} because it is a market-issued asset", ("sym", asset_type.symbol));
         FC_ASSERT((asset_dyn_data_ptr->current_supply - (op.amount.amount + custom_fee)) >= 0);
      }

      return void_result();
   } FC_RETHROW_EXCEPTIONS( error, "Unable to transfer ${a} from ${f} to ${t}", ("a",d.to_pretty_string(op.amount))("f",op.from(d).name)("t",op.to(d).name) );

}  FC_CAPTURE_AND_RETHROW( (op) ) }

void_result transfer_evaluator::do_apply( const transfer_operation& o )
{ try {
   database& d = db();
   const account_object& to_account = *to_account_ptr;

   // !!! see also evaluator.cpp::convert_fee()

   d.adjust_balance(o.from, -o.amount);

   // normal accrual
   if (!to_account.burning_mode_enabled) {
      d.adjust_balance(o.to, o.amount);
   }
   // burning
   else if (asset_dyn_data_ptr)
   {
      d.modify(*asset_dyn_data_ptr, [&](asset_dynamic_data_object& data)
      {
         data.current_supply -= o.amount.amount;
         data.fee_burnt += o.amount.amount;
      });

      // increase burnt EDC of sender
      if ((d.head_block_time() >= HARDFORK_636_TIME) && (o.amount.asset_id == EDC_ASSET))
      {
         d.modify(*from_account_ptr, [&](account_object& acc) {
            acc.edc_burnt += o.amount.amount;
         });
      }
   }

   // edc daily transfers counter
   if ((d.head_block_time() > HARDFORK_627_TIME) && (o.amount.asset_id == EDC_ASSET))
   {
      d.modify(o.from(d), [&](account_object& obj)
      {
         if ((d.head_block_time() < HARDFORK_636_TIME) || !to_account.burning_mode_enabled) {
            obj.edc_transfers_amount_counter += o.amount.amount;
         }

         ++obj.edc_transfers_count;
      });
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

/////////////////////////////////////////////////////////////////////////////////////////////

void_result blind_transfer2_evaluator::do_evaluate( const blind_transfer2_operation& op )
{ try {

   const database& d = db();

   from_account_ptr = &op.from(d);
   to_account_ptr = &op.to(d);

   const account_object& from_account = *from_account_ptr;
   const account_object& to_account = *to_account_ptr;

   const asset_object& asset_type = op.amount.asset_id(d);
   asset_dyn_data_ptr = &asset_type.dynamic_asset_data_id(d);

   settings_ptr = &(const chain::settings_object&)(d.get(settings_id_type(0)));
   const chain::settings_object& settings = *settings_ptr;

   try {

      GRAPHENE_ASSERT(
         is_authorized_asset( d, from_account, asset_type ),
         transfer_from_account_not_whitelisted,
         "'from' account ${from} is not whitelisted for asset ${asset}",
         ("from",op.from)
         ("asset",op.amount.asset_id)
      );
      GRAPHENE_ASSERT(
         is_authorized_asset( d, to_account, asset_type ),
         transfer_to_account_not_whitelisted,
         "'to' account ${to} is not whitelisted for asset ${asset}",
         ("to",op.to)
         ("asset",op.amount.asset_id)
      );
      GRAPHENE_ASSERT(
         not_restricted_account( d, from_account, directionality_type::payer),
         transfer_from_account_restricted,
         "'from' account ${from} is restricted by committee",
         ("from",op.from)
      );
      GRAPHENE_ASSERT(
         not_restricted_account( d, to_account, directionality_type::receiver),
         transfer_to_account_restricted,
         "'to' account ${to} is restricted by committee",
         ("to",op.to)
      );

      if (asset_type.is_transfer_restricted())
      {
         GRAPHENE_ASSERT(
            from_account.id == asset_type.issuer || to_account.id == asset_type.issuer,
            transfer_restricted_transfer_asset,
            "Asset ${asset} has transfer_restricted flag enabled",
            ("asset", op.amount.asset_id)
         );
      }

      if ( (d.head_block_time() > HARDFORK_631_TIME)
            && (op.amount.asset_id == EDC_ASSET)
            && from_account.edc_limit_transfers_enabled )
      {
         if ((d.head_block_time() < HARDFORK_636_TIME) || !to_account.burning_mode_enabled)
         {
            share_type max_amount = (from_account.edc_transfers_max_amount > 0) ? from_account.edc_transfers_max_amount : settings.edc_transfers_daily_limit;

            FC_ASSERT(max_amount >= (from_account.edc_transfers_amount_counter + op.amount.amount)
                      , "Daily transfers limit exceeded. Current transfers counter value: ${a} (+op.amount)"
                      , ("a", from_account.edc_transfers_amount_counter.value));

         }
      }

      //FC_ASSERT(op.amount.asset_id == s.blind_fee.asset_id, "assets are different('${a}','${b}')!", ("a",op.amount.asset_id)("b",s.blind_fee.asset_id));

      custom_fee = settings.blind_transfer_default_fee;
      fee_dyn_data_ptr = &custom_fee.asset_id(d).dynamic_asset_data_id(d);

      if (d.head_block_time() > HARDFORK_627_TIME)
      {
         if ( (d.head_block_time() >= HARDFORK_636_TIME)
              && (asset_type.get_id() == EDC_ASSET)
              && (from_account.rank > e_account_rank::_default)
              // don't take fee from burning operation
              && !to_account.burning_mode_enabled)
         {
            const share_type& amnt = std::round(op.amount.amount.value * d.get_percent(d.get_account_fee_edc_percent_by_rank(from_account)));
            custom_fee = asset(amnt, EDC_ASSET);
         }
         else if ((d.head_block_time() < HARDFORK_636_TIME) || !to_account.burning_mode_enabled)
         {
            optional<chain::settings_fee> fee = d.get_custom_fee(settings.blind_transfer_fees, asset_type.get_id());
            if (fee)
            {
               const share_type& amnt = std::round(op.amount.amount.value * d.get_percent(fee->percent));
               custom_fee = asset(amnt, fee->asset_id);
            }
         }
         // sent to burning account after hf636
         else {
            custom_fee = asset(0, asset_type.get_id());
         }

         fee_dyn_data_ptr = &custom_fee.asset_id(d).dynamic_asset_data_id(d);

         if (asset_type.get_id() == custom_fee.asset_id(d).get_id())
         {
            bool insufficient_balance = d.get_balance(from_account, asset_type).amount >= (op.amount.amount + custom_fee.amount);
            FC_ASSERT(insufficient_balance,
                      "Insufficient Balance [0]: ${balance} (fee: ${fee}), unable to make blind transfer '${total_transfer}' from account '${from}' to '${to}'",
                      ("from", from_account.name)
                      ("to", to_account.name)
                      ("total_transfer", d.to_pretty_string(op.amount))
                      ("fee", d.to_pretty_string(custom_fee))
                      ("balance", d.to_pretty_string(d.get_balance(from_account, asset_type))) );
         }
         else
         {
            bool insufficient_balance = d.get_balance(from_account, asset_type).amount >= op.amount.amount;
            FC_ASSERT(insufficient_balance,
                      "Insufficient Balance [1]: ${balance} (fee: ${fee}), unable to make blind transfer '${total_transfer}' from account '${from}' to '${to}'",
                      ("from", from_account.name)
                      ("to", to_account.name)
                      ("total_transfer", d.to_pretty_string(op.amount))
                      ("fee", d.to_pretty_string(custom_fee))
                      ("balance", d.to_pretty_string(d.get_balance(from_account, asset_type))) );

            const asset_object& custom_fee_type = custom_fee.asset_id(d);
            bool insufficient_fee = d.get_balance(from_account, custom_fee_type).amount >= custom_fee.amount;
            FC_ASSERT(insufficient_fee,
                      "Insufficient balance for fee: ${balance_for_fee}, unable to make blind transfer '${total_transfer}' from account '${from}' to '${to}'",
                      ("from", from_account.name)
                      ("to", to_account.name)
                      ("total_transfer", d.to_pretty_string(op.amount))
                      ("balance_for_fee", d.to_pretty_string(d.get_balance(from_account, custom_fee_type))) );
         }

         if (custom_fee.amount > 0)
         {
            FC_ASSERT(op.fee.amount >= custom_fee.amount, "Wrong fee amount (${fee}) sent. Custom fee amount: ${cfee}",
                      ("fee", op.fee.amount)("cfee", custom_fee.amount));
            FC_ASSERT(op.fee.asset_id == custom_fee.asset_id, "Wrong fee asset (${op_fee_id}) sent. Custom fee id: ${c_fee_id}",
                      ("op_fee_id", op.fee.asset_id)("c_fee_id", custom_fee.asset_id));
         }
      }
      else
      {
         const asset_object& custom_fee_type = custom_fee.asset_id(d);

         bool insufficient_balance = d.get_balance(from_account, asset_type).amount >= op.amount.amount;
         FC_ASSERT(insufficient_balance,
                   "Insufficient Balance [2]: ${balance}(fee: ${fee}), unable to make blind transfer '${total_transfer}' from account '${a}' to '${t}'",
                   ("a",from_account.name)("t",to_account.name)
                   ("total_transfer",d.to_pretty_string(op.amount))
                   ("balance",d.to_pretty_string(d.get_balance(from_account, asset_type))) );

         bool insufficient_fee = d.get_balance(from_account, custom_fee_type).amount >= custom_fee.amount;
         FC_ASSERT(insufficient_fee,
                   "Insufficient fee: ${fee}, unable to make blind transfer '${total_transfer}' from account '${a}' to '${t}'",
                   ("a",from_account.name)("t",to_account.name)
                   ("total_transfer",d.to_pretty_string(op.amount))
                   ("fee",d.to_pretty_string(d.get_balance(from_account, custom_fee_type))) );
      }

      // see also 'asset_reserve_operation'
      if (to_account.burning_mode_enabled)
      {
         FC_ASSERT(!asset_type.is_market_issued(), "Cannot reserve (burn) ${sym} because it is a market-issued asset",
                   ("sym", asset_type.symbol));
         FC_ASSERT((asset_dyn_data_ptr->current_supply - (op.amount.amount + custom_fee.amount)) >= 0);
      }

      return void_result();
   } FC_RETHROW_EXCEPTIONS( error, "Unable to transfer ${a} from ${f} to ${t}", ("a",d.to_pretty_string(op.amount))("f",op.from(d).name)("t",op.to(d).name) );

}  FC_CAPTURE_AND_RETHROW( (op) ) }

asset blind_transfer2_evaluator::do_apply( const blind_transfer2_operation& o )
{ try {
   database& d = db();
   const account_object& to_account = *to_account_ptr;

   // !!! see also evaluator.cpp::convert_fee()

   // amount
   d.adjust_balance(o.from, -o.amount);

   // fee
   if (d.head_block_time() < HARDFORK_627_TIME)
   {
      if (custom_fee.amount > 0) {
         d.adjust_balance(o.from, -custom_fee);
      }
      if (fee_dyn_data_ptr)
      {
         d.modify(*fee_dyn_data_ptr, [&](asset_dynamic_data_object& data)
         {
            data.current_supply -= custom_fee.amount;
            data.fee_burnt += custom_fee.amount;
         });
      }
   }

   // edc daily transfers counter
   if ((d.head_block_time() > HARDFORK_627_TIME) && (o.amount.asset_id == EDC_ASSET))
   {
      d.modify(o.from(d), [&](account_object& obj)
      {
         if ((d.head_block_time() < HARDFORK_636_TIME) || !to_account.burning_mode_enabled) {
            obj.edc_transfers_amount_counter += o.amount.amount;
         }
         ++obj.edc_transfers_count;
      });
   }

   // normal accrual
   if (!to_account.burning_mode_enabled) {
      d.adjust_balance(o.to, o.amount);
   }
   // burning
   else if (asset_dyn_data_ptr)
   {
      d.modify(*asset_dyn_data_ptr, [&](asset_dynamic_data_object& data)
      {
         data.current_supply -= o.amount.amount;
         data.fee_burnt += o.amount.amount;
      });

      // increase burnt EDC of sender
      if ((d.head_block_time() >= HARDFORK_636_TIME) && (o.amount.asset_id == EDC_ASSET))
      {
         d.modify(*from_account_ptr, [&](account_object& acc) {
            acc.edc_burnt += o.amount.amount;
         });
      }
   }

   // new blind_transfer2 object
   d.create<blind_transfer2_object>([&](blind_transfer2_object& obj)
   {
      obj.from     = o.from;
      obj.to       = o.to;
      obj.amount   = o.amount;
      obj.datetime = d.head_block_time();
      obj.memo     = o.memo;
      obj.fee      = custom_fee.amount;
   });

   return custom_fee;
} FC_CAPTURE_AND_RETHROW( (o) ) }

/////////////////////////////////////////////////////////////////////////////////////////////

void_result update_blind_transfer2_settings_evaluator::do_evaluate( const update_blind_transfer2_settings_operation& op )
{ try {

   const database& d = db();

   auto itr = d.find(settings_id_type(0));

   FC_ASSERT(itr, "can't find settings_object'!");

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

void_result update_blind_transfer2_settings_evaluator::do_apply( const update_blind_transfer2_settings_operation& o )
{ try {
   database& d = db();

   d.modify(settings_id_type(0)(d), [&](settings_object& obj) {
      obj.blind_transfer_default_fee = o.blind_fee;
   });

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

/////////////////////////////////////////////////////////////////////////////////////////////

void_result override_transfer_evaluator::do_evaluate( const override_transfer_operation& op )
{ try {
   const database& d = db();

   const asset_object&   asset_type      = op.amount.asset_id(d);
   GRAPHENE_ASSERT(
      asset_type.can_override(),
      override_transfer_not_permitted,
      "override_transfer not permitted for asset ${asset}",
      ("asset", op.amount.asset_id)
      );
   FC_ASSERT( asset_type.issuer == op.issuer );

   const account_object& from_account    = op.from(d);
   to_account_ptr = &op.to(d);

   FC_ASSERT( is_authorized_asset( d, *to_account_ptr, asset_type ) );
   FC_ASSERT( is_authorized_asset( d, from_account, asset_type ) );

   FC_ASSERT( not_restricted_account( d, from_account, directionality_type::payer) );
   FC_ASSERT( not_restricted_account( d, *to_account_ptr, directionality_type::receiver) );

   if( d.head_block_time() <= HARDFORK_419_TIME )
   {
      FC_ASSERT( is_authorized_asset( d, from_account, asset_type ) );
   }
   // the above becomes no-op after hardfork because this check will then be performed in evaluator

   FC_ASSERT( d.get_balance( from_account, asset_type ).amount >= op.amount.amount,
              "", ("total_transfer",op.amount)("balance",d.get_balance(from_account, asset_type).amount) );

   if (to_account_ptr->burning_mode_enabled)
   {
      FC_ASSERT(!asset_type.is_market_issued(), "Cannot reserve (burn) ${sym} because it is a market-issued asset", ("sym", asset_type.symbol));

      asset_dyn_data_ptr = &asset_type.dynamic_asset_data_id(d);
      FC_ASSERT( (asset_dyn_data_ptr->current_supply - op.amount.amount) >= 0 );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

void_result override_transfer_evaluator::do_apply( const override_transfer_operation& o )
{ try {
   database& d = db();

   d.adjust_balance( o.from, -o.amount );

   if (to_account_ptr)
   {
      if (!to_account_ptr->burning_mode_enabled) {
         d.adjust_balance( o.to, o.amount );
      }
      else if (asset_dyn_data_ptr)
      {
         d.modify( *asset_dyn_data_ptr, [&]( asset_dynamic_data_object& data )
         {
            data.current_supply -= o.amount.amount;
            data.fee_burnt += o.amount.amount;
         });
      }
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

} } // graphene::chain
