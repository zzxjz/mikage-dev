#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * Config: Exposes access to various global configuration parameters, most
 * of which are stored in this title's NAND savegame.
 */
namespace Config {

// Lots of these commands are shared between cfg:u, cfg:i, and cfg:s, hence we
// make no effort to separating them into different namespaces

using GetConfigInfoBlk2 = Platform::IPC::IPCCommand<0x1>::add_uint32::add_uint32::add_buffer_mapping_write
                                       ::response;

using SecureInfoGetRegion  = Platform::IPC::IPCCommand<0x2>
                                         ::response::add_uint32;

/**
 * Checks whether the system is running on a Canada or USA region.
 *
 * This information is queried from nand:/rw/sys/SecureInfo_A and from the
 * CountryInfo savedata block. 1 is returned if both indicate USA/Canada.
 */
using RegionIsCanadaOrUSA  = Platform::IPC::IPCCommand<0x4>
                                          ::response::add_uint32;

using SecureInfoGetRegion2 = Platform::IPC::IPCCommand<0x406>
                                          ::response::add_uint32;
using SecureInfoGetRegion3 = Platform::IPC::IPCCommand<0x816>
                                          ::response::add_uint32;

using GetSystemModel = Platform::IPC::IPCCommand<0x5>
                              ::response::add_uint32;
using GetConfigInfoBlk8 = Platform::IPC::IPCCommand<0x401>::add_uint32::add_uint32::add_buffer_mapping_write
                              ::response::add_buffer_mapping_write;
/**
 * Inputs:
 * - Block id
 * - Size of data to be written
 * - Input data
 */
using SetConfigInfoBlk4 = Platform::IPC::IPCCommand<0x402>::add_uint32::add_uint32::add_buffer_mapping_read
                                       ::response::add_buffer_mapping_read;

} // namespace Config

} // namespace Platform
