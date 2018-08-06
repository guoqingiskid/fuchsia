// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_APPMGR_INTEGRATION_TESTS_SANDBOX_NAMESPACE_TEST_H_
#define GARNET_BIN_APPMGR_INTEGRATION_TESTS_SANDBOX_NAMESPACE_TEST_H_

#include <memory>
#include <string>

#include <fuchsia/sys/cpp/fidl.h>
#include "gtest/gtest.h"
#include "lib/component/cpp/startup_context.h"
#include "lib/component/cpp/testing/test_with_environment.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/gtest/real_loop_fixture.h"

class NamespaceTest : public component::testing::TestWithEnvironment {
 protected:
  NamespaceTest()
      : startup_context_(component::StartupContext::CreateFromStartupInfo()) {}

  // Connects to a service provided by the environment.
  template <typename Interface>
  void ConnectToService(fidl::InterfaceRequest<Interface> request,
                        const std::string& service_name = Interface::Name_) {
    if (!has_service_provider_) {
      startup_context_->ConnectToEnvironmentService(env_.NewRequest());
      env_->GetServices(service_provider_.NewRequest());
      has_service_provider_ = true;
    }
    service_provider_->ConnectToService(service_name, request.TakeChannel());
  }

  // Returns whether path exists.
  bool Exists(const char* path);

  // Expect that a path exists, and fail with a descriptive message
  void ExpectExists(const char* path);

  // Expect that a path does not exist, and fail with a descriptive message
  void ExpectDoesNotExist(const char* path);

 private:
  bool has_service_provider_ = false;
  fuchsia::sys::EnvironmentPtr env_;
  fuchsia::sys::ServiceProviderPtr service_provider_;
  std::unique_ptr<component::StartupContext> startup_context_;
};

#endif  // GARNET_BIN_APPMGR_INTEGRATION_TESTS_SANDBOX_NAMESPACE_TEST_H_
