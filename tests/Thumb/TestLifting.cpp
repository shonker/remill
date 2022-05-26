#include <glog/logging.h>
#include <gtest/gtest.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/Interpreter.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <remill/Arch/Arch.h>
#include <remill/Arch/Name.h>
#include <remill/Arch/X86/Runtime/State.h>
#include <remill/BC/ABI.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/Optimizer.h>
#include <remill/BC/Util.h>
#include <remill/OS/OS.h>

#include <functional>
#include <random>

#include "gtest/gtest.h"


int main(int argc, char **argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}


enum TypeId { MEMORY = 0, STATE = 1 };

class LiftingTester {
 private:
  llvm::Module *semantics_module;
  remill::InstructionLifter::LifterPtr lifter;
  std::unique_ptr<remill::IntrinsicTable> table;
  remill::Arch::ArchPtr arch;

 public:
  LiftingTester(llvm::Module *semantics_module_, remill::OSName os_name,
                remill::ArchName arch_name)
      : semantics_module(semantics_module_) {
    this->arch = remill::Arch::Build(&semantics_module_->getContext(), os_name,
                                     arch_name);
    this->arch->InitFromSemanticsModule(semantics_module_);
    this->table =
        std::make_unique<remill::IntrinsicTable>(this->semantics_module);
    this->lifter = this->arch->DefaultLifter(*this->table.get());
  }

  std::unordered_map<TypeId, llvm::Type *> GetTypeMapping() {
    std::unordered_map<TypeId, llvm::Type *> res;

    auto ftype = this->arch->LiftedFunctionType();
    auto mem_type = llvm::cast<llvm::PointerType>(
        ftype->getParamType(remill::kMemoryPointerArgNum));
    auto state_type = llvm::cast<llvm::PointerType>(
        ftype->getParamType(remill::kStatePointerArgNum));


    res.emplace(TypeId::MEMORY, mem_type->getElementType());
    res.emplace(TypeId::STATE, state_type->getElementType());

    return res;
  }


  std::optional<llvm::Function *>
  LiftInstructionFunction(std::string_view fname, std::string_view bytes,
                          uint64_t address) {
    remill::Instruction insn;
    if (!this->arch->DecodeInstruction(address, bytes, insn)) {
      return std::nullopt;
    }

    auto target_func =
        this->arch->DefineLiftedFunction(fname, this->semantics_module);
    LOG(INFO) << "Func sig: "
              << remill::LLVMThingToString(target_func->getType());

    if (remill::LiftStatus::kLiftedInstruction ==
        this->lifter->LiftIntoBlock(insn, &target_func->getEntryBlock())) {
      auto mem_ptr_ref =
          remill::LoadMemoryPointerRef(&target_func->getEntryBlock());

      llvm::IRBuilder bldr(&target_func->getEntryBlock());
      bldr.CreateRet(
          bldr.CreateLoad(this->lifter->GetMemoryType(), mem_ptr_ref));

      return target_func;
    } else {
      target_func->eraseFromParent();
      return std::nullopt;
    }
  }

  const remill::Arch::ArchPtr &GetArch() {
    return this->arch;
  }
};

static constexpr auto kFlagIntrinsicPrefix = "__remill_flag_computation";


bool flag_computation_stub(bool res, ...) {
  return res;
}

class DiffModule {
 public:
  DiffModule(std::unique_ptr<llvm::Module> mod_, llvm::Function *f1_,
             llvm::Function *f2_)
      : mod(std::move(mod_)),
        f1(f1_),
        f2(f2_) {}

  llvm::Module *GetModule() {
    return this->mod.get();
  }

  llvm::Function *GetF1() {
    return this->f1;
  }

  llvm::Function *GetF2() {
    return this->f2;
  }

 private:
  std::unique_ptr<llvm::Module> mod;
  llvm::Function *f1;
  llvm::Function *f2;
};


class MappTypeRemapper : public llvm::ValueMapTypeRemapper {
 private:
  const remill::TypeMap &tmap;

 public:
  MappTypeRemapper(const remill::TypeMap &tmap_) : tmap(tmap_) {}

  virtual llvm::Type *remapType(llvm::Type *SrcTy) override {
    LOG(INFO) << "Attempting to remap: " << remill::LLVMThingToString(SrcTy);
    if (this->tmap.find(SrcTy) != this->tmap.end()) {
      return this->tmap.find(SrcTy)->second;
    }

    return SrcTy;
  }
};

