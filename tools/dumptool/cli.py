#!/usr/bin/env python3

import argparse

from dumptool.services.dump_service import DumpService
from dumptool.models import DumpType


service = DumpService()


def list_dumps(filter_type):
    dumps = service.list_dumps()

    print(
        f"{'ID':<10} "
        f"{'Type':<12} "
        f"{'Size(KB)':<12} "
        f"{'Start Time':<20} "
        f"{'End Time':<20} "
        f"{'Status':<15}"
    )

    for d in dumps:
        if filter_type and d.final_type != filter_type:
            continue

        print(
            f"{d.id:<10} "
            f"{d.final_type:<12} "
            f"{d.size_kb:<12} "
            f"{str(d.started_time or '-'): <20} "
            f"{str(d.ended_time or '-'): <20} "
            f"{d.status:<15}"
        )


def create_dump(dump_type, error_id, failing_id):
    try:
        dump_type = DumpType(dump_type)

        # Hardware requires Error ID
        if dump_type == DumpType.HARDWARE and error_id is None:
            print("✖ Hardware dump requires --error-id")
            return

        # SBE requires Failing ID
        if dump_type == DumpType.SBE and failing_id is None:
            print("✖ SBE dump requires --failing-id")
            return

        result = service.create_dump(
            dump_type,
            error_log_id=error_id,
            failing_unit_id=failing_id,
        )

        # Extract dump ID from returned object path
        dump_id = (
            result
            .split("/")[-1]
            .replace('"', "")
            .replace("'", "")
        )

        print("✔ Dump created successfully")
        print(f"Dump ID : {dump_id}")

    except ValueError as e:
        print(f"✖ {e}")

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
    parser = argparse.ArgumentParser(
        prog="dumptool",
        description="""
OpenBMC Dump Management Tool

Supported dump types:
  bmc
  system
  hostboot
  hardware
  sbe
""",
        formatter_class=argparse.RawTextHelpFormatter,
        epilog="""
Examples:
  dumptool list
  dumptool list --type bmc
  dumptool list --type system
  dumptool list --type hostboot
  dumptool list --type hardware
  dumptool list --type sbe

  dumptool create --type bmc
  dumptool create --type system
  dumptool create --type hostboot
  dumptool create --type hardware --error-id <ERROR_ID>
  dumptool create --type sbe --failing-id <FAILING_UNIT_ID>

  dumptool get-info <DUMP_ID>
  dumptool delete <DUMP_ID>

Placeholders:
  <ERROR_ID>         Error Log ID (e.g. 0xDEADBEEF)
  <FAILING_UNIT_ID>  Failing Unit ID (e.g. 1)
  <DUMP_ID>          Dump ID
""",
    )

    subparsers = parser.add_subparsers(
        dest="command",
        title="Commands",
        metavar="<command>",
    )

    # ---------------- LIST ----------------
    list_parser = subparsers.add_parser(
        "list",
        help="List available dumps",
        description="List all available dumps.",
    )

    list_parser.add_argument(
        "--type",
        choices=[
            "bmc",
            "system",
            "hostboot",
            "hardware",
            "sbe",
        ],
        help="Filter dumps by type.",
    )

    # ---------------- CREATE ----------------
    create_parser = subparsers.add_parser(
        "create",
        help="Create a dump",
        description="Create a new dump.",
    )

    create_parser.add_argument(
        "--type",
        required=True,
        choices=[
            "bmc",
            "system",
            "hostboot",
            "hardware",
            "sbe",
        ],
        help="Type of dump to create.",
    )

    create_parser.add_argument(
        "--error-id",
        type=lambda x: int(x, 0),
        help=(
            "Error Log ID "
            "(required for hardware, "
            "optional for hostboot and sbe)."
        ),
    )

    create_parser.add_argument(
        "--failing-id",
        type=int,
        help=(
            "Failing Unit ID "
            "(required for sbe, "
            "optional for hardware)."
        ),
    )

    # ---------------- DELETE ----------------
    delete_parser = subparsers.add_parser(
        "delete",
        help="Delete a dump",
        description="Delete a dump by ID.",
    )

    delete_parser.add_argument(
        "dump_id",
        metavar="DUMP_ID",
        type=str,
        help="Dump ID to delete.",
    )

    # ---------------- GET INFO ----------------
    info_parser = subparsers.add_parser(
        "get-info",
        help="Show dump information",
        description="Display detailed information about a dump.",
    )

    info_parser.add_argument(
        "dump_id",
        metavar="DUMP_ID",
        type=str,
        help="Dump ID.",
    )

    args = parser.parse_args()

    if args.command == "list":
        list_dumps(args.type)

    elif args.command == "create":
        create_dump(
            args.type,
            args.error_id,
            args.failing_id,
        )

    elif args.command == "delete":
        delete_dump(args.dump_id)

    elif args.command == "get-info":
        get_dump_info(args.dump_id)

    else:
        parser.print_help()


if __name__ == "__main__":
    main()