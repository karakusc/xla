#include "torch_xla/csrc/runtime/ifrt_computation_client.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/types/span.h"
#include "torch_xla/csrc/runtime/computation_client.h"
#include "torch_xla/csrc/runtime/debug_macros.h"
#include "torch_xla/csrc/runtime/env_vars.h"
#include "torch_xla/csrc/runtime/initialize_pjrt.h"
#include "torch_xla/csrc/runtime/stablehlo_helper.h"
#include "torch_xla/csrc/runtime/tf_logging.h"
#include "torch_xla/csrc/runtime/xla_coordinator.h"
#include "tsl/profiler/lib/traceme.h"
#include "xla/client/xla_builder.h"
#include "xla/client/xla_computation.h"
#include "xla/layout_util.h"
#include "xla/literal.h"
#include "xla/pjrt/distributed/distributed.h"
#include "xla/pjrt/pjrt_api.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/pjrt/pjrt_executable.h"
#include "xla/python/ifrt/compiler.h"
#include "xla/python/ifrt/memory.h"
#include "xla/python/ifrt/sharding.h"
#include "xla/python/pjrt_ifrt/pjrt_array.h"
#include "xla/python/pjrt_ifrt/pjrt_client.h"
#include "xla/python/pjrt_ifrt/xla_sharding.h"
#include "xla/shape.h"

using xla::internal::XlaBuilderFriend;

