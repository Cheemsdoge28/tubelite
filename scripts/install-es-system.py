#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import sys

SYSTEM_BLOCK = '''  <system>
    <name>tubelite</name>
    <fullname>TubeLite YouTube Client</fullname>
    <path>{install_dir}</path>
    <extension>.tbl</extension>
    <command>bash %ROM%</command>
    <platform>{platform_tag}</platform>
    <theme>{theme_name}</theme>
  </system>
'''


def insert_system(filename, install_dir, platform_tag='tubelite', theme_name='tubelite'):
    filename = Path(filename)
    with open(filename, encoding='utf-8') as fh:
        ctx = fh.read()

    # Clean up both legacy fire4arkos and existing tubelite entries
    for sys_name in ['fire4arkos', 'tubelite']:
        if re.search(r'<name>' + sys_name + r'</name>', ctx):
            print(f'{sys_name} already present in {filename} — removing old entry for update')
            ctx = re.sub(r'\s*<!-- ' + sys_name.capitalize() + r'.*?</system>', '', ctx, flags=re.DOTALL)
            ctx = re.sub(r'\s*<system>\s*<name>' + sys_name + r'</name>.*?</system>', '', ctx, flags=re.DOTALL)

    system_block = SYSTEM_BLOCK.format(
        install_dir=install_dir,
        platform_tag=platform_tag,
        theme_name=theme_name,
    )

    if not re.search(r'</systemList>', ctx):
        raise RuntimeError(f'Failed to find </systemList> in {filename}')

    backup = filename.with_name(filename.name + '.bak.tubelite')
    filename.rename(backup)

    ctx = re.sub(r'</systemList>', system_block + r'\g<0>', ctx)

    with open(filename, 'w', encoding='utf-8') as fh:
        fh.write(ctx)

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
