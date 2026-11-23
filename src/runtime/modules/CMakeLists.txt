add_library(xv_runtime_dxil xbox_dxil.cpp)
target_include_directories(xv_runtime_dxil PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xv_runtime_dxil PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xv_runtime_dxil PRIVATE xv_options)

# Console modules both consoles have. Each console's runtime installs them under the name
# the DLL has there.
add_library(xv_runtime_modules xg.cpp kernel.cpp memory.cpp dstorage.cpp console_shader.cpp console_shader_rdna.cpp watchdog.cpp acphal.cpp window.cpp system.cpp placement.cpp storage.cpp gameinput.cpp locale.cpp
  d3d12x/device.cpp d3d12x/present.cpp
  gameruntime/runtime.cpp gameruntime/slots.cpp gameruntime/task_queue.cpp gameruntime/async.cpp
  gameruntime/user.cpp gameruntime/save.cpp gameruntime/system.cpp gameruntime/store.cpp
  gameruntime/ui.cpp gameruntime/streaming.cpp)
target_include_directories(xv_runtime_modules PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xv_runtime_modules PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_runtime_modules PRIVATE xv_options xv_core xv_host xv_game_config xv_runtime_common
  xv_runtime_dxil xv_graphics xv_input xv_surface d3d12 dxgi shlwapi onecore ole32 sapi)
