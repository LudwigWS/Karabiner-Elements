#pragma once

#include "boost_utility.hpp"
#include "gcd_utility.hpp"
#include "human_interface_device.hpp"
#include "logger.hpp"
#include "types.hpp"
#include <IOKit/hid/IOHIDManager.h>
#include <unordered_map>

namespace krbn {
class hid_manager final {
public:
  // Return false to ignore device.
  boost::signals2::signal<bool(IOHIDDeviceRef _Nonnull),
                          boost_utility::signal2_combiner_call_while_true>
      device_detecting;

  boost::signals2::signal<void(std::shared_ptr<human_interface_device>)> device_detected;

  boost::signals2::signal<void(std::shared_ptr<human_interface_device>)>
      device_removed;

  hid_manager(const hid_manager&) = delete;

  hid_manager(const std::vector<std::pair<hid_usage_page, hid_usage>>& usage_pairs) : usage_pairs_(usage_pairs),
                                                                                      manager_(nullptr) {
  }

  ~hid_manager(void) {
    stop();
  }

  const std::unordered_map<registry_entry_id, std::shared_ptr<human_interface_device>>& get_hids(void) const {
    return hids_;
  }

  void start(void) {
    if (manager_) {
      stop();
    }

    manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager_) {
      logger::get_logger().error("{0}: failed to IOHIDManagerCreate", __PRETTY_FUNCTION__);
      return;
    }

    if (auto device_matching_dictionaries = iokit_utility::create_device_matching_dictionaries(usage_pairs_)) {
      IOHIDManagerSetDeviceMatchingMultiple(manager_, device_matching_dictionaries);
      CFRelease(device_matching_dictionaries);
    }

    IOHIDManagerRegisterDeviceMatchingCallback(manager_, static_device_matching_callback, this);
    IOHIDManagerRegisterDeviceRemovalCallback(manager_, static_device_removal_callback, this);

    IOHIDManagerScheduleWithRunLoop(manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    refresh_timer_ = std::make_unique<gcd_utility::main_queue_timer>(
        DISPATCH_TIME_NOW,
        5.0 * NSEC_PER_SEC,
        0,
        ^{
          refresh_if_needed();
        });
  }

  void stop(void) {
    // Release manager_ in main thread to avoid callback invocations after object has been destroyed.
    gcd_utility::dispatch_sync_in_main_queue(^{
      refresh_timer_ = nullptr;

      if (manager_) {
        IOHIDManagerUnscheduleFromRunLoop(manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        CFRelease(manager_);
        manager_ = nullptr;
      }

      registry_entry_ids_.clear();
      hids_.clear();
    });
  }

private:
  static void static_device_matching_callback(void* _Nullable context,
                                              IOReturn result,
                                              void* _Nullable sender,
                                              IOHIDDeviceRef _Nonnull device) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<hid_manager*>(context);
    if (!self) {
      return;
    }

    self->device_matching_callback(device);
  }

  void device_matching_callback(IOHIDDeviceRef _Nonnull device) {
    if (!device) {
      return;
    }

    if (!device_detecting(device)) {
      return;
    }

    if (auto registry_entry_id = iokit_utility::find_registry_entry_id(device)) {
      // iokit_utility::find_registry_entry_id will be failed in device_removal_callback at least on macOS 10.13.
      // Thus, we have to memory device and registry_entry_id correspondence by myself.
      registry_entry_ids_[device] = *registry_entry_id;

      // Skip if same device is already matched.
      // (Multiple usage device (e.g. usage::pointer and usage::mouse) will be matched twice.)
      auto it = hids_.find(*registry_entry_id);
      if (it != std::end(hids_)) {
        return;
      }

      auto hid = std::make_shared<human_interface_device>(device, *registry_entry_id);
      hids_[*registry_entry_id] = hid;

      device_detected(hid);
    }
  }

  static void static_device_removal_callback(void* _Nullable context,
                                             IOReturn result,
                                             void* _Nullable sender,
                                             IOHIDDeviceRef _Nonnull device) {
    if (result != kIOReturnSuccess) {
      return;
    }

    auto self = static_cast<hid_manager*>(context);
    if (!self) {
      return;
    }

    self->device_removal_callback(device);
  }

  void device_removal_callback(IOHIDDeviceRef _Nonnull device) {
    if (!device) {
      return;
    }

    // ----------------------------------------

    if (auto registry_entry_id = find_registry_entry_id(device)) {
      auto it = hids_.find(*registry_entry_id);
      if (it != hids_.end()) {
        if (auto hid = it->second) {
          hids_.erase(it);

          hid->set_removed();
          device_removed(hid);
        }
      }

      // There might be multiple devices for one registry_entry_id.
      // (For example, keyboard and mouse combined device.)
      // And there is possibility that removal callback is not called with some device.
      //
      // For example, there are 3 devices for 1 registry_entry_id (4294974284).
      //   - device1 { device:0x7fb9d64078a0 => registry_entry_id:4294974284 }
      //   - device2 { device:0x7fb9d8301390 => registry_entry_id:4294974284 }
      //   - device3 { device:0x7fb9d830a630 => registry_entry_id:4294974284 }
      // And device_removal_callback are called only with device1 and device2.
      //
      // We should remove device1, device2 and device3 at the same time in order to deal with this case.

      for (auto it = std::begin(registry_entry_ids_); it != std::end(registry_entry_ids_);) {
        if (it->second == *registry_entry_id) {
          it = registry_entry_ids_.erase(it);
        } else
          std::advance(it, 1);
      }
    }
  }

  void refresh_if_needed(void) {
    // `device_removal_callback` is sometimes missed
    // and invalid human_interface_devices are remained into hids_.
    // (The problem is likely occur just after wake up from sleep.)
    //
    // We validate the human_interface_device,
    // and then reload devices if there is an invalid human_interface_device.

    for (const auto& hid : hids_) {
      if (!hid.second->validate()) {
        logger::get_logger().warn("Refreshing hid_manager since a dangling human_interface_device is found. ({0})",
                                  hid.second->get_name_for_log());
        start();
        break;
      }
    }
  }

  boost::optional<registry_entry_id> find_registry_entry_id(IOHIDDeviceRef _Nonnull device) {
    auto it = registry_entry_ids_.find(device);
    if (it != std::end(registry_entry_ids_)) {
      return it->second;
    }
    return boost::none;
  }

  std::vector<std::pair<hid_usage_page, hid_usage>> usage_pairs_;
  IOHIDManagerRef _Nullable manager_;
  std::unordered_map<IOHIDDeviceRef, registry_entry_id> registry_entry_ids_;
  std::unordered_map<registry_entry_id, std::shared_ptr<human_interface_device>> hids_;
  std::unique_ptr<gcd_utility::main_queue_timer> refresh_timer_;
};
} // namespace krbn
