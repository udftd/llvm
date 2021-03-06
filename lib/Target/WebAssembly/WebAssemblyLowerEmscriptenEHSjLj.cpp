//=== WebAssemblyLowerEmscriptenEHSjLj.cpp - Lower exceptions for Emscripten =//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file lowers exception-related instructions in order to use
/// Emscripten's JavaScript try and catch mechanism to handle exceptions.
///
/// To handle exceptions, this scheme relies on JavaScript's try and catch
/// syntax and relevant exception-related libraries implemented in JavaScript
/// glue code that will be produced by Emscripten. This is similar to the
/// current Emscripten asm.js exception handling in fastcomp.
/// For fastcomp's EH scheme, see these files in fastcomp LLVM branch:
/// (Location: https://github.com/kripken/emscripten-fastcomp)
/// lib/Target/JSBackend/NaCl/LowerEmExceptionsPass.cpp
/// lib/Target/JSBackend/JSBackend.cpp
/// lib/Target/JSBackend/CallHandlers.h
///
/// This pass does following things:
///
/// 1) Create three global variables: __THREW__, __threwValue, and tempRet0.
///    tempRet0 will be set within __cxa_find_matching_catch() function in
///    JS library, and __THREW__ and __threwValue will be set in invoke wrappers
///    in JS glue code. For what invoke wrappers are, refer to 3).
///
/// 2) Create setThrew and setTempRet0 functions.
///    The global variables created in 1) will exist in wasm address space,
///    but their values should be set in JS code, so we provide these functions
///    as interfaces to JS glue code. These functions are equivalent to the
///    following JS functions, which actually exist in asm.js version of JS
///    library.
///
///    function setThrew(threw, value) {
///      if (__THREW__ == 0) {
///        __THREW__ = threw;
///        __threwValue = value;
///      }
///    }
///
///    function setTempRet0(value) {
///      tempRet0 = value;
///    }
///
/// 3) Lower
///      invoke @func(arg1, arg2) to label %invoke.cont unwind label %lpad
///    into
///      __THREW__ = 0;
///      call @invoke_SIG(func, arg1, arg2)
///      %__THREW__.val = __THREW__;
///      __THREW__ = 0;
///      br %__THREW__.val, label %lpad, label %invoke.cont
///    SIG is a mangled string generated based on the LLVM IR-level function
///    signature. After LLVM IR types are lowered to the target wasm types,
///    the names for these wrappers will change based on wasm types as well,
///    as in invoke_vi (function takes an int and returns void). The bodies of
///    these wrappers will be generated in JS glue code, and inside those
///    wrappers we use JS try-catch to generate actual exception effects. It
///    also calls the original callee function. An example wrapper in JS code
///    would look like this:
///      function invoke_vi(index,a1) {
///        try {
///          Module["dynCall_vi"](index,a1); // This calls original callee
///        } catch(e) {
///          if (typeof e !== 'number' && e !== 'longjmp') throw e;
///          asm["setThrew"](1, 0); // setThrew is called here
///        }
///      }
///    If an exception is thrown, __THREW__ will be set to true in a wrapper,
///    so we can jump to the right BB based on this value.
///
/// 4) Lower
///      %val = landingpad catch c1 catch c2 catch c3 ...
///      ... use %val ...
///    into
///      %fmc = call @__cxa_find_matching_catch_N(c1, c2, c3, ...)
///      %val = {%fmc, tempRet0}
///      ... use %val ...
///    Here N is a number calculated based on the number of clauses.
///    Global variable tempRet0 is set within __cxa_find_matching_catch() in
///    JS glue code.
///
/// 5) Lower
///      resume {%a, %b}
///    into
///      call @__resumeException(%a)
///    where __resumeException() is a function in JS glue code.
///
/// 6) Lower
///      call @llvm.eh.typeid.for(type) (intrinsic)
///    into
///      call @llvm_eh_typeid_for(type)
///    llvm_eh_typeid_for function will be generated in JS glue code.
///
///===----------------------------------------------------------------------===//

#include "WebAssembly.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

using namespace llvm;

