/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <xenia/cpu/exec_module.h>

#include <llvm/Linker.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>
#include <llvm/Support/Threading.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <xenia/cpu/codegen/module_generator.h>
#include <xenia/cpu/sdb.h>
#include <xenia/cpu/ppc/instr.h>
#include <xenia/cpu/ppc/state.h>

#include "cpu/cpu-private.h"
#include "cpu/xethunk/xethunk.h"


using namespace llvm;
using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::codegen;
using namespace xe::cpu::sdb;
using namespace xe::kernel;


ExecModule::ExecModule(
    xe_memory_ref memory, shared_ptr<ExportResolver> export_resolver,
    const char* module_name, const char* module_path,
    shared_ptr<llvm::ExecutionEngine>& engine) {
  memory_ = xe_memory_retain(memory);
  export_resolver_ = export_resolver;
  module_name_ = xestrdupa(module_name);
  module_path_ = xestrdupa(module_path);
  engine_ = engine;

  context_ = shared_ptr<LLVMContext>(new LLVMContext());
}

ExecModule::~ExecModule() {
  if (gen_module_) {
    Uninit();
    engine_->removeModule(gen_module_.get());
  }

  xe_free(module_path_);
  xe_free(module_name_);
  xe_memory_release(memory_);
}

int ExecModule::PrepareUserModule(kernel::UserModule* user_module) {
  sdb_ = shared_ptr<sdb::SymbolDatabase>(
      new sdb::XexSymbolDatabase(memory_, export_resolver_.get(), user_module));

  int result_code = Prepare();
  if (result_code) {
    return result_code;
  }

  // Import variables.
  xe_xex2_ref xex = user_module->xex();
  xe_xex2_release(xex);

  return 0;
}

int ExecModule::PrepareRawBinary(uint32_t start_address, uint32_t end_address) {
  sdb_ = shared_ptr<sdb::SymbolDatabase>(
      new sdb::RawSymbolDatabase(memory_, export_resolver_.get(),
                                 start_address, end_address));

  return Prepare();
}