void CloneFunctionWithTypeMap(llvm::Function *NewFunc, llvm::Function *OldFunc,
                              remill::TypeMap &tmap) {

  remill::ValueMap vmap;
  remill::MDMap md_map;
  remill::CloneFunctionInto(OldFunc, NewFunc, vmap, tmap, md_map);
}

class DifferentialModuleBuilder {
 public:
  static DifferentialModuleBuilder
  Create(remill::OSName os_name_1, remill::ArchName arch_name_1,
         remill::OSName os_name_2, remill::ArchName arch_name_2) {
    // it is expected that compatible arches share a semantics module.
    std::unique_ptr<llvm::LLVMContext> context =
        std::make_unique<llvm::LLVMContext>();
    auto tmp_arch = remill::Arch::Build(context.get(), os_name_1, arch_name_1);
    auto semantics_module = remill::LoadArchSemantics(tmp_arch.get());
    tmp_arch->PrepareModule(semantics_module);
    auto l1 = LiftingTester(semantics_module.get(), os_name_1, arch_name_1);
    auto l2 = LiftingTester(semantics_module.get(), os_name_2, arch_name_2);

    return DifferentialModuleBuilder(std::move(context),
                                     std::move(semantics_module), std::move(l1),
                                     std::move(l2));
  }

 private:
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> semantics_module;

  LiftingTester l1;
  LiftingTester l2;

  DifferentialModuleBuilder(std::unique_ptr<llvm::LLVMContext> context_,
                            std::unique_ptr<llvm::Module> semantics_module_,

                            LiftingTester l1_, LiftingTester l2_)
      : context(std::move(context_)),
        semantics_module(std::move(semantics_module_)),
        l1(std::move(l1_)),
        l2(std::move(l2_)) {}

 public:
  DiffModule build(std::string_view fname_f1, std::string_view fname_f2,
                   std::string_view bytes, uint64_t address) {
    auto module = std::make_unique<llvm::Module>("", *this->context);
    auto f1 = *this->l1.LiftInstructionFunction(fname_f1, bytes, address);
    auto f2 = *this->l2.LiftInstructionFunction(fname_f2, bytes, address);

    auto cloned = llvm::CloneModule(*f1->getParent());
    remill::OptimizeBareModule(cloned);

    auto new_f1 = llvm::Function::Create(
        f1->getFunctionType(), f1->getLinkage(), f1->getName(), module.get());
    auto new_f2 = llvm::Function::Create(
        f2->getFunctionType(), f2->getLinkage(), f2->getName(), module.get());

    remill::CloneFunctionInto(cloned->getFunction(f1->getName()), new_f1);
    remill::CloneFunctionInto(cloned->getFunction(f2->getName()), new_f2);


    return DiffModule(std::move(module), new_f1, new_f2);
  }
};

void RunDefaultOptPipeline(llvm::Module *mod) {
  // Create the analysis managers.
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  // Create the new pass manager builder.
  // Take a look at the PassBuilder constructor parameters for more
  // customization, e.g. specifying a TargetMachine or various debugging
  // options.
  llvm::PassBuilder PB;

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // Create the pass manager.
  // This one corresponds to a typical -O2 optimization pipeline.
  llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(
      llvm::PassBuilder::OptimizationLevel::O2);
}


using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, uint8_t>;


void *MissingFunctionStub(const std::string &name) {
  auto res = llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(name);
  if (res) {
    return res;
  }
  LOG(FATAL) << "Missing function: " << name;
  return nullptr;
}

extern "C" {
uint8_t ___remill_undefined_8(void) {
  return 0;
}
}


std::string PrintState(X86State *state) {}

struct DiffTestResult {
  std::string struct_dump1;
  std::string struct_dump2;
  bool are_equal;
};

class ComparisonRunner {
 private:
  random_bytes_engine rbe;

  void RandomizeState(X86State *state) {
    std::vector<uint8_t> data(sizeof(X86State));
    std::generate(begin(data), end(data), std::ref(rbe));

    std::memcpy(state, data.data(), sizeof(X86State));
  }

 private:
  void ExecuteLiftedFunction(llvm::Function *func, X86State *state) {
    std::string load_error = "";
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr, &load_error);
    if (!load_error.empty()) {
      LOG(FATAL) << "Failed to load: " << load_error;
    }

    auto symb_addr = llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(
        "___remill_undefined_8");


    EXPECT_TRUE(symb_addr != nullptr);
    auto tgt_mod = llvm::CloneModule(*func->getParent());
    tgt_mod->setTargetTriple("");
    tgt_mod->setDataLayout(llvm::DataLayout(""));
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeAllTargetMCs();

