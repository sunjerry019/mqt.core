/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "mlir/Compiler/Target.h"
#include "mlir/Conversion/QCOToQC/QCOToQC.h"
#include "mlir/Dialect/QC/IR/QCDialect.h"
#include "mlir/Dialect/QC/Translation/TranslateQCToOpenQASM3.h"
#include "mlir/Dialect/QCO/Builder/QCOProgramBuilder.h"
#include "mlir/Dialect/QCO/IR/QCODialect.h"
#include "mlir/Dialect/QCO/IR/QCOInterfaces.h"
#include "mlir/Dialect/QCO/IR/QCOOps.h"
#include "mlir/Dialect/QCO/Transforms/Mapping/Mapping.h"
#include "mlir/Dialect/QCO/Transforms/Passes.h"
#include "mlir/Dialect/QTensor/IR/QTensorDialect.h"
#include "mlir/Dialect/QTensor/IR/QTensorOps.h"
#include "mlir/Dialect/Utils/Utils.h"
#include "mlir/Support/Passes.h"

#include <gtest/gtest.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/TypeSwitch.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/LogicalResult.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Diagnostics.h>
#include <mlir/IR/DialectRegistry.h>
#include <mlir/IR/Location.h>
#include <mlir/IR/OwningOpRef.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/IR/Types.h>
#include <mlir/IR/Value.h>
#include <mlir/IR/ValueRange.h>
#include <mlir/IR/Verifier.h>
#include <mlir/Parser/Parser.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Support/LLVM.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>
#include <mlir/Transforms/Passes.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace mlir;
using namespace mlir::qco;
using namespace mlir::utils;

static SmallVector<Value> getQubitValues(ValueRange values) {
  return to_vector(llvm::make_filter_range(
      values, [](Value value) { return isa<QubitType>(value.getType()); }));
}

namespace {

class RydbergIonMappingPassFixture : public testing::Test {
protected:
  void SetUp() override {
    DialectRegistry registry;
    registry.insert<QCODialect, qc::QCDialect, qtensor::QTensorDialect,
                    scf::SCFDialect, arith::ArithDialect, func::FuncDialect>();
    context = std::make_unique<MLIRContext>();
    context->appendDialectRegistry(registry);
    context->loadAllAvailableDialects();
  }

  static LogicalResult runPass(ModuleOp m, const CompilerTarget& target,
                               const MappingPassOptions& options) {
    PassManager pm(m->getContext());
    pm.addPass(createMappingPass(target, options));
    if (failed(pm.run(m))) {
      return failure();
    }

    RewritePatternSet patterns(m.getContext());
    SinkOp::getCanonicalizationPatterns(patterns, m.getContext());
    return applyPatternsGreedily(m, std::move(patterns));
  }

  std::unique_ptr<MLIRContext> context;
};

class RydbergIonMappingPassTest
    : public RydbergIonMappingPassFixture,
      public testing::WithParamInterface<CompilerTarget> {};

}; // namespace

