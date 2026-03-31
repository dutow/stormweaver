nanobind_add_module(_stormweaver
    FREE_THREADED
    module.cpp
)
TARGET_LINK_LIBRARIES(_stormweaver PRIVATE
    libstormweaver
)
SET_PROPERTY(TARGET _stormweaver PROPERTY CXX_STANDARD 23)

foreach(_nb_target nanobind-static nanobind-static-ft)
    if(TARGET ${_nb_target})
        target_compile_options(${_nb_target} PRIVATE
            -Wno-missing-declarations -Wno-error)
    endif()
endforeach()
