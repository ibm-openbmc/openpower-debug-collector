#include "create_pel.hpp"

#include "dump_utils.hpp"

#include <fcntl.h>
#include <libekb.H>
#include <phal_exception.H>
#include <unistd.h>

#include <phosphor-logging/elog.hpp>
#include <xyz/openbmc_project/Logging/Create/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace openpower
{
using namespace phosphor::logging;

namespace dump
{
namespace pel
{

constexpr auto loggingObjectPath = "/xyz/openbmc_project/logging";
constexpr auto loggingInterface = "xyz.openbmc_project.Logging.Create";
constexpr auto opLoggingInterface = "org.open_power.Logging.PEL";

constexpr uint8_t FFDC_FORMAT_SUBTYPE = 0xCB;
constexpr uint8_t FFDC_FORMAT_VERSION = 0x01;

using FFDCInfo = std::vector<std::tuple<
    sdbusplus::xyz::openbmc_project::Logging::server::Create::FFDCFormat,
    uint8_t, uint8_t, sdbusplus::message::unix_fd>>;

using Level = sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

uint32_t createPelWithFFDCfiles(const std::string& event,
                                const std::string_view& errMsg,
                                const FFDCData& ffdcData,
                                const Severity& severity,
                                const FFDCInfo& ffdcInfo)
{
    uint32_t plid = 0;
    try
    {
        auto bus = sdbusplus::bus::new_default();

        std::unordered_map<std::string, std::string> additionalData;
        additionalData.emplace("_PID", std::to_string(getpid()));
        additionalData.emplace("SBE_ERR_MSG", errMsg);
        for (auto& data : ffdcData)
        {
            additionalData.emplace(data);
        }

        auto service = util::getService(bus, opLoggingInterface,
                                        loggingObjectPath);
        auto method = bus.new_method_call(service.c_str(), loggingObjectPath,
                                          opLoggingInterface,
                                          "CreatePELWithFFDCFiles");
        auto level =
            sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
                severity);
        method.append(event, level, additionalData, ffdcInfo);
        auto response = bus.call(method);

        // reply will be tuple containing bmc log id, platform log id
        std::tuple<uint32_t, uint32_t> reply = {0, 0};

        // parse dbus response into reply
        response.read(reply);
        plid = std::get<1>(reply); // platform log id is tuple "second"
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>(std::format("D-Bus call exception",
                                    "OBJPATH={}, INTERFACE={}, EXCEPTION={}",
                                    loggingObjectPath, loggingInterface,
                                    e.what())
                            .c_str());
        throw;
    }
    catch (const std::exception& e)
    {
        throw;
    }
    return plid;
}

uint32_t createSbeErrorPEL(const std::string& event, const sbeError_t& sbeError,
                           const FFDCData& ffdcData, const Severity& severity)
{
    uint32_t plid = 0;
    auto ffdcList = sbeError.getFfdcFileList();
    if (ffdcList.size() > 0)
    {
        for (auto& iter : ffdcList)
        {
            FFDCInfo pelFFDCInfo;
            log<level::INFO>(
                std::format("createSbeErrorPEL capturing FFDC data for",
                            "SLID={}", iter.first)
                    .c_str());

            auto tuple = iter.second;
            pelFFDCInfo.emplace_back(std::make_tuple(
                sdbusplus::xyz::openbmc_project::Logging::server::Create::
                    FFDCFormat::Custom,
                FFDC_FORMAT_SUBTYPE, FFDC_FORMAT_VERSION, std::get<1>(tuple)));

            plid = createPelWithFFDCfiles(event, sbeError.what(), ffdcData,
                                          severity, pelFFDCInfo);
        } // endfor
    }
    // we can create pel without any ffdc data
    else
    {
        FFDCInfo pelFFDCInfo;
        plid = createPelWithFFDCfiles(event, sbeError.what(), ffdcData,
                                      severity, pelFFDCInfo);
    } // endif
    return plid;
}

uint32_t createPOZSbeErrorPEL(const std::string& event,
                              const sbeError_t& sbeError,
                              const FFDCData& ffdcData)
{
    uint32_t plid = 0;
    auto& ffdcList = sbeError.getFfdcFileList();

    // poz sbe errors are created only when there is FFDC data
    for (auto& iter : ffdcList)
    {
        FFDCInfo pelFFDCInfo;
        log<level::INFO>(
            std::format("createPOZSbeErrorPEL capturing FFDC data for",
                        "SLID={}", iter.first)
                .c_str());
        auto tuple = iter.second;
        uint8_t severity = std::get<0>(tuple);

        // convert fapi error to pel error
        Level logSeverity = Level::Error;
        if (severity == static_cast<uint8_t>(
                            openpower::phal::FAPI2_ERRL_SEV_UNDEFINED) ||
            severity ==
                static_cast<uint8_t>(openpower::phal::FAPI2_ERRL_SEV_RECOVERED))
        {
            logSeverity = Level::Informational;
        }
        else if (severity == static_cast<uint8_t>(
                                 openpower::phal::FAPI2_ERRL_SEV_PREDICTIVE))
        {
            logSeverity = Level::Warning;
        }
        else if (severity == static_cast<uint8_t>(
                                 openpower::phal::FAPI2_ERRL_SEV_UNRECOVERABLE))
        {
            logSeverity = Level::Error;
        }
        pelFFDCInfo.emplace_back(std::make_tuple(
            sdbusplus::xyz::openbmc_project::Logging::server::Create::
                FFDCFormat::Custom,
            FFDC_FORMAT_SUBTYPE, FFDC_FORMAT_VERSION, std::get<1>(tuple)));

        plid = createPelWithFFDCfiles(event, sbeError.what(), ffdcData,
                                      logSeverity, pelFFDCInfo);
    } // endfor
    return plid;
}

FFDCFile::FFDCFile(const json& pHALCalloutData) :
    calloutData(pHALCalloutData.dump()),
    calloutFile("/tmp/phalPELCalloutsJson.XXXXXX"), fileFD(-1)
{
    prepareFFDCFile();
}

FFDCFile::~FFDCFile()
{
    removeCalloutFile();
}

int FFDCFile::getFileFD() const
{
    return fileFD;
}

void FFDCFile::prepareFFDCFile()
{
    createCalloutFile();
    writeCalloutData();
    setCalloutFileSeekPos();
}

void FFDCFile::createCalloutFile()
{
    fileFD = mkostemp(const_cast<char*>(calloutFile.c_str()), O_RDWR);

    if (fileFD == -1)
    {
        log<level::ERR>(std::format("Failed to create phalPELCallouts "
                                    "file({}), errorno({}) and errormsg({})",
                                    calloutFile, errno, strerror(errno))
                            .c_str());
        throw std::runtime_error("Failed to create phalPELCallouts file");
    }
}

void FFDCFile::writeCalloutData()
{
    ssize_t rc = write(fileFD, calloutData.c_str(), calloutData.size());

    if (rc == -1)
    {
        log<level::ERR>(std::format("Failed to write phaPELCallout info "
                                    "in file({}), errorno({}), errormsg({})",
                                    calloutFile, errno, strerror(errno))
                            .c_str());
        throw std::runtime_error("Failed to write phalPELCallouts info");
    }
    else if (rc != static_cast<ssize_t>(calloutData.size()))
    {
        log<level::WARNING>(std::format("Could not write all phal callout "
                                        "info in file({}), written byte({}) "
                                        "and total byte({})",
                                        calloutFile, rc, calloutData.size())
                                .c_str());
    }
}

void FFDCFile::setCalloutFileSeekPos()
{
    int rc = lseek(fileFD, 0, SEEK_SET);

    if (rc == -1)
    {
        log<level::ERR>(std::format("Failed to set SEEK_SET for "
                                    "phalPELCallouts in file({}), errorno({}) "
                                    "and errormsg({})",
                                    calloutFile, errno, strerror(errno))
                            .c_str());
        throw std::runtime_error(
            "Failed to set SEEK_SET for phalPELCallouts file");
    }
}

void FFDCFile::removeCalloutFile()
{
    close(fileFD);
    std::remove(calloutFile.c_str());
}

} // namespace pel
} // namespace dump
} // namespace openpower