/// Return true, if the operations within a region fulfill the given coupling
/// constraints.
///
/// Unlike `test_mapping.cpp`'s own `isExecutable`, this generalizes the
/// two-qubit-gate check to any arity: it gathers every input qubit's mapped
/// hardware site and requires every pair among them to be mutually adjacent
/// (a single pair for a two-qubit gate; a "clique"/triangle check for a
/// native three-qubit gate). This circuit never produces nested regions
/// (`scf.for`/`scf.while`/`qco.if`/`qco.index_switch`), so unlike
/// `test_mapping.cpp`'s version, this one does not need to handle them.
static bool isExecutable(Region& body,
                         DenseMap<Value, CompilerTarget::SiteId>& m,
                         const CompilerTarget& target) {
  for (Operation& op : body.getOps()) {
    if (auto staticOp = dyn_cast<StaticOp>(op)) {
      m.try_emplace(staticOp.getQubit(), staticOp.getIndex());
      continue;
    }

    if (auto unitaryOp = dyn_cast<UnitaryOpInterface>(op)) {
      if (!isa<BarrierOp>(op) && unitaryOp.getNumQubits() > 1) {
        SmallVector<CompilerTarget::SiteId, 3> sites;
        sites.reserve(unitaryOp.getNumQubits());
        for (const Value qubit : unitaryOp.getInputQubits()) {
          sites.push_back(m.at(qubit));
        }

        for (size_t i = 0; i < sites.size(); ++i) {
          for (size_t j = i + 1; j < sites.size(); ++j) {
            const auto vertexA = target.vertexForSite(sites[i]);
            const auto vertexB = target.vertexForSite(sites[j]);
            if (!vertexA || !vertexB ||
                !target.areAdjacent(*vertexA, *vertexB)) {
              llvm::dbgs() << "The multi-qubit gate (" << sites[i] << ", "
                           << sites[j] << ") is not executable: \n";
              unitaryOp->dump();
              return false;
            }
          }
        }
      }

      for (const auto [pred, succ] : llvm::zip_equal(
               unitaryOp.getInputQubits(), unitaryOp.getOutputQubits())) {
        const auto hw = m.at(pred);
        m.try_emplace(succ, hw);
      }

      continue;
    }

    if (auto resetOp = dyn_cast<ResetOp>(op)) {
      const auto hw = m.at(resetOp.getQubitIn());
      m.try_emplace(resetOp.getQubitOut(), hw);
      continue;
    }

    if (auto measOp = dyn_cast<MeasureOp>(op)) {
      const auto hw = m.at(measOp.getQubitIn());
      m.try_emplace(measOp.getQubitOut(), hw);
      continue;
    }
  }

  return true;
}

/// Return true, if the entry point fulfills the given coupling constraints.
static bool isExecutable(func::FuncOp entry, const CompilerTarget& target) {
  DenseMap<Value, CompilerTarget::SiteId> m;
  return isExecutable(entry.getFunctionBody(), m, target);
}

/// Return the 12-qubit Rydberg-ion compiler target described by the task: a
/// mostly-linear chain of sites with three triangular "native-gate zones" at
/// {2, 3, 4}, {5, 6, 7}, and {8, 9, 10}, explicitly declaring CCX and CCZ as
/// native three-qubit operations.
///
/// Couplings: 0<>1, 1<>2, 2<>3, 2<>4, 3<>4, 4<>5, 5<>6, 5<>7, 6<>7, 7<>8,
/// 8<>9, 8<>10, 9<>10, 10<>11.
static CompilerTarget getRydbergIonTarget() {
  constexpr size_t numQubits = 12;
  const std::vector<CompilerTarget::Coupling> couplings{
      {0, 1}, {1, 2}, {2, 3}, {2, 4}, {3, 4},  {4, 5},  {5, 6},
      {5, 7}, {6, 7}, {7, 8}, {8, 9}, {8, 10}, {9, 10}, {10, 11},
  };
  const std::vector<CompilerTarget::Operation> operations{
      CompilerTarget::Operation("ccx", 3, 0),
      CompilerTarget::Operation("ccz", 3, 0),
  };
  return CompilerTarget(numQubits, couplings, operations);
}

//===----------------------------------------------------------------------===//
// The Bacon-Shor code QEC memory circuit, mirroring
// `find_acceptable_swaps.py`/`circuits.py`/`ftqc_circuits.py`'s
// `BaconShorCodeCircuitGenerator`: 9 data ("state") qubits encoding one
// logical qubit, and 3 QEC ancilla qubits used to extract X and Z syndromes
// and apply the corresponding corrections. Program qubits are allocated data
// qubits first, then ancilla qubits, so program index order matches the
// Python reference's `state_qubits + qec_ancillas` concatenation used for the
// canonical qubit ids there. The CCZ/CCX correction gates stay atomic
// three-qubit `qco.ctrl` operations all the way through routing; no
// decomposition pass runs anywhere in this file.
//
// This is a QEC memory round only: it never measures its own qubits (that is
// a separate concern, used only alongside some other, not-built-here
// state-preparation circuit to check data integrity before/after a memory
// round), so this file's qubits are always sunk directly, unmeasured.
//===----------------------------------------------------------------------===//