#define DEBUG_TYPE "wasm-lower-em-ehsjlj"

static cl::list<std::string>
    EHWhitelist("emscripten-cxx-exceptions-whitelist",
                cl::desc("The list of function names in which Emscripten-style "
                         "exception handling is enabled (see emscripten "
                         "EMSCRIPTEN_CATCHING_WHITELIST options)"),
                cl::CommaSeparated);

namespace {
class WebAssemblyLowerEmscriptenEHSjLj final : public ModulePass {
  static const char *ThrewGVName;
  static const char *ThrewValueGVName;
  static const char *TempRet0GVName;
  static const char *ResumeFName;
  static const char *EHTypeIDFName;
  static const char *SetThrewFName;
  static const char *SetTempRet0FName;
  static const char *FindMatchingCatchPrefix;
  static const char *InvokePrefix;

  bool DoEH;   // Enable exception handling
  bool DoSjLj; // Enable setjmp/longjmp handling

  GlobalVariable *ThrewGV;
  GlobalVariable *ThrewValueGV;
  GlobalVariable *TempRet0GV;
  Function *ResumeF;
  Function *EHTypeIDF;
  // __cxa_find_matching_catch_N functions.
  // Indexed by the number of clauses in an original landingpad instruction.
  DenseMap<int, Function *> FindMatchingCatches;
  // Map of <function signature string, invoke_ wrappers>
  StringMap<Function *> InvokeWrappers;
  // Set of whitelisted function names for exception handling
  std::set<std::string> EHWhitelistSet;

  const char *getPassName() const override {
    return "WebAssembly Lower Emscripten Exceptions";
  }

  bool runEHOnFunction(Function &F);
  bool runSjLjOnFunction(Function &F);
  // Returns __cxa_find_matching_catch_N function, where N = NumClauses + 2.
  // This is because a landingpad instruction contains two more arguments,
  // a personality function and a cleanup bit, and __cxa_find_matching_catch_N
  // functions are named after the number of arguments in the original
  // landingpad instruction.
  Function *getFindMatchingCatch(Module &M, unsigned NumClauses);

  Function *getInvokeWrapper(Module &M, InvokeInst *II);
  bool areAllExceptionsAllowed() const { return EHWhitelistSet.empty(); }

public:
  static char ID;

  WebAssemblyLowerEmscriptenEHSjLj(bool DoEH = true, bool DoSjLj = true)
      : ModulePass(ID), DoEH(DoEH), DoSjLj(DoSjLj), ThrewGV(nullptr),
        ThrewValueGV(nullptr), TempRet0GV(nullptr), ResumeF(nullptr),
        EHTypeIDF(nullptr) {
    EHWhitelistSet.insert(EHWhitelist.begin(), EHWhitelist.end());
  }
  bool runOnModule(Module &M) override;
};
} // End anonymous namespace

const char *WebAssemblyLowerEmscriptenEHSjLj::ThrewGVName = "__THREW__";
const char *WebAssemblyLowerEmscriptenEHSjLj::ThrewValueGVName = "__threwValue";
const char *WebAssemblyLowerEmscriptenEHSjLj::TempRet0GVName = "__tempRet0";
const char *WebAssemblyLowerEmscriptenEHSjLj::ResumeFName = "__resumeException";
const char *WebAssemblyLowerEmscriptenEHSjLj::EHTypeIDFName =
    "llvm_eh_typeid_for";
const char *WebAssemblyLowerEmscriptenEHSjLj::SetThrewFName = "setThrew";
const char *WebAssemblyLowerEmscriptenEHSjLj::SetTempRet0FName = "setTempRet0";
const char *WebAssemblyLowerEmscriptenEHSjLj::FindMatchingCatchPrefix =
    "__cxa_find_matching_catch_";
const char *WebAssemblyLowerEmscriptenEHSjLj::InvokePrefix = "__invoke_";

char WebAssemblyLowerEmscriptenEHSjLj::ID = 0;
INITIALIZE_PASS(WebAssemblyLowerEmscriptenEHSjLj, DEBUG_TYPE,
                "WebAssembly Lower Emscripten Exceptions / Setjmp / Longjmp",
                false, false)

