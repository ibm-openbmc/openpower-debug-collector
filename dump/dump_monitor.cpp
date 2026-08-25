#include "dump_monitor.hpp"

#include "dump_utils.hpp"

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <regex>
#include <system_error>
#include <vector>

namespace openpower::dump
{

constexpr auto dumpOutPath = "/var/lib/phosphor-debug-collector/opdump";
constexpr auto dumpStatusFailed =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Failed";
// This path must match PLDM's system dump transfer destination.
constexpr auto TEMPORARY_SYSTEM_DUMP_FOLDER =
    "/var/lib/phosphor-debug-collector/tmp";
constexpr auto dumpManagerService = "xyz.openbmc_project.Dump.Manager";
constexpr auto dumpEntryRoot = "/xyz/openbmc_project/dump/system/entry";
constexpr auto objectMapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto objectMapperInterface = "xyz.openbmc_project.ObjectMapper";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";
// This event only means that the writer closed the file. It does not prove
// that the transfer completed successfully.
constexpr uint32_t newSystemDumpEvents = IN_CLOSE_WRITE;

DumpMonitor::DumpMonitor() :
    event(nullptr), bus(sdbusplus::bus::new_default()),
    match(bus,
          sdbusplus::bus::match::rules::interfacesAdded(
              "/xyz/openbmc_project/dump") +
              sdbusplus::bus::match::rules::sender(dumpManagerService),
          [this](sdbusplus::message_t& msg) { handleDBusSignal(msg); })
{
    sd_event* eventPtr = nullptr;
    auto rc = sd_event_default(&eventPtr);
    if (rc < 0)
    {
        lg2::error("Failed to create event loop, rc: {RC}", "RC", rc);
        throw std::system_error(-rc, std::generic_category(),
                                "Failed to create event loop");
    }
    event.reset(eventPtr);

    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);
    setupInotifyWatch();
}

DumpMonitor::~DumpMonitor()
{
    if (inotifyEventSource != nullptr)
    {
        sd_event_source_unref(inotifyEventSource);
    }

    if (inotifyWatchDescriptor >= 0)
    {
        inotify_rm_watch(inotifyFd, inotifyWatchDescriptor);
    }

    if (inotifyFd >= 0)
    {
        close(inotifyFd);
    }
}

int DumpMonitor::run()
{
    return sd_event_loop(event.get());
}

void DumpMonitor::setupInotifyWatch()
{
    std::error_code ec;
    std::filesystem::create_directories(TEMPORARY_SYSTEM_DUMP_FOLDER, ec);
    if (ec)
    {
        lg2::error("Failed to create system dump watch directory {PATH}: "
                   "{ERROR}",
                   "PATH", TEMPORARY_SYSTEM_DUMP_FOLDER, "ERROR", ec.message());
        throw std::system_error(ec, "Failed to create system dump directory");
    }

    inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd < 0)
    {
        auto error = errno;
        lg2::error("Failed to initialize inotify, errno: {ERRNO}", "ERRNO",
                   error);
        throw std::system_error(error, std::generic_category(),
                                "Failed to initialize inotify");
    }

    inotifyWatchDescriptor = inotify_add_watch(
        inotifyFd, TEMPORARY_SYSTEM_DUMP_FOLDER, newSystemDumpEvents);
    if (inotifyWatchDescriptor < 0)
    {
        auto error = errno;
        close(inotifyFd);
        inotifyFd = -1;
        lg2::error("Failed to watch system dump directory {PATH}, errno: "
                   "{ERRNO}",
                   "PATH", TEMPORARY_SYSTEM_DUMP_FOLDER, "ERRNO", error);
        throw std::system_error(error, std::generic_category(),
                                "Failed to add inotify watch");
    }

    auto rc = sd_event_add_io(event.get(), &inotifyEventSource, inotifyFd,
                              EPOLLIN, inotifyCallback, this);
    if (rc < 0)
    {
        auto error = -rc;
        inotify_rm_watch(inotifyFd, inotifyWatchDescriptor);
        inotifyWatchDescriptor = -1;
        close(inotifyFd);
        inotifyFd = -1;
        lg2::error("Failed to add inotify descriptor to event loop, rc: {RC}",
                   "RC", rc);
        throw std::system_error(error, std::generic_category(),
                                "Failed to add inotify event source");
    }
}

