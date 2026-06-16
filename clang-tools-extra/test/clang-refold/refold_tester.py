#!/usr/bin/env python3
import argparse
import os
import sys
import subprocess
import shlex
import shutil
import platform
from pathlib import Path


def run(cmd, output_file=None):
  print('RUN:', cmd)

  # Prepare the redirection arguments
  kwargs = {
    "shell": True,
    "check": True,
    "text": True,
  }

  if output_file:
    # If output_file is set, open it and redirect both stdout and stderr
    try:
      with open(output_file, 'w') as f:
        subprocess.run(cmd, stdout=f, stderr=f, **kwargs)
        return
    except (subprocess.CalledProcessError, IOError) as e:
      # Note: with CalledProcessError, the output is in the file,
      # not in the exception because we didn't use capture_output
      print(
        f'\nCOMMAND FAILED (exit {getattr(e, "returncode", "IOError")}): {cmd}',
        file=sys.stderr
      )
      print(f'Check logs in: {output_file}', file=sys.stderr)
      sys.exit(getattr(e, "returncode", 1))
  else:
    # Original behavior: capture output to memory for display on failure
    try:
      r = subprocess.run(cmd, capture_output=True, **kwargs)
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


def make_relative_to(path_text: str, base_dir: str) -> str:
  path = Path(path_text)
  if not path.is_absolute():
    return path_text

  base = Path(base_dir).resolve()
  normalized = path.resolve()
  try:
    return str(normalized.relative_to(base))
  except ValueError:
    # Keep SDK/system/external absolute paths absolute.  The harness only
    # canonicalizes test-local paths so the refold map does not depend on the
    # build checkout path for %S/... operands.
    return path_text


