set(LLVM_LINK_COMPONENTS
  Support
  )

get_filename_component(SPATCH_SOURCE_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/../../clang-spatch REALPATH)
include_directories(
  ${SPATCH_SOURCE_DIR}
  )

add_extra_unittest(ClangSpatchTests
  EditTest.cpp
  PatternCompilerTest.cpp
  PatternParserTest.cpp
  SmplParserTest.cpp
  )

# The corpus is 1209 third-party .cocci files, so it is located by definition
# rather than copied into the build tree.
target_compile_definitions(ClangSpatchTests
  PRIVATE
  CLANG_SPATCH_TEST_INPUTS="${SPATCH_SOURCE_DIR}/test/Inputs"
  )

clang_target_link_libraries(ClangSpatchTests
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangSerialization
  clangTooling
  )
target_link_libraries(ClangSpatchTests
  PRIVATE
  clangSpatch
  )
