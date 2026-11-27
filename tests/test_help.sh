# tests/test_help.sh - help: the pages match their sources and fit 80
# columns, help is for its reader (an agent gets its tasks, by name; a
# person the front page and the index), the keys pages are the panel's
# source, and the polite names help shows resolve. Sourced by
# test_backmux.sh, which registers these with run_test. Why a file of its
# own: the 500-line rule (AGENTS.md) - new tests go in files by area.

test_help_pages_fit_and_say_it() {
  python3 - "$HOLD_BIN" <<'PY' || return 1
from pathlib import Path
import re
import subprocess
import sys

binary = sys.argv[1]
pages = Path("src/cli/help")
specs = Path("src/cli/specs.c").read_text()

import os
ENV = {k: v for k, v in os.environ.items() if not k.startswith("BACKMUX_")}
ENV["HOLD_TEST_HELP_READER"] = "person"

def help_output(*args):
    result = subprocess.run([binary, *args], capture_output=True, check=False, env=ENV)
    assert result.returncode == 0, (args, result.returncode, result.stderr)
    assert not result.stderr, (args, result.stderr)
    assert result.stdout, (args, "empty help")
    for number, line in enumerate(result.stdout.decode().splitlines(), 1):
        assert len(line) <= 80, (args, number, len(line), line)
    return result.stdout

# A page as --full prints it: tag lines and lines about the reader left out.
KEYS = r"\{!?(name|name_raw|parent|parent_raw|last_from|last_from_raw|last_at|inbox|inbox_raw|next)\}"
TAGS = re.compile(r"^\[(agent|person|inside|outside|all)( (agent|person|inside|outside|all))*\]$")
def lines(path):
    text = path.read_text()
    return text[:-1].split("\n") if text.endswith("\n") else text.split("\n")

def whole(path):
    keep = [l for l in lines(path) if not TAGS.match(l) and not re.search(KEYS, l)]
    return "".join(l + "\n" for l in keep).encode()

# A page as a person outside backmux reads it: blocks for agents left out.
def person(path):
    keep, agent, inside = [], None, None
    for l in lines(path):
        if TAGS.match(l):
            words = l[1:-1].split()
            agent = True if "agent" in words else False if "person" in words else None
            inside = True if "inside" in words else False if "outside" in words else None
            continue
        if agent is True or inside is True:
            continue
        keep.append(l)
    return "".join(l + "\n" for l in keep).encode()

overview = help_output("help")
assert len(overview.splitlines()) <= 50, "the overview should remain a short entry point"
assert overview.split(b"\n", 1)[1] == person(pages / "overview.txt")
for args in [("-h",), ("--help",), ("help", "-h"), ("help", "--help")]:
    assert help_output(*args) == overview, args

# manual is the whole text; help all is the index (never every page again)
complete = help_output("manual")
assert complete.startswith((pages / "reference/all.txt").read_bytes())
index = help_output("help", "all")
assert index == (pages / "index.txt").read_bytes(), "help all is the person's index"
assert len(index.splitlines()) <= 60, "the index stays short"
for section in ("guides", "reference", "extra", "commands", "tasks"):
    for path in sorted((pages / section).glob("*.txt")):
        if path.stem == "all" and section == "reference":
            continue
        topic = "agents" if (section, path.stem) == ("tasks", "index") else path.stem
        assert help_output("help", "--full", topic) == whole(path), (path, "--full differs from source")
        rendered = help_output("help", topic)
        if section != "tasks":
            assert rendered == person(path), (path, "rendered text differs from source")
        if section in ("reference", "commands"):
            assert whole(path) in complete, (path, "missing from the complete reference")
        if section in ("guides", "extra"):
            # the index names every guide
            assert re.search(rb"(?m)^\s+" + path.stem.encode() + rb"\s", index), (path, "undiscoverable guide")
        if section == "tasks" and path.stem != "index":
            assert re.search(rb"(?m)^\s+" + path.stem.encode() + rb"\s", help_output("help", "agents")), (path, "task not indexed")

commands = re.findall(r'^\s+\{"([a-z][a-z0-9_-]*)", (NULL|"[a-z][a-z0-9_-]*"),', specs, re.M)
assert commands, "no command specs found"
for name, alias in commands:
    canonical = name if alias == "NULL" else alias.strip('"')
    page = pages / "commands" / (canonical + ".txt")
    assert page.is_file(), (name, "missing command page")
    expected = person(page)
    assert help_output("help", name) == expected, name
    assert whole(page) in complete, (name, "missing command reference")
    if alias != "NULL":
        assert re.search(rb"(?m)^\s+" + name.encode() + rb"\s+.*\b" + canonical.encode() + rb"\b", complete), (name, "missing alias")
    if name != "help":
        for flag in ("-h", "--help"):
            assert help_output(name, flag) == expected, (name, flag)

# help keys is the keys pages in order; the Ctrl-Space ? panel reads the
# same pages, and every key there is one the prefix code handles.
order = ["about", "prefix", "modes", "pane", "window", "resize", "move", "more"]
assert help_output("help", "--full", "keys") == b"".join(whole(pages / "keys" / (n + ".txt")) for n in order)
assert help_output("help", "keys") == b"".join(person(pages / "keys" / (n + ".txt")) for n in order)
mux = Path("src/console/mux_keys.c").read_text()
handled_raw = re.findall(r"case '(\\?.)':", mux.split("int mux_command(", 1)[1].split("\n}\n", 1)[0])
unescape = {"\\r": "\r", "\\n": "\n", "\\\\": "\\"}
handled = {unescape.get(k, k) for k in handled_raw} - {"\n"}
listed = set()
for line in lines(pages / "keys" / "prefix.txt"):
    if line.startswith("  "):
        key = line.strip().split("  ")[0]
        if key in ("Alt-arrows", "Ctrl-Space", "q"):
            continue  # no prefix; the prefix itself; the detach key (both configurable)
        for k in key.split():
            listed.add("\r" if k == "Enter" else k)
assert listed == handled, ("panel and prefix code differ", sorted(listed ^ handled))

# Every command page starts, under its usage, with the line help all shows.
purpose = {}
for line in lines(pages / "index.txt"):
    for part in (line[:42], line[42:]):
        m = re.match(r"^  ([a-z]+)\s{2,}(\S.*?)\s*$", part)
        if m:
            purpose[m.group(1)] = m.group(2)
for name, alias in commands:
    if alias != "NULL":
        continue
    assert name in purpose, (name, "not in help all")
    page = person(pages / "commands" / (name + ".txt")).decode().split("\n")
    start = max(i for i, l in enumerate(page) if l.startswith("usage:"))
    end = next(i for i in range(start, len(page)) if not page[i].strip())
    assert page[end + 1] == purpose[name], (name, page[end + 1], purpose[name])

renamed = specs.split("help_renamed[][2]", 1)[1].split("};", 1)[0]
for old, new in re.findall(r'\{"([a-z]+)", "([a-z]+)"\}', renamed):
    assert help_output("help", old) == help_output("help", new), (old, new)

for command in ("peek", "send", "wait", "msg", "inbox", "monitor"):
    result = subprocess.run([binary, command, "x", "--bogus"], capture_output=True)
    assert result.returncode != 0, (command, "invalid flag accepted")
    assert all(len(line) <= 80 for line in result.stderr.decode().splitlines()), command
PY
  out_has 'from now on' "$HOLD_BIN" help tail || return 1
  out_has 'log_path' "$HOLD_BIN" help inspect || return 1
  if out_has '--plain' "$HOLD_BIN" help inspect; then echo "help inspect still names --plain" >&2; return 1; fi
  out_has 'usage error' "$HOLD_BIN" help scripting || return 1
}

