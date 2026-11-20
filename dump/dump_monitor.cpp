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
#include <fstream>
#include <regex>
#include <system_error>
#include <vector>

namespace openpower::dump
{

// D-Bus enum values for SBE dump trigger types
constexpr std::string_view SBE_DUMP_TRIGGER_BOOTFAILURE =
    "com.ibm.Dump.Create.SBEDumpTriggerType.BootFailure";
constexpr std::string_view SBE_DUMP_TRIGGER_TIMEOUT =
    "com.ibm.Dump.Create.SBEDumpTriggerType.Timeout";
constexpr std::string_view SBE_DUMP_TRIGGER_DOWNSTREAM =
    "com.ibm.Dump.Create.SBEDumpTriggerType.Downstream";

// Bare trigger type values (after stripping enum prefix)
constexpr std::string_view TRIGGER_BOOTFAILURE = "BootFailure";
constexpr std::string_view TRIGGER_TIMEOUT = "Timeout";

constexpr auto dumpOutPath = "/var/lib/phosphor-debug-collector/opdump";
constexpr auto dumpStatusFailed =
    "xyz.openbmc_project.Common.Progress.OperationStatus.Failed";
constexpr auto dumpManagerService = "xyz.openbmc_project.Dump.Manager";
// Offset within the raw staging file at which the 7-byte dump type tag
// begins (PAPR FILE Directory "Dump File Name" field, layout per
// gendumpheader):
//   0x00  "FILE    "  (8 B)
//   0x08  length      (2 B)
//   0x0A  reserved    (6 B)
//   0x10  flags       (4 B)
//   0x14  entry type  (2 B)
//   0x16  name prefix (2 B)
//   0x18  dump name → first 7 bytes are the type tag ("SYSDUMP" or "RSCDUMP")
constexpr std::size_t DUMP_TYPE_TAG_OFFSET = 0x18;
constexpr std::size_t DUMP_TYPE_TAG_LEN = 7;
constexpr auto objectMapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto objectMapperInterface = "xyz.openbmc_project.ObjectMapper";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto createDumpInterface = "xyz.openbmc_project.Dump.Create";
constexpr auto deleteInterface = "xyz.openbmc_project.Object.Delete";
// Timeout for calls to the dump manager (30 seconds)
constexpr uint64_t dbusCallTimeoutUs = 30ULL * 1000 * 1000;
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
    std::filesystem::create_directories(SYSTEM_DUMP_STAGING_PATH, ec);
    if (ec)
    {
        lg2::error("Failed to create system dump watch directory {PATH}: "
                   "{ERROR}",
                   "PATH", SYSTEM_DUMP_STAGING_PATH, "ERROR", ec.message());
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
        inotifyFd, SYSTEM_DUMP_STAGING_PATH, newSystemDumpEvents);
    if (inotifyWatchDescriptor < 0)
    {
        auto error = errno;
        close(inotifyFd);
        inotifyFd = -1;
        lg2::error("Failed to watch system dump directory {PATH}, errno: "
                   "{ERRNO}",
                   "PATH", SYSTEM_DUMP_STAGING_PATH, "ERRNO", error);
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
                std::string fileName(inotifyEvent->name);
                std::filesystem::path filePath =
                    std::filesystem::path(SYSTEM_DUMP_STAGING_PATH) / fileName;
                try
                {
                    monitor->handleDumpFile(filePath);
                }
                catch (const std::exception& e)
                {
                    // Never allow an exception to cross the C callback.
                    lg2::error("Failed to handle inotify event: {ERROR}",
                               "ERROR", e);
                }
            }

            offset += eventSize;
        }
    }

    return 0;
}

