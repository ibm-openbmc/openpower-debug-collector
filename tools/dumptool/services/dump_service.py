from dumptool.clients.dbus_client import DBusClient
from dumptool.models import DumpType, DumpInfo
from typing import List


class DumpService:
    def __init__(self):
        self.client = DBusClient()

    def list_dumps(self) -> List[DumpInfo]:
        dumps = self.client.list_dumps()
        return [self.client.get_dump_info(d.object_path) for d in dumps]

    def create_dump(self, dump_type: DumpType) -> str:
        return self.client.create_dump(dump_type)

    def delete_dump(self, dump_id: int) -> bool:
        dumps = self.client.list_dumps()
        dump = next((d for d in dumps if d.id == dump_id), None)

        if not dump:
            raise ValueError(f"Dump ID {dump_id} not found")

        return self.client.delete_dump(dump.object_path)

    def get_dump_info(self, dump_id: int) -> DumpInfo:
        dumps = self.client.list_dumps()
        dump = next((d for d in dumps if d.id == dump_id), None)

        if not dump:
            raise ValueError(f"Dump ID {dump_id} not found")

        return self.client.get_dump_info(dump.object_path)


