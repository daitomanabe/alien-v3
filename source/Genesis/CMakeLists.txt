target_sources(alien_genesis
PUBLIC
    Main.cpp)

target_link_libraries(alien_genesis Base)
target_link_libraries(alien_genesis EngineKernels)
target_link_libraries(alien_genesis EngineImpl)
target_link_libraries(alien_genesis EngineInterface)
target_link_libraries(alien_genesis PersisterInterface)

target_include_directories(alien_genesis
    PRIVATE
        ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
)

target_link_libraries(alien_genesis cudart)
target_link_libraries(alien_genesis Boost::boost)
target_link_libraries(alien_genesis OpenGL::GL OpenGL::GLU)
target_link_libraries(alien_genesis GLEW::GLEW)
target_link_libraries(alien_genesis glad::glad)
target_link_libraries(alien_genesis CLI11::CLI11)

target_precompile_headers(alien_genesis PRIVATE
    <memory>
    <string>
    <vector>
)
