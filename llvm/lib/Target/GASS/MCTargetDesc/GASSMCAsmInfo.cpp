#include "GASSMCAsmInfo.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

GASSMCAsmInfo::GASSMCAsmInfo(const Triple &TheTriple,
                             const MCTargetOptions &Options)
    : MCAsmInfo(Options) {
  CommentString = "//";

  // Affect how the output asm file looks like.
  HasSingleParameterDotFile  = false;
  HasDotTypeDotSizeDirective = false;
  SupportsQuotedNames = false;
  SupportsExtendedDwarfLocDirective = false;
  SupportsSignedData = false;

  AsciiDirective = nullptr; // not supported
  AscizDirective = nullptr; // not supported

  WeakDirective   = "\t// .weak\t";
  GlobalDirective = "\t// .globl\t";

  UseIntegratedAssembler = false;
}