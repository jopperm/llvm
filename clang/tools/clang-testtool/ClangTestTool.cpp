//===--- tools/clang-testtool/ClangTestTool.cpp - Clang test tool ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This tool uses the Clang Tooling infrastructure, see
//    http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html
//  for details on setting it up with LLVM source tree.
//
//===----------------------------------------------------------------------===//

#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"

using namespace clang::driver;
using namespace clang::tooling;
using namespace clang;
using namespace llvm;

namespace {

struct GetModuleAction : public ToolAction {
  bool runInvocation(std::shared_ptr<CompilerInvocation> Invocation,
                     FileManager *Files,
                     std::shared_ptr<PCHContainerOperations> PCHContainerOps,
                     DiagnosticConsumer *DiagConsumer) override {
    // Code adapted from `FrontendActionFactory::runInvocation`!

    // Create a compiler instance to handle the actual work.
    CompilerInstance Compiler(std::move(PCHContainerOps));
    Compiler.setInvocation(std::move(Invocation));
    Compiler.setFileManager(Files);

    // Create the compiler's actual diagnostics engine.
    Compiler.createDiagnostics(DiagConsumer, /*ShouldOwnClient=*/false);
    if (!Compiler.hasDiagnostics())
      return false;

    Compiler.createSourceManager(*Files);

    // Ignore `Compiler.getFrontendOpts().ProgramAction` (would be `EmitBC`) and
    // create/execute an `EmitLLVMOnlyAction` (= codegen to LLVM module without
    // emitting anything) instead.
    EmitLLVMOnlyAction ELOA;
    const bool Success = Compiler.ExecuteAction(ELOA);
    Files->clearStatCache();
    if (!Success)
      return false;

    // Take the module and its context to extend the objects' lifetime.
    // Alternatively, we could also pass our own context to the action's
    // constructor.
    // Note that this PoC doesn't handle more than 1 source file.
    Module = ELOA.takeModule();
    Context = ELOA.takeLLVMContext();

    return true;
  }

  std::unique_ptr<llvm::Module> Module;
  llvm::LLVMContext *Context;
};

} // namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  // Initialize targets for clang module support.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // No need to use an `CommonOptionsParser` and get the compilation DB and
  // source path list from there -- we can just hard-wire the source files and
  // compilation flags we need.
  llvm::SmallVector<std::string> SourcePathList = {"rtc.cpp"};
  FixedCompilationDatabase DB{"./", {"-fsycl-device-only"}};
  ClangTool Tool{DB, SourcePathList};

  // Get rid of the default argument adjuster (see `ClangTool::ClangTool`); in
  // particular the `-fsyntax-only` inserter.
  Tool.clearArgumentsAdjusters();

  const char *RTCKernel = R"""(
    #include <sycl/sycl.hpp>
    using namespace sycl;

    extern "C" SYCL_EXT_ONEAPI_FUNCTION_PROPERTY(
      (ext::oneapi::experimental::single_task_kernel))
    void ff_0(int *ptr, int start, int end) {
      for (int i = start; i <= end; i++)
        ptr[i] = start + end;
    }
    )""";

  // Conveniently register an in memory file. This is an overlay over the actual
  // file system, so existing headers etc. will be found.
  Tool.mapVirtualFile("rtc.cpp", RTCKernel);

  // Execute the action. Down the line, a `clang::driver::Driver` will be
  // created in `ToolInvocation::run` because we didn't specify a `-cc1` command
  // line. The driver sets up the `CompilerInvocation` that is then passed to
  // our `GetModuleAction`.
  GetModuleAction Action;
  if (!Tool.run(&Action)) {
    Action.Module->dump();
    Action.Module.reset();
    delete Action.Context;
  }

  return 0;
}