namespace {

/// The X-type gauge/stabilizer generators (eqn. Sx_1, Sx_2, Sx_3), indexed by
/// the ancilla each acts through, listing the data qubits it targets.
constexpr std::array<std::array<int64_t, 6>, 3> kSx{{
    {0, 1, 3, 4, 6, 7},
    {1, 2, 4, 5, 7, 8},
    {0, 2, 3, 5, 6, 8},
}};

/// The Z-type gauge/stabilizer generators (eqn. Sz_1, Sz_2, Sz_3), indexed by
/// the ancilla each acts through, listing the data qubits that control it.
constexpr std::array<std::array<int64_t, 6>, 3> kSz{{
    {0, 3, 1, 4, 2, 5},
    {3, 6, 4, 7, 5, 8},
    {0, 6, 1, 7, 2, 8},
}};

/// Tracks the current SSA values of the Bacon-Shor code's 9 data qubits and 3
/// QEC ancilla qubits as gates are appended.
struct BaconShorCircuit {
  SmallVector<Value> dataQubits = SmallVector<Value>(9);
  SmallVector<Value> ancillaQubits = SmallVector<Value>(3);

  /// The qubit values immediately after allocation, before any gate has
  /// consumed them. `buildBaconShorCircuit` never reads these back; they
  /// exist purely as test instrumentation so that
  /// `buildAndFinalizeBaconShorProgram` can locate, for each program qubit, the
  /// very first operation that consumes it (see `QubitTrace` below).
  SmallVector<Value> initialDataQubits = SmallVector<Value>(9);
  SmallVector<Value> initialAncillaQubits = SmallVector<Value>(3);
};

/// Mirrors `BaconShorCodeCircuitGenerator.x_syndrome_circuit`.
void buildXSyndromeCircuit(QCOProgramBuilder& builder,
                           BaconShorCircuit& circuit) {
  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.reset(anc);
    anc = builder.h(anc);
  }

  for (size_t control = 0; control < kSx.size(); ++control) {
    for (const int64_t target : kSx[control]) {
      std::tie(circuit.ancillaQubits[control],
               circuit.dataQubits[static_cast<size_t>(target)]) =
          builder.cx(circuit.ancillaQubits[control],
                     circuit.dataQubits[static_cast<size_t>(target)]);
    }
  }

  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.h(anc);
  }
}

/// Mirrors `BaconShorCodeCircuitGenerator.correct_x_circuit`: applies a CCZ
/// (as a two-controlled Z) with two ancilla controls and a data-qubit target
/// for each of the three X-correction triples. CCZ is symmetric under
/// permutation of its three qubits, so the control/target split below is
/// equivalent to the Python reference's unordered `ccz(...)` triples.
void buildXCorrectionCircuit(QCOProgramBuilder& builder,
                             BaconShorCircuit& circuit) {
  const auto ccz = [&](size_t dataIdx, size_t anc0, size_t anc1) {
    auto [controlsOut, targetOut] =
        builder.mcz({circuit.ancillaQubits[anc0], circuit.ancillaQubits[anc1]},
                    circuit.dataQubits[dataIdx]);
    circuit.ancillaQubits[anc0] = controlsOut[0];
    circuit.ancillaQubits[anc1] = controlsOut[1];
    circuit.dataQubits[dataIdx] = targetOut;
  };

  ccz(7, 0, 1);
  ccz(6, 0, 2);
  ccz(5, 1, 2);
}

/// Mirrors `BaconShorCodeCircuitGenerator.z_syndrome_circuit`.
void buildZSyndromeCircuit(QCOProgramBuilder& builder,
                           BaconShorCircuit& circuit) {
  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.reset(anc);
  }

  for (size_t target = 0; target < kSz.size(); ++target) {
    for (const int64_t control : kSz[target]) {
      std::tie(circuit.dataQubits[static_cast<size_t>(control)],
               circuit.ancillaQubits[target]) =
          builder.cx(circuit.dataQubits[static_cast<size_t>(control)],
                     circuit.ancillaQubits[target]);
    }
  }
}