std::string DumpMonitor::readDumpTypeTag(const std::filesystem::path& filePath)
{
    // The PAPR FILE Directory "Dump File Name" field starts at offset 0x18.
    // The first DUMP_TYPE_TAG_LEN (7) bytes are the type tag: "SYSDUMP" or
    // "RSCDUMP".  Reading just these bytes avoids loading the full file.
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        lg2::error("readDumpTypeTag: cannot open {FILE}", "FILE", filePath);
        return {};
    }
    file.seekg(static_cast<std::streamoff>(DUMP_TYPE_TAG_OFFSET));
    if (!file)
    {
        lg2::error("readDumpTypeTag: seek failed for {FILE}", "FILE", filePath);
        return {};
    }
    std::string tag(DUMP_TYPE_TAG_LEN, '\0');
    file.read(tag.data(), static_cast<std::streamsize>(DUMP_TYPE_TAG_LEN));
    if (file.gcount() != static_cast<std::streamsize>(DUMP_TYPE_TAG_LEN))
    {
        lg2::error("readDumpTypeTag: short read from {FILE}", "FILE", filePath);
        return {};
    }
    return tag;
}

void DumpMonitor::handleDumpFile(const std::filesystem::path& filePath)
{
    using namespace sdbusplus::common::xyz::openbmc_project::dump::entry;
    using namespace sdbusplus::common::com::ibm::dump::entry;

    // Read the 7-byte type tag from the raw file header to determine
    // which D-Bus entry type to correlate against.
    std::string typeTag = readDumpTypeTag(filePath);

    std::string dumpInterface;
    bool isResource = false;

    if (typeTag == "SYSDUMP")
    {
        lg2::info("handleDumpFile: SYSDUMP file {FILE} — correlating with "
                  "system dump entry",
                  "FILE", filePath);
        dumpInterface = System::interface;
    }
    else if (typeTag == "RSCDUMP")
    {
        lg2::info("handleDumpFile: RSCDUMP file {FILE} — correlating with "
                  "resource dump entry",
                  "FILE", filePath);
        dumpInterface = Resource::interface;
        isResource = true;
    }
    else
    {
        lg2::error("handleDumpFile: unrecognised dump type tag '{TAG}' in "
                   "{FILE} — ignoring",
                   "TAG", typeTag, "FILE", filePath);
        return;
    }

    // The dump manager serves System and Resource entries under the same
    // object tree, so one root is searched and the interface tells the two
    // types apart.
    auto dumpEntry = findInProgressDump(SYSTEM_DUMP_ENTRY_BASE, dumpInterface);

    // No dump was requested for this file, so create a NotStarted entry for it.
    // A NotStarted entry never triggers an MPIPL reboot, handleDBusSignal only
    // acts on InProgress entries.
    bool createdByMonitor = false;
    if (!dumpEntry)
    {
        lg2::info("handleDumpFile: no in-progress {TYPE} dump entry for "
                  "{FILE}, creating a NotStarted entry",
                  "TYPE", typeTag, "FILE", filePath);
        dumpEntry = createNotStartedDump(isResource);
        if (!dumpEntry)
        {
            lg2::error("handleDumpFile: could not create a {TYPE} dump entry "
                       "for file {FILE}",
                       "TYPE", typeTag, "FILE", filePath);
            return;
        }
        createdByMonitor = true;
    }

    // Extract the 8-char hex dump id from the last component of the entry path
    // e.g. /xyz/openbmc_project/dump/system/entry/A0000001 → "A0000001"
    std::filesystem::path entryPath(*dumpEntry);
    std::string dumpIdStr = entryPath.filename().string();

    lg2::info("handleDumpFile: raw dump file {FILE}; packaging as entry {ID}",
              "FILE", filePath, "ID", dumpIdStr);

    // Fork opdreport --staged-src to package the raw dump.
    // The event loop remains running in the parent — D-Bus signals and
    // future inotify events continue to be dispatched normally.
    // phosphor-dump-manager's inotify watch on opdump/ fires when
    // opdreport writes the final file, calling updateEntry() to complete
    // the dump entry.
    std::vector<std::string> args = {
        "opdreport",          "-i",           dumpIdStr,         "-d",
        SYSTEM_DUMP_OUT_PATH, "--staged-src", filePath.string(),
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args)
    {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    bool packaged = false;
    pid_t pid = fork();
    if (pid < 0)
    {
        lg2::error("handleDumpFile: fork failed, errno={ERR}", "ERR", errno);
    }
    else if (pid == 0)
    {
        execvp("opdreport", argv.data());
        _exit(EXIT_FAILURE);
    }
    else
    {
        // Parent: reap child.
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);
        packaged = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
        if (packaged)
        {
            lg2::info("handleDumpFile: opdreport completed successfully");
        }
        else
        {
            lg2::error("handleDumpFile: opdreport failed, wait status "
                       "{STATUS} for {FILE}",
                       "STATUS", wstatus, "FILE", filePath);
        }
    }

    // An entry created here has no other owner, so a dump that could not be
    // packaged must not stay behind as a NotStarted entry without a file.
    if (!packaged && createdByMonitor)
    {
        deleteDumpEntry(*dumpEntry);
    }
}

