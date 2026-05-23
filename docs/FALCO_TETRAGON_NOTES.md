# Falco And Tetragon Notes

The first kernel prototype remains a pre-ringbuf path/cmdline/arg filter. It is
not an integration patch for Falco or Tetragon.

Integration questions to carry into downstream experiments:

- which raw selector bytes are already available at each hook;
- whether DROP/POST/class-id action codes map cleanly into existing selector
  actions;
- ringbuf bytes and lost events before and after prefiltering;
- update cadence and graph-only policy reload latency;
- userspace decode/rule-evaluation work avoided by negative-heavy filters.
