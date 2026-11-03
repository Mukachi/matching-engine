"""S3 of note 002, checked in the compiled code, as the repos before it check theirs.
Design: docs/design/002-book.md

Usage: check_hot_path.py <objdump> <hot_path_subject binary>

Each subject is checked with everything it reaches: every function called,
transitively, may call only memory copies, moves, sets and comparisons, the
stack protector's abort and libstdc++'s red-black tree rebalancing (pure code:
no allocation, no system call; D2 B's std::map), never through a pointer, and
contains no syscall instruction. The subjects: add, cancel part, cancel and
replace, on every combination of D1 (A, B, C) and D2.
"""
import re
import subprocess
import sys

SUBJECTS = [f"me_{op}_{c}" for c in ("aa", "ab", "ba", "bb") for op in ("add", "cancel_part", "cancel", "replace")]
# __memcpy_chk and the like: with a bounds check (_FORTIFY_SOURCE, Ubuntu's default).
MEMORY = {"memcpy@plt", "__memcpy_chk@plt", "memmove@plt", "__memmove_chk@plt", "memcmp@plt", "__stack_chk_fail@plt",
          "memset@plt", "__memset_chk@plt", "memchr@plt",
          # libstdc++'s red-black tree: rebalancing after an insert or an erase, and stepping
          "_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_@plt",
          "_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_@plt",
          "_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base@plt", "_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base@plt",
          "_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base@plt", "_ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base@plt"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


out = subprocess.run([sys.argv[1], "-d", "--no-show-raw-insn", sys.argv[2]],
                     check=True, capture_output=True, text=True).stdout
funcs, name = {}, None
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
    if m:
        name = m[1]
        funcs[name] = []
        continue
    m = re.match(r"^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$", line)
    if m and name is not None:
        funcs[name].append((m[1], m[2]))


def calls(ins):
    direct = {re.search(r"<([^>]*)>", o)[1].split("+")[0] for mnem, o in ins
              if (mnem.startswith("call") or mnem.startswith("jmp")) and "<" in o}
    indirect = [o for mnem, o in ins if mnem.startswith("call") and "<" not in o]
    return direct, indirect


for subject in SUBJECTS:
    if subject not in funcs:
        check(False, f"{subject} found")
        continue
    seen, todo, bad = set(), [subject], []
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        direct, indirect = calls(funcs[f])
        bad += [f"{f}: indirect call {o}" for o in indirect]
        bad += [f"{f}: {m}" for m, _ in funcs[f] if m in ("syscall", "sysenter", "int")]
        for t in direct:
            if t.endswith("@plt"):
                if t not in MEMORY:
                    bad.append(f"{f}: calls {t}")
            elif t in funcs and t != f:
                todo.append(t)
    size = sum(len(funcs[f]) for f in seen)
    check(not bad, f"S3 (note 002) {subject}: {len(seen)} functions, {size} instructions, calls only memory functions, the stack protector and tree rebalancing"
                   + (f": found {bad[:5]}" if bad else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
