OPT_BOOL(nohiz, false, "Disable hierarchical zbuffer")
OPT_BOOL(nozmask, false, "Disable zbuffer compression")
OPT_BOOL(ieeemath, false, "Force IEEE math rules and opcodes where applicable")
OPT_BOOL(ffmath, false, "Force FF math rules and opcodes where applicable")
OPT_BOOL(clamp_max_line_width, false, "Advertise the conformant aliased line-width maximum instead of the framebuffer-sized default. The GA line-to-quad expansion diverges from the reference rasterizer at the line end-caps as width grows, so the framebuffer-sized range exposes widths the hardware cannot rasterize to reference precision. Off by default so applications keep the full wide-line range; enable per-application for coverage-strict workloads.")

#undef OPT_BOOL
