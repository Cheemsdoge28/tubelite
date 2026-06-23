#!/usr/bin/env python3
import os
import sys
import glob
import datetime

def clean_val(val):
    val = val.strip()
    if len(val) >= 2 and ((val[0] == '"' and val[-1] == '"') or (val[0] == "'" and val[-1] == "'")):
        return val[1:-1]
    return val

def check_file(path):
    if not os.path.exists(path):
        return None
    try:
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except Exception as e:
        print(f"[Audio Compat] Warning: Could not read {path}: {e}", file=sys.stderr)
        return None
    
    # Lexical tokenizer
    tokens = []
    i = 0
    n = len(content)
    while i < n:
        c = content[i]
        if c.isspace():
            start = i
            while i < n and content[i].isspace():
                i += 1
            tokens.append(('space', content[start:i]))
        elif c == '#':
            start = i
            while i < n and content[i] not in '\n\r':
                i += 1
            tokens.append(('comment', content[start:i]))
        elif c in '"\'':
            quote = c
            start = i
            i += 1
            while i < n and content[i] != quote:
                if content[i] == '\\' and i + 1 < n:
                    i += 2
                else:
                    i += 1
            if i < n:
                i += 1
            tokens.append(('string', content[start:i]))
        elif c == '=':
            tokens.append(('equal', '='))
            i += 1
        elif c == '{':
            tokens.append(('open', '{'))
            i += 1
        elif c == '}':
            tokens.append(('close', '}'))
            i += 1
        elif c in ';,':
            tokens.append(('sep', c))
            i += 1
        else:
            start = i
            while i < n and not content[i].isspace() and content[i] not in '#"\'={};,':
                i += 1
            tokens.append(('word', content[start:i]))

    # Validate braces and strings
    brace_count = 0
    for t_type, t_text in tokens:
        if t_type == 'open':
            brace_count += 1
        elif t_type == 'close':
            brace_count -= 1
            if brace_count < 0:
                print(f"[Audio Compat] Warning: Config file {path} is malformed. Reason: mismatched braces (excess closing brace '}}').", file=sys.stderr)
                print(f"[Audio Compat] Skipping modifications for this file to prevent corruption.", file=sys.stderr)
                return 'malformed'
        elif t_type == 'string':
            if len(t_text) < 2 or t_text[0] not in '"\'' or t_text[-1] != t_text[0]:
                print(f"[Audio Compat] Warning: Config file {path} is malformed. Reason: unterminated string.", file=sys.stderr)
                print(f"[Audio Compat] Skipping modifications for this file to prevent corruption.", file=sys.stderr)
                return 'malformed'
    if brace_count != 0:
        print(f"[Audio Compat] Warning: Config file {path} is malformed. Reason: mismatched braces (unclosed open brace '{{').", file=sys.stderr)
        print(f"[Audio Compat] Skipping modifications for this file to prevent corruption.", file=sys.stderr)
        return 'malformed'

    # Parse hierarchy
    scope = []
    stmt = []
    dmix_pcms = set()
    rate_indices = {} # full_path -> token_index

    def process_stmt():
        nonlocal stmt
        if not stmt:
            return
        
        eq_idx = -1
        for idx, (t_type, _) in enumerate(stmt):
            if t_type == 'equal':
                eq_idx = idx
                break
        
        if eq_idx != -1:
            if eq_idx > 0 and eq_idx + 1 < len(stmt):
                key = ".".join([clean_val(stmt[j][1]) for j in range(eq_idx)])
                val_tok_idx = stmt[eq_idx + 1][0]
                val_text = clean_val(stmt[eq_idx + 1][1])
            else:
                stmt = []
                return
        else:
            if len(stmt) >= 2:
                key = clean_val(stmt[0][1])
                val_tok_idx = stmt[1][0]
                val_text = clean_val(stmt[1][1])
            else:
                stmt = []
                return

        fq_path = ".".join(scope + [key])

        if fq_path.startswith("pcm."):
            if fq_path.endswith(".type") and val_text == 'dmix':
                pcm_name = fq_path[:-5]
                dmix_pcms.add(pcm_name)
        if key == 'type' and val_text == 'dmix' and scope:
            if scope[0].startswith("pcm."):
                dmix_pcms.add(scope[0])

        if ".slave.rate" in fq_path:
            rate_indices[fq_path] = val_tok_idx

        stmt = []

    for i_tok, (t_type, t_text) in enumerate(tokens):
        if t_type == 'space':
            if '\n' in t_text or '\r' in t_text:
                process_stmt()
        elif t_type == 'comment':
            pass
        elif t_type == 'sep' and t_text == ';':
            process_stmt()
        elif t_type == 'open':
            block_name = ".".join([clean_val(tok[1]) for tok in stmt])
            if not block_name:
                block_name = "unknown"
            scope.append(block_name)
            stmt = []
        elif t_type == 'close':
            process_stmt()
            if scope:
                scope.pop()
        else:
            stmt.append((i_tok, t_text))
    process_stmt()

    return tokens, dmix_pcms, rate_indices

