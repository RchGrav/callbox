# tests/test_screen.sh - the compositor's screen: the grid of windows and the
# view over it, new terminals and splits, backmux shell's own screen, chat
# for agents, and the bar's launcher counters. Sourced by test_backmux.sh,
# which registers these with run_test; its helpers (start_term, pick_pane,
# mux_slots, wait_gone, screen_pos, put_chat_away) are used here. Why a file
# of its own: the 500-line rule (AGENTS.md) - new tests go in files by area.

# A stand-in for an agent harness: a script under the name given (codex is
# one backmux knows), that prints each line it is sent.
fake_harness() {
  mkdir -p "$TEST_ROOT/bin" || return 1
  printf '#!/bin/sh\nstty -echo\necho %s-UP\nwhile IFS= read -r l; do echo "typed:$l"; done\n' "$1" >"$TEST_ROOT/bin/$1" || return 1
  chmod 755 "$TEST_ROOT/bin/$1"
}

mux_viewport() {
  local f
  for f in "$HOME"/.local/state/backmux/chat/mux.*; do
    [ -f "$f" ] || continue
    case "${f##*/}" in mux.*.*) ;; mux.*) sed -n 's/^viewport //p' "$f" ;; esac
  done
}

# The windows are a 3x3 grid and the view moves over it in half-window
# steps: Alt-Right past the right edge slides half a window, so the right
# column of window 1 and the left column of window 2 share the screen. o and
# O step through every pane in order, the view following. Ctrl-Space Enter
# opens your shell as the next pane; | and - split the active pane, and the
# window takes the shape that names it (A, then down: AE; E, then right:
# ACD - A across the top, C and D below).
test_grid_new_terminals_and_splits() {
  local i names
  for i in 1 2 3 4 5; do "$HOLD_BIN" detach --name "g$i" -- sh -c "echo G$i-UP; exec cat" "mark-g$i" >/dev/null || return 1; done
  start_term term g1 120 30 || return 1
  for i in 2 3 4 5; do pick_pane term "mark-g$i" || return 1; done
  [ "$(mux_slots)" = "1A:g1 1B:g2 1C:g3 1D:g4 2A:g5 " ] || { echo "slots: $(mux_slots)" >&2; return 1; }
  "$HOLD_BIN" send term -k C-Space -k t -k 1 || return 1
  for i in $(seq 1 30); do [ "$(mux_viewport)" = "0 0" ] && break; sleep 0.1; done
  # g1, Alt-Right to g2, then past the edge: half a window along, g5 fitted
  # to the column in view
  "$HOLD_BIN" send term -k M-Right || return 1
  sleep 0.3
  "$HOLD_BIN" send term -k M-Right || return 1
  for i in $(seq 1 30); do [ "$(mux_viewport)" = "1 0" ] && break; sleep 0.1; done
  [ "$(mux_viewport)" = "1 0" ] || { echo "Alt-Right did not slide half a window: $(mux_viewport)" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --regex '┤ g2 ├.*┤ g5 ├' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term to-g5 --enter || return 1
  "$HOLD_BIN" wait g5 --match 'to-g5' --timeout 5000 >/dev/null || { echo "the pane that came in is not the active one" >&2; return 1; }
  # O: the pane before (g4); o: back to g5, the view following
  "$HOLD_BIN" send term -k C-Space -k O || return 1
  sleep 0.3
  "$HOLD_BIN" send term to-g4 --enter || return 1
  "$HOLD_BIN" wait g4 --match 'to-g4' --timeout 5000 >/dev/null || { echo "O did not go to the pane before" >&2; return 1; }
  # Enter: a new terminal (your shell) at the next place: window 1 is full,
  # so window 2, where g5 grows from A to AB
  "$HOLD_BIN" send term -k C-Space -k Enter || return 1
  for i in $(seq 1 50); do mux_slots | grep -q ' 2B:' && break; sleep 0.1; done
  mux_slots | grep -q ' 2B:' || { echo "no new terminal at 2B: $(mux_slots)" >&2; return 1; }
  names=$(mux_slots | tr ' ' '\n' | sed -n 's/^2B://p')
  "$HOLD_BIN" end "$names" g1 g2 g3 g4 g5 term >/dev/null 2>&1 || true
}

# Splits: the active pane is halved and the window takes the shape that
# names it - A alone, down: AE (E across the bottom); E, right: ACD (A
# across the top, C and D below). A split with no room says so.
test_splits_make_the_shape_named() {
  local i names
  "$HOLD_BIN" detach --name solo -- sh -c 'echo SOLO-UP; exec cat' >/dev/null || return 1
  start_term term solo 120 30 || return 1
  "$HOLD_BIN" send term -k C-Space -k - || return 1
  for i in $(seq 1 50); do mux_slots | grep -q '1E:' && break; sleep 0.1; done
  mux_slots | grep -Eq '^1A:solo 1E:[a-z0-9_-]+ $' || { echo "split down from A did not make AE: $(mux_slots)" >&2; return 1; }
  "$HOLD_BIN" send term -k C-Space -k '|' || return 1
  for i in $(seq 1 50); do mux_slots | grep -q '1D:' && break; sleep 0.1; done
  mux_slots | grep -Eq '^1A:solo 1C:[a-z0-9_-]+ 1D:[a-z0-9_-]+ $' || { echo "E split right did not make ACD: $(mux_slots)" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  # D (bottom right) split down: no shape has room
  "$HOLD_BIN" send term -k C-Space -k - || return 1
  "$HOLD_BIN" wait term --match 'window full' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  names=$(mux_slots | tr ' ' '\n' | sed -n 's/^1[CD]://p' | tr '\n' ' ')
  # shellcheck disable=SC2086
  "$HOLD_BIN" end solo term $names >/dev/null 2>&1 || true
}

# backmux shell is the person's own screen: it starts on a cleared screen
# (the alternate one: their lines come back when they leave), with chat up
# but the keyboard on the shell, the shell's name bar, and the bar along the
# bottom - counters, the person's name, Info. The prefix then f hides the
# bar; Esc puts chat away (keyboard only); the prefix then c and the name on
# the bar bring it back; the prefix then ? shows the keys in a framed box.
test_shell_is_the_persons_screen() {
  local c r
  "$HOLD_BIN" detach --size 100 28 --name sterm -- /bin/sh >/dev/null || return 1
  "$HOLD_BIN" send sterm 'echo BEFORE-LINE' -k Enter || return 1
  "$HOLD_BIN" wait sterm --regex '^BEFORE-LINE$' --timeout 5000 >/dev/null || return 1
  "$HOLD_BIN" send sterm "\"$HOLD_REAL_BIN\" shell --name sh1 /bin/sh" -k Enter || return 1
  wait_call sh1 || return 1
  "$HOLD_BIN" wait sterm --match 'chat ->' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek sterm >&2; return 1; }
  if "$HOLD_BIN" peek sterm | grep -q 'BEFORE-LINE'; then echo "the shell did not start on a clear screen" >&2; "$HOLD_BIN" peek sterm >&2; return 1; fi
  out_has '"alt_screen": true' "$HOLD_BIN" peek sterm --json || { echo "the shell is not on the alternate screen" >&2; return 1; }
  out_has '^─ 1A ─.*┤ sh1 ├' "$HOLD_BIN" peek sterm || { echo "no name bar" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  out_has '^ \[[0-9]*\] \[[0-9]*\] \[[0-9]*\] .* Info ' "$HOLD_BIN" peek sterm || { echo "no bar" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  # the keyboard is the shell's
  "$HOLD_BIN" send sterm 'echo TYPED-IN-SHELL' -k Enter || return 1
  "$HOLD_BIN" wait sterm --match 'TYPED-IN-SHELL' --timeout 5000 >/dev/null || { echo "keys did not reach the shell" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  # Esc puts chat away once it has the keyboard; the name on the bar brings it back
  put_chat_away sterm || return 1
  read -r c r < <(screen_pos sterm "$(chat_name_on_bar sterm)") || { echo "no name on the bar" >&2; return 1; }
  "$HOLD_BIN" send sterm --mouse "click,$((c + 1)),$r" || return 1
  "$HOLD_BIN" wait sterm --match 'chat ->' --timeout 5000 >/dev/null || { echo "the name on the bar did not open chat" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  "$HOLD_BIN" send sterm --mouse "click,$((c + 1)),$r" || return 1
  wait_gone sterm 'chat ->' || { echo "the name on the bar did not put chat away" >&2; return 1; }
  # the bar hides and comes back
  "$HOLD_BIN" send sterm -k C-Space -k f || return 1
  wait_gone sterm ' Info ' || { echo "f did not hide the bar" >&2; return 1; }
  "$HOLD_BIN" send sterm -k C-Space -k f || return 1
  "$HOLD_BIN" wait sterm --match ' Info ' --timeout 5000 >/dev/null || return 1
  # the keys, framed
  "$HOLD_BIN" send sterm -k C-Space --text '?' || return 1
  "$HOLD_BIN" wait sterm --match 'Keys after Ctrl-Space' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek sterm >&2; return 1; }
  out_has '╭' "$HOLD_BIN" peek sterm || { echo "the keys are not framed" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  "$HOLD_BIN" send sterm -k Escape || return 1
  wait_gone sterm 'Keys after' || return 1
  # leaving gives the person's screen back
  "$HOLD_BIN" send sterm -k C-Space -k q || return 1
  "$HOLD_BIN" wait sterm --regex '^BEFORE-LINE$' --timeout 5000 >/dev/null || { echo "the person's screen did not come back" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  "$HOLD_BIN" end sh1 sterm >/dev/null 2>&1 || true
}

# The person's chat name, as the bar shows it (its first button after the counters).
chat_name_on_bar() {
  "$HOLD_BIN" peek "$1" | tail -1 | sed -n 's/^ \[[0-9]*\] \[[0-9]*\] \[[0-9]*\]  \([a-z0-9_-]*\) .*/\1/p'
}

# Chat lists only agent harnesses: one backmux knows by name, and one the
# person named with --agent - remembered in the config, so it is an agent
# from then on. A plain program is not listed. With Assist ticked, the
# message asks the agent to look at the app on screen with backmux peek and
# to answer with backmux msg.
test_chat_agents_only_and_assist() {
  local me
  fake_harness gemini || return 1
  fake_harness mybot || return 1
  "$HOLD_BIN" detach --name g1 -- "$TEST_ROOT/bin/gemini" >/dev/null || return 1
  "$HOLD_BIN" --agent -dt --name b1 -- "$TEST_ROOT/bin/mybot" >"$TEST_ROOT/agent.out" 2>"$TEST_ROOT/agent.err" || return 1
  grep -q 'mybot is an agent from now on' "$TEST_ROOT/agent.err" || { cat "$TEST_ROOT/agent.err" >&2; return 1; }
  out_has '^mybot$' "$HOLD_BIN" config get agents || { echo "--agent did not remember mybot" >&2; return 1; }
  "$HOLD_BIN" detach --name plainapp -- sh -c 'echo PLAIN-UP; exec cat' >/dev/null || return 1
  start_term term plainapp 110 30 || return 1
  "$HOLD_BIN" send term -k C-Space -k c || return 1
  "$HOLD_BIN" wait term --match 'Select recipient' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --regex 'b1 +mybot' --timeout 5000 >/dev/null || { echo "the --agent harness is not listed" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  out_has 'g1  gemini' "$HOLD_BIN" peek term || { echo "a known harness is not listed" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  if "$HOLD_BIN" peek term | grep -q 'plainapp\|  term '; then echo "a program that is no agent is listed" >&2; "$HOLD_BIN" peek term >&2; return 1; fi
  me=$("$HOLD_BIN" peek term | grep -o '[a-z0-9_-]* chat -> ' | head -1 | cut -d' ' -f1)
  "$HOLD_BIN" send term --text g1 -k Enter || return 1
  "$HOLD_BIN" wait term --match 'chat -> g1 · gemini' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  # Assist: Ctrl-S ticks the box; the text goes first, then the request
  "$HOLD_BIN" send term -k C-s || return 1
  "$HOLD_BIN" wait term --match '[x] Assist' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term 'the menu is wrong' --enter || return 1
  "$HOLD_BIN" wait g1 --regex "typed:\\[msg from $me; reply: backmux msg $me \"\\.\\.\\.\"\\] the menu is wrong +The user is seeking assistance" --timeout 5000 >/dev/null ||
    { echo "the Assist request did not arrive" >&2; "$HOLD_BIN" peek g1 >&2; return 1; }
  out_has 'backmux peek plainapp --image /tmp/backmux-plainapp.png --cursor-ring' "$HOLD_BIN" logs g1 || { echo "Assist did not name the app on screen" >&2; "$HOLD_BIN" logs g1 >&2; return 1; }
  out_has "backmux msg $me \"Your response\"" "$HOLD_BIN" logs g1 || { echo "Assist did not say how to answer" >&2; return 1; }
  out_has '\[Assist request included\]' "$HOLD_BIN" peek term || { "$HOLD_BIN" peek term >&2; return 1; }
  if "$HOLD_BIN" peek term | grep -q '\[x\] Assist'; then echo "the box stayed ticked" >&2; return 1; fi
  # an agent that ends leaves chat: its conversation closes to the list, and
  # it is not listed (nor once deleted)
  "$HOLD_BIN" end g1 >/dev/null 2>&1 || return 1
  "$HOLD_BIN" wait term --match 'Select recipient' --timeout 8000 >/dev/null || { echo "the ended agent's conversation stayed open" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  wait_gone term 'g1  gemini' || { echo "an ended agent is still listed" >&2; return 1; }
  out_has 'b1  mybot' "$HOLD_BIN" peek term || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" delete g1 >/dev/null 2>&1 || return 1
  sleep 1.2
  if "$HOLD_BIN" peek term | grep -q 'g1'; then echo "a deleted agent shows in chat" >&2; "$HOLD_BIN" peek term >&2; return 1; fi
  "$HOLD_BIN" send term -k Escape || return 1
  wait_gone term 'chat ->' || return 1
  "$HOLD_BIN" end b1 plainapp term >/dev/null 2>&1 || true
}

# The bar's counters open the launcher on their list: green the running
# terminals (attach one as a pane), blue the consoles that scroll (a server's
# log opens in a viewer pane named for it, which leaves no record when
# closed), red the ended processes (one is started again, as a pane).
test_bar_counters_launch() {
  local c r line
  "$HOLD_BIN" detach --name base -- sh -c 'echo BASE-UP; exec cat' >/dev/null || return 1
  "$HOLD_BIN" server -w --name srv -- sh -c 'i=0; while :; do i=$((i+1)); echo "SRV-$i"; sleep 1; done' >/dev/null || return 1
  "$HOLD_BIN" -dt --name gone1 -- sh -c 'echo GONE1-RAN; exec cat' mark-gone1 >/dev/null || return 1
  "$HOLD_BIN" end gone1 >/dev/null 2>&1 || return 1
  start_term term base 120 30 || return 1
  # a split puts the bar up
  "$HOLD_BIN" detach --name side -- sh -c 'echo SIDE-UP; exec cat' mark-side >/dev/null || return 1
  pick_pane term mark-side || return 1
  "$HOLD_BIN" wait term --match ' Info ' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  line=$("$HOLD_BIN" peek term | tail -1)
  # blue: the second counter
  c=$(printf '%s\n' "$line" | python3 -c 'import sys; l=sys.stdin.read(); i=l.index("]"); print(l.index("[", i))')
  "$HOLD_BIN" send term --mouse "click,$((c + 1)),29" || return 1
  "$HOLD_BIN" wait term --regex 'Consoles [0-9]' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --regex 'srv .* log' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k Enter || return 1
  "$HOLD_BIN" wait term --match '┤ srv-log ├' --timeout 8000 >/dev/null || { echo "the log did not open in a pane" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" wait term --regex 'SRV-[0-9]' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  # red: the third counter - the ended process starts again, as a pane
  line=$("$HOLD_BIN" peek term | tail -1)
  c=$(printf '%s\n' "$line" | python3 -c 'import sys; l=sys.stdin.read(); i=l.index("]"); j=l.index("]", i + 1); print(l.index("[", j))')
  "$HOLD_BIN" send term --mouse "click,$((c + 1)),29" || return 1
  "$HOLD_BIN" wait term --regex 'Ended [0-9]' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  for r in $(seq 1 10); do "$HOLD_BIN" peek term | grep -q 'Arguments .*mark-gone1' && break; "$HOLD_BIN" send term -k Down; sleep 0.1; done
  "$HOLD_BIN" send term -k Enter || return 1
  "$HOLD_BIN" wait term --match '┤ gone1 ├' --timeout 8000 >/dev/null || { echo "the ended process did not come back as a pane" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  out_has 'Up .* gone1$' "$HOLD_BIN" list || { echo "gone1 is not running again" >&2; return 1; }
  # Tab steps through the lists from the keyboard
  "$HOLD_BIN" send term -k C-Space -k l || return 1
  "$HOLD_BIN" wait term --regex 'Terminals [0-9]' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k Tab || return 1
  "$HOLD_BIN" wait term --regex 'srv .* log' --timeout 5000 >/dev/null || { echo "Tab did not go to the next list" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" send term -k Escape || return 1
  # the viewer leaves no record once it is closed
  "$HOLD_BIN" end srv-log >/dev/null 2>&1 || true
  for r in $(seq 1 30); do out_has ' srv-log$' "$HOLD_BIN" list all -a || break; sleep 0.1; done
  if out_has ' srv-log$' "$HOLD_BIN" list all -a; then echo "the viewer left a record" >&2; return 1; fi
  "$HOLD_BIN" end base side srv gone1 term >/dev/null 2>&1 || true
}

# The slide: with slide on, the view moving to the next window passes
# through frames between the two (X1 part of the way across), then settles
# where it goes. The test build's slide is slowed so a frame between is seen.
test_slide_shows_frames_between() {
  local i try seen=0 col
  "$HOLD_BIN" config set slide on >/dev/null || return 1
  export HOLD_TEST_SLIDE_MS=1200 # long enough to be seen on a busy machine (test builds only)
  "$HOLD_BIN" detach --name l1 -- sh -c 'echo L1-UP; exec cat' >/dev/null || return 1
  start_term term l1 100 20 || { "$HOLD_BIN" logs term 2>&1 | tail -c 600 | cat -v >&2; return 1; }
  "$HOLD_BIN" detach --name x1 -- sh -c 'echo X1-UP; exec cat' mark-x1 >/dev/null || return 1
  pick_pane term mark-x1 || return 1
  "$HOLD_BIN" swap x1 2A >/dev/null 2>&1 || return 1
  "$HOLD_BIN" send term -k C-Space -k t -k 1 || return 1
  for i in $(seq 1 30); do [ "$(mux_viewport)" = "0 0" ] && break; sleep 0.1; done
  for try in 1 2 3; do
    "$HOLD_BIN" send term -k C-Space -k t -k 2 || return 1
    for i in $(seq 1 12); do
      col=$(screen_pos term 'X1-UP' 2>/dev/null | cut -d' ' -f1 || true)
      if [ -n "$col" ] && [ "$col" -gt 0 ]; then seen=1; break; fi
      sleep 0.015
    done
    for i in $(seq 1 30); do [ "$(mux_viewport)" = "2 0" ] && break; sleep 0.1; done
    [ "$seen" -eq 1 ] && break
    "$HOLD_BIN" send term -k C-Space -k t -k 1 || return 1
    sleep 0.4
  done
  [ "$seen" -eq 1 ] || { echo "no frame between the two windows was seen" >&2; return 1; }
  [ "$(mux_viewport)" = "2 0" ] || { echo "the slide did not settle on window 2: $(mux_viewport)" >&2; return 1; }
  "$HOLD_BIN" wait term --regex '^X1-UP' --timeout 3000 >/dev/null || { echo "X1 is not at the left once settled" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" end l1 x1 term >/dev/null 2>&1 || true
}

# Layouts: save keeps the base by name; after panes move, load puts them
# back. Loading never ends anything: a pane not in the layout is let go and
# runs on, and an ended member is started again. The bar shows the layout
# with * once panes moved. backmux shell opens with the default layout, its
# own shell joining it.
test_layouts_save_load_default() {
  local i f
  for i in 1 2 3; do "$HOLD_BIN" detach --name "y$i" -- sh -c "echo Y$i-UP; exec cat" "mark-y$i" >/dev/null || return 1; done
  start_term term y1 120 30 || return 1
  pick_pane term mark-y2 || return 1
  pick_pane term mark-y3 || return 1
  "$HOLD_BIN" layout save work 2>/dev/null || return 1
  f="$HOME/.config/backmux/layouts/work"
  grep -Eq '^pane 1A [0-9a-f]{64} y1$' "$f" || { echo "the layout does not hold y1 at 1A" >&2; cat "$f" >&2; return 1; }
  out_has '^work' "$HOLD_BIN" layout list || return 1
  "$HOLD_BIN" wait term --match ' work ' --timeout 5000 >/dev/null || { echo "the bar does not name the layout" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  "$HOLD_BIN" swap y1 y3 2>/dev/null || return 1
  "$HOLD_BIN" wait term --match ' work* ' --timeout 5000 >/dev/null || { echo "no * after panes moved" >&2; "$HOLD_BIN" peek term >&2; return 1; }
  # a pane not in the layout, and a member that ends
  "$HOLD_BIN" detach --name extra -- sh -c 'echo EXTRA-UP; exec cat' mark-extra >/dev/null || return 1
  pick_pane term mark-extra || return 1
  "$HOLD_BIN" end y2 >/dev/null 2>&1 || return 1
  for i in $(seq 1 30); do mux_slots | grep -q 'y2' || break; sleep 0.1; done
  "$HOLD_BIN" layout load work 2>/dev/null || { echo "load failed" >&2; return 1; }
  for i in $(seq 1 50); do [ "$(mux_slots)" = "1A:y1 1B:y2 1C:y3 " ] && break; sleep 0.1; done
  [ "$(mux_slots)" = "1A:y1 1B:y2 1C:y3 " ] || { echo "load did not put the panes back: $(mux_slots)" >&2; return 1; }
  out_has 'Up .* extra$' "$HOLD_BIN" list || { echo "the pane not in the layout stopped" >&2; return 1; }
  out_has 'Up .* y2$' "$HOLD_BIN" list || { echo "the ended member was not started again" >&2; return 1; }
  # the default, and backmux shell opening with it
  "$HOLD_BIN" layout default work 2>/dev/null || return 1
  [ "$("$HOLD_BIN" layout default)" = work ] || return 1
  "$HOLD_BIN" end term >/dev/null 2>&1 || true
  for i in $(seq 1 30); do [ -z "$(mux_slots)" ] && break; sleep 0.1; done
  "$HOLD_BIN" -dt --size 120 30 --name sterm -- "$HOLD_REAL_BIN" shell --name myshell /bin/sh </dev/null >/dev/null || return 1
  for i in $(seq 1 50); do mux_slots | grep -q 'myshell' && break; sleep 0.1; done
  mux_slots | grep -Eq '^1A:y1 1B:y2 1C:y3 1D:myshell $' || { echo "the shell did not open with its layout: $(mux_slots)" >&2; "$HOLD_BIN" peek sterm >&2; return 1; }
  # the panel
  "$HOLD_BIN" send sterm -k C-Space -k L || return 1
  "$HOLD_BIN" wait sterm --regex 'work +loaded default' --timeout 5000 >/dev/null || { "$HOLD_BIN" peek sterm >&2; return 1; }
  "$HOLD_BIN" send sterm -k Escape || return 1
  "$HOLD_BIN" layout rm work 2>/dev/null || return 1
  [ ! -e "$f" ] || return 1
  "$HOLD_BIN" end y1 y2 y3 extra myshell sterm >/dev/null 2>&1 || true
}

# A place is a target for every verb: 1B is the process at 1B on the
# terminal showing panes. A place that is also a process's name is refused,
# not guessed; with no terminal showing panes, the word is just a word.
test_places_name_panes_for_every_verb() {
  local rc
  "$HOLD_BIN" detach --name pa -- sh -c 'echo PA-UP; exec cat' >/dev/null || return 1
  "$HOLD_BIN" detach --name pb -- sh -c 'echo PB-UP; exec cat' mark-pb >/dev/null || return 1
  start_term term pa 120 30 || return 1
  pick_pane term mark-pb || return 1
  out_has 'PB-UP' "$HOLD_BIN" peek 1B || { echo "peek 1B did not reach pb" >&2; return 1; }
  BACKMUX_ID= BACKMUX_NAME= "$HOLD_BIN" msg 1b 'to the place' >/dev/null 2>&1 || { echo "msg 1b failed" >&2; return 1; }
  "$HOLD_BIN" wait pb --match 'to the place' --timeout 5000 >/dev/null || { echo "msg 1b did not reach pb" >&2; return 1; }
  # a process named like a place: refused
  "$HOLD_BIN" detach --name 1a -- sh -c 'exec cat' >/dev/null || return 1
  rc=0; "$HOLD_BIN" info 1a >/dev/null 2>"$TEST_ROOT/amb.err" || rc=$?
  [ "$rc" -eq 6 ] && grep -q "both place 1A (pa)" "$TEST_ROOT/amb.err" || { echo "a place and a name alike were not refused (rc=$rc)" >&2; cat "$TEST_ROOT/amb.err" >&2; return 1; }
  # no terminal showing panes: 1a is the process named 1a
  "$HOLD_BIN" end term >/dev/null 2>&1 || true
  for rc in $(seq 1 30); do [ -z "$(mux_slots)" ] && break; sleep 0.1; done
  out_has '^name: *1a$' "$HOLD_BIN" info 1a || { echo "with no panes shown, 1a is not the process 1a" >&2; "$HOLD_BIN" info 1a >&2; return 1; }
  "$HOLD_BIN" end pa pb 1a >/dev/null 2>&1 || true
}
