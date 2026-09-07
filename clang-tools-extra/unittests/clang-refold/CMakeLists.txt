set(LLVM_LINK_COMPONENTS
  Support
  )

get_filename_component(CLANG_REFOLD_SOURCE_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/../../clang-refold REALPATH)
include_directories(
  ${CLANG_REFOLD_SOURCE_DIR}
  )

add_extra_unittest(ClangRefoldTests
  RefoldPragmaTaxonomyTests.cpp
  RefoldSourceLinePredicateTests.cpp
  RefoldOutputVerificationPolicyTests.cpp
  RefoldRepairIdentityTests.cpp
  RefoldWitnessCompositionTests.cpp
  RefoldWitnessJoinTests.cpp
  RefoldProofOrderTests.cpp
  RefoldTerminalObligationTests.cpp
  )

clang_target_link_libraries(ClangRefoldTests
  PRIVATE
  clangBasic
  clangFrontend
  clangLex
  clangSerialization
  )
target_link_libraries(ClangRefoldTests
  PRIVATE
  clangRefold
  )
