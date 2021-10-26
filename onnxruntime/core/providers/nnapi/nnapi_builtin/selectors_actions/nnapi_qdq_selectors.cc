// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#if !defined(ORT_MINIMAL_BUILD)

#include "core/providers/nnapi/nnapi_builtin/selectors_actions/nnapi_qdq_selectors.h"

#include "core/graph/graph.h"
#include "core/optimizer/initializer.h"
#include "core/optimizer/qdq_transformer/qdq_util.h"
#include "core/optimizer/utils.h"
#include "core/providers/nnapi/nnapi_builtin/selectors_actions/nnapi_qdq_selector_helper.h"

namespace onnxruntime {
namespace NNAPIQDQ {
namespace {

// adjust for an optional input/output that has an entry but does not exist
int NumActualValues(const Node& node, bool input) {
  const auto& defs = input ? node.InputDefs() : node.OutputDefs();
  return gsl::narrow_cast<int>(std::count_if(defs.cbegin(), defs.cend(),
                                             [](const NodeArg* def) { return def && def->Exists(); }));
}
}  // namespace

inline bool BaseSelector::CheckQDQNodes(const Graph& graph, const Node& node,
                                        const std::vector<const Node*>& dq_nodes,
                                        const std::vector<const Node*>& q_nodes,
                                        int num_dq_inputs) const {
  if (num_dq_inputs == -1) {
    num_dq_inputs = NumActualValues(node, true);
  }

  int num_outputs = NumActualValues(node, false);  // number of outputs that exist

  return num_dq_inputs == gsl::narrow_cast<int>(dq_nodes.size()) &&
         num_outputs == gsl::narrow_cast<int>(q_nodes.size()) &&
         !graph.NodeProducesGraphOutput(node);
}

inline bool BaseSelector::Select(const Graph& graph, const Node& node, std::unique_ptr<ConstNodesToOptimize>& selection) const {
  //TODO: mutable graph design + modify Select()
  std::vector<const Node*> dq_nodes = graph_utils::FindParentsByType(node, QDQ::DQOpName);
  std::vector<const Node*> q_nodes = graph_utils::FindChildrenByType(node, QDQ::QOpName);

  if (!Check(graph, node, dq_nodes, q_nodes)) {
    return false;
  }

  ConstNodesToOptimizeBuilder builder;
  builder.input_nodes.reserve(dq_nodes.size());
  builder.output_nodes.reserve(q_nodes.size());

  for (const Node* dq_node : dq_nodes) {
    builder.input_nodes.push_back(dq_node != nullptr ? dq_node : nullptr);
  }

  builder.target_node = &node;

  for (const Node* q_node : q_nodes) {
    builder.output_nodes.push_back(q_node);
  }

  UpdateBuilder(builder);

  selection = builder.Build();

  return true;
}

inline bool UnarySelector::Check(const Graph& graph, const Node& node,
                                 const std::vector<const Node*>& dq_nodes,
                                 const std::vector<const Node*>& q_nodes) const {
  if (!CheckQDQNodes(graph, node, dq_nodes, q_nodes, 1)) {
    return false;
  }

  int32_t dt_input = dq_nodes[0]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  int32_t dt_output = q_nodes[0]->OutputDefs()[0]->TypeAsProto()->tensor_type().elem_type();

  return ((dt_input == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8 ||
           (int8_allowed_ && dt_input == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT8))) &&
         ((dt_output == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8 ||
           (int8_allowed_ && dt_output == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT8)));
}

inline bool BinarySelector::Check(const Graph& graph,
                                  const Node& node,
                                  const std::vector<const Node*>& dq_nodes,
                                  const std::vector<const Node*>& q_nodes) const {
  if (!CheckQDQNodes(graph, node, dq_nodes, q_nodes)) {
    return false;
  }

  // Currently QLinearAdd and QLinearMul only support activation type uint8_t
  int32_t dt_input_1 = dq_nodes[0]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  int32_t dt_input_2 = dq_nodes[1]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  int32_t dt_output = q_nodes[0]->OutputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  return dt_input_1 == dt_input_2 &&
         dt_input_1 == dt_output;
}

inline bool VariadicSelector::Check(const Graph& graph,
                                    const Node& node,
                                    const std::vector<const Node*>& dq_nodes,
                                    const std::vector<const Node*>& q_nodes) const {
  if (!CheckQDQNodes(graph, node, dq_nodes, q_nodes)) {
    return false;
  }

  // All DQs' inputs and Q's output should have same data type
  int32_t dt_input = dq_nodes[0]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  for (size_t dq_idx = 1; dq_idx < dq_nodes.size(); dq_idx++) {
    if (dt_input != dq_nodes[dq_idx]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type()) {
      return false;
    }
  }

  int32_t dt_output = q_nodes[0]->OutputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  return dt_input == dt_output;
}

inline void VariadicSelector::UpdateBuilder(ConstNodesToOptimizeBuilder& builder) const {
  builder.num_input_defs = 1;  // set to 1 as the first input is variadic
}

inline bool ConvSelector::Check(const Graph& graph,
                                const Node& node,
                                const std::vector<const Node*>& dq_nodes,
                                const std::vector<const Node*>& q_nodes) const {
  if (!CheckQDQNodes(graph, node, dq_nodes, q_nodes)) {
    return false;
  }

  // TODO: only specific to cpu
  // Currently QLinearConv only support activation type uint8_t and output type uint8_t
  // TODO: QDQ conv node uint8_t? support
  int32_t dt_input = dq_nodes[0]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  int32_t dt_output = q_nodes[0]->OutputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  if (dt_input != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8 ||
      dt_output != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8) {
    return false;
  }

  if (dq_nodes.size() < 3) {  // no bias
    return true;
  }

  int32_t dt_bias = dq_nodes[2]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  return dt_bias == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32;
}

inline void ConvSelector::UpdateBuilder(ConstNodesToOptimizeBuilder& builder) const {
  builder.input_nodes.resize(3);  // add nullptr for bias if missing
}

inline bool MatMulSelector::Check(const Graph& graph,
                                  const Node& node,
                                  const std::vector<const Node*>& dq_nodes,
                                  const std::vector<const Node*>& q_nodes) const {
  if (dq_nodes.size() != 2) {
    return false;
  }

  // potential match for QLinearMatMul or MatMulIntegerToFloat
  bool qlinear = !q_nodes.empty();

  if (qlinear) {
    // QLinearMatMul
    if (!CheckQDQNodes(graph, node, dq_nodes, q_nodes)) {
      return false;
    }

    int32_t dt_output = q_nodes[0]->OutputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
    if (dt_output != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8) {
      return false;
    }
  } else {
    // MatMulIntegerToFloat has no Q node, so no call to CheckQDQNodes
  }

  // Currently Quant MatMul only support activation type uint8_t
  int32_t dt_input = dq_nodes[0]->InputDefs()[0]->TypeAsProto()->tensor_type().elem_type();
  return (dt_input == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8);
}

}  // namespace NNAPIQDQ
}  // namespace onnxruntime

#endif  // !defined(ORT_MINIMAL_BUILD)
