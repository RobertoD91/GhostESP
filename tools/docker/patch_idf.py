#!/usr/bin/env python3
"""Apply the ESP-IDF fixups CI needs before GhostESP will compile.

This is the same edit the "Patch ESP-IDF gdbstub compatibility" step in
.github/workflows/compile_all.yml makes; keep the two in sync. A stock
esp-idf v6.1 checkout fails to build esp_gdbstub without it.

Usage: patch_idf.py <idf_target>   (patches $IDF_PATH in place, idempotent)
"""
import os
import re
import sys
from pathlib import Path

XTENSA_TARGETS = {"esp32", "esp32s2", "esp32s3"}


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("IDF_TARGET", "esp32")
    idf = Path(os.environ.get("IDF_PATH", "/opt/esp/idf"))
    if not idf.is_dir():
        raise SystemExit(f"IDF_PATH does not exist: {idf}")

    gdbstub = idf / "components/esp_gdbstub/src/gdbstub.c"
    source = gdbstub.read_text(encoding="utf-8")
    helper_guard = re.compile(
        r"#if\s*\(?\s*CONFIG_ESP_SYSTEM_GDBSTUB_RUNTIME\s*\|\|\s*CONFIG_ESP_GDBSTUB_SUPPORT_TASKS\s*\)?"
    )
    if helper_guard.search(source):
        gdbstub.write_text(helper_guard.sub("#if 1", source), encoding="utf-8")
        print("patched gdbstub.c")

    if target in XTENSA_TARGETS:
        xtensa = idf / "components/esp_gdbstub/src/port/xtensa/gdbstub_xtensa.c"
        source = xtensa.read_text(encoding="utf-8")
        source = source.replace("portNUM_PROCESSORS", "CONFIG_FREERTOS_NUMBER_OF_CORES")
        source, tcb_count = re.subn(
            r"const\s+StaticTask_t\s*\*\s*tcb\s*;", "void *tcb = NULL;", source
        )
        source, lookup_count = re.subn(
            r"tcb\s*=\s*esp_gdbstub_find_tcb_by_frame\s*\(frame\)\s*;", "", source
        )
        source, _ = re.subn(
            r"#if\s+(?:XCHAL_HAVE_FP|0)\s+(?=gdbstub_write_fpu_regs\s*\()",
            "#if 0\n    ",
            source,
        )
        if tcb_count != lookup_count or tcb_count not in {0, 1}:
            raise SystemExit(
                f"unexpected Xtensa FPU patch counts: tcb={tcb_count}, lookup={lookup_count}"
            )
        xtensa.write_text(source, encoding="utf-8")
        print(f"patched gdbstub_xtensa.c (tcb={tcb_count})")


if __name__ == "__main__":
    main()
