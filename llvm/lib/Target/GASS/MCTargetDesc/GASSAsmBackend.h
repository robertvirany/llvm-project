#ifndef LLVM_LIB_TARGET_GASS_MCTARGETDESC_GASSASMBACKEND_H
#define LLVM_LIB_TARGET_GASS_MCTARGETDESC_GASSASMBACKEND_H

#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"

namespace llvm {
class GASSAsmBackend : public MCAsmBackend {
  uint8_t OSABI;
public:
  GASSAsmBackend(uint8_t OSABI)
    : MCAsmBackend(endianness::little),
      OSABI(OSABI) {}

  //=-------Override pure virtual functions---------=//
  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override;

  void applyFixup(const MCFragment &, const MCFixup &,
                  const MCValue &, uint8_t *, uint64_t,
                  bool) override {}

  bool fixupNeedsRelaxationAdvanced(const MCFragment &, const MCFixup &,
                                    const MCValue &, uint64_t,
                                    bool) const override {
    return false;
  }

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override {
    return Count == 0;
  }

  std::unique_ptr<MCObjectWriter>
  createObjectWriter(raw_pwrite_stream &OS) const override;
};

// createObjectWriter
std::unique_ptr<MCObjectWriter>
createCubinObjectWriter(std::unique_ptr<MCELFObjectTargetWriter> MOTW,
                        raw_pwrite_stream &OS, bool ISLittleEndian);
}

#endif