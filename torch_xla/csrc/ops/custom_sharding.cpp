#include "torch_xla/csrc/ops/custom_sharding.h"

#include "torch_xla/csrc/lowering_context.h"
#include "torch_xla/csrc/ops/xla_ops.h"
#include "torch_xla/csrc/xla_lower_util.h"

namespace torch_xla {
namespace {
std::string TypeToString(const CustomSharding::Type& type) {
  switch (type) {
    case CustomSharding::Type::kCustomSharding:
      return "Sharding";
    case CustomSharding::Type::kSPMDFullToShardShape:
      return "SPMDFullToShardShape";
    case CustomSharding::Type::kSPMDShardToFullShape:
      return "SPMDShardToFullShape";
  }
}
}

CustomSharding::CustomSharding(const torch::lazy::Value& input, const CustomSharding::Type& type)
    : XlaNode(xla_custom_sharding, {input}, GetXlaShape(input),
              /*num_outputs=*/1, torch::lazy::MHash(static_cast<int>(type)))
    , type(type) {}

torch::lazy::NodePtr CustomSharding::Clone(torch::lazy::OpList operands) const {
  return torch::lazy::MakeNode<CustomSharding>(operands.at(0), type);
}

XlaOpVector CustomSharding::Lower(LoweringContext* loctx) const {
  xla::XlaOp input = loctx->GetOutputOp(operand(0));
  xla::XlaOp output = BuildCustomSharding(input, TypeToString(type));
  return ReturnOp(output, loctx);
}

std::string CustomSharding::ToString() const {
  std::stringstream ss;
  ss << XlaNode::ToString() << ", " << TypeToString(type);
  return ss.str();
}

}  // namespace torch_xla