int DumpMonitor::inotifyCallback(sd_event_source*, int fd, uint32_t revents,
                                 void* userdata)
{
    if ((revents & EPOLLIN) == 0U)
    {
        lg2::error("Unexpected inotify event flags {EVENTS}", "EVENTS",
                   revents);
        return 0;
    }

    auto* monitor = static_cast<DumpMonitor*>(userdata);
    alignas(inotify_event) std::array<char, 4096> buffer{};

    while (true)
    {
        auto bytesRead = read(fd, buffer.data(), buffer.size());
        if (bytesRead < 0)
        {
            auto error = errno;
            if (error == EINTR)
            {
                continue;
            }
            if (error != EAGAIN && error != EWOULDBLOCK)
            {
                lg2::error("Failed to read inotify events, errno: {ERRNO}",
                           "ERRNO", error);
            }
            break;
        }
        if (bytesRead == 0)
        {
            break;
        }

        size_t offset = 0;
        const auto bytesAvailable = static_cast<size_t>(bytesRead);
        while (offset + sizeof(inotify_event) <= bytesAvailable)
        {
            auto* inotifyEvent = reinterpret_cast<const struct inotify_event*>(
                buffer.data() + offset);
            const auto eventSize = sizeof(inotify_event) + inotifyEvent->len;
            if (offset + eventSize > bytesAvailable)
            {
                lg2::error("Received a truncated inotify event");
                break;
            }

            if ((inotifyEvent->mask & IN_Q_OVERFLOW) != 0U)
            {
                lg2::error("System dump inotify queue overflowed");
            }
            else if (inotifyEvent->len > 0 &&
                     (inotifyEvent->mask & IN_ISDIR) == 0U &&
                     (inotifyEvent->mask & newSystemDumpEvents) != 0U)
            {
                // Skip files that are already packaged SYSDUMP files —
                // these are the finished dumps, not the raw Hostboot file.
                std::string fileName(inotifyEvent->name);
                if (fileName.rfind("SYSDUMP.", 0) == 0)
                {
                    lg2::info("inotifyCallback: ignoring SYSDUMP file {NAME}",
                              "NAME", fileName);
                }
                else
                {
                    try
                    {
                        monitor->handleSystemDumpFile(
                            std::filesystem::path(
                                TEMPORARY_SYSTEM_DUMP_FOLDER) /
                            fileName);
                    }
                    catch (const std::exception& e)
                    {
                        // Never allow an exception to cross the C callback.
                        lg2::error("Failed to handle inotify event: {ERROR}",
                                   "ERROR", e);
                    }
                }
            }

            offset += eventSize;
        }
    }

    return 0;
}

void DumpMonitor::handleSystemDumpFile(const std::filesystem::path& filePath)
{
    auto dumpEntry = findInProgressSystemDump();
    if (!dumpEntry)
    {
        lg2::error("No in-progress system dump entry found for new file {FILE}",
                   "FILE", filePath);
        return;
    }

    // Extract the 8-char hex dump id from the last component of the entry path
    // e.g. /xyz/openbmc_project/dump/system/entry/A0000001 → "A0000001"
    std::filesystem::path entryPath(*dumpEntry);
    std::string dumpIdStr = entryPath.filename().string();

    lg2::info(
        "handleSystemDumpFile: raw dump file {FILE}; packaging as entry {ID}",
        "FILE", filePath, "ID", dumpIdStr);

    // Fork opdreport --mpipl-src to package the raw dump.
    // The event loop remains running in the parent — D-Bus signals and
    // future inotify events continue to be dispatched normally.
    // phosphor-dump-manager's inotify watch on opdump/ fires when
    // opdreport writes the final file, calling updateEntry() to complete
    // the dump entry.
    std::vector<std::string> args = {
        "opdreport",         "-i",          dumpIdStr,         "-d",
        MPIPL_DUMP_OUT_PATH, "--mpipl-src", filePath.string(),
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args)
    {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0)
    {
        lg2::error("handleSystemDumpFile: fork failed, errno={ERR}", "ERR",
                   errno);
        return;
    }
    if (pid == 0)
    {
        execvp("opdreport", argv.data());
        _exit(EXIT_FAILURE);
    }

    // Parent: reap child.  This is inside openpower-dump-monitor, a
    // separate process from phosphor-dump-manager, so waitpid() here
    // never blocks the dump manager's sd-event loop.
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0)
    {
        lg2::error("handleSystemDumpFile: opdreport exited {STATUS} for {FILE}",
                   "STATUS", WEXITSTATUS(wstatus), "FILE", filePath);
    }
    else
    {
        lg2::info("handleSystemDumpFile: opdreport completed successfully");
    }
}

