# dumptool

`dumptool` is an operator-facing command-line interface for managing OpenBMC
dumps through `xyz.openbmc_project.Dump.Manager`. It is installed when
`openpower-debug-collector` is built with `-Dphal_backend=next`.

The tool can:

- list and filter dump entries;
- create BMC, hostboot, hardware, and SBE dumps;
- wait for dump collection to finish;
- show general and dump-type-specific diagnostic properties;
- delete individual dump entries;
- produce table or JSON output; and
- diagnose installation and D-Bus availability problems.

## Requirements

- Python 3.7 or later
- `busctl` from systemd
- access to the OpenBMC system bus
- `xyz.openbmc_project.Dump.Manager`
- IBM dump interfaces for hostboot, hardware, and SBE dump operations

## First checks on a system

Verify which executable and Python package are being loaded before creating a
dump:

```bash
type -a dumptool
dumptool --version
dumptool doctor
```

`doctor` is read-only. It reports the dumptool and Python versions, loaded
`cli.py` path, `busctl` path, supported creation types, dump-manager
reachability, and number of discovered entries.

Expected version output for this implementation:

```text
dumptool 1.1.0
```

## Command summary

```text
dumptool list [OPTIONS]
dumptool create --type TYPE [TYPE OPTIONS] [--wait] [--timeout SECONDS]
dumptool get-info DUMP_ID [--output table|json]
dumptool delete DUMP_ID
dumptool doctor
```

Use `dumptool --help` or `dumptool COMMAND --help` for the complete current
interface. Help text is generated from the same creation contract used for
runtime validation.

## Creating dumps

Creation parameters are deliberately strict:

| Type       | Required parameters             |
| ---------- | ------------------------------- |
| `bmc`      | None                            |
| `hostboot` | `--error-id`                    |
| `hardware` | `--error-id` and `--failing-id` |
| `sbe`      | `--error-id` and `--failing-id` |

`--error-id` is the real PEL/error-log ID associated with the failure. Do not
use illustrative values such as `0xDEADBEEF` for a real diagnostic dump.
`--failing-id` is the failing unit's FAPI position. Both options accept decimal
or `0x`-prefixed hexadecimal integers and are validated as unsigned 64-bit
values.

Examples:

```bash
dumptool create --type bmc
dumptool create --type system

dumptool create --type hostboot \
    --error-id 0x1234ABCD

dumptool create --type hardware \
    --error-id 0x1234ABCD \
    --failing-id 1

dumptool create --type sbe \
    --error-id 0x1234ABCD \
    --failing-id 1
```

The command returns after the dump manager accepts the request. Add `--wait` to
wait for `Completed`, `Failed`, or `Aborted`:

```bash
dumptool create --type hardware \
    --error-id 0x1234ABCD \
    --failing-id 1 \
    --wait \
    --timeout 600
```

The default wait timeout is 300 seconds. `--timeout` is rejected unless `--wait`
is also specified. A failed, aborted, or timed-out collection returns a nonzero
exit status.

For machine-readable output:

```bash
dumptool create --type bmc --output json
dumptool create --type hardware \
    --error-id 0x1234ABCD \
    --failing-id 1 \
    --wait \
    --output json
```

## Listing dumps

```bash
dumptool list
dumptool list --type bmc
dumptool list --type system
dumptool list --type sbe
dumptool list --latest
dumptool list --latest 5
dumptool list --sort start --reverse
dumptool list --output json
```

Supported list filters are `bmc`, `system`, `resource`, `faultlog`, `hostboot`,
`hardware`, `sbe`, `memory-buffer-sbe`, and `unknown`.

The `system` filter includes generic system entries and system-manager subtypes,
including hostboot, hardware, SBE, memory-buffer SBE, and resource entries.
Unknown future entry families remain visible as `unknown` instead of aborting
the complete listing.

Available sort fields are `id`, `type`, `size`, `start`, `end`, and `status`.
`--latest [COUNT]` sorts by start time and cannot be combined with `--sort`.

## Inspecting a dump

```bash
dumptool get-info 30000001
dumptool get-info 30000001 --output json
```

Where available, detailed output includes:

- D-Bus object path
- byte size and UTC timestamps
- display and raw operation status
- offload state and URI
- error-log ID
- failing-unit ID
- SBE trigger type
- pre-collected dump-files path

## Deleting a dump

```bash
dumptool delete 30000001
```

Deletion removes the D-Bus entry and associated dump data and cannot be undone.
The command reports success only after the D-Bus `Delete` method succeeds.

## Exit status

| Status | Meaning                                                  |
| ------ | -------------------------------------------------------- |
| `0`    | Operation succeeded                                      |
| `1`    | D-Bus, timeout, collection, or other operational failure |
| `2`    | Invalid command combination or dump request              |

Errors are written to stderr. Successful JSON remains isolated on stdout for
scripts.

## Troubleshooting an older installation

These messages identify the initial dumptool implementation:

```text
usage: cli.py [-h] {list,create,delete,get-info} ...
cli.py: error: unrecognized arguments: --error-id ... --failing-id ...
'hardware' is not a valid DumpType
```

That installation supports only the older `bmc`, `system`, `resource`, and
`faultlog` model. Update the image or package to a revision containing the
current dumptool implementation. If the image should already be current, run:

```bash
dumptool doctor
python3 -c 'import dumptool.cli as c, dumptool.models as m; print(c.__file__); print([item.value for item in m.DumpType])'
```

The module path reveals stale or shadowing Python installations. On an
`opkg`-based image, the owning package can also be checked with:

```bash
opkg search /usr/bin/dumptool
```

Do not use the old `--type system` command as a substitute for a hardware,
hostboot, or SBE request; it does not provide the required IBM dump parameters.

## On-BMC smoke test

Start with read-only checks:

```bash
dumptool --version
dumptool doctor
dumptool list
dumptool list --output json
```

Then inspect an existing ID from the list:

```bash
dumptool get-info DUMP_ID
dumptool get-info DUMP_ID --output json
```

Only on a system where dump creation is safe, create a BMC dump and wait for
completion:

```bash
dumptool create --type bmc --wait --timeout 600
```

Hardware, hostboot, and SBE smoke tests must use valid platform diagnostic IDs.

## Development and tests

From the repository root:

```bash
PYTHONDONTWRITEBYTECODE=1 \
PYTHONPATH=tools \
python3 -m unittest discover -s tools/dumptool/tests -v
```

For source-tree CLI testing:

```bash
PYTHONPATH=tools ./tools/dumptool-wrapper.sh --help
```

When configured with `-Dphal_backend=next`, Meson registers the same suite as
`dumptool-unit-tests`:

```bash
meson test -C build --suite dumptool --print-errorlogs
```
