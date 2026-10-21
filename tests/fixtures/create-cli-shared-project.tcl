if {[catch {
    set source [file normalize ./source]
    set shared [file normalize "./\u5171\u4eab \u8f93\u5165"]
    if {[file exists $source] || [file exists $shared]} {error "Fixture paths already exist"}
    file mkdir [file join $shared rtl]
    file mkdir [file join $shared include]
    set f [open [file join $shared include defs.vh] w]
    puts $f {`define SHARED_VALUE 1'b1}
    close $f
    set f [open [file join $shared rtl top.v] w]
    puts $f {`include "defs.vh"}
    puts $f {module top(output wire led); assign led = `SHARED_VALUE; endmodule}
    close $f
    create_project demo $source -part xc7a35tcpg236-1
    add_files -norecurse [list [file join $shared rtl top.v]]
    set_property include_dirs [list [file join $shared include]] [current_fileset]
    set_property top top [current_fileset]
    update_compile_order -fileset sources_1
    close_project
    set marker [open $::env(FTB_SUCCESS_FILE) w]
    puts $marker ready
    close $marker
} reason]} {
    puts stderr "FIXTURE_ERROR: $reason"
    exit 1
}
exit 0