std::optional<std::string> DumpMonitor::findInProgressSystemDump()
{
    using namespace sdbusplus::common::xyz::openbmc_project::common;
    using namespace sdbusplus::common::xyz::openbmc_project::dump::entry;

    auto mapperCall =
        bus.new_method_call(objectMapperService, objectMapperPath,
                            objectMapperInterface, "GetSubTreePaths");
    mapperCall.append(dumpEntryRoot, 0,
                      std::vector<std::string>{System::interface});

    std::vector<std::string> systemDumpEntries;
    auto mapperReply = bus.call(mapperCall);
    mapperReply.read(systemDumpEntries);
    std::sort(systemDumpEntries.begin(), systemDumpEntries.end());

    const auto inProgressStatus = Progress::convertOperationStatusToString(
        Progress::OperationStatus::InProgress);
    std::vector<std::string> inProgressEntries;

    for (const auto& entry : systemDumpEntries)
    {
        try
        {
            auto propertyCall = bus.new_method_call(
                dumpManagerService, entry.c_str(), propertiesInterface, "Get");
            propertyCall.append(Progress::interface, "Status");

            std::variant<std::string> status;
            auto propertyReply = bus.call(propertyCall);
            propertyReply.read(status);
            if (std::get<std::string>(status) == inProgressStatus)
            {
                inProgressEntries.push_back(entry);
            }
        }
        catch (const std::exception& e)
        {
            // An entry can disappear between the mapper and property calls.
            // Keep looking so another valid in-progress dump is not lost.
            lg2::error("Failed to read dump progress for {ENTRY}: {ERROR}",
                       "ENTRY", entry, "ERROR", e);
        }
    }

    if (inProgressEntries.empty())
    {
        return std::nullopt;
    }

    if (inProgressEntries.size() > 1)
    {
        lg2::error("Found {COUNT} in-progress system dump entries; using "
                   "{ENTRY}",
                   "COUNT", inProgressEntries.size(), "ENTRY",
                   inProgressEntries.front());
    }

    return inProgressEntries.front();
}

void DumpMonitor::executeCollectionScript(
    const sdbusplus::message::object_path& path, const PropertyMap& properties)
{
    std::vector<std::string> args = {"opdreport"};
    std::regex idFormat("^[a-fA-F0-9]{8}$");
    auto dumpIdStr = path.filename();
    if (!std::regex_match(dumpIdStr, idFormat))
    {
        lg2::error("Failed to extract dump id from path {PATH}", "PATH", path);
        updateProgressStatus(path, dumpStatusFailed);
        return;
    }

    uint32_t dumpId = std::strtoul(dumpIdStr.c_str(), nullptr, 16);
    int dumpType = getDumpTypeFromId(dumpId);
    std::filesystem::path dumpPath =
        std::filesystem::path(dumpOutPath) / dumpIdStr;

    // Add type, ID, and dump path to args
    args.push_back("-t");
    args.push_back(std::to_string(dumpType));
    args.push_back("-i");
    args.push_back(dumpIdStr);
    args.push_back("-d");
    args.push_back(dumpPath.string());

    // Optionally add ErrorLogId and FailingUnitId
    auto errorLogIdIt = properties.find("ErrorLogId");
    if (errorLogIdIt != properties.end())
    {
        uint32_t errorLogId = std::get<uint32_t>(errorLogIdIt->second);
        args.push_back("-e");
        args.push_back(std::to_string(errorLogId));
    }

    auto failingUnitIdIt = properties.find("FailingUnitId");
    if (failingUnitIdIt != properties.end())
    {
        uint32_t failingUnitId = std::get<uint32_t>(failingUnitIdIt->second);
        args.push_back("-f");
        args.push_back(std::to_string(failingUnitId));
    }

    // For SBE BootFailure dumps
    auto sbeTriggerTypeIt = properties.find("SBEDumpTriggerType");
    if (sbeTriggerTypeIt != properties.end())
    {
        std::string triggerType =
            std::get<std::string>(sbeTriggerTypeIt->second);
        args.push_back("-b");
        args.push_back(triggerType);
    }

    auto dumpFilesPathIt = properties.find("DumpFilesPath");
    if (dumpFilesPathIt != properties.end())
    {
        std::string filesPath = std::get<std::string>(dumpFilesPathIt->second);
        args.push_back("-p");
        args.push_back(filesPath);
    }

    std::vector<char*> argv;
    for (auto& arg : args)
    {
        argv.push_back(arg.data());
    }

    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == -1)
    {
        lg2::error("Failed to fork, cannot collect dump, {ERRRNO}", "ERRNO",
                   errno);
        updateProgressStatus(path, dumpStatusFailed);
        exit(EXIT_FAILURE); // Exit explicitly with failure status
    }
    else if (pid == 0)
    {
        // Child process
        execvp("opdreport", argv.data());
        perror("execvp");   // execvp only returns on error
        updateProgressStatus(path, dumpStatusFailed);
        exit(EXIT_FAILURE); // Exit explicitly with failure status
    }
    else
    {
        // Parent process: wait for the child to terminate
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            int exit_status = WEXITSTATUS(status);

            if (exit_status != 0)
            {
                // Handle failure
                lg2::error("Dump failed updating status {PATH}", "PATH", path);
                updateProgressStatus(path, dumpStatusFailed);
            }
        }
    }
}