def prompt_user(message, pcm_name, path):
    import select
    import struct
    import time

    options = [
        "Yes, update rate to 48000 Hz (Recommended)",
        "No, keep current rate"
    ]
    
    selected = 0
    js = None
    
    # Check if joystick exists
    has_joystick = os.path.exists('/dev/input/js0')
    if has_joystick:
        try:
            js_fd = os.open('/dev/input/js0', os.O_RDONLY | os.O_NONBLOCK)
            js = os.fdopen(js_fd, 'rb')
        except Exception as e:
            sys.stderr.write(f"[Audio Compat] Warning: Could not open /dev/input/js0: {e}\n")

    def print_menu():
        # Clear screen/lines if possible, or just print clearly
        if js or os.environ.get('TUBELITE_FROM_ES') == '1':
            sys.stdout.write("\033[H\033[J")
            sys.stdout.write("\033[1m=== TubeLite Audio Compatibility Setup ===\033[0m\n\n")
            sys.stdout.write(f"File: {path}\n")
            sys.stdout.write(f"Configuration: {pcm_name}\n\n")
            sys.stdout.write("TubeLite, MPV, RetroArch, and YouTube audio work best at 48000 Hz.\n")
            sys.stdout.write("Would you like to update the dmix mixer rate to 48000 Hz?\n\n")
            sys.stdout.write("Use DPAD to move, A to select.\n\n")
        else:
            sys.stdout.write("\n")
            sys.stdout.write(f"[Audio Compat] dmix configuration ({pcm_name}) in {path} is 44100 Hz.\n")
            sys.stdout.write("TubeLite, MPV, RetroArch, and YouTube audio work best at 48000 Hz.\n")
            sys.stdout.write("Select an option (Use keys 1-2 to select, or Enter to confirm):\n")
            
        for i, opt in enumerate(options):
            if i == selected:
                sys.stdout.write(f" \033[1;32m-> [{opt}]\033[0m\n")
            else:
                sys.stdout.write(f"    {opt}\n")
        sys.stdout.flush()

    print_menu()

    stdin_source = sys.stdin
    tty_file = None
    if not sys.stdin.isatty():
        try:
            tty_file = open('/dev/tty', 'r')
            stdin_source = tty_file
        except Exception:
            pass

    try:
        while True:
            inputs = [stdin_source]
            if js:
                inputs.append(js)
            
            r, _, _ = select.select(inputs, [], [], 0.1)
            
            if js in r:
                try:
                    data = js.read(8)
                    if data and len(data) == 8:
                        t, val, type, num = struct.unpack('IhBB', data)
                        if type == 1 and val == 1: # Button Down
                            if num == 1: # A button (Select)
                                return selected == 0
                            if num == 0: # B button (Back/Exit/Cancel)
                                return False
                            if num == 8: # UP
                                selected = (selected - 1) % len(options)
                                print_menu()
                            if num == 9: # DOWN
                                selected = (selected + 1) % len(options)
                                print_menu()
                except Exception as e:
                    sys.stderr.write(f"[Audio Compat] Error reading joystick: {e}\n")
            
            if stdin_source in r:
                char = stdin_source.read(1)
                if char.isdigit():
                    val = int(char)
                    if 1 <= val <= len(options):
                        return val == 1
                elif char == '\n':
                    return selected == 0
                elif char.lower() == 'y':
                    return True
                elif char.lower() == 'n':
                    return False
            
            time.sleep(0.01)
    finally:
        if js:
            try:
                js.close()
            except Exception:
                pass
        if tty_file:
            try:
                tty_file.close()
            except Exception:
                pass

