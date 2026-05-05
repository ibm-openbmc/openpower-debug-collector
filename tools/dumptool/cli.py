#!/usr/bin/env python3
import argparse
from dumptool.services.dump_service import DumpService
from dumptool.models import DumpType

service = DumpService()


def list_dumps(filter_type):
    dumps = service.list_dumps()

    print(f"{'ID':<10} {'Type':<12} {'Size(KB)':<12} {'Start Time':<20} {'End Time':<20} {'Status':<15}")

    for d in dumps:
        if filter_type and d.type.value != filter_type:
            continue

        size_kb = d.size_kb
        status = d.status

        print(f"{d.id:<10} {d.final_type:<12} {size_kb:<12} {str(d.started_time or '-'): <20} {str(d.ended_time or '-'): <20} {status:<15}")


def create_dump(dump_type):
    try:
        service.create_dump(DumpType(dump_type))
        print("✔ Dump created successfully")
    except Exception as e:
        print(f"✖ Failed to create dump: {e}")


def delete_dump(dump_id):
    try:
        service.delete_dump(dump_id)
        print("Dump deleted successfully")
    except ValueError:
        print("Dump not found")


def get_dump_info(dump_id):
    try:
        info = service.get_dump_info(dump_id)

        print(f"ID           : {info.id}")
        print(f"Type         : {info.final_type}")
        print(f"Size (KB)    : {info.size_kb}")
        print(f"Start Time   : {info.started_time or 'N/A'}")
        print(f"End Time     : {info.ended_time or 'N/A'}")
        print(f"Status       : {info.status}")
        print(f"Offloaded    : {info.offloaded}")

    except ValueError:
        print("✖ Dump not found")


def main():
    parser = argparse.ArgumentParser(description="Dump Management Tool")

    subparsers = parser.add_subparsers(dest="command")

    # list
    list_parser = subparsers.add_parser("list", help="List all dumps")
    list_parser.add_argument("--type", help="Filter by dump type")

    # create
    create_parser = subparsers.add_parser("create", help="Create a dump")
    create_parser.add_argument("--type", default="bmc", help="Dump type")

    # delete
    delete_parser = subparsers.add_parser("delete", help="Delete a dump")
    delete_parser.add_argument("dump_id", type=int, help="Dump ID")

    # get-info
    info_parser = subparsers.add_parser("get-info", help="Get dump details")
    info_parser.add_argument("dump_id", type=int, help="Dump ID")

    args = parser.parse_args()

    if args.command == "list":
        list_dumps(args.type)

    elif args.command == "create":
        create_dump(args.type)

    elif args.command == "delete":
        delete_dump(args.dump_id)

    elif args.command == "get-info":
        get_dump_info(args.dump_id)

    else:
        parser.print_help()


if __name__ == "__main__":
    main()


