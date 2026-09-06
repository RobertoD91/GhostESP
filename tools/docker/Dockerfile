# GhostESP build image.
#
# Espressif's stock v6.1 image cannot compile this project as-is: esp_gdbstub
# fails with `command_name_matches`, `portNUM_PROCESSORS` and `StaticTask_t`
# errors. CI works around it by patching ESP-IDF in place before every build
# (see the "Patch ESP-IDF gdbstub compatibility" step in
# .github/workflows/compile_all.yml). This image bakes the same patch in once,
# so builds run as your own user without needing to write into $IDF_PATH.
FROM espressif/idf:v6.1

COPY patch_idf.py /tmp/patch_idf.py

# The Xtensa half of the patch only applies to xtensa targets, so apply it for
# every target the image may be used with. The RISC-V targets just skip it.
RUN python3 /tmp/patch_idf.py esp32 && rm /tmp/patch_idf.py

# Let the build's `git rev-parse` (root CMakeLists.txt, for GIT_COMMIT_HASH)
# read a bind-mounted tree owned by a different uid than the container user.
RUN git config --system --add safe.directory '*'
