set(LLVM_LINK_COMPONENTS Support)

add_clang_tool(clang-refold
  clang-refold.cpp
  DiffAlgorithms.cpp
  LineDirectiveInserter.cpp
  RefoldEngine.cpp
  RefoldModel.cpp
  StringUtils.cpp
)

target_link_libraries(clang-refold PRIVATE
  clangBasic
  clangLex
  clangFrontend
  clangSerialization
)
