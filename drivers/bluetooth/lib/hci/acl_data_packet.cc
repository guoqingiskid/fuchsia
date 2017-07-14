// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acl_data_packet.h"

#include <endian.h>

#include "lib/ftl/logging.h"

#include "slab_allocators.h"

namespace bluetooth {
namespace hci {
namespace slab_allocators {

// Slab-allocator traits for ACL data packets.
using LargeACLTraits =
    PacketTraits<ACLDataHeader, kLargeACLDataPacketSize, kNumLargeACLDataPackets>;
using MediumACLTraits =
    PacketTraits<ACLDataHeader, kMediumACLDataPacketSize, kNumMediumACLDataPackets>;
using SmallACLTraits =
    PacketTraits<ACLDataHeader, kSmallACLDataPacketSize, kNumSmallACLDataPackets>;

using LargeACLAllocator = mxtl::SlabAllocator<LargeACLTraits>;
using MediumACLAllocator = mxtl::SlabAllocator<MediumACLTraits>;
using SmallACLAllocator = mxtl::SlabAllocator<SmallACLTraits>;

}  // namespace slab_allocators

namespace {

std::unique_ptr<ACLDataPacket> NewACLDataPacket(size_t payload_size) {
  FTL_DCHECK(payload_size <= slab_allocators::kLargeACLDataPayloadSize);

  if (payload_size <= slab_allocators::kSmallACLDataPayloadSize) {
    auto buffer = slab_allocators::SmallACLAllocator::New(payload_size);
    if (buffer) return buffer;

    // Fall back to the next allocator.
  }

  if (payload_size <= slab_allocators::kMediumACLDataPayloadSize) {
    auto buffer = slab_allocators::MediumACLAllocator::New(payload_size);
    if (buffer) return buffer;

    // Fall back to the next allocator.
  }

  return slab_allocators::LargeACLAllocator::New(payload_size);
}

}  // namespace

// static
std::unique_ptr<ACLDataPacket> ACLDataPacket::New(uint16_t payload_size) {
  return NewACLDataPacket(payload_size);
}

// static
std::unique_ptr<ACLDataPacket> ACLDataPacket::New(ConnectionHandle connection_handle,
                                                  ACLPacketBoundaryFlag packet_boundary_flag,
                                                  ACLBroadcastFlag broadcast_flag,
                                                  uint16_t payload_size) {
  auto packet = NewACLDataPacket(payload_size);
  if (!packet) return nullptr;

  packet->WriteHeader(connection_handle, packet_boundary_flag, broadcast_flag);
  return packet;
}

ConnectionHandle ACLDataPacket::connection_handle() const {
  // Return the lower 12-bits of the first two octets.
  return le16toh(ACLDataPacket::view().header().handle_and_flags) & 0x0FFF;
}

ACLPacketBoundaryFlag ACLDataPacket::packet_boundary_flag() const {
  // Return bits 4-5 in the higher octet of |handle_and_flags| or "0b00xx000000000000".
  return static_cast<ACLPacketBoundaryFlag>(
      (le16toh(ACLDataPacket::view().header().handle_and_flags) >> 12) & 0x0003);
}

ACLBroadcastFlag ACLDataPacket::broadcast_flag() const {
  // Return bits 6-7 in the higher octet of |handle_and_flags| or "0bxx00000000000000".
  return static_cast<ACLBroadcastFlag>(le16toh(view().header().handle_and_flags) >> 14);
}

void ACLDataPacket::InitializeFromBuffer() {
  mutable_view()->Resize(le16toh(view().header().data_total_length));
}

void ACLDataPacket::WriteHeader(ConnectionHandle connection_handle,
                                ACLPacketBoundaryFlag packet_boundary_flag,
                                ACLBroadcastFlag broadcast_flag) {
  // Must fit inside 12-bits.
  FTL_DCHECK(connection_handle <= 0x0FFF);

  // Must fit inside 2-bits.
  FTL_DCHECK(static_cast<uint8_t>(packet_boundary_flag) <= 0x03);
  FTL_DCHECK(static_cast<uint8_t>(broadcast_flag) <= 0x03);

  uint16_t handle_and_flags = connection_handle |
                              (static_cast<uint16_t>(packet_boundary_flag) << 12) |
                              (static_cast<uint16_t>(broadcast_flag) << 14);
  mutable_view()->mutable_header()->handle_and_flags = htole16(handle_and_flags);
  mutable_view()->mutable_header()->data_total_length = htole16(view().payload_size());
}

}  // namespace hci
}  // namespace bluetooth

DECLARE_STATIC_SLAB_ALLOCATOR_STORAGE(::bluetooth::hci::slab_allocators::LargeACLTraits,
                                      ::bluetooth::hci::slab_allocators::kMaxNumSlabs, true);
DECLARE_STATIC_SLAB_ALLOCATOR_STORAGE(::bluetooth::hci::slab_allocators::MediumACLTraits,
                                      ::bluetooth::hci::slab_allocators::kMaxNumSlabs, true);
DECLARE_STATIC_SLAB_ALLOCATOR_STORAGE(::bluetooth::hci::slab_allocators::SmallACLTraits,
                                      ::bluetooth::hci::slab_allocators::kMaxNumSlabs, true);