int ExecModule::Prepare() {
  int result_code = 1;
  std::string error_message;

  char file_name[2048];

  OwningPtr<MemoryBuffer> shared_module_buffer;
  auto_ptr<Module> shared_module;
  auto_ptr<raw_ostream> outs;

  PassManager pm;
  PassManagerBuilder pmb;

  // TODO(benvanik): embed the bc file into the emulator.
  const char *thunk_path = "src/cpu/xethunk/xethunk.bc";

  // Calculate a cache path based on the module, the CPU version, and other
  // bits.
  // TODO(benvanik): cache path calculation.
  //const char *cache_path = "build/generated.bc";

  // Check the cache to see if the bitcode exists.
  // If it does, load that module directly. In the future we could also cache
  // on linked binaries but that requires more safety around versioning.
  // TODO(benvanik): check cache for module bitcode and load.
  // if (path_exists(cache_key)) {
  //   exec_module = load_bitcode(cache_key);
  //   sdb = load_symbol_table(cache_key);
  // }

  // If not found in cache, generate a new module.
  if (!gen_module_.get()) {
    // Load shared bitcode files.
    // These contain globals and common thunk code that are used by the
    // generated code.
    XEEXPECTZERO(MemoryBuffer::getFile(thunk_path, shared_module_buffer));
    shared_module = auto_ptr<Module>(ParseBitcodeFile(
        &*shared_module_buffer, *context_, &error_message));
    XEEXPECTNOTNULL(shared_module.get());

    // Analyze the module and add its symbols to the symbol database.
    XEEXPECTZERO(sdb_->Analyze());

    // Dump the symbol database.
    if (FLAGS_dump_module_map) {
      xesnprintfa(file_name, XECOUNT(file_name),
                  "%s%s.map", FLAGS_dump_path.c_str(), module_name_);
      sdb_->Write(file_name);
    }

    // Initialize the module.
    gen_module_ = shared_ptr<Module>(
        new Module(module_name_, *context_.get()));
    // TODO(benavnik): addModuleFlag?

    // Inject globals.
    // This should be done ASAP to ensure that JITed functions can use the
    // constant addresses.
    XEEXPECTZERO(InjectGlobals());

    // Link shared module into generated module.
    // This gives us a single module that we can optimize and prevents the need
    // for foreward declarations.
    Linker::LinkModules(gen_module_.get(), shared_module.get(), 0,
                        &error_message);

    // Build the module from the source code.
    codegen_ = auto_ptr<ModuleGenerator>(new ModuleGenerator(
        memory_, export_resolver_.get(), module_name_, module_path_,
        sdb_.get(), context_.get(), gen_module_.get(),
        engine_.get()));
    XEEXPECTZERO(codegen_->Generate());

    // Write to cache.
    // TODO(benvanik): cache stuff

    // Dump pre-optimized module to disk.
    if (FLAGS_dump_module_bitcode) {
      xesnprintfa(file_name, XECOUNT(file_name),
                  "%s%s-preopt.bc", FLAGS_dump_path.c_str(), module_name_);
      outs = auto_ptr<raw_ostream>(new raw_fd_ostream(
          file_name, error_message, raw_fd_ostream::F_Binary));
      XEEXPECTTRUE(error_message.empty());
      WriteBitcodeToFile(gen_module_.get(), *outs);
    }
  }

  // Link optimizations.
  XEEXPECTZERO(gen_module_->MaterializeAllPermanently(&error_message));

  // Reset target triple (ignore what's in xethunk).
  gen_module_->setTargetTriple(llvm::sys::getDefaultTargetTriple());

  // Run full module optimizations.
  pm.add(new DataLayout(gen_module_.get()));
  if (FLAGS_optimize_ir_modules) {
    pm.add(createVerifierPass());
    pmb.OptLevel      = 3;
    pmb.SizeLevel     = 0;
    pmb.Inliner       = createFunctionInliningPass();
    pmb.Vectorize     = true;
    pmb.LoopVectorize = true;
    pmb.populateModulePassManager(pm);
    pmb.populateLTOPassManager(pm, false, true);
  }
  pm.add(createVerifierPass());
  pm.run(*gen_module_);

  // Dump post-optimized module to disk.
  if (FLAGS_optimize_ir_modules && FLAGS_dump_module_bitcode) {
    xesnprintfa(file_name, XECOUNT(file_name),
                "%s%s.bc", FLAGS_dump_path.c_str(), module_name_);
    outs = auto_ptr<raw_ostream>(new raw_fd_ostream(
        file_name, error_message, raw_fd_ostream::F_Binary));
    XEEXPECTTRUE(error_message.empty());
    WriteBitcodeToFile(gen_module_.get(), *outs);
  }

  // TODO(benvanik): experiment with LLD to see if we can write out a dll.

  // Initialize the module.
  XEEXPECTZERO(Init());

  // Force JIT of all functions.
  for (Module::iterator it = gen_module_->begin(); it != gen_module_->end();
       ++it) {
    Function* fn = it;
    if (!fn->isDeclaration()) {
      engine_->getPointerToFunction(fn);
    }
  }

  result_code = 0;
XECLEANUP:
  return result_code;
}

void ExecModule::AddFunctionsToMap(FunctionMap& map) {
  codegen_->AddFunctionsToMap(map);
}

void XeTrap(xe_ppc_state_t* state, uint32_t cia) {
  printf("TRAP");
  XEASSERTALWAYS();
}

void XeIndirectBranch(xe_ppc_state_t* state, uint64_t target, uint64_t br_ia) {
  printf("INDIRECT BRANCH %.8X -> %.8X\n", (uint32_t)br_ia, (uint32_t)target);
  XEASSERTALWAYS();
}

