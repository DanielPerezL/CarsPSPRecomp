#!/usr/bin/env bash
# Reimporta el EBOOT descifrado en Ghidra desde cero y aplica todo el etiquetado automático.
# Uso: tools/ghidra_pipeline.sh   (desde la raíz del repo)
set -euo pipefail
GHIDRA="${GHIDRA:-$HOME/tools/ghidra_12.1.3_PUBLIC}"
mkdir -p ghidra
python3 tools/psp_imports.py decrypted/EBOOT.elf > ghidra/imports.csv
"$GHIDRA/support/analyzeHeadless" ghidra Cars -import decrypted/EBOOT.elf \
    -processor Allegrex:LE:32:default -loader-imagebase 08804000 -overwrite \
    -scriptPath tools/ghidra_scripts \
    -postScript ApplyImports.java ghidra/imports.csv \
    -postScript NameBehaviours.java ghidra/behaviours.csv \
    -postScript ReportStats.java > ghidra/analyze.log 2>&1
grep -E 'error:|ERROR|\.java> ' ghidra/analyze.log | sed 's/.*\.java> //; s/ (GhidraScript)//'
