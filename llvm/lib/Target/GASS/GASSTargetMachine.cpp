#include "GASS.h"
#include "GASSTargetMachine.h"
#include "GASSTargetTransformInfo.h"
#include "GASSISelDAGToDAG.h"
#include "GASSSchedStrategy.h"
#include "GASSScheduleDAGMutations.h"
#include "TargetInfo/GASSTargetInfo.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/ConstantHoisting.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Vectorize/LoadStoreVectorizer.h"
#include <memory>

using namespace llvm;

static cl::opt<bool> BenchmarkMode("gass-benchmark-mode",
                                   cl::desc("Enable Benchmark Mode"),
                                   cl::init(false), cl::Hidden);

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeGASSTarget() {
  RegisterTargetMachine<GASSTargetMachine> X(getTheGASSTarget());
  auto *PR = PassRegistry::getPassRegistry();
  initializeGASSStallSettingPass(*PR);
  // initializeGASSBranchOffsetPass(*PR);
  initializeGASSExpandPreRAPseudoPass(*PR);
  initializeGASSAnnotateUniformValuesPass(*PR);
}

GASSTargetMachine::GASSTargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     const TargetOptions &Options,
                                     std::optional<Reloc::Model> RM,
                                     std::optional<CodeModel::Model> CM,
                                     CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT, CPU, FS, Options, Reloc::PIC_,
                        getEffectiveCodeModel(CM, CodeModel::Small), OL),
      Subtarget(std::make_unique<GASSSubtarget>(TT, CPU, FS, *this)),
      TLOF(std::make_unique<GASSTargetObjectFile>()) {
  // TODO: check this.
  setRequiresStructuredCFG(true);
  initAsmInfo(); // What does this do?
}

GASSTargetMachine::~GASSTargetMachine() = default;

TargetTransformInfo
GASSTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<GASSTTIImpl>(this, F));
}


//=---------------------------------------------=//
// PassConfig
//=---------------------------------------------=//
namespace {
class GASSPassConfig : public TargetPassConfig {
public:
  GASSPassConfig(GASSTargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  // Required by ISel
  GASSTargetMachine &getGASSTargetMachine() const {
    return getTM<GASSTargetMachine>();
  }

  // Major APIs
  // void addISelPasses(); // Do NOT override this
  void addMachinePasses() override;

  void addIRPasses() override;

  // Any "last minute" IR passes
  // (which are run just before instruction selector).
  bool addPreISel() override;

  bool addInstSelector() override;

  // Machine Passes
  void addMachineSSAOptimization() override;

  // GASS needs custom regalloc pipeline. (GASSIfConvert after RegisterCoalesce)
  void addOptimizedRegAlloc() override;

  // Set instruction control info
  void addPreEmitPass() override;

  // // override MachineScheduleStrategy
  // ScheduleDAGInstrs *
  // createMachineScheduler(MachineSchedContext *C) const override {
  //   ScheduleDAGMILive *DAG =
  //       new ScheduleDAGMILive(C, std::make_unique<GASSSchedStrategy>(C));
  //   DAG->addMutation(createGASSSM80DepRemoveDAGMutation());
  //   // DAG->addMutation(createGASSTensorCoreChainDAGMutation());
  //   // DAG->addMutation(createGASSCarryInClusterDAGMutation());
  //   return DAG;
  // }
};
} // anonymous namespace

TargetPassConfig *GASSTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new GASSPassConfig(*this, PM);
}

// Major API
void GASSPassConfig::addMachinePasses() {
  AddingMachinePasses = true;

  // Machine - CSE/DCE/LICM/Peephole
  addMachineSSAOptimization();

  // Debugifying the register allocator passes seems to provoke some
  // non-determinism that affects CodeGen and there doesn't seem to be a point
  // where it becomes safe again so stop debugifying here.
  DebugifyIsSafe = false;

  // Run register allocation and passes that are tightly coupled with it,
  // including phi elimination and scheduling.
  if (getOptimizeRegAlloc())
    addOptimizedRegAlloc();
  else
    addFastRegAlloc();

  // Prolog/Epilog inserter needs a TargetMachine to instantiate. But only
  // do so if it hasn't been disabled, substituted, or overridden.
  if (!isPassSubstitutedOrOverridden(&PrologEpilogCodeInserterID))
      addPass(createPrologEpilogInserterPass());

  // Expand pseudo instructions before second scheduling pass.
  addPass(&ExpandPostRAPseudosID);

  addBlockPlacement();

  // Delete IMPLICIT_DEF & Set barriers
  addPreEmitPass();

  AddingMachinePasses = false;
}

