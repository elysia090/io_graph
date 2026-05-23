# BPF Demo

`prefilter_demo.bpf.c` is the standalone demonstration program for the kernel
prototype kfunc API. It runs `bpf_iograph_run_action()` before ringbuf reserve
so the prefilter receives only the action it needs.

`prefilter_demo_user.c` is a libbpf loader for a patched kernel. It reads an
`iog_blob`, sets the `policy` map value size before BPF object load so the map
update receives the exact verified blob length, attaches the `raw_tp/sys_enter`
tracing program, and polls the demo ringbuf.

The Linux selftest loader in `kernel/selftests/bpf/prog_tests/iograph.c`
is the first regression target for the kernel overlay.
