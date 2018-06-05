// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/appmgr/appmgr.h"

namespace fuchsia {
namespace sys {
namespace {
constexpr char kRootLabel[] = "app";
}  // namespace

Appmgr::Appmgr(async_t* async, zx_handle_t pa_directory_request)
    : loader_vfs_(async),
      loader_dir_(fbl::AdoptRef(new fs::PseudoDir())),
      publish_vfs_(async),
      publish_dir_(fbl::AdoptRef(new fs::PseudoDir())) {
  // 1. Serve loader.
  loader_dir_->AddEntry(
      Loader::Name_, fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
        root_loader_.AddBinding(
            fidl::InterfaceRequest<Loader>(std::move(channel)));
        return ZX_OK;
      })));

  zx::channel h1, h2;
  if (zx::channel::create(0, &h1, &h2) < 0) {
    FXL_LOG(FATAL) << "Appmgr unable to create channel.";
    return;
  }

  if (loader_vfs_.ServeDirectory(loader_dir_, std::move(h2)) != ZX_OK) {
    FXL_LOG(FATAL) << "Appmgr unable to serve directory.";
    return;
  }

  root_realm_ = std::make_unique<Realm>(nullptr, std::move(h1), kRootLabel);

  // 2. Publish outgoing directories.
  if (pa_directory_request != ZX_HANDLE_INVALID) {
    auto svc = fbl::AdoptRef(new fs::Service([this](zx::channel channel) {
      return root_realm_->BindSvc(std::move(channel));
    }));
    publish_dir_->AddEntry("hub", root_realm_->hub_dir());
    publish_dir_->AddEntry("svc", svc);
    publish_vfs_.ServeDirectory(publish_dir_,
                                zx::channel(pa_directory_request));
  }

  // 3. Run sysmgr
  auto run_sysmgr = [this] {
    LaunchInfo launch_info;
    launch_info.url = "sysmgr";
    root_realm_->CreateComponent(std::move(launch_info), sysmgr_.NewRequest());
  };
  async::PostTask(async, [this, &run_sysmgr] {
    run_sysmgr();
    sysmgr_.set_error_handler(run_sysmgr);
  });
}

Appmgr::~Appmgr() = default;

}  // namespace sys
}  // namespace fuchsia
