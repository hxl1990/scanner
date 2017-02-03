#pragma once

#include "scanner/kernels/args.pb.h"
#include "scanner/api/evaluator.h"
#include "scanner/api/kernel.h"
#include "scanner/util/cuda.h"
#include "scanner/util/memory.h"

#include "caffe/blob.hpp"
#include "caffe/common.hpp"
#include "caffe/net.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/db.hpp"
#include "caffe/util/io.hpp"

namespace scanner {

class CaffeKernel : public VideoKernel {
public:
  CaffeKernel(const Kernel::Config& config);
  void new_frame_info() override;
  void execute(const BatchedColumns& input_columns,
               BatchedColumns& output_columns) override;
  void set_device();

protected:
  DeviceHandle device_;
  proto::CaffeArgs args_;
  std::unique_ptr<caffe::Net<float>> net_;
};

proto::NetDescriptor descriptor_from_net_file(const std::string& path);

}