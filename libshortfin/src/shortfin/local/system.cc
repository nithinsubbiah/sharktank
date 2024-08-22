// Copyright 2024 Advanced Micro Devices, Inc
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "shortfin/local/system.h"

#include <fmt/core.h>

#include "shortfin/local/scope.h"
#include "shortfin/support/logging.h"

namespace shortfin::local {

// -------------------------------------------------------------------------- //
// System
// -------------------------------------------------------------------------- //

System::System(iree_allocator_t host_allocator)
    : host_allocator_(host_allocator) {
  SHORTFIN_THROW_IF_ERROR(iree_vm_instance_create(IREE_VM_TYPE_CAPACITY_DEFAULT,
                                                  host_allocator_,
                                                  vm_instance_.for_output()));
  // Register types for builtin modules we know we want to handle.
  SHORTFIN_THROW_IF_ERROR(iree_hal_module_register_all_types(vm_instance_));
}

System::~System() {
  bool needs_shutdown = false;
  {
    iree::slim_mutex_lock_guard guard(lock_);
    if (initialized_ && !shutdown_) {
      needs_shutdown = true;
    }
  }
  if (needs_shutdown) {
    logging::warn(
        "Implicit Shutdown from System destructor. Please call Shutdown() "
        "explicitly for maximum stability.");
    Shutdown();
  }
}

void System::Shutdown() {
  // Stop workers.
  std::vector<std::unique_ptr<Worker>> local_workers;
  {
    iree::slim_mutex_lock_guard guard(lock_);
    if (!initialized_ || shutdown_) return;
    shutdown_ = true;
    workers_by_name_.clear();
    local_workers.swap(workers_);
  }

  // Worker drain and shutdown.
  for (auto &worker : local_workers) {
    worker->Kill();
  }
  for (auto &worker : local_workers) {
    if (worker->options().owned_thread) {
      worker->WaitForShutdown();
    }
  }
  blocking_executor_.Kill();

  local_workers.clear();

  // Orderly destruction of heavy-weight objects.
  // Shutdown order is important so we don't leave it to field ordering.
  vm_instance_.reset();

  // Devices.
  devices_.clear();
  named_devices_.clear();
  retained_devices_.clear();

  // HAL drivers.
  hal_drivers_.clear();
}

std::shared_ptr<Scope> System::CreateScope(Worker &worker,
                                           std::span<Device *const> devices) {
  iree::slim_mutex_lock_guard guard(lock_);
  return std::make_shared<Scope>(shared_ptr(), worker, devices);
}

void System::InitializeNodes(int node_count) {
  AssertNotInitialized();
  if (!nodes_.empty()) {
    throw std::logic_error("System::InitializeNodes called more than once");
  }
  nodes_.reserve(node_count);
  for (int i = 0; i < node_count; ++i) {
    nodes_.emplace_back(i);
  }
}

Queue &System::CreateQueue(Queue::Options options) {
  iree::slim_mutex_lock_guard guard(lock_);
  if (queues_by_name_.count(options.name) != 0) {
    throw std::invalid_argument(fmt::format(
        "Cannot create queue with duplicate name '{}'", options.name));
  }
  queues_.push_back(std::make_unique<Queue>(std::move(options)));
  Queue *unowned_queue = queues_.back().get();
  queues_by_name_[unowned_queue->options().name] = unowned_queue;
  return *unowned_queue;
}

Queue &System::named_queue(std::string_view name) {
  iree::slim_mutex_lock_guard guard(lock_);
  auto it = queues_by_name_.find(name);
  if (it == queues_by_name_.end()) {
    throw std::invalid_argument(fmt::format("Queue '{}' not found", name));
  }
  return *it->second;
}

void System::AddWorkerInitializer(std::function<void(Worker &)> initializer) {
  iree::slim_mutex_lock_guard guard(lock_);
  if (!workers_.empty()) {
    throw std::logic_error(
        "AddWorkerInitializer can only be called before workers are created");
  }
  worker_initializers_.push_back(std::move(initializer));
}

void System::InitializeWorker(Worker &worker) {
  for (auto &f : worker_initializers_) {
    f(worker);
  }
}

Worker &System::CreateWorker(Worker::Options options) {
  Worker *unowned_worker;
  {
    iree::slim_mutex_lock_guard guard(lock_);
    if (options.name == std::string_view("__init__")) {
      throw std::invalid_argument(
          "Cannot create worker '__init__' (reserved name)");
    }
    if (workers_by_name_.count(options.name) != 0) {
      throw std::invalid_argument(fmt::format(
          "Cannot create worker with duplicate name '{}'", options.name));
    }
    workers_.push_back(std::make_unique<Worker>(std::move(options)));
    unowned_worker = workers_.back().get();
    workers_by_name_[unowned_worker->name()] = unowned_worker;
  }
  InitializeWorker(*unowned_worker);
  if (unowned_worker->options().owned_thread) {
    unowned_worker->Start();
  }
  return *unowned_worker;
}

Worker &System::init_worker() {
  iree::slim_mutex_lock_guard guard(lock_);
  auto found_it = workers_by_name_.find("__init__");
  if (found_it != workers_by_name_.end()) {
    return *found_it->second;
  }

  // Create.
  Worker::Options options(host_allocator(), "__init__");
  options.owned_thread = false;
  workers_.push_back(std::make_unique<Worker>(std::move(options)));
  Worker *unowned_worker = workers_.back().get();
  workers_by_name_[unowned_worker->name()] = unowned_worker;
  InitializeWorker(*unowned_worker);
  return *unowned_worker;
}

void System::InitializeHalDriver(std::string_view moniker,
                                 iree::hal_driver_ptr driver) {
  AssertNotInitialized();
  auto &slot = hal_drivers_[moniker];
  if (slot) {
    throw std::logic_error(fmt::format(
        "Cannot register multiple hal drivers with moniker '{}'", moniker));
  }
  slot.reset(driver.release());
}

void System::InitializeHalDevice(std::unique_ptr<Device> device) {
  AssertNotInitialized();
  auto device_name = device->name();
  auto [it, success] = named_devices_.try_emplace(device_name, device.get());
  if (!success) {
    throw std::logic_error(
        fmt::format("Cannot register Device '{}' multiple times", device_name));
  }
  devices_.push_back(device.get());
  retained_devices_.push_back(std::move(device));
}

void System::FinishInitialization() {
  iree::slim_mutex_lock_guard guard(lock_);
  AssertNotInitialized();
  initialized_ = true;
}

int64_t System::AllocateProcess(detail::BaseProcess *p) {
  iree::slim_mutex_lock_guard guard(lock_);
  int pid = next_pid_++;
  processes_by_pid_[pid] = p;
  return pid;
}

void System::DeallocateProcess(int64_t pid) {
  iree::slim_mutex_lock_guard guard(lock_);
  processes_by_pid_.erase(pid);
}

}  // namespace shortfin::local