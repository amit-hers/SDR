import re, sys
SC="/tmp/claude-1000/-home-amither-Documents-SDR/8c142fcc-7ead-4043-9775-e33be2b9485a/scratchpad"
rev = sys.argv[1] if len(sys.argv)>1 else "3_0"
src = open(SC+"/xsa/ps7_init.tcl").read()
def body(name):
    m = re.search(r"^proc\s+%s\s*\{\}\s*\{(.*?)^\}" % re.escape(name), src, re.S|re.M)
    return m.group(1) if m else ""

# Mirror Xilinx's XSDB primitives as openocd procs, so the init lines can be
# emitted verbatim instead of being translated one command at a time.
pre = '''# Generated from ps7_init.tcl (silicon rev %s): DDR/clock/MIO bring-up over JTAG.
proc rd32 {addr} { return [lindex [read_memory $addr 32 1] 0] }
proc mwr {args} {
    if {[lindex $args 0] eq "-force"} { set args [lrange $args 1 end] }
    mww [lindex $args 0] [lindex $args 1]
}
proc mask_write {addr mask val} {
    set cur [rd32 $addr]
    set new [expr {($cur & ~$mask) | ($val & $mask)}]
    mww $addr $new
}
proc mask_poll {addr mask} {
    for {set i 0} {$i < 100000} {incr i} {
        if {([rd32 $addr] & $mask) != 0} { return }
    }
    echo "  ps7 poll TIMEOUT addr=$addr mask=$mask"
}
proc mask_delay {addr us} { sleep [expr {$us/1000 + 1}] }
''' % rev

out=[pre]
n=0
for stage in ["mio","pll","clock","ddr","peripherals"]:
    name="ps7_%s_init_data_%s"%(stage,rev)
    b=body(name)
    if not b: print("    WARNING: %s missing"%name); continue
    out.append("echo {  ps7: %s}"%stage)
    for line in b.splitlines():
        s=line.strip()
        if re.match(r"^(mwr|mask_write|mask_poll|mask_delay)\b", s):
            out.append(s); n+=1
open(SC+"/ps7init.ocd","w").write("\n".join(out)+"\n")
print("    wrote ps7init.ocd with %d init commands"%n)
