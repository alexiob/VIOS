.assume ADL=1
.bss
.global _fvm_ds,_fvm_rs,_fvm_loops,_fvm_depth,_fvm_rdepth,_fvm_ldepth,_fvm_ip,_fvm_error,_fvm_budget,_fvm_ds_peak,_fvm_rs_peak
_fvm_ds: .space 768
_fvm_rs: .space 768
_fvm_loops: .space 192
_fvm_depth: .space 3
_fvm_rdepth: .space 3
_fvm_ldepth: .space 3
_fvm_ip: .space 3
_fvm_error: .space 3
_fvm_budget: .space 3

_fvm_ds_peak: .space 3
_fvm_rs_peak: .space 3
