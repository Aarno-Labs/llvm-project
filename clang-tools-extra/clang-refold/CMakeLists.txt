set(LLVM_LINK_COMPONENTS Support)

add_clang_tool(clang-refold
  clang-refold.cpp
  DiffAlgorithms.cpp
  RefoldEngine.cpp
  RefoldModel.cpp
)

target_link_libraries(clang-refold PRIVATE
  clangBasic
  clangLex
  LLVMSupport
)
