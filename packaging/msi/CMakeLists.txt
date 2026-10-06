if(WIN32)
    add_library(exosnap_msi_owner_probe SHARED
        DistributionOwnerProbe.cpp
    )
    add_dependencies(exosnap exosnap_msi_owner_probe)

    target_link_libraries(exosnap_msi_owner_probe PRIVATE
        exosnap::warnings
        msi
        advapi32
    )

    set_target_properties(exosnap_msi_owner_probe PROPERTIES
        OUTPUT_NAME "exosnap-msi-owner-probe"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}>"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
endif()
