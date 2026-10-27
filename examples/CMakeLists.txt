# One standalone, runnable executable per decode mode -- build and run
# any of them to see that mode's API in action:
#   cmake --build build --target heap_example && ./build/examples/heap_example
foreach(name heap_example noalloc_example sax_example)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE constexpr_cbor)
endforeach()