def backup_file(path):
    try:
        timestamp = datetime.datetime.now().strftime("%Y%m%d%H%M%S")
        backup_path = f"{path}.{timestamp}.bak"
        import shutil
        shutil.copy2(path, backup_path)
        return backup_path
    except Exception as e:
        print(f"[Audio Compat] Error: Failed to backup {path}: {e}", file=sys.stderr)
        return None

def main():
    paths = []
    # Check /etc/asound.conf
    paths.append("/etc/asound.conf")
    # Check root asoundrc
    paths.append("/root/.asoundrc")
    # Check user home asoundrcs
    for d in glob.glob("/home/*"):
        if os.path.isdir(d):
            paths.append(os.path.join(d, ".asoundrc"))

    # Filter unique existing files
    config_files = []
    seen = set()
    for p in paths:
        real_p = os.path.realpath(p)
        if os.path.isfile(p) and real_p not in seen:
            config_files.append(p)
            seen.add(real_p)

    if not config_files:
        print("[Audio Compat] Neither ~/.asoundrc nor /etc/asound.conf exists. No audio configuration to check.")
        return 0

    either_file_exists = True
    any_dmix_found = False

    for path in config_files:
        res = check_file(path)
        if res is None:
            continue
        if res == 'malformed':
            # Skip malformed files safely
            continue

        tokens, dmix_pcms, rate_indices = res
        if not dmix_pcms:
            print(f"[Audio Compat] No dmix configuration found in {path}")
            continue

        any_dmix_found = True
        modified = False
        backup_path = None

        for pcm_name in sorted(dmix_pcms):
            rate_key = f"{pcm_name}.slave.rate"
            if rate_key in rate_indices:
                idx = rate_indices[rate_key]
                old_rate_text = tokens[idx][1]
                old_rate_val = clean_val(old_rate_text)
                
                if old_rate_val == '48000':
                    print(f"[Audio Compat] dmix configuration ({pcm_name}) in {path} is already set to 48000 Hz. No changes needed.")
                elif old_rate_val == '44100':
                    prompt_msg = f"A dmix configuration was detected using 44100 Hz in {path} ({pcm_name})."
                    if prompt_user(prompt_msg, pcm_name, path):
                        if not backup_path:
                            backup_path = backup_file(path)
                        if backup_path:
                            quote = ''
                            if old_rate_text.startswith('"'):
                                quote = '"'
                            elif old_rate_text.startswith("'"):
                                quote = "'"
                            new_rate_text = f"{quote}48000{quote}"
                            
                            tokens[idx] = (tokens[idx][0], new_rate_text)
                            print(f"[Audio Compat] Updating dmix rate for {pcm_name}:")
                            print(f"  Old rate: 44100 Hz")
                            print(f"  New rate: 48000 Hz")
                            modified = True
                    else:
                        print(f"[Audio Compat] User declined update for {pcm_name}. No changes made.")
                else:
                    print(f"[Audio Compat] dmix configuration ({pcm_name}) in {path} has custom rate: {old_rate_val} Hz. No changes will be made.")
            else:
                print(f"[Audio Compat] dmix configuration ({pcm_name}) in {path} does not define an explicit slave rate. No changes needed.")

        if modified and backup_path:
            new_content = "".join([t[1] for t in tokens])
            try:
                with open(path, 'w', encoding='utf-8') as f:
                    f.write(new_content)
                print(f"[Audio Compat] Successfully wrote changes to {path}. Backup created at: {backup_path}\n")
            except Exception as e:
                print(f"[Audio Compat] Error: Failed to write to {path}: {e}", file=sys.stderr)

    if not any_dmix_found:
        print("[Audio Compat] No dmix configuration found in any ALSA config files.")

    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[Audio Compat] Installation step interrupted by user.")
        sys.exit(1)
