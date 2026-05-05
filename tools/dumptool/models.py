from dataclasses import dataclass
from enum import Enum
from typing import Optional


class DumpType(Enum):
    BMC = "bmc"
    SYSTEM = "system"
    RESOURCE = "resource"
    FAULTLOG = "faultlog"

    @property
    def object_path(self) -> str:
        return f"/xyz/openbmc_project/dump/{self.value}"

    @staticmethod
    def from_path(path: str):
        if "/dump/bmc/" in path:
            return DumpType.BMC
        elif "/dump/system/" in path:
            return DumpType.SYSTEM
        elif "/dump/resource/" in path:
            return DumpType.RESOURCE
        elif "/dump/faultlog/" in path:
            return DumpType.FAULTLOG
        else:
            raise ValueError("Unknown dump type")


@dataclass
class DumpEntry:
    id: int
    type: DumpType
    object_path: str

    @property
    def final_type(self) -> str:
        if self.type == DumpType.BMC:
            return "bmc"
        if self.type == DumpType.SYSTEM:
            if self.id >= 30000000:
                return "sbe"
            elif self.id >= 20000000:
                return "hostboot"
            else:
                return "system"
        return self.type.value


@dataclass
class DumpInfo:
    id: int
    type: DumpType
    size: Optional[int] = None
    completed: Optional[bool] = None
    offloaded: Optional[bool] = None
    started_time: Optional[str] = None
    ended_time: Optional[str] = None

    @property
    def final_type(self) -> str:
        if self.type == DumpType.BMC:
            return "bmc"
        if self.type == DumpType.SYSTEM:
            if self.id >= 30000000:
                return "sbe"
            elif self.id >= 20000000:
                return "hostboot"
            else:
                return "system"
        return self.type.value

    @property
    def size_kb(self):
        return f"{self.size // 1024} KB" if self.size else "-"

    @property
    def status(self):
        if self.completed is None:
            return "Unknown"
        return "Completed" if self.completed else "In Progress"

# Made with Bob
