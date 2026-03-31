nanobind_add_module(_stormweaver
    module.cpp
)
TARGET_LINK_LIBRARIES(_stormweaver PRIVATE
    libstormweaver
)
SET_PROPERTY(TARGET _stormweaver PROPERTY CXX_STANDARD 23)
