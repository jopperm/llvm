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

class GetModuleAction : public ToolAction {
public:
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
    if (auto M = ELOA.takeModule()) {
      if (auto *F = M->getFunction("_ZTS11hello_world"))
        F->dump();
    }

    Files->clearStatCache();
    return Success;
  }
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

  const char *HelloWorldKernel = R"""(
    #include <sycl/sycl.hpp>
    using namespace sycl;

    class hello_world;

    int main() {
      auto defaultQueue = sycl::queue{};

      defaultQueue
        .submit([&](sycl::handler &cgh) {
          auto os = sycl::stream{128, 128, cgh};

          cgh.single_task<hello_world>([=]() { os << "Hello World!\n"; });
        })
        .wait();

      return 0;
    }
    )""";

  // Conveniently register an in memory file. This is an overlay over the actual
  // file system, so existing headers etc. will be found.
  Tool.mapVirtualFile("rtc.cpp", HelloWorldKernel);

  // Execute the action. Down the line, a `clang::driver::Driver` will be
  // created in `ToolInvocation::run` because we didn't specify a `-cc1` command
  // line. The driver sets up the `CompilerInvocation` that is then passed to
  // our `GetModuleAction`.
  GetModuleAction Action;
  return Tool.run(&Action);
}
