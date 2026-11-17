"""
Architecture registry.  Each arch module provides:

  NAME        e.g. "aarch64"
  TOOL        default qemu-edge-coverage tool binary name
  detect(program)  -> bool   (from the Ghidra program's language)
  bind(program)    (optional) called once after the program is opened; used
                   to cache language-level state (e.g. the TMode register)
  plan_runs(fapi, ins) -> [(dst, label, nzcv, regs, mode), ...]
      Tool runs needed for one instruction.  Empty = QEMU records nothing
      for it, or it cannot be computed statically (indirect branches).
      `mode` is an arch-specific dict of extra CPU state the tool must
      install (e.g. Thumb/IT bits); it participates in the tool cache key
      and is encoded into the -f/-m flags via tool_flags().
  tool_flags(nzcv, mode) -> (flags, flags_mask)   (optional, see below)

The default tool_flags() returns (nzcv << 28, 0xf0000000), i.e. NZCV via
cpsr_write() with the tool's default mask.
"""

ARCHS = {}


def register(arch):
    """Class decorator: put @register on a class implementing the above."""
    ARCHS[arch.NAME] = arch
    return arch


def default_tool_flags(nzcv, mode):
    return nzcv << 28, 0xF0000000


def get_arch(name, program=None):
    """Look up an arch by name; "auto" detects it from the program language."""
    if name != "auto":
        if name not in ARCHS:
            raise SystemExit("unknown arch %r, choose from: auto, %s"
                             % (name, ", ".join(sorted(ARCHS))))
        return ARCHS[name]
    if program is None:
        raise SystemExit("--arch auto needs an open program to detect the language")
    proc = str(program.getLanguageID()).split(":")[0]
    for arch in ARCHS.values():
        if arch.detect(program):
            return arch
    raise SystemExit("no arch module for processor %r" % proc)


class ArchBase(object):
    # subclasses set NAME, TOOL and implement detect()/plan_runs()
    tool_flags = staticmethod(default_tool_flags)

    @staticmethod
    def bind(program):
        """Called once after the program is opened (no-op by default)."""
        pass
