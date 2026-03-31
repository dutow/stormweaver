if(NOT "${TEST_PG_DIR}" STREQUAL "" AND nanobind_FOUND)

    add_test(NAME stormweaver-basic
        COMMAND "${CMAKE_BINARY_DIR}/stormweaver"
                "${CMAKE_SOURCE_DIR}/scenarios/ci/basic.py"
                "-i" "${TEST_PG_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

endif()
