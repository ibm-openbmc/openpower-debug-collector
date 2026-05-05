import subprocess
import datetime
from typing import List, Dict
from dumptool.models import DumpEntry, DumpType, DumpInfo

class DBusClient:
    BUSNAME = "xyz.openbmc_project.Dump.Manager"

    #List Dumps
    def list_dumps(self) -> List[DumpEntry]:
        result = subprocess.run(
            ["busctl", "tree", self.BUSNAME],
            capture_output=True,
            text=True
        )

        dumps = []

        for line in result.stdout.strip().split("\n"):
            line = line.strip()

            if "/xyz/openbmc_project/dump/" in line and "/entry/" in line:
                path = line.split()[-1]

                try:
                    dump_id = int(path.split("/")[-1])
                    dumps.append(
                        DumpEntry(
                            id=dump_id,
                            type=DumpType.from_path(path),
                            object_path=path
                        )
                    )
                except ValueError:
                    continue

        return dumps

    #Create Dump
    def create_dump(self, dump_type: DumpType, params: Dict = {}) -> str:
        result = subprocess.run(
            [
                "busctl", "call",
                self.BUSNAME,
                dump_type.object_path,
                "xyz.openbmc_project.Dump.Create",
                "CreateDump",
                "a{sv}", "0"
            ],
            capture_output=True,
            text=True
        )

        print("STDOUT:", result.stdout)
        return result.stdout.strip()

    #Delete Dump
    def delete_dump(self, object_path: str) -> bool:
        result = subprocess.run(
            [
                "busctl", "call",
                self.BUSNAME,
                object_path,
                "xyz.openbmc_project.Object.Delete",
                "Delete"
            ],
            capture_output=True,
            text=True
        )

        return result.returncode == 0

    #Get Dump Info
    def get_dump_info(self, object_path: str) -> DumpInfo:

        def get_property(prop, interface):
            result = subprocess.run(
                [
                    "busctl", "get-property",
                    self.BUSNAME,
                    object_path,
                    interface,
                    prop
                ],
                capture_output=True,
                text=True
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
            except:
                return str(value)

        def parse_status(status_raw):
            if not status_raw:
                return None
            if "Completed" in status_raw:
                return True
            elif "InProgress" in status_raw:
                return False
            return None


        size = get_property("Size", "xyz.openbmc_project.Dump.Entry")
        offloaded = get_property("Offloaded", "xyz.openbmc_project.Dump.Entry")

        started_time_raw = get_property("StartTime", "xyz.openbmc_project.Common.Progress")
        ended_time_raw = get_property("CompletedTime", "xyz.openbmc_project.Common.Progress")
        status_raw = get_property("Status", "xyz.openbmc_project.Common.Progress")


        completed = parse_status(status_raw)
        started_time = format_time(started_time_raw)
        ended_time = format_time(ended_time_raw)

        dump_id = int(object_path.split("/")[-1])
        dump_type = DumpType.from_path(object_path)

        return DumpInfo(
            id=dump_id,
            type=dump_type,
            size=size,
            completed=completed,
            offloaded=offloaded,
            started_time=started_time,
            ended_time=ended_time
        )

