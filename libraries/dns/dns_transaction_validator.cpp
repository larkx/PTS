#include <bts/dns/dns_transaction_validator.hpp>
#include <bts/dns/dns_util.hpp>

namespace bts { namespace dns {

dns_transaction_validator::dns_transaction_validator(dns_db *db) : transaction_validator(db)
{
    _dns_db = dynamic_cast<dns_db *>(db);
    FC_ASSERT(_dns_db != nullptr);
}

dns_transaction_validator::~dns_transaction_validator()
{
}


block_evaluation_state_ptr dns_transaction_validator::create_block_state() const
{
    return std::make_shared<dns_block_evaluation_state>();
}

transaction_summary dns_transaction_validator::evaluate(const signed_transaction &tx, 
                                                        const block_evaluation_state_ptr &block_state)
{
    dns_tx_evaluation_state state(tx);

    return on_evaluate(state, block_state);
}

void dns_transaction_validator::validate_input(const meta_trx_input &in, transaction_evaluation_state &state,
                                               const block_evaluation_state_ptr &block_state)
{
    if (is_dns_output(in.output))
    {
        claim_domain_output dns_input = to_dns_output(in.output);
        dns_tx_evaluation_state &dns_state = dynamic_cast<dns_tx_evaluation_state &>(state);
        const dns_block_evaluation_state_ptr dns_block_state = std::dynamic_pointer_cast<dns_block_evaluation_state>(block_state);
        FC_ASSERT(dns_block_state);

        validate_domain_input(dns_input, in.output.amount, dns_state, dns_block_state);
    }
    else
    {
        transaction_validator::validate_input(in, state, block_state);
    }
}

void dns_transaction_validator::validate_output(const trx_output &out, transaction_evaluation_state &state,
                                                const block_evaluation_state_ptr &block_state)
{
    if (is_dns_output(out))
    {
        claim_domain_output dns_output = to_dns_output(out);
        dns_tx_evaluation_state &dns_state = dynamic_cast<dns_tx_evaluation_state &>(state);
        const dns_block_evaluation_state_ptr dns_block_state = std::dynamic_pointer_cast<dns_block_evaluation_state>(block_state);
        FC_ASSERT(dns_block_state);

        validate_domain_output(dns_output, out.amount, dns_state, dns_block_state);

        /* Add name to name pool */
        dns_block_state->name_pool.push_back(dns_output.name);
    }
    else
    {
        transaction_validator::validate_output(out, state, block_state);
    }
}

void dns_transaction_validator::validate_domain_input(const claim_domain_output &input, const asset &amount, 
                                                      dns_tx_evaluation_state &state,
                                                      const dns_block_evaluation_state_ptr &block_state)
{
    ilog("Validating domain claim input");
    FC_ASSERT(!state.seen_domain_input, "More than one domain claim input in tx: ${tx}", ("tx", state.trx));

    FC_ASSERT(_dns_db->has_dns_record(input.name), "Input references invalid name");

    state.input = input;
    state.input_amount = amount;
    state.seen_domain_input = true;
}

void dns_transaction_validator::validate_domain_output(const claim_domain_output &output, const asset &amount, 
                                                       dns_tx_evaluation_state &state,
                                                       const dns_block_evaluation_state_ptr &block_state)
{
    ilog("Validating domain claim output");
    FC_ASSERT(!state.seen_domain_output, "More than one domain claim output in tx: ${tx}", ("tx", state.trx));
    state.seen_domain_output = true;

    FC_ASSERT(is_valid_name(output.name), "Invalid name");
    FC_ASSERT(is_valid_value(output.value), "Invalid value");
    FC_ASSERT(is_valid_state(output.state), "Invalid state");
    FC_ASSERT(is_valid_amount(amount), "Invalid amount");

    /* Check name status */
    bool new_or_expired;
    output_reference prev_tx_ref;
    auto available = name_is_available(output.name, block_state->name_pool, *_dns_db, new_or_expired, prev_tx_ref);

    /* If we haven't seen a domain input then the only valid output is a new domain auction */
    if (!state.seen_domain_input)
    {
        ilog("Have not seen a domain claim input on this tx");
        FC_ASSERT(new_or_expired && available, "Name already exists (and is younger than 1 block-year)"); 
        return;
    }

    /* Otherwise, the transaction must have a domain input and it must exist in the database,
     * and it can't be expired */
    ilog("Seen a domain input");
    FC_ASSERT(!new_or_expired, "Name new or expired");
    FC_ASSERT(output.name == state.input.name, "Bid tx refers to different input and output names");

    /* Bid in existing auction */
    if (!auction_is_closed(prev_tx_ref, *_dns_db))
    {
        ilog("Currently in an auction");
        FC_ASSERT(available, "Name not available");
        FC_ASSERT(state.input.state == claim_domain_output::possibly_in_auction, "Input not for auction");

        asset amount_back;
        FC_ASSERT(is_valid_bid_price(state.input_amount, amount, amount_back), "Invalid bid amount");
        state.add_required_fees(amount - amount_back);

        /* Check for output to past owner */
        bool found = false;
        for (auto other_out : state.trx.outputs)
        {
            bool right_claim = other_out.claim_func == claim_by_signature;
            bool enough = other_out.amount >= amount_back;
            bool to_owner = right_claim
                && other_out.as<claim_by_signature_output>().owner == state.input.owner;

            if (right_claim && enough && to_owner) 
            {
                found = true;
                break;
            }
        }

        FC_ASSERT(found, "Bid did not pay enough to previous owner");

        return;
    }

    /* Update or sale */
    ilog("Auction is over.");
    FC_ASSERT(!domain_is_expired(prev_tx_ref, *_dns_db), "Domain is expired");

    /* If updating record */
    if (output.state == claim_domain_output::not_in_auction)
    {
        /* Keep output amount constant when updating domain record */
        FC_ASSERT(amount == state.input_amount, "Output amount should not change when updating record");
    }

    /* If you're the owner, do whatever you like! */
    FC_ASSERT(state.has_signature(output.owner), "Domain tx missing required signature: ${tx}", ("tx", state.trx));
    ilog("Tx signed by owner");
}

}} // bts::blockchain