ModulePass *llvm::createWebAssemblyLowerEmscriptenEHSjLj(bool DoEH,
                                                         bool DoSjLj) {
  return new WebAssemblyLowerEmscriptenEHSjLj(DoEH, DoSjLj);
}

static bool canThrow(const Value *V) {
  if (const auto *F = dyn_cast<const Function>(V)) {
    // Intrinsics cannot throw
    if (F->isIntrinsic())
      return false;
    StringRef Name = F->getName();
    // leave setjmp and longjmp (mostly) alone, we process them properly later
    if (Name == "setjmp" || Name == "longjmp")
      return false;
    return true;
  }
  // not a function, so an indirect call - can throw, we can't tell
  return true;
}

// Returns an available name for a global value.
// If the proposed name already exists in the module, adds '_' at the end of
// the name until the name is available.
static inline std::string createGlobalValueName(const Module &M,
                                                const std::string &Propose) {
  std::string Name = Propose;
  while (M.getNamedGlobal(Name))
    Name += "_";
  return Name;
}

// Simple function name mangler.
// This function simply takes LLVM's string representation of parameter types
// and concatenate them with '_'. There are non-alphanumeric characters but llc
// is ok with it, and we need to postprocess these names after the lowering
// phase anyway.
static std::string getSignature(FunctionType *FTy) {
  std::string Sig;
  raw_string_ostream OS(Sig);
  OS << *FTy->getReturnType();
  for (Type *ParamTy : FTy->params())
    OS << "_" << *ParamTy;
  if (FTy->isVarArg())
    OS << "_...";
  Sig = OS.str();
  Sig.erase(remove_if(Sig, isspace), Sig.end());
  // When s2wasm parses .s file, a comma means the end of an argument. So a
  // mangled function name can contain any character but a comma.
  std::replace(Sig.begin(), Sig.end(), ',', '.');
  return Sig;
}

Function *
WebAssemblyLowerEmscriptenEHSjLj::getFindMatchingCatch(Module &M,
                                                       unsigned NumClauses) {
  if (FindMatchingCatches.count(NumClauses))
    return FindMatchingCatches[NumClauses];
  PointerType *Int8PtrTy = Type::getInt8PtrTy(M.getContext());
  SmallVector<Type *, 16> Args(NumClauses, Int8PtrTy);
  FunctionType *FTy = FunctionType::get(Int8PtrTy, Args, false);
  Function *F =
      Function::Create(FTy, GlobalValue::ExternalLinkage,
                       FindMatchingCatchPrefix + Twine(NumClauses + 2), &M);
  FindMatchingCatches[NumClauses] = F;
  return F;
}

Function *WebAssemblyLowerEmscriptenEHSjLj::getInvokeWrapper(Module &M,
                                                             InvokeInst *II) {
  SmallVector<Type *, 16> ArgTys;
  Value *Callee = II->getCalledValue();
  FunctionType *CalleeFTy;
  if (auto *F = dyn_cast<Function>(Callee))
    CalleeFTy = F->getFunctionType();
  else {
    auto *CalleeTy = dyn_cast<PointerType>(Callee->getType())->getElementType();
    CalleeFTy = dyn_cast<FunctionType>(CalleeTy);
  }

  std::string Sig = getSignature(CalleeFTy);
  if (InvokeWrappers.find(Sig) != InvokeWrappers.end())
    return InvokeWrappers[Sig];

  // Put the pointer to the callee as first argument
  ArgTys.push_back(PointerType::getUnqual(CalleeFTy));
  // Add argument types
  ArgTys.append(CalleeFTy->param_begin(), CalleeFTy->param_end());

  FunctionType *FTy = FunctionType::get(CalleeFTy->getReturnType(), ArgTys,
                                        CalleeFTy->isVarArg());
  Function *F = Function::Create(FTy, GlobalValue::ExternalLinkage,
                                 InvokePrefix + Sig, &M);
  InvokeWrappers[Sig] = F;
  return F;
}

