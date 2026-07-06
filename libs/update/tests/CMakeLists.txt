# libs/update/tests/CMakeLists.txt

exosnap_add_gtest(
    NAME    update_semver_tests
    SOURCES test_update_semver.cpp
    LIBRARIES exosnap::update
)

exosnap_add_gtest(
    NAME    update_signature_tests
    SOURCES test_update_signature.cpp
    LIBRARIES exosnap::update
)

exosnap_add_gtest(
    NAME    update_guard_tests
    SOURCES test_update_guard.cpp
    LIBRARIES exosnap::update
)

exosnap_add_gtest(
    NAME    update_manifest_tests
    SOURCES test_update_manifest.cpp
    LIBRARIES exosnap::update
)

exosnap_add_gtest(
    NAME    test_zip_extract
    SOURCES test_zip_extract.cpp
    LIBRARIES exosnap::update exosnap_miniz
)

exosnap_add_gtest(
    NAME    test_http_download
    SOURCES test_http_download.cpp
    LIBRARIES exosnap::update
)

exosnap_add_gtest(
    NAME    test_release_locator
    SOURCES test_release_locator.cpp
    LIBRARIES exosnap::update
)
