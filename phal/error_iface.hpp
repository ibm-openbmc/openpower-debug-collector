#pragma once

#include "chipop_iface.hpp"
#include "targeting_iface.hpp"

#ifdef NEXT_PHAL
#include <errl_handle.H>
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>

namespace openpower::dump::phal::error
{

/**
 * @brief Log chip-op error and create PEL
 *
 * Legacy: Parses sbeError_t FFDC and creates detailed PEL
 * Next: Logs error with basic info (Phase 1), full PEL support in Phase 2
 *
 * @param err The chip-op error
 * @param chip Target where error occurred
 * @param cmdClass Command class (for PEL)
 * @param cmdType Command type (for PEL)
 * @param dumpPath Dump collection path
 * @return true if error is recoverable (continue dump collection)
 *         false if error is critical (exclude target from collection)
 */
bool logChipOpError(const chipop::ChipOpError& err,
                    targeting::TargetHandle chip, uint32_t cmdClass,
                    uint32_t cmdType, const std::filesystem::path& dumpPath);

/**
 * @brief Create a PEL for a chip-op error and return the PEL/log ID
 *
 * Legacy: Calls D-Bus CreatePELWithFFDCFiles via phosphor-logging
 * Next: Logs via lg2 only (Phase 1 stub), returns 0
 *
 * @param err       The chip-op error
 * @param chip      Target where error occurred
 * @param event     D-Bus event name (e.g. sbeTypeAttributes.chipOpFailure)
 * @param dumpPath  Dump collection path (for FFDC context)
 * @return uint32_t  D-Bus log entry ID (0 if not created)
 */
uint32_t createChipOpErrorPEL(
    const chipop::ChipOpError& err, targeting::TargetHandle chip,
    const std::string& event, const std::filesystem::path& dumpPath);

/**
 * @brief Get PEL information from log ID
 *
 * Legacy: Retrieves PEL ID and SRC from D-Bus logging
 * Next: Stub (returns {0, ""})
 *
 * @param logId The log ID returned from createChipOpErrorPEL
 * @return tuple of (PEL ID, SRC string)
 */
std::tuple<uint32_t, std::string> getPelInfo(uint32_t logId);

#ifdef NEXT_PHAL
/**
 * @brief Commit a hostfw error handle as a PEL
 *
 * Publishes every terminal and observation entry with its diagnostic data.
 * A publication failure does not prevent later entries from being attempted
 * or change the hardware operation's completion status. Observation and
 * terminal entries retain their supplied callout actions and native severity
 * under the existing severity mapping. This boundary invokes no local guard,
 * deconfigure or dump actions.
 *
 * @param[in] err Error handle from hostfw (moved/consumed)
 * @return First successfully published PEL ID, or 0 when none was published.
 *         Entries are ordered terminal-first, so a published terminal entry
 *         takes precedence over observations.
 */
uint32_t commitHostfwError(errl::ErrlHandleOpt&& err);

/**
 * @brief Publish all diagnostics and return the HostFW terminal failure state.
 *
 * Collection payload and previously written files remain valid when the
 * handle contains only observations. PEL publication does not change that
 * operation outcome. An optional ID receives the first successfully created
 * PEL.
 */
inline bool commitHostfwErrorAndCheckFailure(errl::ErrlHandleOpt&& err,
                                             uint32_t* pelId = nullptr)
{
    const bool terminal = errl::hasTerminalErrors(err);
    const auto id = commitHostfwError(std::move(err));
    if (pelId)
    {
        *pelId = id;
    }
    return terminal;
}
#endif

} // namespace openpower::dump::phal::error
