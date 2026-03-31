#include <nanobind/nanobind.h>

namespace nb = nanobind;

NB_MODULE(_stormweaver, m) {
    m.attr("__version__") = "0.1.0";
}
