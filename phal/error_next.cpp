#include "../dump/dump_utils.hpp"
#include "error_iface.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <xyz/openbmc_project/Logging/Create/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace openpower::dump::phal::error
{

using DBusFFDCFormat =
    sdbusplus::xyz::openbmc_project::Logging::server::Create::FFDCFormat;

bool logChipOpError([[maybe_unused]] const chipop::ChipOpError& err,
                    [[maybe_unused]] targeting::TargetHandle chip,
                    [[maybe_unused]] uint32_t cmdClass,
                    [[maybe_unused]] uint32_t cmdType,
                    [[maybe_unused]] const std::filesystem::path& dumpPath)
{
    // Stub: Not implemented for next backend
    return false;
}

uint32_t createChipOpErrorPEL(
    [[maybe_unused]] const chipop::ChipOpError& err,
    [[maybe_unused]] targeting::TargetHandle chip,
    [[maybe_unused]] const std::string& event,
    [[maybe_unused]] const std::filesystem::path& dumpPath)
{
    // Stub: Not implemented for next backend
    return 0;
}

std::tuple<uint32_t, std::string> getPelInfo([[maybe_unused]] uint32_t logId)
{
    // Stub: Not implemented for next backend
    return {0, ""};
}

#ifdef NEXT_PHAL
// Helper to convert errl severity to D-Bus level string
static std::string getSeverityLevel(int severity)
{
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    // Map severity to Level enum
    // Severity levels from errl: 0=Emergency, 1=Alert, 2=Critical, 3=Error,
    // etc. Map to Logging.Entry.Level
    Level level = Level::Error; // Default
    if (severity <= 2)
    {
        level = Level::Critical;
    }
    else if (severity == 3)
    {
        level = Level::Error;
    }
    else if (severity == 4)
    {
        level = Level::Warning;
    }
    else
    {
        level = Level::Informational;
    }

    return sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
        level);
}

uint32_t commitHostfwError(errl::ErrlHandleOpt&& err)
{
    if (!err)
    {
        return 0;
    }

    try
    {
        auto bus = sdbusplus::bus::new_default();
        constexpr auto loggingObjectPath = "/xyz/openbmc_project/logging";
        constexpr auto opLoggingInterface = "org.open_power.Logging.PEL";

        // Get entries from error handle
        const auto& entries = err->getEntries();
        if (entries.empty())
        {
            lg2::warning("commitHostfwError: Error handle has no entries");
            return 0;
        }

        uint32_t pelId = 0;

        // Process each entry in the error handle (typically just 1)
        for (const auto& entry : entries)
        {
            // Build additional data map
            std::map<std::string, std::string> additionalData;
            additionalData["_PID"] = std::to_string(getpid());
            for (const auto& [key, value] : entry->getAdditionalData())
            {
                additionalData[key] = value;
            }

            // Get severity level (cast errl::Level enum to int)
            auto level =
                getSeverityLevel(static_cast<int>(entry->getSeverity()));

            // Get service via D-Bus introspection
            auto service = openpower::dump::util::getService(
                bus, opLoggingInterface, loggingObjectPath);

            // Prepare D-Bus call to CreatePELWithFFDCFiles (no files for hostfw
            // errors)
            auto method = bus.new_method_call(
                service.c_str(), loggingObjectPath, opLoggingInterface,
                "CreatePELWithFFDCFiles");

            // Empty FFDC file list (tuple format: FFDCFormat enum, subtype,
            // version, fd)
            std::vector<std::tuple<DBusFFDCFormat, uint8_t, uint8_t,
                                   sdbusplus::message::unix_fd>>
                files;

            // Append parameters: message, level, additionalData, files
            method.append(entry->getMessage(), level, additionalData, files);

            // Call D-Bus method and get response
            auto reply = bus.call(method);
            // Reply is a struct[uint32, uint32] containing (logId, pelId)
            std::tuple<uint32_t, uint32_t> ids{0, 0};
            reply.read(ids);
            uint32_t logId = std::get<0>(ids);
            pelId = std::get<1>(ids);

            lg2::info("commitHostfwError: Created PEL from hostfw error, "
                      "logId={LOG_ID}, pelId={PEL_ID}",
                      "LOG_ID", logId, "PEL_ID", pelId);
        }

        return pelId;
    }
    catch (const std::exception& e)
    {
        lg2::error("commitHostfwError: Failed to create PEL: {EXCEPTION}",
                   "EXCEPTION", e.what());
        return 0;
    }
}
#endif // NEXT_PHAL

} // namespace openpower::dump::phal::error
