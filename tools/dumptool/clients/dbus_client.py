import datetime
import re
import shlex
import subprocess
from typing import List

from dumptool.models import (
    DUMP_CREATE_SPECS,
    DumpEntry,
    DumpInfo,
    DumpType,
    validate_create_parameters,
)


class DBusError(RuntimeError):
    """An actionable failure while communicating with D-Bus through busctl."""


class DBusClient:
    BUSNAME = "xyz.openbmc_project.Dump.Manager"
    DEFAULT_TIMEOUT = 30
    ENTRY_PATTERN = re.compile(
        r"(/xyz/openbmc_project/dump/[^/\s]+/entry/[^/\s]+)$"
    )

    def __init__(self, timeout=DEFAULT_TIMEOUT):
        self.timeout = timeout

    def _run_busctl(self, command, action):
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=self.timeout,
            )
        except FileNotFoundError as error:
            raise DBusError(
                "busctl was not found; install systemd busctl before"
                " using dumptool"
            ) from error
        except subprocess.TimeoutExpired as error:
            raise DBusError(
                f"{action} timed out after {self.timeout} seconds"
            ) from error

        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            if not detail:
                detail = f"busctl exited with status {result.returncode}"
            raise DBusError(f"{action} failed: {detail}")

        return result.stdout.strip()

    # List Dumps
    def list_dumps(self) -> List[DumpEntry]:
        output = self._run_busctl(
            ["busctl", "tree", self.BUSNAME],
            "List dumps",
        )

        dumps = []
        seen_paths = set()

        for line in output.splitlines():
            match = self.ENTRY_PATTERN.search(line.strip())
            if not match:
                continue

            path = match.group(1)
            if path in seen_paths:
                continue
            seen_paths.add(path)

            dumps.append(
                DumpEntry(
                    id=path.rsplit("/", 1)[-1],
                    type=DumpType.from_path(path),
                    object_path=path,
                )
            )

        return dumps

    # Create Dump
    def create_dump(
        self,
        dump_type: DumpType,
        error_log_id=None,
        failing_unit_id=None,
    ) -> str:
        validate_create_parameters(dump_type, error_log_id, failing_unit_id)
        spec = DUMP_CREATE_SPECS[dump_type]

        parameters = []
        if spec.dbus_type is not None:
            parameters.append(
                (
                    "com.ibm.Dump.Create.CreateParameters.DumpType",
                    "s",
                    f"com.ibm.Dump.Create.DumpType.{spec.dbus_type}",
                )
            )
        if error_log_id is not None:
            parameters.append(
                (
                    "com.ibm.Dump.Create.CreateParameters.ErrorLogId",
                    "t",
                    str(error_log_id),
                )
            )
        if failing_unit_id is not None:
            parameters.append(
                (
                    "com.ibm.Dump.Create.CreateParameters.FailingUnitId",
                    "t",
                    str(failing_unit_id),
                )
            )

        cmd = [
            "busctl",
            "call",
            self.BUSNAME,
            dump_type.object_path,
            "xyz.openbmc_project.Dump.Create",
            "CreateDump",
            "a{sv}",
            str(len(parameters)),
        ]
        for key, signature, value in parameters:
            cmd.extend((key, signature, value))

        output = shlex.split(self._run_busctl(cmd, "Create dump"))
        if (
            len(output) != 2
            or output[0] != "o"
            or not output[1].startswith("/")
        ):
            raise DBusError(
                "Create dump failed: invalid object path in response"
            )

        return output[1]

    # Delete Dump
    def delete_dump(self, object_path: str) -> bool:
        self._run_busctl(
            [
                "busctl",
                "call",
                self.BUSNAME,
                object_path,
                "xyz.openbmc_project.Object.Delete",
                "Delete",
            ],
            f"Delete dump {object_path.rsplit('/', 1)[-1]}",
        )

        return True

    # Get Dump Info
    def get_dump_info(self, object_path: str) -> DumpInfo:
        dump_id = object_path.rsplit("/", 1)[-1]

        def get_property(prop, interface, optional=False):
            try:
                output_raw = self._run_busctl(
                    [
                        "busctl",
                        "get-property",
                        self.BUSNAME,
                        object_path,
                        interface,
                        prop,
                    ],
                    f"Read {prop} for dump {dump_id}",
                )
            except DBusError as error:
                missing_property_errors = (
                    "UnknownProperty",
                    "UnknownInterface",
                    "Unknown property",
                    "Interface not found",
                )
                if optional and any(
                    marker in str(error) for marker in missing_property_errors
                ):
                    return None
                raise

            output = shlex.split(output_raw)

            if len(output) < 2:
                return None

            dtype, value = output[0], output[1]

            if dtype == "b":
                return value.lower() == "true"

            elif dtype in ["u", "t", "x", "i"]:
                return int(value, 0)

            else:
                return value

        def format_time(value):
            if not value or value == 0:
                return None

            try:
                ts = int(value) / 1_000_000
                dt = datetime.datetime.utcfromtimestamp(ts)
                return dt.strftime("%Y-%m-%d %H:%M:%SZ")

            except Exception:
                return str(value)

        def get_subtype(introspection):
            if "com.ibm.Dump.Entry.Hardware" in introspection:
                return "hardware"

            if "com.ibm.Dump.Entry.Hostboot" in introspection:
                return "hostboot"

            if "com.ibm.Dump.Entry.SBE" in introspection:
                try:
                    prefix = int(dump_id, 16) & 0xF0000000
                except ValueError:
                    prefix = None
                if prefix == 0x40000000:
                    return "memory-buffer-sbe"
                return "sbe"

            if "com.ibm.Dump.Entry.Resource" in introspection:
                return "resource"

            return DumpType.from_path(object_path).value

        introspection = self._run_busctl(
            [
                "busctl",
                "introspect",
                self.BUSNAME,
                object_path,
            ],
            f"Inspect dump {dump_id}",
        )

        size = get_property(
            "Size",
            "xyz.openbmc_project.Dump.Entry",
        )

        offloaded = get_property(
            "Offloaded",
            "xyz.openbmc_project.Dump.Entry",
        )

        offload_uri = get_property(
            "OffloadUri",
            "xyz.openbmc_project.Dump.Entry",
            optional=True,
        )

        started_time_raw = get_property(
            "StartTime",
            "xyz.openbmc_project.Common.Progress",
        )

        ended_time_raw = get_property(
            "CompletedTime",
            "xyz.openbmc_project.Common.Progress",
        )

        status_raw = get_property(
            "Status",
            "xyz.openbmc_project.Common.Progress",
        )

        started_time = format_time(started_time_raw)
        ended_time = format_time(ended_time_raw)

        dump_type = DumpType.from_path(object_path)
        subtype = get_subtype(introspection)

        PELID_IFACE = "org.open_power.Logging.PEL.PELID"

        error_log_id = None
        pel_id = None
        failing_unit_id = None
        dump_files_path = None
        sbe_dump_trigger_type = None
        subtype_interfaces = {
            "hostboot": "com.ibm.Dump.Entry.Hostboot",
            "hardware": "com.ibm.Dump.Entry.Hardware",
            "sbe": "com.ibm.Dump.Entry.SBE",
            "memory-buffer-sbe": "com.ibm.Dump.Entry.SBE",
        }
        subtype_interface = subtype_interfaces.get(subtype)
        if subtype_interface:
            error_log_id = get_property(
                "ErrorLogId",
                subtype_interface,
                optional=True,
            )

        # Read PELID from the new interface (available on all dump types that
        # carry an error-log association, including system dumps)
        pel_id = get_property("PELID", PELID_IFACE, optional=True)
        if pel_id == 0:
            pel_id = None
        if subtype in ("hardware", "sbe", "memory-buffer-sbe"):
            failing_unit_id = get_property(
                "FailingUnitId",
                subtype_interface,
                optional=True,
            )
        if subtype in ("sbe", "memory-buffer-sbe"):
            dump_files_path = get_property(
                "DumpFilesPath",
                subtype_interface,
                optional=True,
            )
            sbe_dump_trigger_type = get_property(
                "SBEDumpTriggerType",
                subtype_interface,
                optional=True,
            )

        return DumpInfo(
            id=dump_id,
            type=dump_type,
            subtype=subtype,
            object_path=object_path,
            size=size,
            offloaded=offloaded,
            offload_uri=offload_uri,
            started_time=started_time,
            ended_time=ended_time,
            started_time_us=started_time_raw,
            ended_time_us=ended_time_raw,
            operation_status=status_raw,
            error_log_id=error_log_id,
            pel_id=pel_id,
            failing_unit_id=failing_unit_id,
            dump_files_path=dump_files_path,
            sbe_dump_trigger_type=sbe_dump_trigger_type,
        )
