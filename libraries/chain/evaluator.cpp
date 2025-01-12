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
#include <graphene/chain/database.hpp>
#include <graphene/chain/evaluator.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/is_authorized_asset.hpp>
#include <graphene/chain/transaction_evaluation_state.hpp>

#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/fba_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/protocol/fee_schedule.hpp>
#include <graphene/chain/settings_object.hpp>
#include <graphene/chain/witnesses_info_object.hpp>

#include <fc/uint128.hpp>

namespace graphene { namespace chain {
database& generic_evaluator::db()const { return trx_state->db(); }

   operation_result generic_evaluator::start_evaluate( transaction_evaluation_state& eval_state, const operation& op, bool apply )
   { try {
      trx_state = &eval_state;
      const database& d = db();

      //check_required_authorities(op);
      if (d.head_block_time() > HARDFORK_620_TIME)
      {
         if (op.which() == operation::tag<transfer_operation>::value)
         {
            const transfer_operation& tr_op = op.get<transfer_operation>();

            asset_id_type should_pay_in = tr_op.amount.asset_id(d).params.fee_paying_asset;
            FC_ASSERT( tr_op.fee.asset_id == should_pay_in, "You should pay fee in ${a}. Payed in ${b}", ("a", should_pay_in(d).symbol)("b", tr_op.fee.asset_id) );
         }
      }
      auto result = evaluate( op );

      if( apply ) result = this->apply( op );

      return result;
   } FC_CAPTURE_AND_RETHROW() }

   void generic_evaluator::prepare_fee(account_id_type account_id, asset fee)
   {
      const database& d = db();

      FC_ASSERT( fee.amount >= 0 );

      fee_from_account = fee;

      fee_paying_account = &account_id(d);
      fee_paying_account_statistics = &fee_paying_account->statistics(d);

      fee_asset = &fee.asset_id(d);
      fee_asset_dyn_data = &fee_asset->dynamic_asset_data_id(d);

      if (d.head_block_time() > HARDFORK_419_TIME)
      {
         FC_ASSERT( is_authorized_asset( d, *fee_paying_account, *fee_asset ), "Account ${acct} '${name}' attempted to pay fee by using asset ${a} '${sym}', which is unauthorized due to whitelist / blacklist",
            ("acct", fee_paying_account->id)("name", fee_paying_account->name)("a", fee_asset->id)("sym", fee_asset->symbol) );
      }

      FC_ASSERT( not_restricted_account( d, *fee_paying_account, directionality_type::payer), "Account ${acct} '${name}' is restricted by committee",
            ("acct", fee_paying_account->id)("name", fee_paying_account->name));

      FC_ASSERT( !fee_paying_account->verification_is_required, "Please contact support");

      if (fee_from_account.asset_id == CORE_ASSET) {
         core_fee_paid = fee_from_account.amount;
      }
      else
      {
         asset fee_from_pool = fee_from_account * fee_asset->options.core_exchange_rate;
         FC_ASSERT(fee_from_pool.asset_id == CORE_ASSET);
         core_fee_paid = fee_from_pool.amount;

          // if (d.head_block_time() > HARDFORK_616_TIME)
          //   FC_ASSERT( core_fee_paid <= fee_asset_dyn_data->fee_pool, "Fee pool balance of '${b}' is less than the ${r} required to convert ${c}",
          //          ("r", db().to_pretty_string( fee_from_pool))("b",db().to_pretty_string(fee_asset_dyn_data->fee_pool))("c",db().to_pretty_string(fee)) );
      }
   }

   void generic_evaluator::convert_fee()
   {
      if (!trx_state->skip_fee)
      {
         asset_id_type asset_id = fee_asset->get_id();
         if (asset_id != CORE_ASSET)
         {
            database& d = db();
            if (d.head_block_time() > HARDFORK_623_TIME)
            {
               d.modify(*fee_asset_dyn_data, [this](asset_dynamic_data_object& dyn_data)
               {
                  dyn_data.current_supply -= fee_from_account.amount;
                  dyn_data.fee_burnt += fee_from_account.amount;
               });

               // witness fee reward
               const settings_object& settings = d.get(settings_id_type(0));
               if ( (fee_from_account.asset_id == EDC_ASSET) && (settings.witness_fees_percent > 0) )
               {
                  const witnesses_info_object& witnesses_info = d.get(witnesses_info_id_type(0));
                  d.modify(witnesses_info, [&](witnesses_info_object& obj) {
                     obj.witness_fees_reward_edc_amount += fee_from_account.amount;
                  });
               }
            }
            else
            {
               d.modify(*fee_asset_dyn_data, [this](asset_dynamic_data_object& d)
               {
                  d.accumulated_fees += fee_from_account.amount;
                  d.fee_pool -= core_fee_paid;
               });
            }
         }
      }
   }

   void generic_evaluator::pay_fee()
   { try {
      if (!trx_state->skip_fee)
      {
         database& d = db();
         /// TODO: db().pay_fee( account_id, core_fee );
         d.modify(*fee_paying_account_statistics, [&](account_statistics_object& s) {
            s.pay_fee( core_fee_paid, d.get_global_properties().parameters.cashback_vesting_threshold );
         });
      }
   } FC_CAPTURE_AND_RETHROW() }

   void generic_evaluator::pay_fba_fee( uint64_t fba_id )
   {
      database& d = db();
      const fba_accumulator_object& fba = d.get< fba_accumulator_object >( fba_accumulator_id_type( fba_id ) );
      if (!fba.is_configured(d))
      {
         generic_evaluator::pay_fee();
         return;
      }
      d.modify(fba, [&]( fba_accumulator_object& _fba) {
         _fba.accumulated_fba_fees += core_fee_paid;
      } );
   }

   share_type generic_evaluator::calculate_fee_for_operation(const operation& op) const {
     return db().current_fee_schedule().calculate_fee(op).amount;
   }
   void generic_evaluator::db_adjust_balance(const account_id_type& fee_payer, asset fee_amount) {
      db().adjust_balance(fee_payer, fee_amount);
   }

} }
