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

#include <graphene/protocol/asset_ops.hpp>
#include <fc/io/raw.hpp>

namespace graphene { namespace protocol {

/**
 *  Valid symbols can contain [A-Z0-9], and '.'
 *  They must start with [A, Z]
 *  They must end with [A, Z] (deprecated)
 *  They can contain a maximum of one '.'
 */
bool is_valid_symbol( const string& symbol )
{
    if( symbol.size() < GRAPHENE_MIN_ASSET_SYMBOL_LENGTH )
        return false;

    if( symbol.size() > GRAPHENE_MAX_ASSET_SYMBOL_LENGTH )
        return false;

    if( !isalpha( symbol.front() ) )
        return false;

//    if( !isalpha( symbol.back() ) )
//        return false;

    bool dot_already_present = false;
    for( const auto c : symbol )
    {
        if( (isalpha( c ) && isupper( c )) || isdigit(c) )
            continue;

        if( c == '.' )
        {
            if( dot_already_present )
                return false;

            dot_already_present = true;
            continue;
        }

        return false;
    }

    return true;
}

share_type asset_issue_operation::calculate_fee(const fee_parameters_type& k)const
{
   return k.fee + calculate_data_fee( fc::raw::pack_size(memo), k.price_per_kbyte );
}

share_type asset_create_operation::calculate_fee(const asset_create_operation::fee_parameters_type& param)const
{
   auto core_fee_required = param.long_symbol; 

   switch(symbol.size()) {
      case 3: core_fee_required = param.symbol3;
          break;
      case 4: core_fee_required = param.symbol4;
          break;
      default:
          break;
   }

   // common_options contains several lists and a string. Charge fees for its size
   core_fee_required += calculate_data_fee( fc::raw::pack_size(*this), param.price_per_kbyte );

   return core_fee_required;
}

void  asset_create_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( is_valid_symbol(symbol) );
   common_options.validate();
   if( common_options.issuer_permissions & (disable_force_settle|global_settle) )
      FC_ASSERT( bitasset_opts.valid() );
   if( is_prediction_market )
   {
      FC_ASSERT( bitasset_opts.valid(), "Cannot have a User-Issued Asset implement a prediction market." );
      FC_ASSERT( common_options.issuer_permissions & global_settle );
   }
   if( bitasset_opts ) bitasset_opts->validate();

   asset dummy = asset(1) * common_options.core_exchange_rate;
   FC_ASSERT(dummy.asset_id == EDC_ASSET);
   FC_ASSERT(precision <= 12);
}
share_type allow_create_asset_operation::calculate_fee( const allow_create_asset_operation::fee_parameters_type& k )const
{
    return 0;
}

void asset_update_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   if( new_issuer )
      FC_ASSERT(issuer != *new_issuer);
   new_options.validate();

   asset dummy = asset(1, asset_to_update) * new_options.core_exchange_rate;
   FC_ASSERT(dummy.asset_id == CORE_ASSET);
}

share_type asset_update_operation::calculate_fee(const asset_update_operation::fee_parameters_type& k)const
{
   return k.fee + calculate_data_fee( fc::raw::pack_size(*this), k.price_per_kbyte );
}

void asset_update2_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   if( new_issuer )
      FC_ASSERT(issuer != *new_issuer);
   new_options.validate();

   asset dummy = asset(1, asset_to_update) * new_options.core_exchange_rate;
   FC_ASSERT(dummy.asset_id == CORE_ASSET);
}

share_type asset_update2_operation::calculate_fee(const asset_update2_operation::fee_parameters_type& k)const
{
   return 0;
}

void asset_publish_feed_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   feed.validate();

   // maybe some of these could be moved to feed.validate()
   if( !feed.core_exchange_rate.is_null() )
   {
      feed.core_exchange_rate.validate();
   }
   if( (!feed.settlement_price.is_null()) && (!feed.core_exchange_rate.is_null()) )
   {
      FC_ASSERT( feed.settlement_price.base.asset_id == feed.core_exchange_rate.base.asset_id );
   }

   FC_ASSERT( !feed.settlement_price.is_null() );
   FC_ASSERT( !feed.core_exchange_rate.is_null() );
   FC_ASSERT( feed.is_for( asset_id ) );
}

void asset_reserve_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( amount_to_reserve.amount.value <= GRAPHENE_MAX_SHARE_SUPPLY );
   FC_ASSERT( amount_to_reserve.amount.value > 0 );
}

