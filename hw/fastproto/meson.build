# hw/fastproto: fast prototyping of machines, device stubs and hooks.
#
# Every *.c file is picked up automatically:
#   common/**    -> compiled for every arch that has a folder below
#   <arch>/**    -> compiled for system targets of that arch; the folder name
#                   is a key of hw_arch (arm, riscv, i386, mips, ppc, ...)
# Skipped: templates/ and every path component starting with '_'
#          (rename a file/folder to _foo to quickly disable it).
#
# NOTE: meson globs only at configure time. After adding/removing files run
#       `touch hw/fastproto/meson.build` (ninja then re-runs meson).

fp_find = find_program('find', required: true)

fp_glob = {}
foreach dir : ['common'] + hw_arch.keys()
  fp_path = meson.current_source_dir() / dir
  if fs.is_dir(fp_path)
    fp_out = run_command(fp_find, fp_path, '-name', '*.c',
                         '-not', '-path', fp_path / '_*',
                         '-not', '-path', fp_path / '*' / '_*',
                         check: true).stdout().strip()
    fp_glob += { dir: fp_out == '' ? [] : fp_out.split('\n') }
  endif
endforeach

fp_common = fp_glob.get('common', [])
foreach arch, fp_files : fp_glob
  if arch != 'common'
    hw_arch[arch].add(files(fp_common + fp_files))
  endif
endforeach
