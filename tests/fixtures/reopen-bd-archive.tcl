# Run against a disposable copy of the archived create-bd-project.tcl fixture.
if {[catch {
    set root [file normalize $::env(FTB_REOPEN_ROOT)]
    open_project "$root/fixture.xpr"
    set wrapper "$root/fixture.srcs/_archive_generated/fixture.gen/sources_1/bd/design_1/hdl/design_1_wrapper.v"
    if {![file isfile $wrapper] || [llength [get_files -quiet $wrapper]] != 1} {
        error "Archived wrapper is missing or is not a project source"
    }
    set bd [get_files design_1.bd]
    open_bd_design $bd
    validate_bd_design
    generate_target all $bd
    update_compile_order -fileset sources_1
    set ordered [get_files -compile_order sources -used_in synthesis]
    if {[lsearch -exact $ordered $wrapper] < 0} {error "Wrapper is absent from the synthesis compile order"}
    foreach path $ordered {
        if {![file isfile $path]} {error "Missing synthesis source: $path"}
        set normalized [string tolower [file normalize $path]]
        if {[string first "[string tolower $root]/" $normalized] != 0} {error "Source outside the archive: $path"}
    }
    close_project
    puts "FTB_REOPEN_OK: wrapper and regenerated BD sources are available inside the archive"
} reason]} {
    puts stderr "REOPEN_ERROR: $reason"
    exit 1
}
exit 0