/// Mirrors `BaconShorCodeCircuitGenerator.correct_z_circuit`: applies a CCX
/// (Toffoli) with two ancilla controls and a data-qubit target for each of
/// the three Z-correction triples, matching the Python reference's
/// `ccx(ancilla, ancilla, data)` argument order exactly, since CCX
/// distinguishes its target from its controls.
void buildZCorrectionCircuit(QCOProgramBuilder& builder,
                             BaconShorCircuit& circuit) {
  const auto ccx = [&](size_t anc0, size_t anc1, size_t dataIdx) {
    auto [controlsOut, targetOut] =
        builder.mcx({circuit.ancillaQubits[anc0], circuit.ancillaQubits[anc1]},
                    circuit.dataQubits[dataIdx]);
    circuit.ancillaQubits[anc0] = controlsOut[0];
    circuit.ancillaQubits[anc1] = controlsOut[1];
    circuit.dataQubits[dataIdx] = targetOut;
  };

  ccx(0, 1, 5);
  ccx(0, 2, 2);
  ccx(1, 2, 8);

  for (Value& anc : circuit.ancillaQubits) {
    anc = builder.reset(anc);
  }
}

/// Builds the full Bacon-Shor QEC round: X-syndrome extraction, X-correction,
/// Z-syndrome extraction, Z-correction, mirroring
/// `find_acceptable_swaps.py`'s `full_circuit = x_syndrome_circuit +
/// correct_x_circuit + z_syndrome_circuit + correct_z_circuit`.
BaconShorCircuit buildBaconShorCircuit(QCOProgramBuilder& builder) {
  BaconShorCircuit circuit;
  for (size_t i = 0; i < circuit.dataQubits.size(); ++i) {
    circuit.dataQubits[i] = circuit.initialDataQubits[i] = builder.allocQubit();
  }
  for (size_t i = 0; i < circuit.ancillaQubits.size(); ++i) {
    circuit.ancillaQubits[i] = circuit.initialAncillaQubits[i] =
        builder.allocQubit();
  }

  buildXSyndromeCircuit(builder, circuit);
  buildXCorrectionCircuit(builder, circuit);
  buildZSyndromeCircuit(builder, circuit);
  buildZCorrectionCircuit(builder, circuit);

  return circuit;
}

/// Traces a single named Bacon-Shor program qubit (one of the 9 data qubits
/// or 3 QEC ancillas) through the mapping pass, so its physical site can be
/// recovered before and after routing without reaching into the pass's
/// internal `Layout`.
///
/// The anchors are two operations that the mapping pass never erases or
/// replaces (`place()` only erases `AllocQubitOp`/`ExtractOp`/`InsertOp`/
/// `DeallocOp`, and routing only rewires *operands* via
/// `replaceAllUsesExcept`, never the surviving gate `Operation*`s
/// themselves): the gate that first consumes the qubit right after
/// allocation, and the `qco.measure` that consumes it last (added by
/// `buildAndFinalizeBaconShorProgram`). After the pass has run, re-reading
/// the anchor's current operand and looking that value up in the site map
/// `isExecutable` builds recovers the qubit's site at that point in time.
struct QubitTrace {
  std::string name;
  Operation* firstUser = nullptr;
  unsigned firstOperandIdx = 0;
  Operation* lastUser = nullptr;
  unsigned lastOperandIdx = 0;
};

/// A finalized Bacon-Shor program together with a `QubitTrace` per program
/// qubit (data qubits first, then ancillas), for post-routing analysis.
struct BaconShorProgram {
  OwningOpRef<ModuleOp> module;
  SmallVector<QubitTrace, 12> traces;
};

