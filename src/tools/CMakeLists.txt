add_executable(xbox_dxil_check xbox_dxil_check.cpp)
target_compile_definitions(xbox_dxil_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbox_dxil_check PRIVATE xv_options xv_runtime_dxil d3d12)
set_target_properties(xbox_dxil_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

if(XV_VULKAN)
  add_executable(xvalkyrie-vk-check vulkan_check.cpp)
  target_link_libraries(xvalkyrie-vk-check PRIVATE xv_options xv_vulkan nlohmann_json::nlohmann_json)
  set_target_properties(xvalkyrie-vk-check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
endif()
