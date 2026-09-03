# C++ because the Opus wrappers are C++; the surface exposed to the rest of
# the firmware stays plain C, declared in include/mpx_voice_link.h.
idf_component_register(
    SRCS
        "src/mpx_voice_codec.cc"
        "src/mpx_voice_cmd.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        mpx_audio
        console
)