void asset_issue_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( asset_to_issue.amount.value <= GRAPHENE_MAX_SHARE_SUPPLY );
   FC_ASSERT( asset_to_issue.amount.value > 0 );
   FC_ASSERT( asset_to_issue.asset_id != CORE_ASSET );
}

void bonus_operation::validate()const
{
   FC_ASSERT( asset_to_issue != CORE_ASSET );
}
void daily_issue_operation::validate()const
{
   FC_ASSERT( asset_to_issue.amount.value <= GRAPHENE_MAX_SHARE_SUPPLY );
   FC_ASSERT( asset_to_issue.amount.value > 0 );
   FC_ASSERT( asset_to_issue.asset_id != CORE_ASSET );
}
void referral_issue_operation::validate()const
{
   FC_ASSERT( asset_to_issue.amount.value <= GRAPHENE_MAX_SHARE_SUPPLY );
   FC_ASSERT( asset_to_issue.amount.value > 0 );
   FC_ASSERT( asset_to_issue.asset_id != CORE_ASSET );
}

void asset_fund_fee_pool_operation::validate() const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( fee.asset_id == CORE_ASSET );
   FC_ASSERT( amount > 0 );
}

void edc_asset_fund_fee_pool_operation::validate() const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( fee.asset_id == EDC_ASSET ); // should be EDC
   FC_ASSERT( amount > 0 );
}

void asset_settle_operation::validate() const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( amount.amount >= 0 );
}

void asset_update_bitasset_operation::validate() const
{
   FC_ASSERT( fee.amount >= 0 );
   new_options.validate();
}

void asset_update_feed_producers_operation::validate() const
{
   FC_ASSERT( fee.amount >= 0 );
}

void asset_global_settle_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( asset_to_settle == settle_price.base.asset_id );
}

void bitasset_options::validate() const
{
   FC_ASSERT(minimum_feeds > 0);
   FC_ASSERT(force_settlement_offset_percent <= GRAPHENE_100_PERCENT);
   FC_ASSERT(maximum_force_settlement_volume <= GRAPHENE_100_PERCENT);
}

void asset_options::validate()const
{
   FC_ASSERT( max_supply > 0 );
   FC_ASSERT( max_supply <= GRAPHENE_MAX_SHARE_SUPPLY );
   FC_ASSERT( market_fee_percent <= GRAPHENE_100_PERCENT );
   FC_ASSERT( max_market_fee >= 0 && max_market_fee <= GRAPHENE_MAX_SHARE_SUPPLY );
   // There must be no high bits in permissions whose meaning is not known.
   FC_ASSERT( !(issuer_permissions & ~ASSET_ISSUER_PERMISSION_MASK) );
   // The global_settle flag may never be set (this is a permission only)
   FC_ASSERT( !(flags & global_settle) );
   // the witness_fed and committee_fed flags cannot be set simultaneously
   FC_ASSERT( (flags & (witness_fed_asset | committee_fed_asset)) != (witness_fed_asset | committee_fed_asset) );
   core_exchange_rate.validate();
   FC_ASSERT( core_exchange_rate.base.asset_id.instance.value == 0 ||
              core_exchange_rate.quote.asset_id.instance.value == 0 );

   if(!whitelist_authorities.empty() || !blacklist_authorities.empty())
      FC_ASSERT( flags & white_list );
   for( auto item : whitelist_markets )
   {
      FC_ASSERT( blacklist_markets.find(item) == blacklist_markets.end() );
   }
   for( auto item : blacklist_markets )
   {
      FC_ASSERT( whitelist_markets.find(item) == whitelist_markets.end() );
   }
}

void asset_claim_fees_operation::validate()const {
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( amount_to_claim.amount > 0 );
}

} } // namespace graphene::protocol

GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_claim_fees_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_create_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::allow_create_asset_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_global_settle_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_settle_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_settle_cancel_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_fund_fee_pool_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::edc_asset_fund_fee_pool_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update2_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update_bitasset_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update_feed_producers_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_publish_feed_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_issue_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::bonus_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::daily_issue_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::referral_issue_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_reserve_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::denominate_operation::fee_parameters_type )

GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_claim_fees_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_options )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::bitasset_options )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::allow_create_asset_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_create_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_parameters )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update2_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update_bitasset_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_update_feed_producers_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_publish_feed_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_settle_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_settle_cancel_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_global_settle_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_issue_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_reserve_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::asset_fund_fee_pool_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::edc_asset_fund_fee_pool_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::bonus_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::daily_issue_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::referral_issue_operation)
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::denominate_operation)