void GASSPassConfig::addIRPasses() {
  addPass(createDeadCodeEliminationPass());
  addPass(createGASSDeleteDeadPHIsPass());

  // Can be useful (following NVPTX)
  // addPass(createGVNPass());
  // or
  addPass(createEarlyCSEPass());

  addPass(createGASSAddrSpacePass()); // required by infer address space
  addPass(createSROAPass());
  addPass(createInferAddressSpacesPass());

  // LSR and other generic IR passes
  // TargetPassConfig::addIRPasses();
  // LSR leads to incorrect result in some cases (?)
  {
    addPass(createVerifierPass());

    // LSR here. We don't need them (?)
    addPass(createCanonicalizeFreezeInLoopsPass());
    // debug
    // addPass(createGASSIVDebugPass());
    // addPass(createLoopStrengthReducePass());

    // Constant intrinsics are lowered by PreISelIntrinsicLowering.
    addPass(createConstantHoistingPass());

    addPass(createUnreachableBlockEliminationPass());

    // addPass(createConstantHoistingPass());
    // Some other passes...
  }

  addPass(createGASSCodeGenPreparePass());
  addPass(createLoadStoreVectorizerPass());
  // Following the practice in AArch64
  // TODO: disable it for now.
  // addPass(createLICMPass());

  // addPass(createSinkingPass());
}

bool GASSPassConfig::addPreISel() {
  // TODO: can we eliminate this?
  // addPass(createGASSSinkingPass());
  // following the practice in AMDGPU
  // addPass(createGASSAnnotateUniformValues());
  return false;
}

bool GASSPassConfig::addInstSelector() {
  addPass(new GASSDAGToDAGISelLegacy(getGASSTargetMachine()));

  addPass(createGASSConstantMemPropagatePass());
  addPass(createGASSMachineDCEPass());

  addPass(createGASSExpandPreRAPseudoPass());
  return false;
}

// Machine passes
void GASSPassConfig::addMachineSSAOptimization() {
  // Optimize PHIs before DCE: removing dead PHI cycles may make more
  // instructions dead.
  addPass(&OptimizePHIsLegacyID);

  addPass(&EarlyMachineLICMID);
  addPass(&MachineCSELegacyID);
  addPass(&MachineSinkingLegacyID);

  // I don't understand how LLVM's peephole pass works
}

void GASSPassConfig::addOptimizedRegAlloc() {
  // Dead-instruction elimination requires machine SSA.
  addPass(&DeadMachineInstructionElimID);
  addPass(&DetectDeadLanesID);

  // TODO: what's this?
  addPass(&ProcessImplicitDefsID);

  // Live intervals must be computed before PHI elimination.
  addPass(&UnreachableMachineBlockElimID);
  addPass(&LiveIntervalsID);

  // Edge splitting is smarter with machine loop info.
  addPass(&MachineLoopInfoID);
  addPass(&PHIEliminationID);

  addPass(&TwoAddressInstructionPassID);
  addPass(&RegisterCoalescerID); // Standard

  // The machine scheduler may accidentally create disconnected components
  // when moving subregister definitions around, avoid this by splitting them to
  // separate vregs before. Splitting can also improve reg. allocation quality.
  addPass(&RenameIndependentSubregsID);

  // addPass(&MachineFunctionPrinterPassID);

  //==***************** GASS specific *************************==//
  // PreRA IfConvert
  addPass(createGASSIfConversionPass());
  //==*********************************************************==//

  // PreRA instruction scheduling.
  addPass(&MachineSchedulerID);

  addPass(createGASSMarkUndeadPass());
  // addPass(createGASSIVDebugPass());

  // // Compute Register Pressure at each line
  // addPass(createRegPressureComputePass());
  if (addRegAssignAndRewriteOptimized()) {
    // Allow targets to expand pseudo instructions depending on the choice of
    // registers before MachineCopyPropagation.
    addPostRewrite();

    // Copy propagate to forward register uses and try to eliminate COPYs that
    // were not coalesced.
    addPass(&MachineCopyPropagationID);

    // Run post-ra machine LICM to hoist reloads / remats.
    //
    // FIXME: can this move into MachineLateOptimization?
    addPass(&MachineLICMID);
  }
}


// NVGPU specific passes
void GASSPassConfig::addPreEmitPass() {
  // This pass changes IMPLICT_DEF to NOP
  addPass(createGASSPreEmitPreparePass());
  // A schedule pass to sink LDGs
  // addPass(createGASSLDGSinkPass());
  // Debug pass
  // addPass(createGASSMachineFunctionCFGPrinterPass());
  // addPass(createGASSPhysRegLivenessPass());
  addPass(createGASSBarrierSettingPass());
  // StallSetting here (called by GASSAsmPrinter)
}
