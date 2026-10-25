# Raw trace capture (M5): the pre-window ring, the episode rules and the trace
# file format. Pure C, host-tested by firmware/Makefile (test/test_trace.c); the
# files are written by main/trace_task.c.
idf_component_register(
    SRCS
        "src/trace.c"
    INCLUDE_DIRS "include"
    REQUIRES
        fusion
)