bool WebAssemblyLowerEmscriptenEHSjLj::runOnModule(Module &M) {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  IntegerType *Int1Ty = IRB.getInt1Ty();
  PointerType *Int8PtrTy = IRB.getInt8PtrTy();
  IntegerType *Int32Ty = IRB.getInt32Ty();
  Type *VoidTy = IRB.getVoidTy();

  // Create global variables __THREW__, threwValue, and tempRet0, which are
  // used in common for both exception handling and setjmp/longjmp handling
  ThrewGV =
      new GlobalVariable(M, Int1Ty, false, GlobalValue::ExternalLinkage,
                         IRB.getFalse(), createGlobalValueName(M, ThrewGVName));
  ThrewValueGV = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, IRB.getInt32(0),
      createGlobalValueName(M, ThrewValueGVName));
  TempRet0GV = new GlobalVariable(M, Int32Ty, false,
                                  GlobalValue::ExternalLinkage, IRB.getInt32(0),
                                  createGlobalValueName(M, TempRet0GVName));

  bool Changed = false;

  // Exception handling
  if (DoEH) {
    // Register __resumeException function
    FunctionType *ResumeFTy = FunctionType::get(VoidTy, Int8PtrTy, false);
    ResumeF = Function::Create(ResumeFTy, GlobalValue::ExternalLinkage,
                               ResumeFName, &M);

    // Register llvm_eh_typeid_for function
    FunctionType *EHTypeIDTy = FunctionType::get(Int32Ty, Int8PtrTy, false);
    EHTypeIDF = Function::Create(EHTypeIDTy, GlobalValue::ExternalLinkage,
                                 EHTypeIDFName, &M);

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      Changed |= runEHOnFunction(F);
    }
  }

  // TODO: Run CFGSimplify like the emscripten JSBackend?

  // Setjmp/longjmp handling
  if (DoSjLj) {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      Changed |= runSjLjOnFunction(F);
    }
  }

  if (!Changed)
    return false;

  // If we have made any changes while doing exception handling or
  // setjmp/longjmp handling, we have to create these functions for JavaScript
  // to call.
  assert(!M.getNamedGlobal(SetThrewFName) && "setThrew already exists");
  assert(!M.getNamedGlobal(SetTempRet0FName) && "setTempRet0 already exists");

  // Create setThrew function
  SmallVector<Type *, 2> Params = {Int1Ty, Int32Ty};
  FunctionType *FTy = FunctionType::get(VoidTy, Params, false);
  Function *F =
      Function::Create(FTy, GlobalValue::ExternalLinkage, SetThrewFName, &M);
  Argument *Arg1 = &*(F->arg_begin());
  Argument *Arg2 = &*(++F->arg_begin());
  Arg1->setName("threw");
  Arg2->setName("value");
  BasicBlock *EntryBB = BasicBlock::Create(C, "entry", F);
  BasicBlock *ThenBB = BasicBlock::Create(C, "if.then", F);
  BasicBlock *EndBB = BasicBlock::Create(C, "if.end", F);

  IRB.SetInsertPoint(EntryBB);
  Value *Threw = IRB.CreateLoad(ThrewGV, ThrewGV->getName() + ".val");
  Value *Cmp = IRB.CreateICmpEQ(Threw, IRB.getFalse(), "cmp");
  IRB.CreateCondBr(Cmp, ThenBB, EndBB);

  IRB.SetInsertPoint(ThenBB);
  IRB.CreateStore(Arg1, ThrewGV);
  IRB.CreateStore(Arg2, ThrewValueGV);
  IRB.CreateBr(EndBB);

  IRB.SetInsertPoint(EndBB);
  IRB.CreateRetVoid();

  // Create setTempRet0 function
  Params = {Int32Ty};
  FTy = FunctionType::get(VoidTy, Params, false);
  F = Function::Create(FTy, GlobalValue::ExternalLinkage, SetTempRet0FName, &M);
  F->arg_begin()->setName("value");
  EntryBB = BasicBlock::Create(C, "entry", F);
  IRB.SetInsertPoint(EntryBB);
  IRB.CreateStore(&*F->arg_begin(), TempRet0GV);
  IRB.CreateRetVoid();

  return true;
}

