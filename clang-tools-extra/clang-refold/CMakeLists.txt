set(LLVM_LINK_COMPONENTS Support)

add_clang_tool(clang-refold
  clang-refold.cpp
  DiffAlgorithms.cpp
  FinalLineControlModel.cpp
  LineDirectiveInserter.cpp
  RefoldEngine.cpp
  RefoldIncludePathProof.cpp
  RefoldIncludeReplayProof.cpp
  RefoldProofLattice.cpp
  RefoldTerminalProof.cpp
  RefoldOwnerStateProof.cpp
  RefoldLineControlProof.cpp
  RefoldMacroStateProof.cpp
  RefoldSidebandReplayProof.cpp
  RefoldSourceGraphProof.cpp
  RefoldSourceGraphWriter.cpp
  RefoldModel.cpp
  StringUtils.cpp
)

target_link_libraries(clang-refold PRIVATE
  clangBasic
  clangLex
  clangFrontend
  clangSerialization
)
