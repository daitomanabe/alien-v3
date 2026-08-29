target_sources(alien_server
PUBLIC
    Main.cpp)

target_link_libraries(alien_server Base)
target_link_libraries(alien_server EngineKernels)
target_link_libraries(alien_server EngineImpl)
target_link_libraries(alien_server EngineInterface)
target_link_libraries(alien_server PersisterInterface)

target_include_directories(alien_server
    PRIVATE
        ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
)

target_link_libraries(alien_server cudart)
target_link_libraries(alien_server Boost::boost)
target_link_libraries(alien_server OpenGL::GL OpenGL::GLU)
target_link_libraries(alien_server GLEW::GLEW)
target_link_libraries(alien_server glad::glad)
target_link_libraries(alien_server CLI11::CLI11)

target_precompile_headers(alien_server PRIVATE
    <memory>
    <string>
    <vector>
)
