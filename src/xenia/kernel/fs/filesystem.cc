/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/fs/filesystem.h"

#include "poly/string.h"
#include "xenia/kernel/fs/devices/disc_image_device.h"
#include "xenia/kernel/fs/devices/host_path_device.h"
#include "xenia/kernel/fs/devices/stfs_container_device.h"
#include "poly/fs.h"

namespace xe {
namespace kernel {
namespace fs {

FileSystem::FileSystem() {}

FileSystem::~FileSystem() {
  // Delete all devices.
  // This will explode if anyone is still using data from them.
  for (std::vector<Device*>::iterator it = devices_.begin();
       it != devices_.end(); ++it) {
    delete *it;
  }
  devices_.clear();
  symlinks_.clear();
}

fs::FileSystemType FileSystem::InferType(const std::wstring& local_path) {
  auto last_slash = local_path.find_last_of(poly::path_separator);
  auto last_dot = local_path.find_last_of('.');
  if (last_dot < last_slash) {
    last_dot = std::wstring::npos;
  }
  if (last_dot == std::wstring::npos) {
    // Likely an STFS container.
    return FileSystemType::STFS_TITLE;
  } else if (local_path.substr(last_dot) == L".xex") {
    // Treat as a naked xex file.
    return FileSystemType::XEX_FILE;
  } else {
    // Assume a disc image.
    return FileSystemType::DISC_IMAGE;
  }
}

int FileSystem::InitializeFromPath(fs::FileSystemType type,
                                   const std::wstring& local_path) {
  switch (type) {
    case FileSystemType::STFS_TITLE: {
      // Register the container in the virtual filesystem.
      int result_code =
          RegisterSTFSContainerDevice("\\Device\\Cdrom0", local_path);
      if (result_code) {
        XELOGE("Unable to mount STFS container");
        return result_code;
      }

      // TODO(benvanik): figure out paths.
      // Create symlinks to the device.
      CreateSymbolicLink("game:", "\\Device\\Cdrom0");
      CreateSymbolicLink("d:", "\\Device\\Cdrom0");
      break;
    }
    case FileSystemType::XEX_FILE: {
      // Get the parent path of the file.
      auto last_slash = local_path.find_last_of(poly::path_separator);
      std::wstring parent_path = local_path.substr(0, last_slash);

      // Register the local directory in the virtual filesystem.
      int result_code = RegisterHostPathDevice(
          "\\Device\\Harddisk1\\Partition0", parent_path, true);
      if (result_code) {
        XELOGE("Unable to mount local directory");
        return result_code;
      }

      // Create symlinks to the device.
      CreateSymbolicLink("game:", "\\Device\\Harddisk1\\Partition0");
      CreateSymbolicLink("d:", "\\Device\\Harddisk1\\Partition0");
      break;
    }
    case FileSystemType::DISC_IMAGE: {
      // Register the disc image in the virtual filesystem.
      int result_code = RegisterDiscImageDevice("\\Device\\Cdrom0", local_path);
      if (result_code) {
        XELOGE("Unable to mount disc image");
        return result_code;
      }

      // Create symlinks to the device.
      CreateSymbolicLink("game:", "\\Device\\Cdrom0");
      CreateSymbolicLink("d:", "\\Device\\Cdrom0");
      break;
    }
  }
  return 0;
}

int FileSystem::RegisterDevice(const std::string& path, Device* device) {
  devices_.push_back(device);
  return 0;
}

int FileSystem::RegisterHostPathDevice(const std::string& path,
                                       const std::wstring& local_path,
                                       bool read_only) {
  Device* device = new HostPathDevice(path, local_path, read_only);
  return RegisterDevice(path, device);
}

int FileSystem::RegisterDiscImageDevice(const std::string& path,
                                        const std::wstring& local_path) {
  DiscImageDevice* device = new DiscImageDevice(path, local_path);
  if (device->Init()) {
    return 1;
  }
  return RegisterDevice(path, device);
}

int FileSystem::RegisterSTFSContainerDevice(const std::string& path,
                                            const std::wstring& local_path) {
  STFSContainerDevice* device = new STFSContainerDevice(path, local_path);
  if (device->Init()) {
    return 1;
  }
  return RegisterDevice(path, device);
}

int FileSystem::CreateSymbolicLink(const std::string& path,
                                   const std::string& target) {
  symlinks_.insert({path, target});
  return 0;
}

int FileSystem::DeleteSymbolicLink(const std::string& path) {
  auto& it = symlinks_.find(path);
  if (it == symlinks_.end()) {
    return 1;
  }
  symlinks_.erase(it);
  return 0;
}

std::unique_ptr<Entry> FileSystem::ResolvePath(const std::string& path) {
  // Resolve relative paths
  std::string normalized_path(poly::fs::CanonicalizePath(path));

  // If no path (starts with a slash) do it module-relative.
  // Which for now, we just make game:.
  if (normalized_path[0] == '\\') {
    normalized_path = "game:" + normalized_path;
  }

  // Resolve symlinks.
  // TODO(benvanik): more robust symlink handling - right now we assume simple
  //     drive path -> device mappings with nothing nested.
  std::string full_path = normalized_path;
  for (const auto& it : symlinks_) {
    if (poly::find_first_of_case(normalized_path, it.first) == 0) {
      // Found symlink, fixup by replacing the prefix.
      full_path = it.second + full_path.substr(it.first.size());
      break;
    }
  }

  // Scan all devices.
  for (auto& device : devices_) {
    if (poly::find_first_of_case(full_path, device->path()) == 0) {
      // Found! Trim the device prefix off and pass down.
      auto device_path = full_path.substr(device->path().size());
      return device->ResolvePath(device_path.c_str());
    }
  }

  XELOGE("ResolvePath(%s) failed - no root found", path.c_str());
  return nullptr;
}

X_STATUS FileSystem::Open(std::unique_ptr<Entry> entry,
                          KernelState* kernel_state, Mode mode, bool async,
                          XFile** out_file) {
  auto result = entry->Open(kernel_state, mode, async, out_file);
  if (XSUCCEEDED(result)) {
    entry.release();
  }
  return result;
}

}  // namespace fs
}  // namespace kernel
}  // namespace xe
