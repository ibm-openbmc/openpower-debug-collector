import subprocess
import datetime
from typing import List

from dumptool.models import DumpEntry, DumpType, DumpInfo


class DBusClient:
    BUSNAME = "xyz.openbmc_project.Dump.Manager"

    # List Dumps
    def list_dumps(self) -> List[DumpEntry]:
        result = subprocess.run(
            ["busctl", "tree", self.BUSNAME],
            capture_output=True,
            text=True,
        )

        dumps = []

        for line in result.stdout.strip().split("\n"):
            line = line.strip()

            if "/xyz/openbmc_project/dump/" in line and "/entry/" in line:
                path = line.split()[-1]
                dump_id = path.split("/")[-1]

                dumps.append(
                    DumpEntry(
                        id=dump_id,
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

        # BMC dump
        if dump_type == DumpType.BMC:
            cmd = [
                "busctl",
                "call",
                self.BUSNAME,
                dump_type.object_path,
                "xyz.openbmc_project.Dump.Create",
                "CreateDump",
                "a{sv}",
                "0",
            ]

        # System dump
        elif dump_type == DumpType.SYSTEM:
            cmd = [
                "busctl",
                "call",
                self.BUSNAME,
                "/xyz/openbmc_project/dump/system",
                "xyz.openbmc_project.Dump.Create",
                "CreateDump",
                "a{sv}",
                "1",
                "com.ibm.Dump.Create.CreateParameters.DumpType",
                "s",
                "com.ibm.Dump.Create.DumpType.System",
            ]

        elif dump_type in (
            DumpType.HOSTBOOT,
            DumpType.HARDWARE,
            DumpType.SBE,
        ):
            error_id = (
                error_log_id
                if error_log_id is not None
                else 0xDEADBEEF
            )

            failing_id = (
                failing_unit_id
                if failing_unit_id is not None
                else 1
            )

            subtype_map = {
                DumpType.HOSTBOOT: "Hostboot",
                DumpType.HARDWARE: "Hardware",
                DumpType.SBE: "SBE",
            }

            cmd = [
                "busctl",
                "call",
                self.BUSNAME,
                "/xyz/openbmc_project/dump/system",
                "xyz.openbmc_project.Dump.Create",
                "CreateDump",
                "a{sv}",
                "3",
                "com.ibm.Dump.Create.CreateParameters.DumpType",
                "s",
                f"com.ibm.Dump.Create.DumpType.{subtype_map[dump_type]}",
                "com.ibm.Dump.Create.CreateParameters.ErrorLogId",
                "t",
                str(error_id),
                "com.ibm.Dump.Create.CreateParameters.FailingUnitId",
                "t",
                str(failing_id),
            ]

        else:
            raise ValueError(f"Unsupported dump type: {dump_type.value}")

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
        )

        if result.returncode != 0:
            raise RuntimeError(
                result.stderr.strip() or result.stdout.strip()
            )

        return result.stdout.strip()

    # Delete Dump
    def delete_dump(self, object_path: str) -> bool:
        result = subprocess.run(
            [
                "busctl",
                "call",
                self.BUSNAME,
                object_path,
                "xyz.openbmc_project.Object.Delete",
                "Delete",
            ],
            capture_output=True,
            text=True,
        )

        return result.returncode == 0

    # Get Dump Info
    def get_dump_info(self, object_path: str) -> DumpInfo:

        def get_property(prop, interface):
            result = subprocess.run(
                [
                    "busctl",
                    "get-property",
                    self.BUSNAME,
                    object_path,
                    interface,
                    prop,
                ],
                capture_output=True,
                text=True,
            )

            if result.returncode != 0:
                return None

            output = result.stdout.strip().split()

            if len(output) < 2:
                return None

            dtype, value = output[0], output[1]

            if dtype == "b":
                return value.lower() == "true"

            elif dtype in ["u", "t", "x"]:
                return int(value)

            else:
                return value

        def format_time(value):
            if not value or value == 0:
                return None

            try:
                ts = int(value) / 1_000_000
                dt = datetime.datetime.utcfromtimestamp(ts)
                return dt.strftime("%Y-%m-%d %H:%M:%S")

            except Exception:
                return str(value)

        def parse_status(status_raw):
            if not status_raw:
                return None

            if "Completed" in status_raw:
                return True

            elif "InProgress" in status_raw:
                return False

            return None

        def get_subtype():
            result = subprocess.run(
                [
                    "busctl",
                    "introspect",
                    self.BUSNAME,
                    object_path,
                ],
                capture_output=True,
                text=True,
            )

            output = result.stdout

            if "com.ibm.Dump.Entry.Hardware" in output:
                return "hardware"

            if "com.ibm.Dump.Entry.Hostboot" in output:
                return "hostboot"

            if "com.ibm.Dump.Entry.SBE" in output:
                return "sbe"

            return DumpType.from_path(object_path).value

        size = get_property(
            "Size",
            "xyz.openbmc_project.Dump.Entry",
        )

        offloaded = get_property(
            "Offloaded",
            "xyz.openbmc_project.Dump.Entry",
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

        completed = parse_status(status_raw)
        started_time = format_time(started_time_raw)
        ended_time = format_time(ended_time_raw)

        dump_id = object_path.split("/")[-1]
        dump_type = DumpType.from_path(object_path)
        subtype = get_subtype()

        return DumpInfo(
            id=dump_id,
            type=dump_type,
            subtype=subtype,
            size=size,
            completed=completed,
            offloaded=offloaded,
            started_time=started_time,
            ended_time=ended_time,
        )