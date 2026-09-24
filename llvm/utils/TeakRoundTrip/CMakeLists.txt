# Exhaustive round-trip check of the Teak assembler against the teakra
# disassembler (see teak-roundtrip.cpp and test/MC/Teak/roundtrip-all.test).
set(CMAKE_CXX_STANDARD 17)
set(TEAK_ASMPARSER_DIR ${LLVM_MAIN_SRC_DIR}/lib/Target/Teak/AsmParser)
include_directories(${TEAK_ASMPARSER_DIR})

add_llvm_utility(teak-roundtrip
  teak-roundtrip.cpp
  ${TEAK_ASMPARSER_DIR}/disassembler.cpp
  )
