// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "aibench/executors/mace/mace_executor.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace aibench {

inline Status ReadBinaryFile(std::vector<unsigned char> *data,
                             const std::string &filename) {
  std::ifstream ifs(filename, std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    return Status::RUNTIME_ERROR;
  }
  ifs.seekg(0, ifs.end);
  size_t length = ifs.tellg();
  ifs.seekg(0, ifs.beg);

  data->reserve(length);
  data->insert(data->begin(), std::istreambuf_iterator<char>(ifs),
               std::istreambuf_iterator<char>());
  if (ifs.fail()) {
    return Status::RUNTIME_ERROR;
  }
  ifs.close();

  return Status::SUCCESS;
}

mace::DeviceType GetDeviceType(const Runtime &runtime) {
  switch (runtime) {
    case CPU: return mace::DeviceType::CPU;
    case GPU: return mace::DeviceType::GPU;
    case DSP: return mace::DeviceType::HEXAGON;
    default: return mace::DeviceType::CPU;
  }
}

Status MaceExecutor::CreateEngine(const char *model_name,
                                  std::shared_ptr<mace::MaceEngine> *engine) {
  mace::DeviceType device_type = GetDeviceType(GetRuntime());
  mace::MaceEngineConfig config(device_type);
  config.SetCPUThreadPolicy(num_threads_,
                            mace::CPUAffinityPolicy::AFFINITY_BIG_ONLY,
                            true);
  if (device_type == mace::DeviceType::GPU) {
    const char *storage_path_ptr = getenv("MACE_INTERNAL_STORAGE_PATH");
    const std::string storage_path =
        std::string(storage_path_ptr == nullptr ?
                    "./interior" : storage_path_ptr);
    gpu_context_ = mace::GPUContextBuilder()
        .SetStoragePath(storage_path)
        .Finalize();
    config.SetGPUContext(gpu_context_);
    config.SetGPUHints(
        mace::GPUPerfHint::PERF_HIGH,
        mace::GPUPriorityHint::PRIORITY_HIGH);
  }
  std::vector<unsigned char> model_pb_data;
  std::string model_pb_file(model_name);
  model_pb_file.append(".pb");
  if (ReadBinaryFile(&model_pb_data, model_pb_file) != Status::SUCCESS) {
    std::cout << "Failed to read file: " << model_name << std::endl;
    return Status::RUNTIME_ERROR;
  }
  std::string model_data_file(model_name);
  model_data_file.append(".data");
  mace::MaceStatus create_engine_status;
  create_engine_status = mace::CreateMaceEngineFromProto(model_pb_data,
                                                         model_data_file,
                                                         input_names_,
                                                         output_names_,
                                                         config,
                                                         engine);
  return create_engine_status == mace::MACE_SUCCESS ? Status::SUCCESS
                                                    : Status::RUNTIME_ERROR;
}

Status MaceExecutor::Init(const char *model_name, int num_threads) {
  num_threads_ = num_threads;
  mace::DeviceType device_type = GetDeviceType(GetRuntime());
  if (device_type == mace::DeviceType::GPU) {
    // Mace needs to compile opencl kernel once per new target, and since then
    // compiled code is stored on the target, which will speedup following run.
    std::shared_ptr<mace::MaceEngine> engine;
    CreateEngine(model_name, &engine);
  }
  return Status::SUCCESS;
}

Status MaceExecutor::Prepare(const char *model_name) {
  CreateEngine(model_name, &engine_);
  return Status::SUCCESS;
}

Status MaceExecutor::Run(const std::map<std::string, BaseTensor> &inputs,
                         std::map<std::string, BaseTensor> *outputs) {
  (void) outputs;
  std::map<std::string, mace::MaceTensor> mace_inputs;
  std::map<std::string, mace::MaceTensor> mace_outputs;
  for (const auto &input : inputs) {
    mace_inputs[input.first] = mace::MaceTensor(input.second.shape(),
                                                input.second.data());
  }
  for (const auto &output : *outputs) {
    mace_outputs[output.first] = mace::MaceTensor(output.second.shape(),
                                                  output.second.data());
  }
  mace::MaceStatus run_status = engine_->Run(mace_inputs, &mace_outputs);
  return run_status == mace::MACE_SUCCESS ? Status::SUCCESS
                                          : Status::RUNTIME_ERROR;
}

void MaceExecutor::Finish() {
  engine_.reset();
}

}  // namespace aibench