# Help is for its reader. An agent inside backmux gets its own front page
# for help, help all and help agents: its name in the polite form, who
# started it, who wrote last, its tasks, and nothing it doesn't need (keys,
# the outside inbox flow). An agent outside gets the replies task instead.
# --full and manual stay whole. A harness among the parents is what makes
# the reader an agent.
test_help_for_its_reader() {
  local out id full lead
  "$HOLD_BIN" -dt --name sleek_carbon -- /bin/sh -c 'while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" -dt --name lead -- /bin/sh -c 'while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" msg sleek_carbon "hello" >/dev/null 2>&1 || return 1
  id=$("$HOLD_BIN" inspect sleek_carbon | python3 -c 'import json,sys; d=json.load(sys.stdin); d=d[0] if isinstance(d,list) else d; print(d.get("id") or d.get("Id"))') || return 1

  out=$(HOLD_TEST_HELP_READER=agent BACKMUX_ID="$id" BACKMUX_NAME=sleek_carbon BACKMUX_PARENT=lead "$HOLD_BIN" help) || return 1
  [[ $out == *"running natively inside backmux"* && $out == *"Your Agent ID: Sleek Carbon (sleek_carbon)"* && $out == *"not as an afterthought"* ]] || { echo "inside: no polite ID: $out" >&2; return 1; }
  [[ $out == *"Started by: Lead; report back: backmux msg lead"* ]] || { echo "inside: no Started by: $out" >&2; return 1; }
  [[ $out == *"Last message: "*"; reply: backmux msg "* ]] || { echo "inside: no last message: $out" >&2; return 1; }
  [[ $out == *"  assist "* && $out != *"replies"* && $out != *"Ctrl-Space"* && $out != *"{"* ]] || { echo "inside: wrong tasks: $out" >&2; return 1; }
  [ "$(printf '%s\n' "$out" | wc -l)" -le 30 ] || { echo "inside: front page too long" >&2; return 1; }
  [ "$(HOLD_TEST_HELP_READER=agent BACKMUX_ID="$id" BACKMUX_NAME=sleek_carbon BACKMUX_PARENT=lead "$HOLD_BIN" help all)" = "$out" ] || { echo "help all is not the agent's index" >&2; return 1; }
  [ "$(HOLD_TEST_HELP_READER=agent BACKMUX_ID="$id" BACKMUX_NAME=sleek_carbon BACKMUX_PARENT=lead "$HOLD_BIN" help agents)" = "$out" ] || { echo "help agents is not the agent's index" >&2; return 1; }
  lead=$("$HOLD_BIN" inspect lead | python3 -c 'import json,sys; d=json.load(sys.stdin); d=d[0] if isinstance(d,list) else d; print(d.get("id") or d.get("Id"))') || return 1
  out=$(HOLD_TEST_HELP_READER=agent BACKMUX_ID="$id" BACKMUX_NAME=sleek_carbon BACKMUX_PARENT="$lead" "$HOLD_BIN" help) || return 1
  [[ $out == *"Started by: Lead; report back: backmux msg lead"* ]] || { echo "parent by ID not shown by name: $out" >&2; return 1; }
  out=$(HOLD_TEST_HELP_READER=agent BACKMUX_ID="$id" BACKMUX_NAME=sleek_carbon "$HOLD_BIN" help reply) || return 1
  [[ $out == *"for Sleek Carbon"* && $out == *"msg --received"* && $out != *"inbox"* ]] || { echo "inside reply page: $out" >&2; return 1; }

  out=$(env -u BACKMUX_ID -u BACKMUX_NAME -u BACKMUX_PARENT HOLD_TEST_HELP_READER=agent "$HOLD_BIN" help all) || return 1
  [[ $out == *"outside backmux"* && $out == *"Your Agent ID: "* && $out == *"  replies "* && $out != *"  assist "* ]] || { echo "outside: $out" >&2; return 1; }
  out=$(env -u BACKMUX_ID -u BACKMUX_NAME HOLD_TEST_HELP_READER=agent "$HOLD_BIN" help reply) || return 1
  [[ $out == *"backmux help replies"* && $out != *"what has arrived here"* ]] || { echo "outside reply page: $out" >&2; return 1; }

  out=$(HOLD_TEST_HELP_READER=person "$HOLD_BIN" help) || return 1
  [[ $out == "backmux "* && $out == *"LAUNCH PROGRAMS"* ]] || { echo "person: not the front page" >&2; return 1; }
  full=$(HOLD_TEST_HELP_READER=agent BACKMUX_ID="$id" BACKMUX_NAME=sleek_carbon "$HOLD_BIN" help --full agents) || return 1
  [[ $full == *"As an agent running inside backmux"* && $full == *"As an agent outside backmux"* && $full == *"An agent can:"* && $full != *"{"* && $full != *"Sleek Carbon"* ]] || { echo "--full: $full" >&2; return 1; }

  # Detection: a harness among the parents (here sh, copied as claude).
  cp /bin/sh "$TEST_ROOT/claude" || return 1
  out=$(env -u HOLD_TEST_HELP_READER -u BACKMUX_ID -u BACKMUX_NAME "$TEST_ROOT/claude" -c '"$1" help; :' sh "$HOLD_REAL_BIN") || return 1
  [[ $out == *"outside backmux"* && $out == *"Your Agent ID: "* ]] || { echo "a claude parent did not make an agent reader: $out" >&2; return 1; }
}