    auto res = remill::VerifyModuleMsg(tgt_mod.get());
    if (res.has_value()) {
      LOG(FATAL) << *res;
    }

    llvm::EngineBuilder builder(std::move(tgt_mod));


    std::string estr;
    auto eptr = builder.setEngineKind(llvm::EngineKind::JIT)
                    .setErrorStr(&estr)
                    .create();

    if (eptr == nullptr) {
      LOG(FATAL) << estr;
    }

    std::unique_ptr<llvm::ExecutionEngine> engine(eptr);


    auto target = engine->FindFunctionNamed(func->getName());
    this->StubOutFlagComputationInstrinsics(target->getParent(), *engine);

    engine->InstallLazyFunctionCreator(&MissingFunctionStub);
    engine->DisableSymbolSearching(false);
    // expect traditional remill lifted insn
    assert(func->arg_size() == 3);

    void *memory = nullptr;


    auto returned =
        (void *(*) (X86State *, uint32_t, void *) ) engine->getFunctionAddress(
            target->getName().str());

    assert(returned != nullptr);
    returned(state, 0, memory);
  }

  void StubOutFlagComputationInstrinsics(llvm::Module *mod,
                                         llvm::ExecutionEngine &exec_engine) {
    for (auto &func : mod->getFunctionList()) {
      if (func.isDeclaration() &&
          func.getName().startswith(kFlagIntrinsicPrefix)) {
        exec_engine.addGlobalMapping(&func, (void *) &flag_computation_stub);
      }
    }
  }

 private:
  static void print_into_buffer(std::string &buff, const char *fmt, ...) {}

  static std::string dump_struct(X86State *st) {
    std::string buf;

    __builtin_dump_struct(st, &printf);
    return buf;
  }

 public:
  DiffTestResult SingleCmpRun(llvm::Function *f1, llvm::Function *f2) {
    auto func1_state = (X86State *) alloca(sizeof(X86State));
    RandomizeState(func1_state);
    auto func2_state = (X86State *) alloca(sizeof(X86State));

    std::memcpy(func2_state, func1_state, sizeof(X86State));

    assert(std::memcmp(func1_state, func2_state, sizeof(X86State)) == 0);

    ExecuteLiftedFunction(f1, func1_state);
    ExecuteLiftedFunction(f2, func2_state);
    LOG(INFO) << func1_state->gpr.rdx.dword;
    LOG(INFO) << func1_state->gpr.rdx.dword;
    auto are_equal =
        std::memcmp(func1_state, func2_state, sizeof(X86State)) == 0;
    return {"", "", are_equal};
  }
};


TEST(DifferentialTests, TestROR) {
  auto modulebuilder = DifferentialModuleBuilder::Create(
      remill::OSName::kOSLinux, remill::ArchName::kArchX86_SLEIGH,
      remill::OSName::kOSLinux, remill::ArchName::kArchX86);

  std::string_view insn_data("\xC1\xC8\x02", 3);

  auto diffmod = modulebuilder.build("sleigh_ror", "x86_ror", insn_data, 0);

  ComparisonRunner comp_runner;
  for (int i = 0; i < 10; i++) {
    EXPECT_TRUE(
        comp_runner.SingleCmpRun(diffmod.GetF1(), diffmod.GetF2()).are_equal);
  }
}

TEST(DifferentialTests, SimpleAddDifferenceX86) {
  auto modulebuilder = DifferentialModuleBuilder::Create(
      remill::OSName::kOSLinux, remill::ArchName::kArchX86_SLEIGH,
      remill::OSName::kOSLinux, remill::ArchName::kArchX86);

  std::string_view insn_data("\x01\xca", 2);

  auto diffmod = modulebuilder.build("sleigh_add", "x86_add", insn_data, 0);

  ComparisonRunner comp_runner;
  for (int i = 0; i < 10; i++) {
    EXPECT_TRUE(
        comp_runner.SingleCmpRun(diffmod.GetF1(), diffmod.GetF2()).are_equal);
  }
}

TEST(LiftingRegressions, AsrsFailsInContext) {

  llvm::LLVMContext curr_context;
  auto arch = remill::Arch::Build(&curr_context, remill::OSName::kOSLinux,
                                  remill::ArchName::kArchThumb2LittleEndian);
  EXPECT_NE(arch.get(), nullptr);

  remill::Instruction insn;

  std::string_view insn_data("\x00\x11", 2);
  LOG(INFO) << "string len: " << insn_data.size();
  EXPECT_TRUE(!arch->DecodeInstruction(0x12049, insn_data, insn));
}

TEST(Regressions, ASANMCjitOnAarch64SIMDPass) {}
