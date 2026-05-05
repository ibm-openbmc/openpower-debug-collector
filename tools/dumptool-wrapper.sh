#!/bin/sh
# Wrapper script to run dumptool as a Python module
exec python3 -m dumptool.cli "$@"

