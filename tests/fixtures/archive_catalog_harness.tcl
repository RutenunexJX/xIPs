# Model catalog availability and reset ordering; real Vivado fixtures cover the adapter.
set mode [lindex $argv 1]
proc version {args} {return "2023.1"}
proc open_project {path} {}
proc current_project {} {return project}
proc close_project {} {}
proc get_runs {args} {return impl}
proc get_files {args} {
    if {[lsearch -exact $args -filter] >= 0} {return design.bd}
    return [list "$::env(FTB_PROJECT_ROOT)/demo.srcs/top.v"]
}
proc get_ips {args} {return {standalone rtl}}
proc get_property {key object} {
    switch $key {
        DIRECTORY {return $::env(FTB_PROJECT_ROOT)}
        IPDEF {
            if {$object eq "rtl"} {return "xilinx.com:module_ref:local_rtl:1.0"}
            if {$::mode eq "missing-ip"} {return "user.org:ip:missing:1.0"}
            return "xilinx.com:ip:known:1.0"
        }
        TYPE {return ip}
        VLNV {
            if {$::mode eq "missing-bd-ip"} {return "user.org:ip:missing_bd:1.0"}
            return "xilinx.com:ip:known:1.0"
        }
        default {error "Unexpected property: $key"}
    }
}
proc get_ipdefs {args} {
    if {$::mode eq "catalog-error"} {error "Catalog lookup failed"}
    # An unrelated result must not satisfy an exact missing definition.
    return "xilinx.com:ip:known:1.0"
}
proc open_bd_design {file} {
    if {$::mode eq "unreadable-bd"} {error "Cannot load block design"}
}
proc get_bd_cells {args} {return cell}
proc current_bd_design {} {return design}
proc close_bd_design {bd} {}
proc reset_project {} {
    set f [open $::env(FTB_RESET_MARKER) w]; puts $f "reset"; close $f
}
source [lindex $argv 0]
