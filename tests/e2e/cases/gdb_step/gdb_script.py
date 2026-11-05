# gdb script for the gdb_step case: prints GDB-* lines the harness checks.
import gdb


def out(line):
    gdb.write(line + "\n")
    gdb.flush()


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set breakpoint pending on")
gdb.execute("set print thread-events off")

gdb.Breakpoint("e2e_patched")
gdb.execute("run")

# The patch is live. Break at a line that only the new code has.
bp = gdb.Breakpoint("game.cpp:5")
gdb.execute("continue")

frame = gdb.selected_frame()
sal = frame.find_sal()
fn = frame.function()
objfile = fn.symtab.objfile.filename if fn and fn.symtab else "?"
out("GDB-STOP %s %s:%d objfile=%s" % (frame.name(), sal.symtab.filename if sal.symtab else "?", sal.line, objfile))

counter = int(gdb.parse_and_eval("counter"))  # kept from the old program: must read its real storage
try:
    bonus = int(gdb.parse_and_eval("bonus"))
except gdb.error:  # optimized out in -O2 builds
    bonus = None
if bonus is None:
    out("GDB-VARS bonus=<optimized out> counter=%d %s" % (counter, "ok" if counter > 0 else "MISMATCH"))
else:
    out("GDB-VARS bonus=%d counter=%d %s" % (bonus, counter, "ok" if bonus == 2 * counter and counter > 0 else "MISMATCH"))

bt = gdb.execute("bt", to_string=True)
ok = "value" in bt and "case_frame" in bt and "main" in bt
out("GDB-BT %s %s" % ("ok" if ok else "BAD", bt.strip().replace("\n", " | ")))

gdb.execute("finish", to_string=True)  # back into old code
out("GDB-FINISH %s" % gdb.selected_frame().name())

bp.delete()
out("GDB-DONE")
gdb.execute("continue")  # until the harness tells the program to stop