std::optional<std::string> DumpMonitor::createNotStartedDump(bool isResource)
{
    using OpCreate = sdbusplus::common::com::ibm::dump::Create;
    using Progress = sdbusplus::common::xyz::openbmc_project::common::Progress;

    try
    {
        // Parameters are keyed and valued as the dump manager expects them
        std::map<std::string, std::variant<std::string, uint64_t>> params;
        params[OpCreate::convertCreateParametersToString(
            OpCreate::CreateParameters::DumpType)] =
            OpCreate::convertDumpTypeToString(
                isResource ? OpCreate::DumpType::Resource
                           : OpCreate::DumpType::System);
        params[OpCreate::convertCreateParametersToString(
            OpCreate::CreateParameters::OperationStatus)] =
            Progress::convertOperationStatusToString(
                Progress::OperationStatus::NotStarted);

        // The manager object is the parent of the system entry root. The
        // dump manager creates both System and Resource entries on it.
        auto managerPath =
            std::filesystem::path(SYSTEM_DUMP_ENTRY_BASE).parent_path();
        auto method =
            bus.new_method_call(dumpManagerService, managerPath.c_str(),
                                createDumpInterface, "CreateDump");
        method.append(params);
        auto reply = bus.call(method, dbusCallTimeoutUs);

        sdbusplus::message::object_path entryPath;
        reply.read(entryPath);
        return entryPath.str;
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create a NotStarted dump entry: {ERROR}", "ERROR",
                   e);
    }
    return std::nullopt;
}

void DumpMonitor::deleteDumpEntry(const std::string& path)
{
    try
    {
        auto method = bus.new_method_call(dumpManagerService, path.c_str(),
                                          deleteInterface, "Delete");
        bus.call_noreply(method, dbusCallTimeoutUs);
        lg2::info("Deleted dump entry {PATH}", "PATH", path);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to delete dump entry {PATH}: {ERROR}", "PATH", path,
                   "ERROR", e);
    }
}

