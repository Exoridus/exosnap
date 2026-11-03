find_package(Qt6 6.12 REQUIRED COMPONENTS Quick3D)

exosnap_add_gtest(
    NAME controller3d_tests
    TEST_PREFIX controller3d.
    SOURCES ../../tests/test_controller3d.cpp ControllerMechanics.cpp ControllerGeometry.cpp ControllerGeometry.h
        ${PROJECT_SOURCE_DIR}/libs/engine/src/gamepad_frame_sampler.cpp
        ControllerState.cpp ControllerState.h
        ../Quick/GamepadOverlay.cpp ../Quick/GamepadOverlay.h ../Quick/GamepadSprites.cpp ../Quick/GamepadArtwork.cpp
    LIBRARIES Qt6::Quick3D Qt6::Svg Qt6::Xml exosnap::gamepad
)
target_include_directories(controller3d_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ../Quick
    ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/libs/engine/include)

qt_add_executable(exosnap_controller3d WIN32 main.cpp)
set_source_files_properties(../../tests/ControllerDepthProbe.qml PROPERTIES QT_RESOURCE_ALIAS ControllerDepthProbe.qml)
qt_add_qml_module(exosnap_controller3d
    URI ExoSnap.Controller3D
    DEPENDENCIES QtQuick QtQuick3D
    QML_FILES PilotWindow.qml ControllerScene.qml ControllerStick.qml ../../tests/ControllerDepthProbe.qml
    SOURCES ControllerMechanics.cpp ControllerMechanics.h ControllerGeometry.cpp ControllerGeometry.h
        ControllerState.cpp ControllerState.h
)
target_sources(exosnap_controller3d PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/engine/src/gamepad_frame_sampler.cpp
    ../Quick/GamepadOverlay.cpp ../Quick/GamepadOverlay.h ../Quick/GamepadSprites.cpp ../Quick/GamepadArtwork.cpp)
target_include_directories(exosnap_controller3d PRIVATE ../Quick ${PROJECT_SOURCE_DIR}/app
    ${PROJECT_SOURCE_DIR}/libs/engine/include)
target_link_libraries(exosnap_controller3d PRIVATE Qt6::Quick3D Qt6::QuickControls2 Qt6::Svg Qt6::Xml exosnap::gamepad)
set_target_properties(exosnap_controller3d PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller3d")
qt_add_resources(exosnap_controller3d "pilot_mapping"
    PREFIX "/gamepad"
    BASE "${PROJECT_SOURCE_DIR}/app/assets/gamepad"
    FILES "${PROJECT_SOURCE_DIR}/app/assets/gamepad/controllers/dualsense/mapping.json"
        "${PROJECT_SOURCE_DIR}/app/assets/gamepad/controllers/dualsense/source.svg"
)
if(WIN32 AND EXOSNAP_WINDEPLOYQT)
    add_custom_command(TARGET exosnap_controller3d POST_BUILD
        COMMAND "${EXOSNAP_WINDEPLOYQT}" --no-translations --no-compiler-runtime --skip-plugin-types qmltooling
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}"
            "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:exosnap_controller3d>"
        COMMENT "Staging the Controller3D pilot runtime and Quick3D plugins"
        VERBATIM
    )
endif()

add_test(NAME controller3d.render COMMAND exosnap_controller3d
    --visual-test "${CMAKE_CURRENT_BINARY_DIR}/render-test.png" --visual-test-size 480x320)
set_tests_properties(controller3d.render PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=rhi;QSG_RHI_BACKEND=d3d11;QSG_RHI_PREFER_SOFTWARE_RENDERER=1;QSG_RENDER_LOOP=basic;QT_FORCE_STDERR_LOGGING=1"
    TIMEOUT 30)
exosnap_set_test_phase(controller3d.render gpu)
foreach(mode IN ITEMS normal ghost)
    add_test(NAME controller3d.depth.${mode} COMMAND exosnap_controller3d
        --visual-test "${CMAKE_CURRENT_BINARY_DIR}/depth-${mode}.png" --visual-test-size 320x214)
    set_tests_properties(controller3d.depth.${mode} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=rhi;QSG_RHI_BACKEND=d3d11;QSG_RHI_PREFER_SOFTWARE_RENDERER=1;QSG_RENDER_LOOP=basic;QT_FORCE_STDERR_LOGGING=1;EXOSNAP_VISUAL_CONTROLLER3D=${mode}-depth"
        TIMEOUT 30)
    exosnap_set_test_phase(controller3d.depth.${mode} gpu)
endforeach()
