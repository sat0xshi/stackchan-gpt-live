#!/usr/bin/env python3
"""Provision an OpenAI API key into StackChan's NVS partition.

The key is read without terminal echo, written only to mode-0600 temporary
files, and never passed on a command line. This replaces the complete NVS
partition, so run it before configuring Wi-Fi.
"""

from __future__ import annotations

import argparse
import csv
import getpass
import os
from pathlib import Path
import subprocess
import sys
import tempfile


NVS_OFFSET = "0x9000"
NVS_SIZE = "0x4000"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/ttyACM0")
    parser.add_argument(
        "--key",
        help="Read the key from this environment variable instead of prompting",
        metavar="ENV_VAR",
    )
    args = parser.parse_args()

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        parser.error("IDF_PATH is not set; run this from an ESP-IDF export shell")
    idf_python_env = os.environ.get("IDF_PYTHON_ENV_PATH")
    idf_python = (
        Path(idf_python_env) / "bin" / "python"
        if idf_python_env
        else Path(sys.executable)
    )
    if not idf_python.is_file():
        parser.error("ESP-IDF Python environment was not found; re-run export.sh")

    key = os.environ.get(args.key, "") if args.key else getpass.getpass("OpenAI API key: ")
    if not key:
        parser.error("the API key is empty")

    generator = (
        Path(idf_path)
        / "components"
        / "nvs_flash"
        / "nvs_partition_generator"
        / "nvs_partition_gen.py"
    )
    esptool = Path(idf_path) / "components" / "esptool_py" / "esptool" / "esptool.py"
    if not generator.is_file() or not esptool.is_file():
        parser.error("ESP-IDF NVS generator or esptool was not found")

    with tempfile.TemporaryDirectory(prefix="stackchan-nvs-") as temp_dir:
        os.chmod(temp_dir, 0o700)
        csv_path = Path(temp_dir) / "openai.csv"
        bin_path = Path(temp_dir) / "openai-nvs.bin"
        with csv_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("key", "type", "encoding", "value"))
            writer.writerow(("openai", "namespace", "", ""))
            writer.writerow(("api_key", "data", "string", key))
        os.chmod(csv_path, 0o600)

        subprocess.run(
            [str(idf_python), str(generator), "generate", str(csv_path), str(bin_path), NVS_SIZE],
            check=True,
        )
        subprocess.run(
            [
                str(idf_python),
                str(esptool),
                "--chip",
                "esp32s3",
                "--port",
                args.port,
                "write_flash",
                NVS_OFFSET,
                str(bin_path),
            ],
            check=True,
        )

    key = "\0" * len(key)
    print("OpenAI key provisioned. Configure Wi-Fi on the device next.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
