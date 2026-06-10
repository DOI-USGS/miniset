find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found. Tests will not be built.")
    message(STATUS "Install GoogleTest or set GTest_DIR to enable tests.")
    return()
endif()

# Test helper function
function(add_knoten_test test_name)
    add_executable(${test_name} ${ARGN})
    target_link_libraries(${test_name}
        PRIVATE
            miniset
            GTest::GTest
            GTest::Main
    )
    gtest_discover_tests(${test_name})
endfunction()

# All tests in flat structure
add_knoten_test(test_math test_math.cpp)
add_knoten_test(test_ellipsoid test_ellipsoid.cpp)
add_knoten_test(test_dem test_dem.cpp)
add_knoten_test(test_csm_interface test_csm_interface.cpp)
add_knoten_test(test_angles test_angles.cpp)
add_knoten_test(test_coordinate_transforms test_coordinate_transforms.cpp)

# Integration tests for camproject
add_knoten_test(test_camproject test_camproject.cpp)

# Test loading CSM from GDAL metadata
if(TARGET GTest::GTest)
    add_knoten_test(test_csm_from_gdal test_csm_from_gdal.cpp)

    # Copy test data to build directory for CSM and camproject tests
    add_custom_command(TARGET test_csm_from_gdal POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/data
        ${CMAKE_CURRENT_BINARY_DIR}/data
        COMMENT "Copying test data to build directory"
    )
endif()

# Test campt function with both GeoTIFF and ISIS3 cubes
add_knoten_test(test_campt test_campt.cpp)
