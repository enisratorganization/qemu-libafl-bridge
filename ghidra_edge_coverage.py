#!/usr/bin/env python3
"""
Backward-compatible launcher: the implementation lives in the
`ghidra_edge_coverage/` package next to this file.

  python ghidra_edge_coverage.py ...        # same as before
  python -m ghidra_edge_coverage ...        # equivalent
  python ghidra_edge_coverage.py --help     # full usage documentation
"""
from ghidra_edge_coverage.cli import main

if __name__ == "__main__":
    main()