namespace torch_xla {
namespace runtime {

namespace {

static std::string spmd_device_str = "SPMD:0";

// Initializes a distributed runtime client if dist_service_addr is specified
std::shared_ptr<xla::DistributedRuntimeClient>
MaybeInitializeDistributedRuntimeClient(int local_rank,
                                        std::string dist_service_addr) {
  std::shared_ptr<xla::DistributedRuntimeClient> client;
  if (!dist_service_addr.empty()) {
    xla::DistributedRuntimeClient::Options options;
    /* TODO(jonbolin): Use global rank for multi-host setup */
    options.node_id = local_rank;
    client = xla::GetDistributedRuntimeClient(dist_service_addr, options);
    XLA_CHECK(client->Connect().ok())
        << "Failed to initialize distributed runtime client";
  }
  return std::move(client);
}

// Builds a map from the device's global ordinal to its index in the `devices`
// array.
std::unordered_map<int, int> build_index_map(
    const std::vector<std::string>& devices) {
  std::unordered_map<int, int> device_index;
  for (int i = 0; i < devices.size(); ++i) {
    std::vector<std::string> device_spec = absl::StrSplit(devices[i], ':');
    XLA_CHECK_EQ(device_spec.size(), 2)
        << "Invalid device specification: " << devices[i];
    int global_ordinal = std::stoi(device_spec[1]);
    device_index[global_ordinal] = i;
  }
  return device_index;
}

}  // namespace

std::string IfrtComputationClient::PjRtDeviceToString(
    xla::PjRtDevice* const device) const {
  std::string platform =
      absl::AsciiStrToUpper(device->client()->platform_name());
  int ordinal = global_ordinals_.at(device->id());
  std::string str = absl::StrFormat("%s:%d", platform, ordinal);
  return str;
}

std::vector<std::string> IfrtComputationClient::PjRtDevicesToString(
    absl::Span<xla::PjRtDevice* const> devices) const {
  std::vector<std::string> strs;
  strs.reserve(devices.size());

  for (auto* device : devices) {
    strs.push_back(PjRtDeviceToString(device));
  }

  return strs;
}

IfrtComputationClient::IfrtComputationClient() {
  std::string device_type = sys_util::GetEnvString(env::kEnvPjRtDevice, "");
  client_ = xla::ifrt::PjRtClient::Create(std::move(InitializePjRt(device_type)));

  // PjRtDevice IDs are not guaranteed to be dense, so we need to track
  // a device's global ordinal separately from its device ID. Order the
  // devices by increasing ID to assign global ordinals.
  std::vector<xla::PjRtDevice*> ordered_devices(client_->device_count());
  std::partial_sort_copy(client_->devices().begin(), client_->devices().end(),
                         ordered_devices.begin(), ordered_devices.end(),
                         [](auto& a, auto& b) { return a->id() < b->id(); });
  for (auto* device : ordered_devices) {
    global_ordinals_[device->id()] = global_ordinals_.size();
    std::string device_str = PjRtDeviceToString(device);
    string_to_device_.emplace(device_str, device);
  }

  auto tracked_devices = GetLocalDevices();
  tracked_devices.emplace_back(spmd_device_str);
  operation_manager_ = std::move(OperationManager(std::move(tracked_devices)));
}

IfrtComputationClient::~IfrtComputationClient() {
  // In the GPU case, the PjRtClient depends on the DistributedRuntimeClient
  // tracked in XlaCoordinator, so the PjRtClient must be destroyed first.
  client_ = nullptr;
  coordinator_ = nullptr;
}

bool IfrtComputationClient::CoordinatorInitialized() const {
  return coordinator_ != nullptr;
}

void IfrtComputationClient::InitializeCoordinator(int global_rank,
                                                  int world_size,
                                                  std::string master_addr,
                                                  std::string port) {
  XLA_CHECK(coordinator_ == nullptr)
      << "Can only initialize the XlaCoordinator once.";
  coordinator_ = std::make_unique<XlaCoordinator>(global_rank, world_size,
                                                  master_addr, port);
}

XlaCoordinator& IfrtComputationClient::GetCoordinator() {
  XLA_CHECK(coordinator_ != nullptr)
      << "XlaCoordinator has not been initialized";
  return *coordinator_;
}

void IfrtComputationClient::IfrtData::Assign(
    const torch::lazy::BackendData& data) {
  const IfrtData& pjrt_data = dynamic_cast<const IfrtData&>(data);
  if (&pjrt_data != this) {
    buffer = pjrt_data.buffer;
  }
}

xla::OpSharding IfrtComputationClient::IfrtData::GetSharding() const {
  XLA_CHECK(HasSharding()) << "Check HasSharding first";
  return *sharding_;
}

ComputationClient::DataPtr IfrtComputationClient::CreateDataPlaceholder(
    std::string device, xla::Shape shape) {
  return std::make_shared<IfrtData>(device, shape);
}

std::vector<ComputationClient::DataPtr> IfrtComputationClient::GetDataShards(
    ComputationClient::DataPtr data) {
  tsl::profiler::TraceMe activity("IfrtComputationClient::GetDataShards",
                                  tsl::profiler::TraceMeLevel::kInfo);
  std::vector<ComputationClient::DataPtr> shards;
  if (data->HasSharding()) {
    auto ifrt_data = std::dynamic_pointer_cast<IfrtData>(data);
    std::vector<tsl::RCReference<xla::ifrt::Array>> arrays = ifrt_data->buffer->DisassembleIntoSingleDeviceArrays(xla::ifrt::ArrayCopySemantics::kAlwaysCopy).value();

    for (auto array : arrays) {
      shards.push_back(std::make_shared<IfrtData>(
          PjRtDeviceToString(array->sharding().devices()[0]), array));
    }
  } else {
    shards.push_back(data);
  }
  return shards;
}

ComputationClient::DataPtr IfrtComputationClient::GetDataShard(
    ComputationClient::DataPtr data, size_t index) {
  tsl::profiler::TraceMe activity("IfrtComputationClient::GetDataShard",
                                  tsl::profiler::TraceMeLevel::kInfo);
  return GetDataShards(data)[index];
}

ComputationClient::DataPtr IfrtComputationClient::WrapDataShards(
    const std::vector<DataPtr>& shards, std::string device, xla::Shape shape,
    xla::OpSharding sharding) {
  // TODO: implement CreateDataPlaceholder for sharded data
  if (shards.size() == 0) {
    TF_LOG(INFO) << "creating sharded placeholder";
    return std::make_shared<IfrtData>(device, shape, tsl::RCReference<xla::ifrt::Array>(), sharding);
  }
  std::vector<tsl::RCReference<xla::ifrt::Array>> arrays;
  std::vector<xla::ifrt::Shape> shard_shapes;
  for (auto& shard : shards) {
    auto ifrt_shard = std::dynamic_pointer_cast<IfrtData>(shard);
    arrays.push_back(ifrt_shard->buffer);
    shard_shapes.push_back(ifrt_shard->buffer->shape());
  }
  xla::ifrt::Shape ifrt_shape(shape.dimensions());
  xla::ifrt::DeviceList devices_list({client_->addressable_devices().begin(), client_->addressable_devices().end()});
  XLA_CHECK_EQ(shard_shapes.size(), devices_list.size());
  std::unique_ptr<xla::ifrt::Sharding> ifrt_sharding = xla::ifrt::ConcreteSharding::Create(
    devices_list,
    xla::ifrt::MemoryKind(),
    ifrt_shape,
    shard_shapes
  );
  // TODO: Attach HloSharding instead when it is supported
  // std::unique_ptr<xla::ifrt::Sharding> ifrt_sharding = xla::ifrt::HloSharding::Create(
  //   devices_list,
  //   xla::ifrt::MemoryKind(),
  //   xla::HloSharding::FromProto(sharding).value()
  // );
  tsl::RCReference<xla::ifrt::Array> sharded_array = client_->AssembleArrayFromSingleDeviceArrays(
    ifrt_shape, std::move(ifrt_sharding), absl::MakeSpan(arrays), xla::ifrt::ArrayCopySemantics::kAlwaysCopy).value();
  return std::make_shared<IfrtData>(device, shape, sharded_array, sharding);
}

std::optional<xla::OpSharding> IfrtComputationClient::GetDataSharding(
    DataPtr handle) {
  auto ifrt_data = std::dynamic_pointer_cast<IfrtData>(handle);
  return ifrt_data->sharding_;
}

std::vector<ComputationClient::DataPtr> IfrtComputationClient::TransferToServer(
      absl::Span<const std::shared_ptr<const TensorSource>> tensors) {
  metrics::TimedSection timed(TransferToServerMetric());
  tsl::profiler::TraceMe activity("IfrtComputationClient::TransferToServer",
                                  tsl::profiler::TraceMeLevel::kInfo);
  std::vector<ComputationClient::DataPtr> datas;
  datas.reserve(tensors.size());
  int64_t total_size = 0;
  for (auto& tensor : tensors) {
    xla::PjRtDevice* pjrt_device = StringToPjRtDevice(tensor->device());

    total_size += xla::ShapeUtil::ByteSizeOf(tensor->shape());

    tsl::RCReference<xla::ifrt::Array> buffer =
        client_
            ->MakeArrayFromHostBuffer(
                tensor->data(),
                xla::ifrt::ToDType(tensor->primitive_type()).value(),
                xla::ifrt::Shape(tensor->dimensions()),
                tensor->byte_strides(),
                // TODO: what is MemoryKind?
                xla::ifrt::SingleDeviceSharding::Create(
                    pjrt_device, xla::ifrt::MemoryKind()),
                xla::PjRtClient::HostBufferSemantics::
                    kImmutableUntilTransferCompletes,
                [tensor]() { /* frees tensor */ })
            .value();

    ComputationClient::DataPtr data =
        std::make_shared<IfrtData>(tensor->device(), tensor->shape(), buffer);
    datas.push_back(data);
  }
  OutboundDataMetric()->AddSample(total_size);
  CreateDataHandlesCounter()->AddValue(datas.size());

  return datas;
}

ComputationClient::DataPtr IfrtComputationClient::TransferShardsToServer(
      absl::Span<const std::shared_ptr<const TensorSource>> tensor_shards,
      std::string device, xla::Shape shape, xla::OpSharding sharding) {
  tsl::profiler::TraceMe activity(
      "IfrtComputationClient::TransferShardsToServer",
      tsl::profiler::TraceMeLevel::kInfo);
  // TODO(jonbolin): Consider using CopyToDevice when sharding is REPLICATED.
  // We are opting out of CopyToDevice for now due to the synchronization
  // issues observed in ShardingUtil::InputHandler, but because CopyToDevice
  // directly copies buffers between devices using ICI, it can be much faster
  // than transferring from the host to each device.
  auto data_shards = TransferToServer(tensor_shards);
  std::vector<tsl::RCReference<xla::ifrt::Array>> arrays;
  std::vector<xla::ifrt::Shape> shard_shapes;
  for (auto& shard : data_shards) {
    auto ifrt_shard = std::dynamic_pointer_cast<IfrtData>(shard);
    arrays.push_back(ifrt_shard->buffer);
    shard_shapes.push_back(ifrt_shard->buffer->shape());
  }
  xla::ifrt::Shape ifrt_shape(shape.dimensions());
  xla::ifrt::DeviceList devices_list({client_->addressable_devices().begin(), client_->addressable_devices().end()});
  std::unique_ptr<xla::ifrt::Sharding> ifrt_sharding = xla::ifrt::ConcreteSharding::Create(
    devices_list,
    xla::ifrt::MemoryKind(),
    ifrt_shape,
    shard_shapes
  );
  // TODO: Attach HloSharding instead when it is supported
  // std::unique_ptr<xla::ifrt::Sharding> ifrt_sharding = xla::ifrt::HloSharding::Create(
  //   devices_list,
  //   xla::ifrt::MemoryKind(),
  //   xla::HloSharding::FromProto(sharding).value()
  // );
  tsl::RCReference<xla::ifrt::Array> sharded_array = client_->AssembleArrayFromSingleDeviceArrays(
    ifrt_shape, std::move(ifrt_sharding), absl::MakeSpan(arrays), xla::ifrt::ArrayCopySemantics::kAlwaysCopy).value();
  return std::make_shared<IfrtData>(device, shape, sharded_array, sharding);
}

ComputationClient::DataPtr IfrtComputationClient::CopyToDevice(
    ComputationClient::DataPtr data, std::string dst) {
  XLA_ERROR() << __FUNCTION__ << " not implemented";
}

tsl::RCReference<xla::ifrt::Array> IfrtComputationClient::ReplicateShardedData(
    const std::shared_ptr<IfrtData> handle) {

  if (handle->buffer->sharding().devices().size() == 1) {
    return handle->buffer;
  }

  XLA_COUNTER("ReplicateShardedData", 1);
  TF_VLOG(1) << "ReplicateShardedData (handle=" << handle->GetHandle()
              << ", shape=" << handle->shape() << ")";
  // TODO: handle replicated data
  // if (sharded_data->GetSharding().type() == xla::OpSharding::REPLICATED) {
  //   // Data is replicated, return the first shard
  //   return sharded_data->shards[0];
  // }
  xla::XlaBuilder builder("ReplicateShardedData");
  xla::Shape shape = handle->shape();
  builder.SetSharding(handle->GetSharding());

  // perform a simple identity calculation to reassemble the input as
  // replicated output.
  xla::XlaOp x = xla::Parameter(&builder, 0, shape, "p0");
  builder.SetSharding(xla::HloSharding::Replicate().ToProto());
  xla::XlaOp scalar_zero_op = xla::ConvertElementType(
      xla::ConstantR0(&builder, 0), shape.element_type());
  xla::XlaOp y = xla::Add(x, scalar_zero_op);
  auto instruction = XlaBuilderFriend::GetInstruction(y);
  *instruction->mutable_sharding() =
  xla::HloSharding::Replicate().ToProto();

  xla::XlaComputation computation =
      ConsumeValue(builder.Build(/*remove_dynamic_dimensions=*/false));
  xla::ProgramShape program_shape =
      ConsumeValue(computation.GetProgramShape());

  std::string device = GetDefaultDevice();
  std::vector<torch_xla::runtime::ComputationClient::CompileInstance>
      instances;
  instances.push_back({std::move(computation), device,
                        GetCompilationDevices(device, {}), &shape,
                        /*should_wrap_parameter=*/false,
                        /*is_sharded=*/true,
                        /*allow_spmd_sharding_propagation_to_output=*/false});
  std::vector<
      std::shared_ptr<torch_xla::runtime::ComputationClient::Computation>>
      computations = Compile(std::move(instances));

  XLA_CHECK_EQ(handle->buffer->sharding().devices().size(), GetLocalDevices().size());

  torch_xla::runtime::ComputationClient::ExecuteReplicatedOptions
      execute_options;

  auto sharded_results =
      ExecuteReplicated(*computations.front(), {{handle}},
                        GetLocalDevices(), execute_options);
  auto replicated_output = std::dynamic_pointer_cast<IfrtData>(sharded_results[0])->buffer->FullyReplicatedShard(xla::ifrt::ArrayCopySemantics::kAlwaysCopy);
  // TODO: sanity check outputs
  return *replicated_output;
}

std::vector<xla::Literal> IfrtComputationClient::TransferFromServer(
    absl::Span<const DataPtr> handles) {
  metrics::TimedSection timed(TransferFromServerMetric());
  tsl::profiler::TraceMe activity("IfrtComputationClient::TransferFromServer",
                                  tsl::profiler::TraceMeLevel::kInfo);
  std::vector<xla::Literal> literals;
  literals.reserve(handles.size());
  int64_t total_size = 0;
  for (auto handle : handles) {
    // Use XLA replication to reassemble the sharded data. If input handle
    // is not sharded, then it is a no-op.
    auto ifrt_data = std::dynamic_pointer_cast<IfrtData>(handle);
    tsl::RCReference<xla::ifrt::Array> replicated_array = ReplicateShardedData(ifrt_data);

    // TODO: handle dynamic shapes
    auto& literal = literals.emplace_back(
        xla::ShapeUtil::DeviceShapeToHostShape(ifrt_data->shape()));
    std::vector<int64_t> byte_strides(literal.shape().dimensions_size());
    XLA_CHECK_OK(xla::ShapeUtil::ByteStrides(literal.shape(),
                                             absl::MakeSpan(byte_strides)));
    XLA_CHECK_OK(
        replicated_array
            ->CopyToHostBuffer(literal.untyped_data(), byte_strides,
                               xla::ifrt::ArrayCopySemantics::kAlwaysCopy)
            .Await());

    total_size += literal.size_bytes();
  }
  InboundDataMetric()->AddSample(total_size);

  return literals;
}

std::vector<ComputationClient::ComputationPtr> IfrtComputationClient::Compile(
    std::vector<ComputationClient::CompileInstance> instances) {
  metrics::TimedSection timed(CompileMetric());
  tsl::profiler::TraceMe activity("IfrtComputationClient::Compile",
                                  tsl::profiler::TraceMeLevel::kInfo);
  std::vector<ComputationClient::ComputationPtr> computations;

  for (auto& instance : instances) {
    xla::CompileOptions compile_options;
    if (instance.is_sharded) {
      // TODO(yeounoh) multi-host, multi-slice configurations
      compile_options.executable_build_options.set_use_spmd_partitioning(true);
      // We can override the compiler's default behavior to replicate the
      // outputs. Setting this to true would wrapping the sharded outputs in
      // PjRtShardedData.
      compile_options.executable_build_options
          .set_allow_spmd_sharding_propagation_to_output(
              {instance.allow_spmd_sharding_propagation_to_output});
      compile_options.executable_build_options.set_num_partitions(
          client_->device_count());
      compile_options.executable_build_options.set_num_replicas(1);
      compile_options.parameter_is_tupled_arguments =
          instance.parameter_is_tupled_arguments;

      // TODO(244391366) verify this is correct for the collectives ops
      xla::DeviceAssignment device_assignment(1, client_->device_count());
      // DeviceAssignment values must be the PjRtDevice ID, so we need to
      // unwind the global ordinal mapping.
      for (const auto& [device_id, global_ordinal] : global_ordinals_) {
        device_assignment(0, global_ordinal) = device_id;
      }
      compile_options.executable_build_options.set_device_assignment(
          device_assignment);
    } else {
      // TODO(wcromar): set compile_options.argument_layouts, enable strict
      // shapes
      compile_options.executable_build_options.set_num_partitions(1);
      compile_options.executable_build_options.set_num_replicas(
          client_->device_count());
      compile_options.parameter_is_tupled_arguments =
          instance.parameter_is_tupled_arguments;

      xla::DeviceAssignment device_assignment(client_->device_count(), 1);
      // DeviceAssignment values must be the PjRtDevice ID, so we need to
      // unwind the global ordinal mapping.
      for (const auto& [device_id, global_ordinal] : global_ordinals_) {
        device_assignment(global_ordinal, 0) = device_id;
      }
      compile_options.executable_build_options.set_device_assignment(
          device_assignment);
    }

    // Convert HLO to StableHLO for Ifrt client compilation.
    mlir::MLIRContext context;
    mlir::ModuleOp mlir_module =
        mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
    torch_xla::runtime::ConvertHloToStableHlo(
        instance.computation.mutable_proto(), &mlir_module);
    std::unique_ptr<xla::ifrt::LoadedExecutable> executable =
        ConsumeValue(client_->GetDefaultCompiler()->Compile(
            std::make_unique<xla::ifrt::XlaProgram>(std::move(mlir_module)),
            std::make_unique<xla::ifrt::XlaCompileOptions>(compile_options)));
    StableHloCompileCounter()->AddValue(1);

    const auto& hlo_modules = ConsumeValue(executable->GetHloModules());
    xla::HloComputation* hlo_computation = hlo_modules[0]->entry_computation();

    std::shared_ptr<IfrtComputation> pjrt_computation =
        std::make_shared<IfrtComputation>(
            std::move(xla::XlaComputation(hlo_modules[0]->ToProto())),
            instance.devices, std::move(executable));

    computations.push_back(pjrt_computation);

    CreateCompileHandlesCounter()->AddValue(1);
  }

  return computations;
}

std::vector<ComputationClient::DataPtr>
IfrtComputationClient::ExecuteComputation(
    const ComputationClient::Computation& computation,
    absl::Span<const ComputationClient::DataPtr> arguments,
    const std::string& device, const ExecuteComputationOptions& options) {
  // TODO: Implement sharded exec in IFRT
  XLA_ERROR() << __FUNCTION__ << " not implemented";
  // // Shared ownership of the timed section ensures that it will only get logged
  // // once both `ExecuteComputation` and the async work in `ExecuteSharded` are
  // // complete; a copy is held from the lambda that releases it when done.
  // auto timed = std::make_shared<metrics::TimedSection>(ExecuteMetric());
  // tsl::profiler::TraceMe activity("IfrtComputationClient::ExecuteComputation",
  //                                 tsl::profiler::TraceMeLevel::kInfo);
  // TF_VLOG(1) << "Executing Ifrt computation on " << device;
  // const IfrtComputation& pjrt_computation =
  //     dynamic_cast<const IfrtComputation&>(computation);

  // xla::PjRtDevice* pjrt_device = StringToPjRtDevice(device);
  // XLA_CHECK(pjrt_device->IsAddressable()) << pjrt_device->DebugString();

  // std::vector<tsl::RCReference<xla::ifrt::Array>> buffers;
  // buffers.reserve(arguments.size());
  // for (auto& argument : arguments) {
  //   const IfrtData* pjrt_data = dynamic_cast<IfrtData*>(argument.get());

  //   // XLA_CHECK(pjrt_device == pjrt_data->buffer->device())
  //   //     << pjrt_device->DebugString() << " vs "
  //   //     << pjrt_data->buffer->device()->DebugString();
  //   buffers.push_back(pjrt_data->buffer);
  // }

  // xla::ExecuteOptions execute_options;
  // execute_options.untuple_result = options.explode_tuple;
  // execute_options.strict_shape_checking = false;

  // // Required as of cl/518733871
  // execute_options.use_major_to_minor_data_layout_for_callbacks = true;

  // xla::ifrt::DeviceList device_list({pjrt_device});
  // xla::ifrt::LoadedExecutable::ExecuteResult result =
  //     pjrt_computation.executable
  //         ->Execute(absl::MakeSpan(buffers), execute_options, device_list)
  //         .value();

  // xla::ifrt::Future<xla::Status> returned_future = result.status;

  // auto results = result.outputs;
  // std::vector<DataPtr> datas;
  // datas.reserve(results.size());
  // for (auto& result : results) {
  //   tsl::RCReference<xla::ifrt::Array> buffer = std::move(result);

  //   std::shared_ptr<IfrtData> data =
  //       std::make_shared<IfrtData>(device, std::move(buffer));

  //   datas.push_back(data);
  // }
  // CreateDataHandlesCounter()->AddValue(datas.size());

  // auto mwait = std::make_shared<util::MultiWait>(1);
  // auto lockfn = [&, this, device, returned_future = std::move(returned_future),
  //                timed]() mutable {
  //   TF_VLOG(5) << "ExecuteComputation acquiring PJRT device lock for "
  //              << device;
  //   // Grab the shared lock and block the `WaitDeviceOps` until buffer is
  //   // ready.
  //   // TODO(JackCaoG): This lock should acquired outside of the lockfn and
  //   // passed in. It is possible that lockfn started after ExecuteComputation
  //   // released the xla_graph_executor lock, which will create a short windows
  //   // where device is unlcoked while execution is still running.
  //   auto lock = lock_device_shared(device);
  //   TF_VLOG(5) << "ExecuteComputation acquiring PJRT device lock for " << device
  //              << " Done";
  //   // Signal that `ExecuteSharded` has completed for the ExecuteTime
  //   // metric. Copies the `timed` shared pointer into the lambda.
  //   XLA_CHECK(returned_future.IsValid())
  //       << "returned_future in ExecuteComputation is empty";
  //   returned_future.OnReady(
  //       [timed, lock = std::move(lock)](xla::Status unused) mutable {
  //         timed.reset();
  //         TF_VLOG(3) << "ExecuteComputation returned_future->OnReady finished";
  //       });
  // };

  // env::ScheduleIoClosure(util::MultiWait::Completer(mwait, std::move(lockfn)));

  // TF_VLOG(1) << "Returning " << datas.size() << " results";
  // return datas;
}

std::vector<ComputationClient::DataPtr>
IfrtComputationClient::ExecuteReplicated(
    const ComputationClient::Computation& computation,
    const absl::Span<const ComputationClient::DataPtr> arguments,
    // TODO: devices isn't doing anything helpful here
    absl::Span<const std::string> devices,
    const ExecuteReplicatedOptions& options) {
  // XLA_ERROR() << __FUNCTION__ << " not implemented";
  // Shared ownership of the timed section ensures that it will only get logged
  // once both `ExecuteReplicated` and the async work in `Execute` are
  // complete; a copy is held from the lambda that releases it when done.
  // TODO: fix timing
  auto timed =
      std::make_shared<metrics::TimedSection>(ExecuteReplicatedMetric());
  tsl::profiler::TraceMe activity("IfrtComputationClient::ExecuteReplicated",
                                  tsl::profiler::TraceMeLevel::kInfo);
  const IfrtComputation& ifrt_computation =
      dynamic_cast<const IfrtComputation&>(computation);
  // XLA_CHECK(devices.size() == arguments.size())
  //     << "ExecuteReplicated over " << devices.size() << " devices, but "
  //     << arguments.size() << " arguments devices.";
  // TODO: parallelize again if necessary
  std::vector<tsl::RCReference<xla::ifrt::Array>> argument_handles(arguments.size());
  for (int32_t i = 0; i < arguments.size(); ++i) {
    auto ifrt_data = std::dynamic_pointer_cast<IfrtData>(arguments[i]);
    argument_handles[i] = ifrt_data->buffer;
  }

  xla::ExecuteOptions execute_options;
  execute_options.untuple_result = options.explode_tuple;
  execute_options.strict_shape_checking = true;
  // TODO(yeounoh) currently only support single-slice execution
  execute_options.multi_slice_config = nullptr;

  xla::ifrt::LoadedExecutable::ExecuteResult result =
      ifrt_computation.executable
          ->Execute(absl::MakeSpan(argument_handles), execute_options, std::nullopt)
          .value();

  xla::ifrt::Future<xla::Status> returned_future = result.status;
  auto results = result.outputs;

  std::vector<ComputationClient::DataPtr> data_handles;
  data_handles.reserve(results.size());

  XLA_CHECK(ifrt_computation.executable->GetOutputShardings().has_value());
  auto output_shardings = *(ifrt_computation.executable->GetOutputShardings());
  XLA_CHECK_EQ(output_shardings.size(), results.size());

  for (int32_t i = 0; i < results.size(); ++i) {
    std::shared_ptr<IfrtData> data =
        std::make_shared<IfrtData>("SPMD:0", results[i], output_shardings[i]);
    data_handles.push_back(data);
  }

  // TODO: any useful debug logging
  return {data_handles};
}

size_t IfrtComputationClient::GetNumDevices() const {
  return client_->addressable_device_count();
}

std::string IfrtComputationClient::GetDefaultDevice() const {
  return PjRtDeviceToString(client_->addressable_devices()[0]);
}

std::vector<std::string> IfrtComputationClient::GetLocalDevices() const {
  return PjRtDevicesToString(client_->addressable_devices());
}

std::vector<std::string> IfrtComputationClient::GetAllDevices() const {
  return PjRtDevicesToString(client_->devices());
}

int IfrtComputationClient::GetNumProcesses() const {
  int max_process_index = client_->process_index();
  for (auto* device : client_->devices()) {
    max_process_index = std::max(max_process_index, device->process_index());
  }

  return max_process_index + 1;
};

const absl::flat_hash_map<
    std::string, torch_xla::runtime::ComputationClient::DeviceAttribute>&
IfrtComputationClient::GetDeviceAttributes(const std::string& device) {
  return IfrtComputationClient::StringToPjRtDevice(device)->Attributes();
}

void IfrtComputationClient::SetReplicationDevices(
    std::shared_ptr<std::vector<std::string>> devices) {
  replication_devices_ = std::move(devices);
}

std::shared_ptr<std::vector<std::string>>
IfrtComputationClient::GetReplicationDevices() {
  return replication_devices_;
}

xla::PjRtDevice* IfrtComputationClient::StringToPjRtDevice(
    const std::string& device) {
  XLA_CHECK(string_to_device_.find(device) != string_to_device_.end())
      << "Unknown device " << device;
  xla::PjRtDevice* pjrt_device = string_to_device_[device];
  return pjrt_device;
}

void IfrtComputationClient::WaitDeviceOps(
    absl::Span<const std::string> devices) {
  TF_VLOG(3) << "Waiting for " << absl::StrJoin(devices, ", ");
  operation_manager_.WaitForDevices(devices.empty() ? GetLocalDevices()
                                                    : devices);
}

std::map<std::string, Metric> IfrtComputationClient::GetMetrics() const {
  // TODO(jonbolin): Add any PJRt-client-specific metrics here
  return {};
}

}  // namespace runtime
}  // namespace torch_xla