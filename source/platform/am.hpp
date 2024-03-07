#pragma once

#include "ipc.hpp"

namespace Platform {

/**
 * Application Manager: Interface used most prominently by the Home Menu to
 * get information about installed titles
 */
namespace AM {

// All IPC commands in this module have unique command IDs and hence
// are not put in per-service namespaces

namespace IPC = Platform::IPC;

/**
 * Inputs:
 * - Media Type
 *
 * Outputs:
 * - Number of title IDs for this media type
 */
struct GetNumPrograms : IPC::IPCCommand<0x1>::add_uint32
                           ::response::add_uint32 {};

/**
 * Inputs:
 * - Maximum title count
 * - Media Type
 *
 * Outputs:
 * - Number of title IDs actually returned
 * - Title IDs
 */
struct GetProgramList : IPC::IPCCommand<0x2>::add_uint32::add_uint32::add_buffer_mapping_write
                           ::response::add_uint32::add_buffer_mapping_write {};

/**
 * Inputs:
 * - Media Type
 * - Number of titles to query ProgramInfos for
 * - List of title IDs to query ProgramInfos for
 *
 * Outputs:
 * - ProgramInfos for each input title id
 */
struct GetProgramInfos : IPC::IPCCommand<0x3>::add_uint32::add_uint32::add_buffer_mapping_read::add_buffer_mapping_write
                            ::response::add_uint32::add_buffer_mapping_read::add_buffer_mapping_write {};

/**
 * Inputs:
 * - Media Type
 * - Title ID
 *
 * Outputs:
 * - Number of content infos for this title
 */
struct GetDLCContentInfoCount : IPC::IPCCommand<0x1001>::add_uint32::add_uint64
                                   ::response::add_uint32 {};

/**
 * Inputs:
 * - Number of content infos to read
 * - Media Type
 * - Title ID
 * - Offset (index of first content info to read?)
 * - Output buffer
 *
 * Outputs:
 * - Number of content infos read
 */
struct ListDLCContentInfos : IPC::IPCCommand<0x1003>::add_uint32::add_uint32::add_uint64::add_uint32::add_buffer_mapping_write
                                ::response::add_uint32::add_buffer_mapping_write {};

/**
 * Inputs:
 * - Media Type
 * - Number of titles in the input list
 * - Title IDs to get title infos for
 * - Output buffer for title infos
 */
struct GetDLCTitleInfos : IPC::IPCCommand<0x1005>::add_uint32::add_uint32::add_buffer_mapping_read::add_buffer_mapping_write
                             ::response::add_buffer_mapping_read::add_buffer_mapping_write {};

/**
 * Inputs:
 * - Number of ticket infos to get
 * - Title ID
 * - Offset (index of first ticket to get?)
 * - Output buffer for ticket infos
 *
 * Outputs:
 * - Number of ticket infos read for this title
 */
struct ListDataTitleTicketInfos : IPC::IPCCommand<0x1007>::add_uint32::add_uint64::add_uint32::add_buffer_mapping_write
                                     ::response::add_uint32::add_buffer_mapping_write {};

} // namespace AM

} // namespace Platform
