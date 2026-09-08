set(LLVM_LINK_COMPONENTS
  Support
)

add_clang_tool(clang-spatch
  ClangSpatch.cpp
  )

clang_target_link_libraries(clang-spatch
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangTooling
  )

target_link_libraries(clang-spatch
  PRIVATE
  clangSpatch
  )