void XeInvalidInstruction(xe_ppc_state_t* state, uint32_t cia, uint32_t data) {
  ppc::InstrData i;
  i.address = cia;
  i.code = data;
  i.type = ppc::GetInstrType(i.code);

  if (!i.type) {
    XELOGCPU(XT("INVALID INSTRUCTION %.8X: %.8X ???"),
             i.address, i.code);
  } else if (i.type->disassemble) {
    ppc::InstrDisasm d;
    i.type->disassemble(i, d);
    std::string disasm;
    d.Dump(disasm);
    XELOGCPU(XT("INVALID INSTRUCTION %.8X: %.8X %s"),
             i.address, i.code, disasm.c_str());
  } else {
    XELOGCPU(XT("INVALID INSTRUCTION %.8X: %.8X %s"),
             i.address, i.code, i.type->name);
  }
}

void XeTraceKernelCall(xe_ppc_state_t* state, uint64_t cia, uint64_t call_ia,
                       KernelExport* kernel_export) {
  XELOGCPU(XT("TRACE: %.8X -> k.%.8X (%s)"),
           (uint32_t)call_ia - 4, (uint32_t)cia,
           kernel_export ? kernel_export->name : "unknown");
}

void XeTraceUserCall(xe_ppc_state_t* state, uint64_t cia, uint64_t call_ia,
                     FunctionSymbol* fn) {
  XELOGCPU(XT("TRACE: %.8X -> u.%.8X (%s)"),
           (uint32_t)call_ia - 4, (uint32_t)cia, fn->name);
}

void XeTraceInstruction(xe_ppc_state_t* state, uint32_t cia, uint32_t data) {
  ppc::InstrType* type = ppc::GetInstrType(data);
  XELOGCPU(XT("TRACE: %.8X %.8X %s %s"),
           cia, data,
           type && type->emit ? " " : "X",
           type ? type->name : "<unknown>");

  // TODO(benvanik): better disassembly, printing of current register values/etc
}

int ExecModule::InjectGlobals() {
  LLVMContext& context = *context_.get();
  const DataLayout* dl = engine_->getDataLayout();
  Type* intPtrTy = dl->getIntPtrType(context);
  Type* int8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
  GlobalVariable* gv;

  // xe_memory_base
  // This is the base void* pointer to the memory space.
  gv = new GlobalVariable(
      *gen_module_,
      int8PtrTy,
      true,
      GlobalValue::ExternalLinkage,
      0,
      "xe_memory_base");
  // Align to 64b - this makes SSE faster.
  gv->setAlignment(64);
  gv->setInitializer(ConstantExpr::getIntToPtr(
      ConstantInt::get(intPtrTy, (uintptr_t)xe_memory_addr(memory_, 0)),
      int8PtrTy));

  // Control methods:
  std::vector<Type*> trapArgs;
  trapArgs.push_back(int8PtrTy);
  trapArgs.push_back(Type::getInt32Ty(context));
  FunctionType* trapTy = FunctionType::get(
      Type::getVoidTy(context), trapArgs, false);
  engine_->addGlobalMapping(Function::Create(
      trapTy, Function::ExternalLinkage, "XeTrap",
      gen_module_.get()), (void*)&XeTrap);

  std::vector<Type*> indirectBranchArgs;
  indirectBranchArgs.push_back(int8PtrTy);
  indirectBranchArgs.push_back(Type::getInt64Ty(context));
  indirectBranchArgs.push_back(Type::getInt64Ty(context));
  FunctionType* indirectBranchTy = FunctionType::get(
      Type::getVoidTy(context), indirectBranchArgs, false);
  engine_->addGlobalMapping(Function::Create(
      indirectBranchTy, Function::ExternalLinkage, "XeIndirectBranch",
      gen_module_.get()), (void*)&XeIndirectBranch);

  // Debugging methods:
  std::vector<Type*> invalidInstructionArgs;
  invalidInstructionArgs.push_back(int8PtrTy);
  invalidInstructionArgs.push_back(Type::getInt32Ty(context));
  invalidInstructionArgs.push_back(Type::getInt32Ty(context));
  FunctionType* invalidInstructionTy = FunctionType::get(
      Type::getVoidTy(context), invalidInstructionArgs, false);
  engine_->addGlobalMapping(Function::Create(
      invalidInstructionTy, Function::ExternalLinkage, "XeInvalidInstruction",
      gen_module_.get()), (void*)&XeInvalidInstruction);

  // Tracing methods:
  std::vector<Type*> traceCallArgs;
  traceCallArgs.push_back(int8PtrTy);
  traceCallArgs.push_back(Type::getInt64Ty(context));
  traceCallArgs.push_back(Type::getInt64Ty(context));
  traceCallArgs.push_back(Type::getInt64Ty(context));
  FunctionType* traceCallTy = FunctionType::get(
      Type::getVoidTy(context), traceCallArgs, false);
  std::vector<Type*> traceInstructionArgs;
  traceInstructionArgs.push_back(int8PtrTy);
  traceInstructionArgs.push_back(Type::getInt32Ty(context));
  traceInstructionArgs.push_back(Type::getInt32Ty(context));
  FunctionType* traceInstructionTy = FunctionType::get(
      Type::getVoidTy(context), traceInstructionArgs, false);

  engine_->addGlobalMapping(Function::Create(
      traceCallTy, Function::ExternalLinkage, "XeTraceKernelCall",
      gen_module_.get()), (void*)&XeTraceKernelCall);
  engine_->addGlobalMapping(Function::Create(
      traceCallTy, Function::ExternalLinkage, "XeTraceUserCall",
      gen_module_.get()), (void*)&XeTraceUserCall);
  engine_->addGlobalMapping(Function::Create(
      traceInstructionTy, Function::ExternalLinkage, "XeTraceInstruction",
      gen_module_.get()), (void*)&XeTraceInstruction);

  return 0;
}

