#include "../dump/dump_utils.hpp"
#include "error_iface.hpp"
#include "hostfw_error_publish.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <xyz/openbmc_project/Logging/Create/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <cerrno>
#include <map>
#include <optional>
#include <string>
#include <system_error>
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
static std::string getSeverityLevel(errl::Level severity)
{
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    Level level = Level::Error;
    switch (severity)
    {
        case errl::Level::Emergency:
        case errl::Level::Alert:
        case errl::Level::Critical:
            level = Level::Critical;
            break;
        case errl::Level::Error:
            level = Level::Error;
            break;
        case errl::Level::Warning:
            level = Level::Warning;
            break;
        case errl::Level::Notice:
        case errl::Level::Informational:
        case errl::Level::Debug:
            level = Level::Informational;
            break;
    }

    return sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
        level);
}

namespace
{
struct OpenedFfdcFiles
{
    OpenedFfdcFiles() = default;
    OpenedFfdcFiles(const OpenedFfdcFiles&) = delete;
    OpenedFfdcFiles& operator=(const OpenedFfdcFiles&) = delete;

    std::vector<int> descriptors;
    ~OpenedFfdcFiles() noexcept
    {
        for (const int fd : descriptors)
        {
            ::close(fd);
        }
    }
};

using FfdcFiles = std::vector<
    std::tuple<DBusFFDCFormat, uint8_t, uint8_t, sdbusplus::message::unix_fd>>;

void appendFfdcFiles(const errl::ErrlEntry& entry, FfdcFiles& files,
                     OpenedFfdcFiles& opened)
{
    if (const auto& attachments = entry.getFfdcFiles(); attachments)
    {
        // Allocate before opening files so retaining an owned descriptor cannot
        // throw and leak it. HostFW-owned descriptors remain borrowed.
        opened.descriptors.reserve(attachments->size());
        files.reserve(attachments->size());
        for (const auto& [attachment, path] : *attachments)
        {
            int fd = attachment.fd;
            if (fd < 0)
            {
                fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
                if (fd < 0)
                {
                    throw std::system_error(
                        errno, std::generic_category(),
                        "Cannot open HostFW FFDC: " + path.string());
                }
                opened.descriptors.push_back(fd);
            }
            DBusFFDCFormat format = DBusFFDCFormat::Custom;
            switch (attachment.format)
            {
                case errl::FFDCFormat::JSON:
                    format = DBusFFDCFormat::JSON;
                    break;
                case errl::FFDCFormat::CBOR:
                    format = DBusFFDCFormat::CBOR;
                    break;
                case errl::FFDCFormat::Text:
                    format = DBusFFDCFormat::Text;
                    break;
                case errl::FFDCFormat::Custom:
                    break;
            }
            files.emplace_back(format, attachment.subType, attachment.version,
                               sdbusplus::message::unix_fd{fd});
        }
    }
}
} // namespace

uint32_t commitHostfwError(errl::ErrlHandleOpt&& err)
{
    if (!err || !err->hasEntries())
    {
        return 0;
    }
    constexpr auto path = "/xyz/openbmc_project/logging";
    constexpr auto interface = "org.open_power.Logging.PEL";
    // Created lazily so a D-Bus failure is reported per entry, and reused so
    // the connection and service lookup happen once per handle.
    std::optional<sdbusplus::bus_t> bus;
    std::string service;
    return publishHostfwEntries(
        *err,
        [&](const errl::ErrlEntry& entry) {
            std::map<std::string, std::string> data(
                entry.getAdditionalData().begin(),
                entry.getAdditionalData().end());
            data["_PID"] = std::to_string(getpid());

            // Observation classification changes the operation outcome only;
            // retain the severity and requested callout actions for
            // publication.
            OpenedFfdcFiles opened;
            FfdcFiles files;
            appendFfdcFiles(entry, files, opened);
            if (!bus)
            {
                bus.emplace(sdbusplus::bus::new_default());
            }
            if (service.empty())
            {
                service =
                    openpower::dump::util::getService(*bus, interface, path);
            }
            auto method = bus->new_method_call(service.c_str(), path, interface,
                                               "CreatePELWithFFDCFiles");
            method.append(entry.getMessage(),
                          getSeverityLevel(entry.getSeverity()), data, files);
            auto reply = bus->call(method);
            std::tuple<uint32_t, uint32_t> ids{};
            reply.read(ids);
            const auto [logId, pelId] = ids;
            if (pelId != 0)
            {
                lg2::info("Published HostFW entry {ENTRY}: logId={LOG_ID}, "
                          "pelId={PEL_ID}",
                          "ENTRY", entry.getMessage(), "LOG_ID", logId,
                          "PEL_ID", pelId);
            }
            return pelId;
        },
        [](const errl::ErrlEntry& entry, const char* detail) {
            lg2::error("Failed to publish HostFW entry {ENTRY}: {ERROR}",
                       "ENTRY", entry.getMessage(), "ERROR", detail);
        });
}
#endif // NEXT_PHAL

} // namespace openpower::dump::phal::error