/// Build the full Bacon-Shor QEC round on a fresh `QCOProgramBuilder`, then
/// measure and sink all 12 qubits, returning the finalized module together
/// with the classical bit values.
///
/// The measurement here is test instrumentation, not part of the Bacon-Shor
/// circuit's own semantics (`buildBaconShorCircuit` above never measures
/// anything, matching the Python reference exactly): without it, the whole
/// computation has no externally observable effect (QCO's gates are `Pure`,
/// and nothing else consumes the classical return value), so the fixture's
/// `runPass` helper's post-pass `SinkOp` canonicalization cleanup legitimately
/// treats the entire circuit — every gate, and any SWAPs the mapping pass
/// inserted — as dead code and erases all of it, silently turning every
/// assertion below into a vacuous pass over an empty function. Measuring
/// forces the routed gates to remain observable so the tests below actually
/// exercise them.
static BaconShorProgram buildAndFinalizeBaconShorProgram(MLIRContext* ctx) {
  QCOProgramBuilder builder(ctx);
  constexpr int64_t numQubits = 12;
  builder.initialize(SmallVector<Type>(numQubits, builder.getI1Type()));

  BaconShorCircuit circuit = buildBaconShorCircuit(builder);

  // Anchor each program qubit's *first* user now, while its initial,
  // freshly allocated value still has exactly one use (the whole circuit
  // is already built at this point, just not yet measured/finalized).
  SmallVector<QubitTrace, 12> traces;
  const auto traceFirstUse = [](const std::string& name, Value initial) {
    assert(initial.hasOneUse() &&
           "expected a freshly allocated qubit to have exactly one use");
    OpOperand& use = *initial.getUses().begin();
    return QubitTrace{name, use.getOwner(), use.getOperandNumber(), nullptr, 0};
  };
  for (size_t i = 0; i < circuit.initialDataQubits.size(); ++i) {
    traces.push_back(traceFirstUse("data" + std::to_string(i),
                                   circuit.initialDataQubits[i]));
  }
  for (size_t i = 0; i < circuit.initialAncillaQubits.size(); ++i) {
    traces.push_back(traceFirstUse("ancilla" + std::to_string(i),
                                   circuit.initialAncillaQubits[i]));
  }

  // Separate the routed circuit from the dummy instrumentation measurements
  // with a barrier, so the two are visually distinguishable in the OpenQASM3
  // dump and the mapping pass cannot reorder circuit gates across the
  // measurement boundary.
  SmallVector<Value> preMeasureQubits(circuit.dataQubits.begin(),
                                      circuit.dataQubits.end());
  preMeasureQubits.append(circuit.ancillaQubits.begin(),
                          circuit.ancillaQubits.end());
  const ValueRange barriered = builder.barrier(preMeasureQubits);
  std::copy_n(barriered.begin(), circuit.dataQubits.size(),
              circuit.dataQubits.begin());
  std::copy_n(barriered.begin() + circuit.dataQubits.size(),
              circuit.ancillaQubits.size(), circuit.ancillaQubits.begin());

  SmallVector<Value> bits(numQubits);
  for (size_t i = 0; i < circuit.dataQubits.size(); ++i) {
    std::tie(circuit.dataQubits[i], bits[i]) =
        builder.measure(circuit.dataQubits[i]);
    traces[i].lastUser = bits[i].getDefiningOp();
  }
  for (size_t i = 0; i < circuit.ancillaQubits.size(); ++i) {
    std::tie(circuit.ancillaQubits[i], bits[9 + i]) =
        builder.measure(circuit.ancillaQubits[i]);
    traces[9 + i].lastUser = bits[9 + i].getDefiningOp();
  }
  for (const Value q : circuit.dataQubits) {
    builder.sink(q);
  }
  for (const Value q : circuit.ancillaQubits) {
    builder.sink(q);
  }

  return BaconShorProgram{builder.finalize(bits), std::move(traces)};
}