std::optional<std::string> DumpMonitor::findInProgressDump(
    const std::string& entryRoot, const std::string& dumpInterface)
{
    using namespace sdbusplus::common::xyz::openbmc_project::common;

    std::vector<std::string> dumpEntries;
    try
    {
        auto mapperCall =
            bus.new_method_call(objectMapperService, objectMapperPath,
                                objectMapperInterface, "GetSubTreePaths");
        mapperCall.append(entryRoot, 0,
                          std::vector<std::string>{dumpInterface});
        auto mapperReply = bus.call(mapperCall);
        mapperReply.read(dumpEntries);
    }
    catch (const std::exception& e)
    {
        // ObjectMapper returns an error when no objects exist under the
        // subtree root — treat as empty.
        lg2::info("findInProgressDump: mapper query for {ROOT} failed "
                  "(no entries?): {ERROR}",
                  "ROOT", entryRoot, "ERROR", e);
        return std::nullopt;
    }

    std::sort(dumpEntries.begin(), dumpEntries.end());

    const auto inProgressStatus = Progress::convertOperationStatusToString(
        Progress::OperationStatus::InProgress);
    std::vector<std::string> inProgressEntries;

    for (const auto& entry : dumpEntries)
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
        lg2::error("Found {COUNT} in-progress {INTF} dump entries; using "
                   "{ENTRY}",
                   "COUNT", inProgressEntries.size(), "INTF", dumpInterface,
                   "ENTRY", inProgressEntries.front());
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

    // Inspect SBEDumpTriggerType and route based on trigger type.
    // Pass trigger type to opdreport and collect dumps as needed.
    // If no trigger type is specified, proceed without the -b flag
    // (will use default behavior).
    auto sbeTriggerTypeIt = properties.find("SBEDumpTriggerType");

    if (sbeTriggerTypeIt != properties.end())
    {
        std::string fullTriggerType =
            std::get<std::string>(sbeTriggerTypeIt->second);

        // Pass the trigger type to opdreport via -b flag
        // Extract bare trigger type for logging and comparison
        std::string_view triggerTypeView(fullTriggerType);
        size_t lastDot = triggerTypeView.rfind('.');
        std::string bareTriggerType =
            (lastDot != std::string::npos)
                ? std::string(triggerTypeView.substr(lastDot + 1))
                : fullTriggerType;

        args.push_back("-b");
        args.push_back(bareTriggerType);

        // Route based on full D-Bus enum value
        if (fullTriggerType == SBE_DUMP_TRIGGER_TIMEOUT)
        {
            // For SBE Timeout: dump_monitor no longer collects; let opdreport
            // invoke dump-collect which calls
            // SbeDumpCollector::collectTriggeredSBEDump() Timeout collection is
            // now handled in SbeDumpCollector
            lg2::info("SBE Timeout dump: routing to dump-collect via opdreport "
                      "for {PATH}",
                      "PATH", path);
        }
        else if (fullTriggerType == SBE_DUMP_TRIGGER_BOOTFAILURE)
        {
            // For BootFailure: dump files were pre-collected; pass path via -p
            auto dumpFilesPathIt = properties.find("DumpFilesPath");
            if (dumpFilesPathIt != properties.end())
            {
                std::string filesPath =
                    std::get<std::string>(dumpFilesPathIt->second);
                args.push_back("-p");
                args.push_back(filesPath);
            }
        }
        else if (fullTriggerType == SBE_DUMP_TRIGGER_DOWNSTREAM)
        {
            // Downstream: SPPE is alive — no hreset, no pre-collected files.
            // EID is already carried as -e (ErrorLogId from the busctl call).
            // dump-collect will call getSBEDump until it returns empty data.
            lg2::info("SBE Downstream dump: routing to dump-collect via "
                      "opdreport for {PATH}",
                      "PATH", path);
        }
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

        // If the mpreboot marker is already present, the user has initiated
        // MPIPL manually via ecmd istep pre_mpipl.  The host is already in
        // MPIPL context and the isteps will drive the dump collection — do
        // not trigger a second MPIPL reboot.
        constexpr auto mprebootMarker = "/run/openbmc/mpreboot@0";
        if (std::filesystem::exists(mprebootMarker))
        {
            lg2::info("mpreboot marker present at {PATH}; skipping MPIPL "
                      "reboot trigger for system dump {DUMP}",
                      "PATH", mprebootMarker, "DUMP", path);
            return;
        }

        // Disruptive system dump: trigger MPIPL reboot so the host writes
        // the raw dump to SYSTEM_DUMP_STAGING_PATH.  The always-on inotify
        // watch (setupInotifyWatch / inotifyCallback) will detect the file and
        // call handleDumpFile() which reads the header type tag to route it.
        startMpReboot(path);
    }
    else
    {
        executeCollectionScript(path, properties);
    }
}

} // namespace openpower::dump
