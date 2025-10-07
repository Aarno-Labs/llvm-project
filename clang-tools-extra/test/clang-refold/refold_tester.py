#!/usr/bin/env python3
import argparse
import os
import sys
import subprocess
import shlex
import platform

def run(cmd):
  print('RUN:', cmd)
  try:
    r = subprocess.run(cmd, shell=True, check=True,
                       text=True, capture_output=True)
    # If you want to show stdout on success too, uncomment:
    # if r.stdout:
    #   print(r.stdout, end='')
    return
  except subprocess.CalledProcessError as e:
    print(f'\nCOMMAND FAILED (exit {e.returncode}): {cmd}', file=sys.stderr)
    if e.stdout:
      print('\n--- stdout ---', file=sys.stderr)
      print(e.stdout, end='', file=sys.stderr)
    if e.stderr:
      print('\n--- stderr ---', file=sys.stderr)
      print(e.stderr, end='', file=sys.stderr)
    sys.exit(e.returncode)


def get_macos_sdk_flag():
  # Ask Xcode for the current macOS SDK path
  try:
    r = subprocess.run(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
        check=True, text=True, capture_output=True
    )
    sdk = r.stdout.strip()
    if not sdk:
      return ''
    # Quote the path for shell injection
    return f'-isysroot {shlex.quote(sdk)}'
  except subprocess.CalledProcessError as e:
    # If xcrun fails, don't hard-fail the entire test runner; just omit isysroot.
    print("WARNING: failed to obtain macOS SDK with xcrun; "
          "continuing without -isysroot", file=sys.stderr)
    if e.stderr:
      print(e.stderr, file=sys.stderr, end='')
    return ''


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--clang', required=True)
  ap.add_argument('--refolder', required=True)
  ap.add_argument('--headers', required=True)
  ap.add_argument('--expected', required=True)
  ap.add_argument('--log', required=True)
  ap.add_argument('--src', required=True)
  ap.add_argument('--tmp', required=True)

  # Positional: test name + optional macro specs (“NAME” or “NAME=VALUE”).
  ap.add_argument('testname')
  ap.add_argument('macros', nargs='*')  # e.g. BOOL_FLAG  or  ARG_FLAG=ARG
  args = ap.parse_args()

  testname = args.testname
  src = args.src
  tmp_out = os.path.join(args.tmp, 'outputs')
  os.makedirs(tmp_out, exist_ok=True)

  # expected dir label: NAME=VALUE → NAME@VALUE
  def labelize(m: str) -> str:
    return m.replace('=', '@')

  if args.macros:
    label = 'define_' + '_'.join(labelize(m) for m in args.macros)
    exp_base = os.path.join(args.expected, testname, label)
  else:
    exp_base = os.path.join(args.expected, testname)

  # Output artifacts
  out_i = os.path.join(tmp_out, f'{testname}.c.i')
  out_json = os.path.join(tmp_out, f'{testname}.c.refold.json')
  out_mod = os.path.join(tmp_out, f'{testname}.c.mod')

  # Expected artifacts
  # exp_json = os.path.join(exp_base, f'{testname}.c.refold.json')
  exp_i = os.path.join(exp_base, f'{testname}.c.i')
  exp_i_mod = os.path.join(exp_base, f'{testname}.c.i.mod')
  exp_mod = os.path.join(exp_base, f'{testname}.c.mod')

  # Build -D flags verbatim from macro specs
  dflags = ' '.join(f'-D{m}' for m in args.macros)

  # 1) Preprocess to .c.i and produce refold map JSON
  clang_cmd = ''
  if platform.system() == 'Darwin':
    isysroot = get_macos_sdk_flag()
    if isysroot:
      clang_cmd = (
          f'{shlex.quote(args.clang)} -E -P {isysroot} '
          f'-I {shlex.quote(args.headers)} {dflags} '
          f'--refold-map={shlex.quote(out_json)} '
          f'{shlex.quote(src)} -o {shlex.quote(out_i)}'
      )
  if not clang_cmd:
    clang_cmd = (
        f'{shlex.quote(args.clang)} -E -P -I {shlex.quote(args.headers)} '
        f'{dflags} --refold-map={shlex.quote(out_json)} '
        f'{shlex.quote(src)} -o {shlex.quote(out_i)}'
    )
  run(clang_cmd)

  # 2) Compare producer artifacts
  # run(f'diff -u {shlex.quote(exp_json)} {shlex.quote(out_json)}')
  run(f'diff -u {shlex.quote(exp_i)}  {shlex.quote(out_i)}')

  # 3) Run clang-refold
  clang_refold_cmd = (
      f'{shlex.quote(args.refolder)} --log-level={shlex.quote(args.log)} '
      f'--pp {shlex.quote(out_i)} '
      f'--pp-mod {shlex.quote(exp_i_mod)} '
      f'--refold-map {shlex.quote(out_json)} '
      f'--out {shlex.quote(out_mod)}'
  )
  run(clang_refold_cmd)

  # 4) Compare final refolded output
  run(f'diff -u {shlex.quote(exp_mod)} {shlex.quote(out_mod)}')


if __name__ == '__main__':
  main()
