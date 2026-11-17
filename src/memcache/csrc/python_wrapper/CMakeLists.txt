file(GLOB SOURCES "*.cpp")
set(PYTHON_PACKAGE_NAME "_pymmc")

find_package(pybind11 REQUIRED)

pybind11_add_module(_pymmc ${SOURCES} pymmc.cpp)

target_link_libraries(_pymmc PUBLIC mmc_shared)

install(TARGETS _pymmc
        LIBRARY DESTINATION ${TARGET_INSTALL_DIR}/memcache/lib64
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
)
