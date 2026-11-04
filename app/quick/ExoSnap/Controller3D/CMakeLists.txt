find_package(Qt6 6.12 REQUIRED COMPONENTS Quick3D)

qt_add_qml_module(exosnap_controller3d_scene
    STATIC
    URI ExoSnap.Controller3D
    PLUGIN_TARGET exosnap_controller3d_sceneplugin
    CLASS_NAME ExoSnapController3DPlugin
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ExoSnap/Controller3D"
    DEPENDENCIES QtQuick QtQuick3D
    QML_FILES ControllerScene.qml ControllerStick.qml
    SOURCES ControllerMechanics.cpp ControllerMechanics.h ControllerGeometry.cpp ControllerGeometry.h
        ControllerState.cpp ControllerState.h NativeRenderTarget.cpp NativeRenderTarget.h
        NativeGamepadProducer.cpp NativeGamepadProducer.h
)
target_include_directories(exosnap_controller3d_scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../Quick
    ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/libs/engine/include)
target_link_libraries(exosnap_controller3d_scene
    PUBLIC Qt6::Quick3D exosnap::gamepad
    PRIVATE tomlplusplus::tomlplusplus exosnap::warnings d3d11.lib dxgi.lib runtimeobject.lib)
target_compile_definitions(exosnap_controller3d_scene PUBLIC NOMINMAX)

exosnap_add_gtest(
    NAME controller3d_tests
    TEST_PREFIX controller3d.
    SOURCES ../../tests/test_controller3d.cpp Controller3DPluginImport.cpp
        ${PROJECT_SOURCE_DIR}/libs/engine/src/device_generation.cpp
        ${PROJECT_SOURCE_DIR}/libs/engine/src/gamepad_frame_sampler.cpp
        ${PROJECT_SOURCE_DIR}/libs/engine/src/visual_scene_runtime.cpp
        ${PROJECT_SOURCE_DIR}/libs/engine/src/visual_gpu_lane.cpp
        ${PROJECT_SOURCE_DIR}/libs/engine/src/gpu_timestamp_profiler.cpp
        ${PROJECT_SOURCE_DIR}/libs/engine/src/preview_shared_texture.cpp
        ../Quick/GamepadOverlay.cpp ../Quick/GamepadOverlay.h ../Quick/GamepadSprites.cpp ../Quick/GamepadArtwork.cpp
    LIBRARIES exosnap_controller3d_sceneplugin Qt6::Quick3D Qt6::Svg Qt6::Xml exosnap::gamepad tomlplusplus::tomlplusplus
        d3d11.lib dxgi.lib runtimeobject.lib
)
target_include_directories(controller3d_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ../Quick
    ${PROJECT_SOURCE_DIR}/app ${PROJECT_SOURCE_DIR}/libs/engine/include)

qt_add_executable(exosnap_controller3d WIN32 main.cpp Controller3DPluginImport.cpp)
set_source_files_properties(../../tests/ControllerDepthProbe.qml PROPERTIES QT_RESOURCE_ALIAS ControllerDepthProbe.qml)
qt_add_qml_module(exosnap_controller3d
    URI ExoSnap.Controller3DPilot
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pilot/ExoSnap/Controller3DPilot"
    DEPENDENCIES QtQuick QtQuick3D TARGET exosnap_controller3d_scene
    QML_FILES PilotWindow.qml ../../tests/ControllerDepthProbe.qml
)
target_sources(exosnap_controller3d PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/engine/src/device_generation.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/gamepad_frame_sampler.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/visual_scene_runtime.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/visual_gpu_lane.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/gpu_timestamp_profiler.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/preview_shared_texture.cpp
    ../Quick/GamepadOverlay.cpp ../Quick/GamepadOverlay.h ../Quick/GamepadSprites.cpp ../Quick/GamepadArtwork.cpp)
target_include_directories(exosnap_controller3d PRIVATE ../Quick ${PROJECT_SOURCE_DIR}/app
    ${PROJECT_SOURCE_DIR}/libs/engine/include)
target_link_libraries(exosnap_controller3d PRIVATE exosnap_controller3d_sceneplugin
    Qt6::Quick3D Qt6::QuickControls2 Qt6::Svg Qt6::Xml exosnap::gamepad tomlplusplus::tomlplusplus
    d3d11.lib dxgi.lib runtimeobject.lib)
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

qt_add_executable(exosnap_controller3d_native NativeRenderProbe.cpp NativeGamepadProducerChecks.cpp
    Controller3DPluginImport.cpp)
qt_add_qml_module(exosnap_controller3d_native
    URI ExoSnap.Controller3DProbe
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native/ExoSnap/Controller3DProbe"
    DEPENDENCIES QtQuick QtQuick3D TARGET exosnap_controller3d_scene
    QML_FILES ../../tests/ControllerDepthProbe.qml
)
target_sources(exosnap_controller3d_native PRIVATE
    ../Quick/GamepadOverlay.cpp ../Quick/GamepadOverlay.h ../Quick/GamepadSprites.cpp ../Quick/GamepadArtwork.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/device_generation.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/gamepad_frame_sampler.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/gpu_compositor.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/visual_scene_runtime.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/visual_gpu_lane.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/gpu_timestamp_profiler.cpp
    ${PROJECT_SOURCE_DIR}/libs/engine/src/preview_shared_texture.cpp)
target_include_directories(exosnap_controller3d_native PRIVATE ../Quick ${PROJECT_SOURCE_DIR}/app
    ${PROJECT_SOURCE_DIR}/libs/engine/include ${PROJECT_SOURCE_DIR}/libs/capability/include)
target_compile_definitions(exosnap_controller3d_native PRIVATE NOMINMAX)
target_link_libraries(exosnap_controller3d_native PRIVATE exosnap_controller3d_sceneplugin Qt6::Quick3D Qt6::Svg Qt6::Xml
    exosnap::gamepad tomlplusplus::tomlplusplus exosnap::warnings d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib)
qt_add_resources(exosnap_controller3d_native "native_mapping"
    PREFIX "/gamepad"
    BASE "${PROJECT_SOURCE_DIR}/app/assets/gamepad"
    FILES "${PROJECT_SOURCE_DIR}/app/assets/gamepad/controllers/dualsense/mapping.json"
        "${PROJECT_SOURCE_DIR}/app/assets/gamepad/controllers/dualsense/source.svg"
)
if(WIN32 AND EXOSNAP_WINDEPLOYQT)
    add_custom_command(TARGET exosnap_controller3d_native POST_BUILD
        COMMAND "${EXOSNAP_WINDEPLOYQT}" --no-translations --no-compiler-runtime --skip-plugin-types qmltooling
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}"
            "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:exosnap_controller3d_native>"
        COMMENT "Staging the native controller render probe runtime"
        VERBATIM
    )
endif()
add_test(NAME controller3d.native.render COMMAND exosnap_controller3d_native
    --warp --output "${CMAKE_CURRENT_BINARY_DIR}/native-evidence")
set_tests_properties(controller3d.native.render PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=rhi;QSG_RHI_BACKEND=d3d11;QSG_RENDER_LOOP=basic;QT_FORCE_STDERR_LOGGING=1"
    TIMEOUT 90)
exosnap_set_test_phase(controller3d.native.render gpu)
