"""
Replay a QEMU edge-coverage hitmap in Ghidra.

The modified QEMU hashes (edge_id, pc) of every *conditional* control-flow
decision into a probabilistic hitmap.  This package walks the program in a
Ghidra project, replays each such instruction once per possible outcome in
the `qemu-edge-coverage-<arch>` tool, and marks the outcomes whose hitmap
offset was recorded as "hit edges".  Optional filter stages then remove
false positives caused by hash collisions.

See `cli.py` for the command line interface and `--help`.
"""

__version__ = "2.0"
