set(LLVM_LINK_COMPONENTS
  support
)

add_clang_library(clangSpatch STATIC
  SmplParser.cpp
  PatternCompiler.cpp
  PatternParser.cpp
  PathQuery.cpp
  PatchRunner.cpp
)

clang_target_link_libraries(clangSpatch
  PRIVATE
  clangAST
  clangASTMatchers
  clangAnalysis
  clangBasic
  clangDynamicASTMatchers
  clangFrontend
  clangSerialization
  clangTooling
  )

add_subdirectory(tool)
