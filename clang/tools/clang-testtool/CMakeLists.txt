set( LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Support
  Core
  )

add_clang_tool(clang-testtool
  ClangTestTool.cpp
  )

clang_target_link_libraries(clang-testtool
  PRIVATE
  clangBasic
  clangDriver
  clangFrontend
  clangCodeGen
  clangTooling
  clangSerialization
  )
