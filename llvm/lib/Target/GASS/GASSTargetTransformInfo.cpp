#include "GASSTargetTransformInfo.h"
#include "GASS.h"
#include "llvm/IR/IntrinsicsNVPTX.h"

using namespace llvm;

#define DEBUG_TYPE "gass-tti"

static bool readsThreadId(const IntrinsicInst *II) {
  switch (II->getIntrinsicID()) {
  default: return false;
  case Intrinsic::nvvm_read_ptx_sreg_tid_x:
  case Intrinsic::nvvm_read_ptx_sreg_tid_y:
  case Intrinsic::nvvm_read_ptx_sreg_tid_z:
    return true;
  }
}

static bool readsLaneId(const IntrinsicInst *II) {
  return II->getIntrinsicID() == Intrinsic::nvvm_read_ptx_sreg_laneid;
}

// Follow the practice in NVPTX
bool GASSTTIImpl::isSourceOfDivergence(const Value *V) const {
  // We assume that arguments to __global__ functions are uniform
  if (const auto *Arg = dyn_cast<Argument>(V))
    return false;

  if (const auto *I = dyn_cast<Instruction>(V)) {
    // Without pointer analysis, we conservatively assume values loaded
    // from generic or local address space are divergent.
    if (const auto *LI = dyn_cast<LoadInst>(I)) {
      unsigned AS = LI->getPointerAddressSpace();
      return AS == GASS::GENERIC || AS == GASS::LOCAL;
    }

    // Atomic instructions may cause divergence.
    if (I->isAtomic())
      return true;
    if (const auto *II = dyn_cast<IntrinsicInst>(I)) {
      if (readsThreadId(II) || readsLaneId(II))
        return true;
      // Conservatively consider the return value of function calls as divergent.
      if (isa<CallInst>(I))
        return true;
    }
  }
  return false;
}