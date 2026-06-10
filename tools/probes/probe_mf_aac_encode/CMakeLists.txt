add_executable(probe_mf_aac_encode
    src/main.cpp
    src/mf_aac_probe.h
    src/mf_aac_probe.cpp
)

target_link_libraries(probe_mf_aac_encode PRIVATE
    ole32.lib
    mfplat.lib
    mf.lib
    mfuuid.lib
    wmcodecdspuuid.lib      # provides CLSID_AACMFTEncoder GUID, referenced via <wmcodecdsp.h>
    exosnap::warnings
)

set_property(TARGET probe_mf_aac_encode PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_mf_aac_encode PROPERTY CXX_STANDARD_REQUIRED ON)
