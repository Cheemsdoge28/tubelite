#!/usr/bin/env python3
"""Insert or update the TubeLite system entry in an EmulationStation config.

Designed for robustness on ArkOS handhelds:
  - Atomic write: new content is written to a .tmp file then renamed into
    place, so a crash mid-write never leaves the config truncated.
  - Safe single backup: one .bak.tubelite file is kept; if it already
    exists it is overwritten (no unlimited accumulation).
  - Post-write validation: after writing we re-read the file and verify
    that both <systemList> structure and the new <system> block are present.
  - Idempotency: if the entry already exists with matching install_dir,
    platform_tag and theme_name, the file is left untouched.
"""

from pathlib import Path
import argparse
import re
import sys
import os

SYSTEM_BLOCK = '''  <!-- TubeLite YouTube Client — added by install-es-system.py -->
  <system>
    <name>tubelite</name>
    <fullname>TubeLite YouTube Client</fullname>
    <path>{install_dir}</path>
    <extension>.tbl</extension>
    <command>bash %ROM%</command>
    <platform>{platform_tag}</platform>
    <theme>{theme_name}</theme>
  </system>
'''

# Patterns for legacy entries that should be cleaned up
LEGACY_NAMES = ['fire4arkos', 'tubelite']


def _build_expected_block(install_dir, platform_tag, theme_name):
    """Return the exact system block we want to see in the file."""
    return SYSTEM_BLOCK.format(
        install_dir=install_dir,
        platform_tag=platform_tag,
        theme_name=theme_name,
    )


def _entry_matches(ctx, install_dir, platform_tag, theme_name):
    """Check if a tubelite entry already exists with the exact same parameters."""
    m = re.search(
        r'<system>\s*'
        r'<name>tubelite</name>\s*'
        r'<fullname>.*?</fullname>\s*'
        r'<path>(.*?)</path>\s*'
        r'<extension>.*?</extension>\s*'
        r'<command>.*?</command>\s*'
        r'<platform>(.*?)</platform>\s*'
        r'<theme>(.*?)</theme>\s*'
        r'</system>',
        ctx, re.DOTALL)
    if not m:
        return False
    return (m.group(1).strip() == install_dir and
            m.group(2).strip() == platform_tag and
            m.group(3).strip() == theme_name)


def _remove_legacy_entries(ctx):
    """Remove all legacy fire4arkos and existing tubelite entries."""
    for sys_name in LEGACY_NAMES:
        if re.search(r'<name>' + sys_name + r'</name>', ctx):
            print(f'  Removing old {sys_name} entry')
            # Remove comment-delimited blocks (<!-- Fire4arkos ... </system>)
            ctx = re.sub(
                r'\s*<!-- (?:' + sys_name.capitalize() + r'|TubeLite).*?</system>',
                '', ctx, flags=re.DOTALL)
            # Remove bare <system><name>sys_name</name>...</system> blocks
            ctx = re.sub(
                r'\s*<system>\s*<name>' + sys_name + r'</name>.*?</system>',
                '', ctx, flags=re.DOTALL)
    return ctx


def _validate_output(content, filename):
    """Raise if the output is structurally broken."""
    # <systemList> may carry XML namespace attributes, so check with a regex
    if not re.search(r'<systemList[\s>]', content):
        raise RuntimeError(f'Output for {filename} is missing <systemList> tag')
    if '</systemList>' not in content:
        raise RuntimeError(f'Output for {filename} is missing </systemList> tag')
    if '<name>tubelite</name>' not in content:
        raise RuntimeError(f'Output for {filename} is missing tubelite system entry')


def insert_system(filename, install_dir, platform_tag='tubelite', theme_name='tubelite'):
    filename = Path(filename)
    if not filename.exists():
        raise FileNotFoundError(f'Config file does not exist: {filename}')

    with open(filename, encoding='utf-8') as fh:
        ctx = fh.read()

    # ── Idempotency: skip if already up-to-date ─────────────────────
    if _entry_matches(ctx, install_dir, platform_tag, theme_name):
        print(f'TubeLite entry in {filename} is already up-to-date — skipping')
        return True

    # ── Validate structure ──────────────────────────────────────────
    if not re.search(r'</systemList>', ctx):
        raise RuntimeError(f'Cannot find </systemList> in {filename} — file may be corrupt')

    # ── Remove legacy / stale entries ───────────────────────────────
    ctx = _remove_legacy_entries(ctx)

    # ── Insert new block before </systemList> ───────────────────────
    system_block = _build_expected_block(install_dir, platform_tag, theme_name)
    ctx = re.sub(r'</systemList>', system_block + r'\g<0>', ctx)

    # ── Validate before writing ─────────────────────────────────────
    _validate_output(ctx, filename)

    # ── Atomic write: .tmp → rename ─────────────────────────────────
    # 1. Create backup (single rotation — overwrite any existing backup)
    backup = filename.with_name(filename.name + '.bak.tubelite')
    try:
        # Copy instead of rename so the original stays in place until
        # we've successfully written the new file.
        import shutil
        shutil.copy2(str(filename), str(backup))
    except OSError as e:
        print(f'  Warning: could not create backup {backup}: {e}', file=sys.stderr)

    # 2. Write new content to a temporary file in the same directory
    tmp_file = filename.with_name(filename.name + '.tmp.tubelite')
    try:
        with open(tmp_file, 'w', encoding='utf-8') as fh:
            fh.write(ctx)
        fh.close()

        # 3. Validate the temp file is readable and correct
        with open(tmp_file, encoding='utf-8') as fh:
            verify = fh.read()
        _validate_output(verify, tmp_file)

        # 4. Atomic replace (on Linux, os.rename is atomic on same FS)
        os.replace(str(tmp_file), str(filename))
    except Exception:
        # Clean up temp file on any failure — original is untouched
        if tmp_file.exists():
            tmp_file.unlink()
        raise

    print(f'Successfully modified {filename} (backup: {backup})')
    return True


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Insert TubeLite into EmulationStation config')
    parser.add_argument('--cfg-file', dest='cfg_file', action='store', type=str, default='/etc/emulationstation/es_systems.cfg', help='cfg file to process')
    parser.add_argument('--install-dir', dest='install_dir', action='store', type=str, default='/roms/tools/tubelite', help='Install directory for the TubeLite package')
    parser.add_argument('--platform-tag', dest='platform_tag', action='store', type=str, default='tubelite', help='Platform tag to register')
    parser.add_argument('--theme-name', dest='theme_name', action='store', type=str, default='tubelite', help='Theme name to register')
    args = parser.parse_args()

    try:
        insert_system(
            args.cfg_file,
            install_dir=args.install_dir,
            platform_tag=args.platform_tag,
            theme_name=args.theme_name,
        )
    except Exception as exc:
        print(f'Failed to modify {args.cfg_file}: {exc}', file=sys.stderr)
        raise SystemExit(1)
