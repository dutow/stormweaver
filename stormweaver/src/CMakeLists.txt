nanobind_add_module(_stormweaver
    module.cpp
)
TARGET_LINK_LIBRARIES(_stormweaver PRIVATE
    libstormweaver
)
SET_PROPERTY(TARGET _stormweaver PROPERTY CXX_STANDARD 23)

if(TARGET nanobind-static)
    target_compile_options(nanobind-static PRIVATE
        -Wno-missing-declarations -Wno-error)
endif()