def relativize_clang_flag_paths(flags, base_dir: str):
  # Path-bearing clang options accepted either as two argv entries, e.g.
  #   -I path
  # or as a single joined argv entry, e.g.
  #   -Ipath
  separate_path_opts = {
      '-I', '-iquote', '-isystem', '-idirafter',
      '-F', '-iframework',
      '-include', '-imacros', '-include-pch',
  }
  joined_path_prefixes = (
      '-iquote', '-isystem', '-idirafter',
      '-iframework', '-I', '-F',
  )

  out = []
  i = 0
  while i < len(flags):
    flag = flags[i]
    if flag in separate_path_opts and i + 1 < len(flags):
      out.append(flag)
      out.append(make_relative_to(flags[i + 1], base_dir))
      i += 2
      continue

    rewritten = False
    for prefix in joined_path_prefixes:
      if flag.startswith(prefix) and len(flag) > len(prefix):
        out.append(prefix + make_relative_to(flag[len(prefix):], base_dir))
        rewritten = True
        break
    if rewritten:
      i += 1
      continue

    out.append(flag)
    i += 1
  return out


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--with-lines', action='store_true', required=False)
  ap.add_argument('--clang', required=True)
  ap.add_argument('--refolder', required=True)
  ap.add_argument('--headers', required=True)
  ap.add_argument('--expected', required=True)
  ap.add_argument('--log', required=True)
  ap.add_argument('--src', required=True)
  ap.add_argument('--tmp', required=True)
  ap.add_argument(
      '--clang-flags-mode', action='store_true', required=False,
      help=('Treat trailing positional arguments as raw clang producer flags '
            'instead of macro specs. In this mode, no default header -I is '
            'injected; the RUN line owns the full producer include path.'))

  # Positional: test name + optional extras.  In the default mode the extras
  # are macro specs ("NAME" or "NAME=VALUE").  In --clang-flags-mode they
  # are passed verbatim to the producer clang command.  A leading "--" is
  # accepted as a separator and is not forwarded to clang.
  ap.add_argument('testname')
  ap.add_argument('extras', nargs=argparse.REMAINDER)
  args = ap.parse_args()

  extras = list(args.extras)
  if extras and extras[0] == '--':
    extras = extras[1:]

  if not args.clang_flags_mode:
    bad_macro_specs = [m for m in extras if m.startswith('-')]
    if bad_macro_specs:
      ap.error(
          'trailing arguments look like clang flags, but --clang-flags-mode '
          'was not set.  Use %clang-refold-tester-clang-flags for raw '
          'producer flags; first suspicious argument: ' + bad_macro_specs[0])

  testname = args.testname
  src = args.src
  src_dirname, src_basename = os.path.split(src)
  tmp_out = os.path.join(args.tmp, 'outputs')
  os.makedirs(tmp_out, exist_ok=True)

  # expected dir label: NAME=VALUE → NAME@VALUE
  def labelize(m: str) -> str:
    return m.replace('=', '@')

  if args.clang_flags_mode:
    exp_base = os.path.join(args.expected, testname)
  elif extras:
    label = 'define_' + '_'.join(labelize(m) for m in extras)
    exp_base = os.path.join(args.expected, testname, label)
  else:
    exp_base = os.path.join(args.expected, testname)

  # Output artifacts
  out_i = os.path.join(tmp_out, f'{testname}.c.i')
  out_json = os.path.join(tmp_out, f'{testname}.c.refold.json')
  out_mod = os.path.join(tmp_out, f'{testname}.c.mod')
  out_out = os.path.join(tmp_out, f'{testname}.out')
  verify_out = os.path.join(tmp_out, f'{testname}.verify-out')
  src_out = os.path.join(tmp_out, src_basename)
  out_i_mod = os.path.join(tmp_out, f'{testname}.c.i.mod')

  # Expected artifacts
  # exp_json = os.path.join(exp_base, f'{testname}.c.refold.json')
  exp_i = os.path.join(exp_base, f'{testname}.c.i')
  exp_i_mod = os.path.join(exp_base, f'{testname}.c.i.mod')
  exp_mod = os.path.join(exp_base, f'{testname}.c.mod')

  if args.clang_flags_mode:
    producer_flags = relativize_clang_flag_paths(extras, src_dirname)
  else:
    header_path = Path(args.headers)
    header_rel_path = header_path.relative_to(src_dirname)
    producer_flags = ['-I', str(header_rel_path)]
    producer_flags.extend(f'-D{m}' for m in extras)

  producer_flags_str = ' '.join(shlex.quote(f) for f in producer_flags)

  # 1) Preprocess to .c.i and produce refold map JSON
  clang_cmd = ''
  if platform.system() == 'Darwin':
    isysroot = get_macos_sdk_flag()
    if isysroot:
      clang_cmd = (
          f'{shlex.quote(args.clang)} -E -P {isysroot} '
          f'{producer_flags_str} '
          f'--refold-map={shlex.quote(out_json)} '
          f'{shlex.quote(src_basename)} -o {shlex.quote(out_i)}'
      )
  if not clang_cmd:
    clang_cmd = (
        f'{shlex.quote(args.clang)} -E -P '
        f'{producer_flags_str} '
        f'--refold-map={shlex.quote(out_json)} '
        f'{shlex.quote(src_basename)} -o {shlex.quote(out_i)}'
    )
  os.chdir(src_dirname)
  run(clang_cmd)

  # 2) Compare producer artifacts
  # run(f'diff -u {shlex.quote(exp_json)} {shlex.quote(out_json)}')
  run(f'diff -u {shlex.quote(exp_i)}  {shlex.quote(out_i)}')

  # 3) Run clang-refold
  line_flag = '' if args.with_lines else '--no-lines'
  clang_refold_cmd = (
      f'{shlex.quote(args.refolder)} {line_flag} --strict '
      f'--log-level={shlex.quote(args.log)} '
      f'--pp {shlex.quote(out_i)} '
      f'--pp-mod {shlex.quote(exp_i_mod)} '
      f'--refold-map {shlex.quote(out_json)} '
      f'--out {shlex.quote(out_mod)}'
  )
  run(clang_refold_cmd, out_out)
  shutil.copy2(src, src_out)
  shutil.copy2(exp_i_mod, out_i_mod)

  # 4) Compare final refolded output
  run(f'diff -u {shlex.quote(exp_mod)} {shlex.quote(out_mod)}')

  # 5) Run the checker
  verify_out = os.path.join(tmp_out, f'{testname}.verify.out')
  clang_refold_checker_cmd = (
      f'{shlex.quote(args.refolder)} {line_flag} '
      f'--log-level={shlex.quote(args.log)} '
      f'--check {shlex.quote(out_mod)} '
      f'--pp-mod {shlex.quote(exp_i_mod)} '
      f'--refold-map {shlex.quote(out_json)}'
  )
  run(clang_refold_checker_cmd, verify_out)


if __name__ == '__main__':
  main()