void DumpMonitor::handleDBusSignal(sdbusplus::message_t& msg)
{
    sdbusplus::message::object_path objectPath;
    InterfaceMap interfaces;

    msg.read(objectPath, interfaces);

    lg2::info("Signal received at {PATH}: ", "PATH", objectPath);

    // There can be new a entry created after the completion of the collection
    // with completed status, so pick only entries with InProgress status.
    if (isInProgress(interfaces))
    {
        for (const auto& interfaceName : monitoredInterfaces)
        {
            auto it = interfaces.find(interfaceName);
            if (it != interfaces.end())
            {
                lg2::info("An entry created, collecting a new dump {DUMP}",
                          "DUMP", interfaceName);
                initiateDumpCollection(objectPath, interfaceName, it->second);
            }
        }
    }
}

void DumpMonitor::updateProgressStatus(const std::string& path,
                                       const std::string& status)
{
    auto b = sdbusplus::bus::new_default();
    const std::string interfaceName = "xyz.openbmc_project.Common.Progress";
    const std::string propertyName = "Status";
    const std::string serviceName = "xyz.openbmc_project.Dump.Manager";

    try
    {
        auto statusVariant = std::variant<std::string>(status);
        util::setProperty<std::variant<std::string>>(
            interfaceName, propertyName, path, b, statusVariant);
        lg2::info("Status updated successfully to {STATUS} {PATH}", "STATUS",
                  status, "PATH", path);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to update status {STATUS} {PATH} {ERROR}", "STATUS",
                   status, "PATH", path, "ERROR", e);
    }
}

void DumpMonitor::startMpReboot(
    const sdbusplus::message::object_path& objectPath [[maybe_unused]])
{
    constexpr auto systemdService = "org.freedesktop.systemd1";
    constexpr auto systemdObjPath = "/org/freedesktop/systemd1";
    constexpr auto systemdInterface = "org.freedesktop.systemd1.Manager";
    constexpr auto diagModeTarget = "obmc-host-crash@0.target";

    try
    {
        auto b = sdbusplus::bus::new_default();
        auto method = b.new_method_call(systemdService, systemdObjPath,
                                        systemdInterface, "StartUnit");
        method.append(diagModeTarget); // unit to activate
        method.append("replace");
        b.call_noreply(method);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to start memory preserving reboot");
#if 0
        // Disabled during testing: do not mark the dump entry as Failed
        // when the MPIPL reboot cannot be triggered.  The entry stays
        // InProgress so the inotify watch on tmp/ can still pick up the
        // raw dump file if it arrives via another path.
        updateProgressStatus(objectPath, dumpStatusFailed);
#endif
    }
}

void DumpMonitor::initiateDumpCollection(const std::string& path,
                                         const std::string& intf,
                                         const PropertyMap& properties)
{
    using namespace sdbusplus::common::xyz::openbmc_project::dump::entry;
    if (intf == System::interface)
    {
        // Only disruptive system dumps trigger an MPIPL reboot.
        // Non-disruptive (resource-style) system dumps are handled entirely
        // by the host and require no action here.
        // If the property is absent, assume disruptive (safe default).
        auto systemImpactIt = properties.find("SystemImpact");
        if (systemImpactIt != properties.end() &&
            std::get<std::string>(systemImpactIt->second) !=
                System::convertSystemImpactToString(
                    System::SystemImpact::Disruptive))
        {
            lg2::info("Ignoring non-disruptive system dump {PATH}", "PATH",
                      path);
            return;
        }

        // Disruptive system dump: trigger MPIPL reboot so the host writes
        // the raw dump to MPIPL_STAGING_PATH.  The always-on inotify watch
        // (setupInotifyWatch / inotifyCallback) will detect the file and
        // call handleSystemDumpFile() without blocking the event loop.
        startMpReboot(path);
    }
    else
    {
        executeCollectionScript(path, properties);
    }
}

} // namespace openpower::dump