bool WebAssemblyLowerEmscriptenEHSjLj::runEHOnFunction(Function &F) {
  Module &M = *F.getParent();
  LLVMContext &C = F.getContext();
  IRBuilder<> IRB(C);
  bool Changed = false;
  SmallVector<Instruction *, 64> ToErase;
  SmallPtrSet<LandingPadInst *, 32> LandingPads;
  bool AllowExceptions =
      areAllExceptionsAllowed() || EHWhitelistSet.count(F.getName());

  for (BasicBlock &BB : F) {
    auto *II = dyn_cast<InvokeInst>(BB.getTerminator());
    if (!II)
      continue;
    Changed = true;
    LandingPads.insert(II->getLandingPadInst());
    IRB.SetInsertPoint(II);

    bool NeedInvoke = AllowExceptions && canThrow(II->getCalledValue());
    if (NeedInvoke) {
      // If we are calling a function that is noreturn, we must remove that
      // attribute. The code we insert here does expect it to return, after we
      // catch the exception.
      if (II->doesNotReturn()) {
        if (auto *F = dyn_cast<Function>(II->getCalledValue()))
          F->removeFnAttr(Attribute::NoReturn);
        AttributeSet NewAttrs = II->getAttributes();
        NewAttrs.removeAttribute(C, AttributeSet::FunctionIndex,
                                 Attribute::NoReturn);
        II->setAttributes(NewAttrs);
      }

      // Pre-invoke
      // __THREW__ = 0;
      IRB.CreateStore(IRB.getFalse(), ThrewGV);

      // Invoke function wrapper in JavaScript
      SmallVector<Value *, 16> CallArgs;
      // Put the pointer to the callee as first argument, so it can be called
      // within the invoke wrapper later
      CallArgs.push_back(II->getCalledValue());
      CallArgs.append(II->arg_begin(), II->arg_end());
      CallInst *NewCall = IRB.CreateCall(getInvokeWrapper(M, II), CallArgs);
      NewCall->takeName(II);
      NewCall->setCallingConv(II->getCallingConv());
      NewCall->setDebugLoc(II->getDebugLoc());

      // Because we added the pointer to the callee as first argument, all
      // argument attribute indices have to be incremented by one.
      SmallVector<AttributeSet, 8> AttributesVec;
      const AttributeSet &InvokePAL = II->getAttributes();
      CallSite::arg_iterator AI = II->arg_begin();
      unsigned i = 1; // Argument attribute index starts from 1
      for (unsigned e = II->getNumArgOperands(); i <= e; ++AI, ++i) {
        if (InvokePAL.hasAttributes(i)) {
          AttrBuilder B(InvokePAL, i);
          AttributesVec.push_back(AttributeSet::get(C, i + 1, B));
        }
      }
      // Add any return attributes.
      if (InvokePAL.hasAttributes(AttributeSet::ReturnIndex))
        AttributesVec.push_back(
            AttributeSet::get(C, InvokePAL.getRetAttributes()));
      // Add any function attributes.
      if (InvokePAL.hasAttributes(AttributeSet::FunctionIndex))
        AttributesVec.push_back(
            AttributeSet::get(C, InvokePAL.getFnAttributes()));
      // Reconstruct the AttributesList based on the vector we constructed.
      AttributeSet NewCallPAL = AttributeSet::get(C, AttributesVec);
      NewCall->setAttributes(NewCallPAL);

      II->replaceAllUsesWith(NewCall);
      ToErase.push_back(II);

      // Post-invoke
      // %__THREW__.val = __THREW__; __THREW__ = 0;
      Value *Threw = IRB.CreateLoad(ThrewGV, ThrewGV->getName() + ".val");
      IRB.CreateStore(IRB.getFalse(), ThrewGV);

      // Insert a branch based on __THREW__ variable
      IRB.CreateCondBr(Threw, II->getUnwindDest(), II->getNormalDest());

    } else {
      // This can't throw, and we don't need this invoke, just replace it with a
      // call+branch
      SmallVector<Value *, 16> CallArgs(II->arg_begin(), II->arg_end());
      CallInst *NewCall = IRB.CreateCall(II->getCalledValue(), CallArgs);
      NewCall->takeName(II);
      NewCall->setCallingConv(II->getCallingConv());
      NewCall->setDebugLoc(II->getDebugLoc());
      NewCall->setAttributes(II->getAttributes());
      II->replaceAllUsesWith(NewCall);
      ToErase.push_back(II);

      IRB.CreateBr(II->getNormalDest());

      // Remove any PHI node entries from the exception destination
      II->getUnwindDest()->removePredecessor(&BB);
    }
  }

  // Process resume instructions
  for (BasicBlock &BB : F) {
    // Scan the body of the basic block for resumes
    for (Instruction &I : BB) {
      auto *RI = dyn_cast<ResumeInst>(&I);
      if (!RI)
        continue;

      // Split the input into legal values
      Value *Input = RI->getValue();
      IRB.SetInsertPoint(RI);
      Value *Low = IRB.CreateExtractValue(Input, 0, "low");

      // Create a call to __resumeException function
      Value *Args[] = {Low};
      IRB.CreateCall(ResumeF, Args);

      // Add a terminator to the block
      IRB.CreateUnreachable();
      ToErase.push_back(RI);
    }
  }

  // Process llvm.eh.typeid.for intrinsics
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      const Function *Callee = CI->getCalledFunction();
      if (!Callee)
        continue;
      if (Callee->getIntrinsicID() != Intrinsic::eh_typeid_for)
        continue;

      IRB.SetInsertPoint(CI);
      CallInst *NewCI =
          IRB.CreateCall(EHTypeIDF, CI->getArgOperand(0), "typeid");
      CI->replaceAllUsesWith(NewCI);
      ToErase.push_back(CI);
    }
  }

  // Look for orphan landingpads, can occur in blocks with no predecesors
  for (BasicBlock &BB : F) {
    Instruction *I = BB.getFirstNonPHI();
    if (auto *LPI = dyn_cast<LandingPadInst>(I))
      LandingPads.insert(LPI);
  }

  // Handle all the landingpad for this function together, as multiple invokes
  // may share a single lp
  for (LandingPadInst *LPI : LandingPads) {
    IRB.SetInsertPoint(LPI);
    SmallVector<Value *, 16> FMCArgs;
    for (unsigned i = 0, e = LPI->getNumClauses(); i < e; ++i) {
      Constant *Clause = LPI->getClause(i);
      // As a temporary workaround for the lack of aggregate varargs support
      // in the interface between JS and wasm, break out filter operands into
      // their component elements.
      if (LPI->isFilter(i)) {
        ArrayType *ATy = cast<ArrayType>(Clause->getType());
        for (unsigned j = 0, e = ATy->getNumElements(); j < e; ++j) {
          Value *EV = IRB.CreateExtractValue(Clause, makeArrayRef(j), "filter");
          FMCArgs.push_back(EV);
        }
      } else
        FMCArgs.push_back(Clause);
    }

    // Create a call to __cxa_find_matching_catch_N function
    Function *FMCF = getFindMatchingCatch(M, FMCArgs.size());
    CallInst *FMCI = IRB.CreateCall(FMCF, FMCArgs, "fmc");
    Value *Undef = UndefValue::get(LPI->getType());
    Value *Pair0 = IRB.CreateInsertValue(Undef, FMCI, 0, "pair0");
    Value *TempRet0 = IRB.CreateLoad(TempRet0GV, TempRet0GV->getName() + "val");
    Value *Pair1 = IRB.CreateInsertValue(Pair0, TempRet0, 1, "pair1");

    LPI->replaceAllUsesWith(Pair1);
    ToErase.push_back(LPI);
  }

  // Erase everything we no longer need in this function
  for (Instruction *I : ToErase)
    I->eraseFromParent();

  return Changed;
}

bool WebAssemblyLowerEmscriptenEHSjLj::runSjLjOnFunction(Function &F) {
  // TODO
  return false;
}