/// Trace a qubit operand back to the physical site its `qco.static` was
/// created with, i.e. the site the mapping pass's initial layout actually
/// assigned to it, unaffected by any routing decision.
///
/// A qubit's very first real (non-`qco.static`) use may be preceded by a
/// `qco.swap` the router inserted purely to satisfy adjacency for that same
/// gate, so `v` is not necessarily the qubit's `qco.static` value itself.
/// Walking backward from `v` therefore has to follow *identity*, not site:
/// `insertSWAPs` (`Mapping.cpp`) wires a swap's first result to continue
/// whichever logical qubit its *second* input represented, and vice versa
/// (`replaceAllUsesExcept(in0, out1, ...)` / `replaceAllUsesExcept(in1,
/// out0, ...)`), so recovering "this same logical qubit, one step earlier"
/// means crossing to the *other* input at each swap — the opposite of the
/// same-slot, site-preserving traversal `isExecutable`'s site map uses.
static CompilerTarget::SiteId traceToInitialSite(Value v) {
  while (auto swap = dyn_cast_or_null<SWAPOp>(v.getDefiningOp())) {
    v = v == swap.getQubit0Out() ? swap.getQubit1In() : swap.getQubit0In();
  }
  return cast<StaticOp>(v.getDefiningOp()).getIndex();
}

/// Lower a routed QCO module to portable OpenQASM3 on a throwaway clone, so
/// the resulting program can be pasted into any OpenQASM3-capable visualizer
/// to inspect the router's actual placement/routing decisions.
///
/// The clone (not `m` itself) is converted so that `m` survives, still in QCO
/// form, for the QCO-specific assertions (`numSwaps`,
/// `numNativeMultiQubitGates`) that run after the dump. `qco.static`'s `index`
/// becomes `qc.static`'s unchanged across `QCOToQC` (see `ConvertQCOStaticOp`),
/// and `translateQCToOpenQASM3` renders every `qc.static` qubit as an OpenQASM3
/// hardware-qubit reference (`$N`), so the emitted program's qubit references
/// are exactly the physical sites `MappingPass` assigned — including every
/// inserted `qco.swap`, which lowers to an ordinary `swap $a, $b;` gate call
/// alongside the rest of the circuit.
static FailureOr<std::string> routedProgramToOpenQASM3(ModuleOp m) {
  OwningOpRef<ModuleOp> qcModule(cast<ModuleOp>(m->clone()));
  PassManager pm(qcModule->getContext());
  pm.addPass(createQCOToQC());
  if (failed(pm.run(*qcModule)) || failed(verify(*qcModule))) {
    return failure();
  }
  return qc::translateQCToOpenQASM3(*qcModule);
}

/// Print the fully routed program (every surviving gate and every inserted
/// `qco.swap`, verbatim as MLIR and as OpenQASM3) together with each named
/// program qubit's initial and final physical site, to `os` for manual
/// analysis.
///
/// This always runs (it is not gated behind `-debug`) and is written to
/// stdout by its caller: rerunning the test after changing a
/// `MappingPassOptions` field (e.g. `qubitTypeLabels`, `alpha`, `lambda`,
/// `seed`) is meant to be enough to inspect how the routing decision
/// changes, without re-instrumenting the test by hand. The OpenQASM3 section
/// exists specifically so the routed circuit can be visualized in an external
/// tool, which the verbatim MLIR dump does not support.
static void
dumpRoutedProgram(llvm::raw_ostream& os, ModuleOp m,
                  const MappingPassOptions& options,
                  ArrayRef<QubitTrace> traces,
                  const DenseMap<Value, CompilerTarget::SiteId>& siteMap,
                  StringRef openQasm3) {
  os << "\n"
        "================================================================\n"
        "Bacon-Shor routing analysis dump\n"
        "================================================================\n"
     << "options: nlookahead=" << options.nlookahead
     << " alpha=" << options.alpha << " lambda=" << options.lambda
     << " niterations=" << options.niterations << " ntrials=" << options.ntrials
     << " seed=" << options.seed << " qubitTypeLabels=\""
     << (options.qubitTypeLabels.empty() ? "<default>"
                                         : options.qubitTypeLabels)
     << "\"\n\n"
     << "--- routed program (gates + inserted qco.swap ops) ---\n";
  m.print(os);
  os << "\n\n--- program qubit -> physical site (initial -> final) ---\n";
  for (const QubitTrace& trace : traces) {
    const auto initialSite =
        traceToInitialSite(trace.firstUser->getOperand(trace.firstOperandIdx));
    const auto finalSite =
        siteMap.at(trace.lastUser->getOperand(trace.lastOperandIdx));
    os << "  " << trace.name << ": " << initialSite << " -> " << finalSite
       << "\n";
  }
  os << "\n--- routed program as OpenQASM3 (paste into any OpenQASM3 "
        "visualizer; physical sites are hardware-qubit references, e.g. "
        "$7) ---\n"
     << openQasm3
     << "================================================================\n";
  // Flush immediately: `os` (llvm::outs()) buffers independently of gtest's
  // own stdio-based progress output, so without an explicit flush here the
  // two can interleave out of call order once stdout is redirected to a
  // file (stdio switches from line- to full-buffering off a tty).
  os.flush();
}

} // namespace

