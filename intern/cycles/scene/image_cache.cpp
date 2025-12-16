/* SPDX-FileCopyrightText: 2011-2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/image_cache.h"

CCL_NAMESPACE_BEGIN

static const char *name_from_type(ImageDataType type)
{
  switch (type) {
    case IMAGE_DATA_TYPE_FLOAT4:
      return "float4";
    case IMAGE_DATA_TYPE_BYTE4:
      return "byte4";
    case IMAGE_DATA_TYPE_HALF4:
      return "half4";
    case IMAGE_DATA_TYPE_FLOAT:
      return "float";
    case IMAGE_DATA_TYPE_BYTE:
      return "byte";
    case IMAGE_DATA_TYPE_HALF:
      return "half";
    case IMAGE_DATA_TYPE_USHORT4:
      return "ushort4";
    case IMAGE_DATA_TYPE_USHORT:
      return "ushort";
    case IMAGE_DATA_TYPE_NANOVDB_FLOAT:
      return "nanovdb_float";
    case IMAGE_DATA_TYPE_NANOVDB_FLOAT3:
      return "nanovdb_float3";
    case IMAGE_DATA_TYPE_NANOVDB_FLOAT4:
      return "nanovdb_float4";
    case IMAGE_DATA_TYPE_NANOVDB_FPN:
      return "nanovdb_fpn";
    case IMAGE_DATA_TYPE_NANOVDB_FP16:
      return "nanovdb_fp16";
    case IMAGE_DATA_TYPE_NANOVDB_EMPTY:
      return "nanovdb_empty";
    case IMAGE_DATA_NUM_TYPES:
      assert(!"System enumerator type, should never be used");
      return "";
  }
  assert(!"Unhandled image data type");
  return "";
}

/* ImageCache::DeviceImage */

ImageCache::DeviceImage::DeviceImage(Device *device,
                                     std::string &&name,
                                     const uint slot,
                                     const ImageDataType type,
                                     const InterpolationType interpolation,
                                     const ExtensionType extension)
    : mem_name(std::move(name)),
      mem(device, mem_name.c_str(), slot, type, interpolation, extension)
{
}

/* ImageCache */

ImageCache::ImageCache() = default;

ImageCache::~ImageCache()
{
  assert(device_images.empty());
}

void ImageCache::device_free()
{
  device_images.clear();
}

device_image &ImageCache::alloc_full(Device *device,
                                     ImageDataType type,
                                     InterpolationType interpolation,
                                     ExtensionType extension,
                                     const int64_t width,
                                     const int64_t height,
                                     uint &slot)
{
  thread_scoped_lock device_lock(device_mutex);

  slot = device_images.size();

  std::string name = string_printf("tex_image_%s_%03u", name_from_type(type), slot);
  unique_ptr<DeviceImage> img = make_unique<DeviceImage>(
      device, std::move(name), slot, type, interpolation, extension);

  img->mem.alloc(width, height);

  device_images.push_back(std::move(img));

  device_image &mem = device_images.back()->mem;
  updated_device_images.insert(&mem);

  return mem;
}

void ImageCache::free_full(const uint slot)
{
  // TODO: shrink array
  thread_scoped_lock device_lock(device_mutex);
  device_images.steal(slot);
}

device_image &ImageCache::alloc_tile(Device *device,
                                     ImageDataType type,
                                     InterpolationType interpolation,
                                     ExtensionType extension,
                                     const int tile_size_padded,
                                     KernelTileDescriptor &r_tile_descriptor)
{
  // TODO: more fine grained locking
  thread_scoped_lock device_lock(device_mutex);

  // TODO: support same interpolation and extension through padding?
  DeviceImage *img = nullptr;
  int tile_offset = -1;

  /* Find image with free space. */
  for (DeviceImage *candidate_img : device_images) {
    if (candidate_img == nullptr || candidate_img->num_free == 0) {
      continue;
    }

    const KernelImageInfo &info = candidate_img->mem.info;
    if (!(info.data_type == type && info.interpolation == interpolation &&
          info.extension == extension && info.height == tile_size_padded))
    {
      continue;
    }

    /* Find unoccupied space in image. */
    img = candidate_img;
    for (int offset = 0; offset < img->occupied.size(); offset++) {
      if (!img->occupied[offset]) {
        tile_offset = offset;
        break;
      }
    }

    break;
  }

  if (img == nullptr) {
    /* Allocate new image. */
    const int slot = device_images.size();

    std::string name = string_printf("tile_image_%s_%03d", name_from_type(type), slot);
    unique_ptr<DeviceImage> new_img = make_unique<DeviceImage>(
        device, std::move(name), slot, type, interpolation, extension);
    img = new_img.get();
    device_images.push_back(std::move(new_img));

    // TODO: move constant out of here. Compute based on memory size?
    const size_t num_tiles = 64;

    img->mem.alloc(tile_size_padded * num_tiles, tile_size_padded);
    img->num_free = num_tiles;
    img->occupied.resize(num_tiles);
    tile_offset = 0;
  }

  /* Mark tile as occupied and compute descriptor. */
  img->occupied[tile_offset] = true;
  img->num_free--;
  r_tile_descriptor = kernel_tile_descriptor_encode(img->mem.slot, tile_offset);

  updated_device_images.insert(&img->mem);

  return img->mem;
}

void ImageCache::free_tile(const KernelTileDescriptor tile)
{
  thread_scoped_lock device_lock(device_mutex);

  const uint tile_slot = kernel_tile_descriptor_slot(tile);
  const uint tile_offset = kernel_tile_descriptor_offset(tile);

  for (size_t i = 0; i < device_images.size(); i++) {
    DeviceImage *img = device_images[i];

    if (img && img->mem.slot == tile_slot) {
      img->occupied[tile_offset] = false;
      img->num_free++;

      if (img->occupied.size() == img->num_free) {
        // TODO: shrink array
        device_images.steal(i);
        return;
      }
    }
  }
}

void ImageCache::copy_to_device_if_modified()
{
  thread_scoped_lock device_lock(device_mutex);

  // TODO: parallellize?
  // TODO: copy only modified subset?
  for (device_image *mem : updated_device_images) {
    mem->copy_to_device();
  }

  updated_device_images.clear();
}

CCL_NAMESPACE_END
