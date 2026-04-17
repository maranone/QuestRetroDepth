#!/bin/bash
# -----------------------------------------------------------------------------
# Script Name: hex2asm.sh
# Description: Converts hex files to binary, then disassembles the binaries
#              using the dispel DisPel DEADC0DE Edition Version 1.1.4.
# Author: Your Tim Böttiger
# Created on: 2024-11-15
# License: MIT License
# -----------------------------------------------------------------------------

HEX_CMD="xxd -r -p"
ASM_CMD="../../dispel-snesdc-edition/dispel -f=assembler -o "
ASM_ANALIZED_CMD="../../dispel-snesdc-edition/dispel -f=annotated -o "

HEX_DIR="../deadc0de/hexdumps"
BIN_DIR="../deadc0de/binaries"
ASM_DIR="../deadc0de/assembler/plain"
ASM_ANALYZED_DIR="../deadc0de/assembler/analyzed"

mkdir -p "$BIN_DIR"
mkdir -p "$ASM_DIR"

for hex_file in "$HEX_DIR"/*.hex; do
  base_name=$(basename "$hex_file" .hex)
  bin_file="$BIN_DIR/$base_name.bin"
  asm_file="$ASM_DIR/$base_name.asm"
  asm_analyzed_file="$ASM_ANALYZED_DIR/$base_name.asm"

  echo "Processing $hex_file..."
  $HEX_CMD "$hex_file" > "$bin_file"
  if [[ $? -eq 0 ]]; then
    echo "  Created binary $base_name.bin"
  else
    echo "  E!! Failed to create binary from hex"
    continue
  fi

  $ASM_CMD "$asm_file" "$bin_file"
  if [[ $? -eq 0 ]]; then
    echo "  Disassembled $base_name.asm from binary"
  else
    echo "  E!! Failed to disassemble file from binary"
  fi

  $ASM_ANALIZED_CMD "$asm_analyzed_file" "$bin_file"
  if [[ $? -eq 0 ]]; then
    echo "  Analyzed $base_name.asm from binary"
  else
    echo "  E!! Failed to analyze file"
  fi
done

echo "Done!"
