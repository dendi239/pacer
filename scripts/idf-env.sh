# Sourced by pixi on env activation (see [tool.pixi.activation] in pyproject.toml).
# Replaces `. $IDF_PATH/export.sh`: idf.py's Python deps live in the pixi env
# itself (see [tool.pixi.pypi-dependencies]), so all export.sh has left to do is
# point at the ESP-IDF checkout and put the cross-toolchains on PATH.
#
# One-time setup on a new machine:
#   git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf
#   pixi run fw-setup   # downloads the xtensa toolchain into $IDF_TOOLS_PATH

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
# Pixi's python provides the packages; don't pin exact versions against IDF's
# constraint files.
export IDF_PYTHON_CHECK_CONSTRAINTS=no
# The pixi env *is* the IDF python env.
if [ -n "$CONDA_PREFIX" ]; then
    export IDF_PYTHON_ENV_PATH="$CONDA_PREFIX"
fi

# idf.py itself
if [ -d "$IDF_PATH/tools" ]; then
    PATH="$IDF_PATH/tools:$PATH"
fi

# Cross-toolchains installed by `idf_tools.py install` (fw-setup task).
for _idf_tool_bin in \
    "$IDF_TOOLS_PATH"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin \
    "$IDF_TOOLS_PATH"/tools/xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin \
    "$IDF_TOOLS_PATH"/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin; do
    if [ -d "$_idf_tool_bin" ]; then
        PATH="$_idf_tool_bin:$PATH"
    fi
done
unset _idf_tool_bin
export PATH

# ROM ELF symbols, used by `idf.py monitor` to decode backtraces.
for _idf_rom_elfs in "$IDF_TOOLS_PATH"/tools/esp-rom-elfs/*/; do
    if [ -d "$_idf_rom_elfs" ]; then
        export ESP_ROM_ELF_DIR="$_idf_rom_elfs"
    fi
done
unset _idf_rom_elfs
