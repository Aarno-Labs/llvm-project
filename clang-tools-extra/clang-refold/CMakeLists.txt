set(LLVM_LINK_COMPONENTS Support)

add_clang_tool(clang-refold
  clang-refold.cpp
  DiffAlgorithms.cpp
  FinalLineControlModel.cpp
  LineDirectiveInserter.cpp
  RefoldEngine.cpp
  RefoldIncludeReplayProof.cpp
  RefoldProofLattice.cpp
  RefoldTerminalProof.cpp
  RefoldOwnerStateProof.cpp
  RefoldLineControlProof.cpp
  RefoldMacroStateProof.cpp
  RefoldSidebandReplayProof.cpp
  RefoldModel.cpp
  StringUtils.cpp
)

target_link_libraries(clang-refold PRIVATE
  clangBasic
  clangLex
  clangFrontend
  clangSerialization
)