# The polite form help shows ("Sleek Carbon") resolves to the name, in any
# case, a space for an underscore; an exact name still wins.
test_friendly_names_resolve() {
  local rc err="$TEST_ROOT/friendly.err"
  "$HOLD_BIN" -dt --name sleek_carbon -- /bin/sh -c 'while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" -dt --name Editor -- /bin/sh -c 'while :; do sleep 1; done' </dev/null >/dev/null || return 1
  "$HOLD_BIN" -dt --name editor -- /bin/sh -c 'while :; do sleep 1; done' </dev/null >/dev/null || return 1
  out_has 'name: *sleek_carbon' "$HOLD_BIN" info "Sleek Carbon" || { echo "Sleek Carbon did not resolve" >&2; return 1; }
  out_has 'name: *sleek_carbon' "$HOLD_BIN" info "sleek carbon" || { echo "sleek carbon did not resolve" >&2; return 1; }
  out_has 'name: *Editor$' "$HOLD_BIN" info Editor || { echo "the exact name lost" >&2; return 1; }
  out_has 'name: *editor$' "$HOLD_BIN" info EDITOR || { echo "EDITOR did not fall back to editor" >&2; return 1; }
  rc=0; "$HOLD_BIN" info "No Such" >/dev/null 2>"$err" || rc=$?
  [ "$rc" -eq 5 ] && grep -q "'No Such'" "$err" || { echo "not found: rc=$rc $(cat "$err")" >&2; return 1; }
  "$HOLD_BIN" msg "Sleek Carbon" polite-hello >/dev/null 2>&1 || { echo "msg to the polite name failed" >&2; return 1; }
  "$HOLD_BIN" wait sleek_carbon --match polite-hello --timeout 5000 >/dev/null || { echo "msg to the polite name not delivered" >&2; return 1; }
}