TEST_F(RydbergIonMappingPassFixture, MapBaconShorCodeOnRydbergIonTarget) {
  const auto target = getRydbergIonTarget();

  auto program = buildAndFinalizeBaconShorProgram(context.get());
  auto& m = program.module;
  ASSERT_TRUE(succeeded(verify(*m)));

  // Label the 9 data qubits "B" (data) and the 3 QEC ancillas "A"
  // (auxiliary), in program-allocation order, and route with the
  // state-dependent A/B swap heuristic enabled. The CCZ/CCX gates are native
  // on this target (see getRydbergIonTarget), so no decomposition pass runs
  // here at all.
  const std::string qubitTypeLabels = std::string(9, 'B') + std::string(3, 'A');
  const MappingPassOptions options{.ntrials = 1,
                                   .qubitTypeLabels = qubitTypeLabels};
  ASSERT_TRUE(runPass(m.get(), target, options).succeeded());
  ASSERT_TRUE(succeeded(verify(*m)));

  DenseMap<Value, CompilerTarget::SiteId> siteMap;
  EXPECT_TRUE(
      isExecutable(getEntryPoint(m.get()).getFunctionBody(), siteMap, target));

  const FailureOr<std::string> openQasm3 = routedProgramToOpenQASM3(m.get());
  ASSERT_TRUE(succeeded(openQasm3));

  // Always printed to stdout (not gated behind -debug): shows the fully
  // routed circuit plus each program qubit's initial/final physical site, so
  // that rerunning with different `MappingPassOptions` (e.g. `qubitTypeLabels`,
  // `alpha`, `lambda`, `seed`) can be inspected without extra instrumentation.
  dumpRoutedProgram(llvm::outs(), m.get(), options, program.traces, siteMap,
                    *openQasm3);

  size_t numSwaps = 0;
  m->walk([&](SWAPOp) { ++numSwaps; });
  EXPECT_GT(numSwaps, 0U);

  // `builder.cx`/`cz` are themselves built as a `CtrlOp` with one control
  // (see `CompilerTarget::supports`'s one-control special case), so only
  // count `CtrlOp`s with two controls (the CCX/CCZ shape) here, not every
  // `CtrlOp` in the module.
  size_t numNativeMultiQubitGates = 0;
  m->walk([&](CtrlOp ctrl) {
    if (ctrl.getNumControls() != 2) {
      return;
    }
    ++numNativeMultiQubitGates;
    EXPECT_EQ(ctrl.getNumQubits(), 3U);
  });
  EXPECT_EQ(numNativeMultiQubitGates, 6U)
      << "expected all 6 CCZ/CCX gates to survive routing as undecomposed, "
         "two-control CtrlOps";
}

TEST_F(RydbergIonMappingPassFixture,
       MapBaconShorCodeOnRydbergIonTargetWithDefaultCost) {
  const auto target = getRydbergIonTarget();

  auto program = buildAndFinalizeBaconShorProgram(context.get());
  auto& m = program.module;
  ASSERT_TRUE(succeeded(verify(*m)));

  // Leaving `qubitTypeLabels` unset (the default) must still compile the
  // same circuit successfully, reproducing the pass's previous behavior.
  ASSERT_TRUE(
      runPass(m.get(), target, MappingPassOptions{.ntrials = 1}).succeeded());
  ASSERT_TRUE(succeeded(verify(*m)));
  EXPECT_TRUE(isExecutable(getEntryPoint(m.get()), target));
}
