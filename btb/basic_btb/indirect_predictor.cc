#include "indirect_predictor.h"

std::pair<champsim::address, bool> indirect_predictor::prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid)
{
  using namespace champsim::data::data_literals;
  auto hash = ip.slice_upper<2_b>().to<unsigned long long>() ^ conditional_history.at(context.value).to_ullong() ^ champsim::asid_word(asid);
  return {predictor[hash % std::size(predictor)], true};
}

void indirect_predictor::update_target(champsim::address ip, champsim::address branch_target, champsim::context_index context, champsim::address_space_id asid)
{
  using namespace champsim::data::data_literals;
  auto hash = ip.slice_upper<2_b>().to<unsigned long long>() ^ conditional_history.at(context.value).to_ullong() ^ champsim::asid_word(asid);
  predictor[hash % std::size(predictor)] = branch_target;
}

void indirect_predictor::update_direction(champsim::context_index context, bool taken)
{
  auto& history = conditional_history.at(context.value);
  history <<= 1;
  history.set(0, taken);
}
