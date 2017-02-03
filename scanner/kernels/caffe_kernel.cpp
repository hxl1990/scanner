#include "scanner/kernels/caffe_kernel.h"
#include "scanner/engine/db.h"
#include "toml/toml.h"
#include "caffe/blob.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/db.hpp"
#include "caffe/util/io.hpp"
#include "caffe/common.hpp"
#include "caffe/net.hpp"

namespace scanner {

using caffe::Blob;
using caffe::BlobProto;
using caffe::Caffe;
using caffe::Net;

using CustomNetConfiguration =
  void(*)(const FrameInfo& frame_info, caffe::Net<float>* net);

caffe::Caffe::Brew device_type_to_caffe_mode(DeviceType type) {
  caffe::Caffe::Brew caffe_type;

  switch (type) {
    case DeviceType::GPU:
      caffe_type = caffe::Caffe::GPU;
      break;
    case DeviceType::CPU:
      caffe_type = caffe::Caffe::CPU;
      break;
    default:
      // TODO(apoms): error message
      exit(EXIT_FAILURE);
      break;
  }

  return caffe_type;
}

proto::NetDescriptor descriptor_from_net_file(const std::string& net_file_path) {
  std::ifstream net_file{net_file_path};

  toml::ParseResult pr = toml::parse(net_file);
  if (!pr.valid()) {
    LOG(FATAL) << pr.errorReason;
  }
  const toml::Value& root = pr.value;

  proto::NetDescriptor descriptor;

  auto net = root.find("net");
  if (!net) {
    std::cout << "Missing 'net': net description map" << std::endl;
    exit(EXIT_FAILURE);
  }

  auto model_path = net->find("model");
  if (!model_path) {
    std::cout << "Missing 'net.model': path to model" << std::endl;
    exit(EXIT_FAILURE);
  }
  auto weights_path = net->find("weights");
  if (!weights_path) {
    std::cout << "Missing 'net.weights': path to model weights" << std::endl;
    exit(EXIT_FAILURE);
  }
  auto input_layers = net->find("input_layers");
  if (!input_layers) {
    std::cout << "Missing 'net.input_layers': name of input layers "
              << std::endl;
    exit(EXIT_FAILURE);
  }
  auto output_layers = net->find("output_layers");
  if (!output_layers) {
    std::cout << "Missing 'net.output_layers': name of output layers "
              << std::endl;
    exit(EXIT_FAILURE);
  }
  auto input_format = net->find("input");
  if (!input_format) {
    std::cout << "Missing 'net.input': description of net input format "
              << std::endl;
    exit(EXIT_FAILURE);
  }
  auto dimensions_ordering = input_format->find("dimensions");
  if (!dimensions_ordering) {
    std::cout << "Missing 'net.input.dimensions': ordering of dimensions "
              << "for input format " << std::endl;
    exit(EXIT_FAILURE);
  }
  auto channel_ordering = input_format->find("channel_ordering");
  if (!channel_ordering) {
    std::cout << "Missing 'net.input.channel_ordering': ordering of channels "
              << "for input format " << std::endl;
    exit(EXIT_FAILURE);
  }

  descriptor.set_model_path(model_path->as<std::string>());
  descriptor.set_model_weights_path(weights_path->as<std::string>());
  for (const toml::Value& v : input_layers->as<toml::Array>()) {
    descriptor.add_input_layer_names(v.as<std::string>());
  }
  for (const toml::Value& v : output_layers->as<toml::Array>()) {
    descriptor.add_output_layer_names(v.as<std::string>());
  }

  auto input_width = net->find("input_width");
  auto input_height = net->find("input_height");
  auto preserve_aspect_ratio = net->find("preserve_aspect_ratio");
  bool preserve_aspect = false;
  if (preserve_aspect_ratio) {
    preserve_aspect = preserve_aspect_ratio->as<bool>();
  }
  descriptor.set_preserve_aspect_ratio (preserve_aspect);

  descriptor.set_input_width(-1);
  descriptor.set_input_height(-1);
  if (preserve_aspect) {
    if (input_height) {
      descriptor.set_input_height(input_height->as<i32>());
    } else if (input_width) {
      descriptor.set_input_width(input_width->as<i32>());
    } else {
      std::cout << "'preserve_aspect_ratio': must specify only one of "
                   "input_width or input_height"
                << std::endl;
      exit(EXIT_FAILURE);
    }
  } else if (input_width && input_height) {
    descriptor.set_input_width(input_width->as<i32>());
    descriptor.set_input_height(input_height->as<i32>());
  }

  auto pad_mod = net->find("pad_mod");
  descriptor.set_pad_mod(pad_mod ? pad_mod->as<i32>() : -1);

  auto normalize = net->find("normalize");
  descriptor.set_normalize(normalize ? normalize->as<bool>() : false);

  auto transpose = net->find("transpose");
  descriptor.set_transpose(transpose ? transpose->as<bool>() : false);

  auto mean_image = root.find("mean-image");
  if (!mean_image) {
    std::cout << "Missing 'mean-image': mean image descripton map" << std::endl;
    exit(EXIT_FAILURE);
  }

  if (mean_image->has("colors")) {
    auto mean_blue = mean_image->find("colors.blue");
    if (!mean_blue) {
      std::cout << "Missing 'mean-image.colors.blue'" << std::endl;
      exit(EXIT_FAILURE);
    }
    auto mean_green = mean_image->find("colors.green");
    if (!mean_green) {
      std::cout << "Missing 'mean-image.colors.green'" << std::endl;
      exit(EXIT_FAILURE);
    }
    auto mean_red = mean_image->find("colors.red");
    if (!mean_red) {
      std::cout << "Missing 'mean-image.colors.red'" << std::endl;
      exit(EXIT_FAILURE);
    }

    float blue = mean_blue->as<double>();
    float green = mean_green->as<double>();
    float red = mean_red->as<double>();

    for (const toml::Value& v : channel_ordering->as<toml::Array>()) {
      std::string color = v.as<std::string>();
      if (color == "red") {
        descriptor.add_mean_colors(red);
      } else if (color == "green") {
        descriptor.add_mean_colors(green);
      } else if (color == "blue") {
        descriptor.add_mean_colors(blue);
      }
    }
  } else if (mean_image->has("path")) {
    std::string mean_path = mean_image->get<std::string>("path");

    auto mean_image_width = mean_image->find("width");
    if (!mean_image_width) {
      std::cout << "Missing 'mean-image.width': width of mean" << std::endl;
      exit(EXIT_FAILURE);
    }
    auto mean_image_height = mean_image->find("height");
    if (!mean_image_height) {
      std::cout << "Missing 'mean-image.height': height of mean" << std::endl;
      exit(EXIT_FAILURE);
    }

    descriptor.set_mean_width(mean_image_width->as<int>());
    descriptor.set_mean_height(mean_image_height->as<int>());

    int mean_size = descriptor.mean_width() * descriptor.mean_height();

    // Load mean image
    Blob<float> data_mean;
    BlobProto blob_proto;
    bool result = ReadProtoFromBinaryFile(mean_path, &blob_proto);
    data_mean.FromProto(blob_proto);

    memcpy(descriptor.mutable_mean_image(), data_mean.cpu_data(),
           sizeof(float) * mean_size * 3);
  } else if (!mean_image->has("empty")) {
    std::cout << "Missing 'mean-image.{colors,path,empty}': must specify "
              << "color channel values or path of mean image file or that "
              << "there is no mean" << std::endl;
    exit(EXIT_FAILURE);
  }

  return descriptor;
}

CaffeKernel::CaffeKernel(const Kernel::Config& config)
  : VideoKernel(config),
    device_(config.devices[0]) {
  args_.ParseFromArray(config.args.data(), config.args.size());

  set_device();
  // Initialize our network
  auto& descriptor = args_.net_descriptor();
  net_.reset(new caffe::Net<float>(descriptor.model_path(), caffe::TEST));
  net_->CopyTrainedLayersFrom(descriptor.model_weights_path());
  // Initialize memory
  const boost::shared_ptr<caffe::Blob<float>> input_blob{
    net_->blob_by_name(descriptor.input_layer_names(0))};
  input_blob->Reshape({args_.batch_size(), input_blob->shape(1),
                       input_blob->shape(2), input_blob->shape(3)});

  size_t intended_output = descriptor.output_layer_names().size();
  size_t actual_output = config.output_columns.size();
  LOG_IF(FATAL, intended_output != actual_output)
    << "# output columns in net descriptor (" << intended_output << ") "
    << "does not match number of output columns registered for evaluator "
    << "(" << actual_output << "). If you have multiple net outputs, you must "
    << "registry your own evaluator using the CaffeKernel.";
}

void CaffeKernel::new_frame_info()  {
  i32 frame_width = frame_info_.width();
  i32 frame_height = frame_info_.height();

  set_device();

  auto& descriptor = args_.net_descriptor();
  assert(descriptor.input_layer_names().size() > 0);
  const boost::shared_ptr<caffe::Blob<float>> input_blob{
    net_->blob_by_name(descriptor.input_layer_names(0))};
  if (input_blob->shape(0) != args_.batch_size()) {
    input_blob->Reshape({args_.batch_size(), input_blob->shape(1),
          input_blob->shape(2), input_blob->shape(3)});
  }

  i32 width, height;
  if (descriptor.transpose()) {
    width = frame_height;
    height = frame_width;
  } else {
    width = frame_width;
    height = frame_height;
  }
  if (descriptor.preserve_aspect_ratio()) {
    if (descriptor.input_width() != -1) {
      width = descriptor.input_width();
      f32 scale = static_cast<f32>(descriptor.input_width()) / width;
      width = width * scale;
      height = height * scale;
    } else if (descriptor.input_height() != -1) {
      f32 scale = static_cast<f32>(descriptor.input_height()) / height;
      width = width * scale;
      height = height * scale;
    }
  } else if (descriptor.input_width() != -1) {
    width = descriptor.input_width();
    height = descriptor.input_height();
  }

  if (descriptor.pad_mod() != -1) {
    i32 pad = descriptor.pad_mod();
    width += (width % pad) ? pad - (width % pad) : 0;
    height += (height % pad) ? pad - (height % pad) : 0;
  }

  input_blob->Reshape(
    {input_blob->shape(0), input_blob->shape(1), height, width});
}

void CaffeKernel::execute(const BatchedColumns& input_columns,
                          BatchedColumns& output_columns) {
  check_frame_info(device_, input_columns.back());
  set_device();

  auto& descriptor = args_.net_descriptor();
  std::vector<boost::shared_ptr<caffe::Blob<float>>> input_blobs;
  for (const std::string& name : descriptor.input_layer_names()) {
    input_blobs.emplace_back(net_->blob_by_name(name));
  }
  assert(input_blobs.size() > 0);

  size_t num_outputs = descriptor.output_layer_names().size();
  i32 input_count = (i32)input_columns[0].rows.size();
  i32 out_col_idx = 0;
  i32 batch_size = args_.batch_size();
  for (i32 frame = 0; frame < input_count; frame += batch_size) {
    i32 batch_count = std::min(input_count - frame, batch_size);
    if (input_blobs[0]->shape(0) != batch_count) {
      input_blobs[0]->Reshape({batch_count, input_blobs[0]->shape(1),
            input_blobs[0]->shape(2),
            input_blobs[0]->shape(3)});
    }

    for (i32 i = 0; i < input_blobs.size(); ++i) {
      f32* net_input_buffer = nullptr;
      if (device_.type == DeviceType::GPU) {
        net_input_buffer = input_blobs[i]->mutable_gpu_data();
      } else {
        net_input_buffer = input_blobs[i]->mutable_cpu_data();
      }

      size_t offset = 0;
      for (i32 j = 0; j < batch_count; ++j) {
        memcpy_buffer((u8*)net_input_buffer + offset, device_,
                      input_columns[i].rows[frame + j].buffer, device_,
                      input_columns[i].rows[frame + j].size);
        offset += input_columns[i].rows[frame + j].size;
      }
    }

    // Compute features
    auto net_start = now();
    net_->ForwardPrefilled();
    if (profiler_) {
      cudaDeviceSynchronize();
      profiler_->add_interval("caffe:net", net_start, now());
    }

    // Save batch of frames
    size_t total_size = 0;
    i32 total_rows = num_outputs * batch_count;
    for (size_t i = 0; i < num_outputs; ++i) {
      const std::string& output_layer_name = descriptor.output_layer_names(i);
      const boost::shared_ptr<caffe::Blob<float>> output_blob{
        net_->blob_by_name(output_layer_name)};
      size_t output_length = output_blob->count() / batch_count;
      size_t output_size = output_length * sizeof(float);
      total_size += output_size * batch_count;
    }

    u8* output_block = new_block_buffer(device_, total_size, total_rows);
    std::vector<u8*> dest_buffers, src_buffers;
    std::vector<size_t> sizes;
    for (size_t i = 0; i < num_outputs; ++i) {
      const std::string& output_layer_name = descriptor.output_layer_names(i);
      const boost::shared_ptr<caffe::Blob<float>> output_blob{
          net_->blob_by_name(output_layer_name)};
      size_t output_length = output_blob->count() / batch_count;
      size_t output_size = output_length * sizeof(float);
      dest_buffers.push_back(output_block);
      src_buffers.push_back(
        (u8*) (device_.type == DeviceType::CPU
               ? output_blob->cpu_data()
               : output_blob->gpu_data()));
      sizes.push_back(output_size * batch_count);
      for (i32 b = 0; b < batch_count; b++) {
        output_columns[out_col_idx + i].rows.push_back(
          Row{output_block, output_size});
        output_block += output_size;
      }
    }

    memcpy_vec(dest_buffers, device_, src_buffers, device_, sizes);
  }
}

void CaffeKernel::set_device() {
  caffe::Caffe::set_mode(device_type_to_caffe_mode(device_.type));
  if (device_.type == DeviceType::GPU) {
    CUDA_PROTECT({
        // HACK(apoms): caffe does not keep track of device it was initialized
        //  with. For example, if you call cudaSetDevice here before
        //  Caffe::SetDevice, caffe will think the GPU did not change and not
        //  reinit cublas. Need to patch caffe.
        caffe::Caffe::SetDevice(device_.id);
    });
  }
}

REGISTER_EVALUATOR(Caffe).outputs({"caffe_output"});
REGISTER_KERNEL(Caffe, CaffeKernel).device(DeviceType::CPU).num_devices(1);
REGISTER_KERNEL(Caffe, CaffeKernel).device(DeviceType::GPU).num_devices(1);

}