int ExecModule::Init() {
  // Setup all kernel variables.
  std::vector<VariableSymbol*> variables;
  if (sdb_->GetAllVariables(variables)) {
    return 1;
  }
  uint8_t* mem = xe_memory_addr(memory_, 0);
  for (std::vector<VariableSymbol*>::iterator it = variables.begin();
       it != variables.end(); ++it) {
    VariableSymbol* var = *it;
    if (!var->kernel_export) {
      continue;
    }
    KernelExport* kernel_export = var->kernel_export;

    // Grab, if available.
    uint32_t* slot = (uint32_t*)(mem + var->address);
    if (kernel_export->type == KernelExport::Function) {
      // Not exactly sure what this should be...
      // TODO(benvanik): find out what import variables are.
    } else {
      if (kernel_export->is_implemented) {
        // Implemented - replace with pointer.
        *slot = XESWAP32BE(kernel_export->variable_ptr);
      } else {
        // Not implemented - write with a dummy value.
        *slot = XESWAP32BE(0xDEADBEEF);
        XELOGCPU(XT("WARNING: imported a variable with no value: %s"),
                 kernel_export->name);
      }
    }
  }

  // Run static initializers. I'm not sure we'll have any, but who knows.
  engine_->runStaticConstructorsDestructors(gen_module_.get(), false);

  // Grab the init function and call it.
  Function* xe_module_init = gen_module_->getFunction("xe_module_init");
  std::vector<GenericValue> args;
  GenericValue ret = engine_->runFunction(xe_module_init, args);

  return static_cast<int>(ret.IntVal.getSExtValue());
}

int ExecModule::Uninit() {
  // Grab function and call it.
  Function* xe_module_uninit = gen_module_->getFunction("xe_module_uninit");
  std::vector<GenericValue> args;
  engine_->runFunction(xe_module_uninit, args);

  // Run static destructors.
  engine_->runStaticConstructorsDestructors(gen_module_.get(), true);

  return 0;
}

void ExecModule::Dump() {
  sdb_->Dump();
}