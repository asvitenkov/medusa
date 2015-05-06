#include "llvm_emulator.hpp"

#include "medusa/log.hpp"

#ifdef WIN32
# include "llvm/Support/Host.h"
#endif

#include <llvm/ExecutionEngine/MCJIT.h>

#include <llvm/Object/ObjectFile.h> // NOTE: Needed to avoid incomplete type llvm::object::ObjectFile

MEDUSA_NAMESPACE_USE

// This function allows the JIT'ed code to read from a register
extern "C" EMUL_LLVM_EXPORT bool JitReadRegister(CpuContext* pCpuCtxt, u32 Reg, void* pVal, u32 BitSize)
{
  if (!pCpuCtxt->ReadRegister(Reg, pVal, BitSize))
  {
    Log::Write("emul_llvm").Level(LogError) << "failed to read register " << pCpuCtxt->GetCpuInformation().ConvertIdentifierToName(Reg) << LogEnd;
    return false;
  }
  return true;
}

static llvm::Function* s_pReadRegisterFunc;

// This function allows the JIT'ed code to write into a register
extern "C" EMUL_LLVM_EXPORT bool JitWriteRegister(CpuContext* pCpuCtxt, u32 Reg, void const* pVal, u32 BitSize)
{
  if (!pCpuCtxt->WriteRegister(Reg, pVal, BitSize))
  {
    Log::Write("emul_llvm").Level(LogError) << "failed to write register " << pCpuCtxt->GetCpuInformation().ConvertIdentifierToName(Reg) << LogEnd;
    return false;
  }
  return true;
}

static llvm::Function* s_pWriteRegisterFunc;

// This function allows the JIT'ed code to convert logical address to linear address
extern "C" EMUL_LLVM_EXPORT u64 JitTranslateAddress(u8* pCpuCtxtObj, TBase Base, TOffset Offset)
{
  auto pCpuCtxt = reinterpret_cast<CpuContext*>(pCpuCtxtObj);
  u64 LinAddr;
  if (!pCpuCtxt->Translate(Address(Base, Offset), LinAddr))
    return Offset;

  return LinAddr;
}

static llvm::Function* s_pTranslateAddressFunc;

// This function allows the JIT'ed code to access to memory
extern "C" EMUL_LLVM_EXPORT void* JitGetMemory(u8* pCpuCtxtObj, u8* pMemCtxtObj, TBase Base, TOffset Offset, u32 BitSize)
{
  auto pCpuCtxt = reinterpret_cast<CpuContext*>(pCpuCtxtObj);
  auto pMemCtxt = reinterpret_cast<MemoryContext*>(pMemCtxtObj);
  void* pMemory;

  Address CurAddr(Base, Offset);
  u64 LinAddr;
  if (!pCpuCtxt->Translate(CurAddr, LinAddr))
  {
    Log::Write("emul_llvm").Level(LogWarning) << "unable to translate address " << CurAddr << LogEnd;
    return nullptr;
  }

  u32 MemOff;
  u32 MemSize;
  if (!pMemCtxt->FindMemory(LinAddr, pMemory, MemOff, MemSize))
  {
    Log::Write("emul_llvm").Level(LogWarning) << "invalid memory access: linear address: " << LinAddr << LogEnd;
    return nullptr;
  }

  return reinterpret_cast<u8*>(pMemory) + MemOff;
}

static llvm::Function* s_pGetMemoryFunc = nullptr;

// This function allows the JIT'ed code to call instruction hook
extern "C" EMUL_LLVM_EXPORT void JitCallInstructionHook(u8* pEmulObj)
{
  auto pEmul = reinterpret_cast<Emulator*>(pEmulObj);
  pEmul->CallInstructionHook();
}

static llvm::Function* s_pCallInstructionHookFunc = nullptr;

// This function allows the JIT'ed code to handle hook
extern "C" EMUL_LLVM_EXPORT void JitHandleHook(u8* pEmulObj, TBase Base, TOffset Offset, u32 HookType)
{
  auto pEmul = reinterpret_cast<Emulator*>(pEmulObj);
  pEmul->TestHook(Address(Base, Offset), HookType);
}

static llvm::Function* s_pHandleHookFunc = nullptr;

LlvmEmulator::LlvmEmulator(CpuInformation const* pCpuInfo, CpuContext* pCpuCtxt, MemoryContext *pMemCtxt)
  : Emulator(pCpuInfo, pCpuCtxt, pMemCtxt)
  , m_Builder(llvm::getGlobalContext())
{
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
}

LlvmEmulator::~LlvmEmulator(void)
{
}

bool LlvmEmulator::Execute(Address const& rAddress, Expression::LSPType const& rExprList)
{
  u64 LinAddr;
  if (!m_pCpuCtxt->Translate(rAddress, LinAddr))
    return nullptr;

  // Check if the code to be executed is already JIT'ed
  auto pCode = m_FunctionCache[LinAddr];
  if (pCode == nullptr)
  {
    // Create a new LLVM function
    auto pExecFunc = m_JitHelper.CreateFunction(rAddress.ToString());

    // Expose its parameters: CpuCtxt and MemCtxt
    auto itParam = pExecFunc->arg_begin();
    auto pCpuCtxtObjParam = itParam++;
    auto pMemCtxtObjParam = itParam;

    // Insert a new basic block
    auto pBscBlk = llvm::BasicBlock::Create(llvm::getGlobalContext(), llvm::StringRef("entry_") + rAddress.ToString(), pExecFunc);
    m_Builder.SetInsertPoint(pBscBlk);

    // This visitor will now emit code into the basic block using the LLVM builder
    LlvmExpressionVisitor Visitor(
      this,
      m_Hooks,
      m_pCpuCtxt, m_pMemCtxt,
      m_Vars,
      m_Builder,
      pCpuCtxtObjParam, pMemCtxtObjParam);

    // Emit the code for each expression
    for (auto spExpr : rExprList)
    {
      Log::Write("emul_llvm").Level(LogError) << "expr: " << spExpr->ToString() << LogEnd;
      if (spExpr->Visit(&Visitor) == nullptr)
      {
        Log::Write("emul_llvm").Level(LogError) << "failed to JIT expression at " << rAddress << LogEnd;
        return false;
      }
    }

    m_Builder.CreateRetVoid();
    //pExecFunc->dump();
    pCode = m_JitHelper.GetFunctionCode(rAddress.ToString());
    if (pCode == nullptr)
    {
      Log::Write("emul_llvm").Level(LogError) << "failed to JIT code for function " << pExecFunc->getName().data() << LogEnd;
      if (rExprList.empty())
        Log::Write("emul_llvm").Level(LogError) << "no semantic" << LogEnd;
      for (auto spExpr : rExprList)
        Log::Write("emul_llvm").Level(LogError) << "expr: " << spExpr->ToString() << LogEnd;
      return false;
    }
    m_FunctionCache[LinAddr] = pCode;
  }

  pCode(reinterpret_cast<u8*>(m_pCpuCtxt), reinterpret_cast<u8*>(m_pMemCtxt));

  return true;
}

LlvmEmulator::LlvmJitHelper::LlvmJitHelper(void)
: m_pCurMod(nullptr)
{

}

LlvmEmulator::LlvmJitHelper::~LlvmJitHelper(void)
{
  for (auto& rModEE : m_ModuleExecEngineMap)
  {
    delete rModEE.second;
  }
}

llvm::Function* LlvmEmulator::LlvmJitHelper::CreateFunction(std::string const& rFnName)
{
  for (auto const* pMod : m_Modules)
  {
    auto pFunc = pMod->getFunction(std::string("fn_") + rFnName);
    if (pFunc != nullptr)
      return pFunc;
  }

  // Define method(CpuContext* pCpuCtxt, MemoryContext* pMemCtxt)
  static llvm::FunctionType* s_pExecFuncType = nullptr;
  if (s_pExecFuncType == nullptr)
  {
    auto pBytePtrType = llvm::Type::getInt8PtrTy(llvm::getGlobalContext());
    std::vector<llvm::Type*> Params;
    Params.push_back(pBytePtrType);
    Params.push_back(pBytePtrType);
    s_pExecFuncType = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm::getGlobalContext()), Params, false);
  }

  if (m_pCurMod == nullptr)
    _CreateModule(rFnName);
  auto pExecFunc = llvm::Function::Create(s_pExecFuncType, llvm::GlobalValue::InternalLinkage, llvm::StringRef("fn_") + rFnName, m_pCurMod);
  return pExecFunc;
}

LlvmEmulator::BasicBlockCode LlvmEmulator::LlvmJitHelper::GetFunctionCode(std::string const& rFnName)
{
  // We need a fresh module here
  if (m_pCurMod == nullptr)
    return nullptr;

  auto FPM = new llvm::FunctionPassManager(m_pCurMod);
  
  // Set up the optimizer pipeline.  Start with registering info about how the
  // target lays out data structures.
  //FPM->add(new llvm::DataLayout(*EE->getDataLayout()));
  // Provide basic AliasAnalysis support for GVN.
  FPM->add(llvm::createBasicAliasAnalysisPass());
  // Promote allocas to registers.
  FPM->add(llvm::createPromoteMemoryToRegisterPass());
  // Do simple "peephole" optimizations and bit-twiddling optzns.
  FPM->add(llvm::createInstructionCombiningPass());
  // Reassociate expressions.
  FPM->add(llvm::createReassociatePass());
  // Eliminate Common SubExpressions.
  FPM->add(llvm::createGVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  FPM->add(llvm::createCFGSimplificationPass());

  FPM->doInitialization();

  for (auto& rFunc : *m_pCurMod)
    FPM->run(rFunc);

  auto EE = m_ModuleExecEngineMap[m_pCurMod];
  EE->finalizeObject();
  auto pFuncCode = (BasicBlockCode)EE->getFunctionAddress("fn_" + rFnName);
  m_pCurMod = nullptr;
  return pFuncCode;
}

void LlvmEmulator::LlvmJitHelper::_CreateModule(std::string const& rModName)
{
  m_pCurMod = new llvm::Module("medusa-llvm-emulator-module-" + rModName, llvm::getGlobalContext());
  // LLVM doesn't support COFF format https://the-ravi-programming-language.readthedocs.org/en/latest/llvm-notes.html
#ifdef _WIN32
  auto Triple = llvm::sys::getProcessTriple();
  m_pCurMod->setTargetTriple(Triple + "-elf");
#endif

  // Create the engine builder
  std::string ErrStr;
  auto EE = llvm::EngineBuilder(std::unique_ptr<llvm::Module>(m_pCurMod)).setErrorStr(&ErrStr).create();
  m_ModuleExecEngineMap[m_pCurMod] = EE;

  auto& rCtxt = llvm::getGlobalContext();

  // Initialize ReadRegister function type
  {
    static std::vector<llvm::Type*> Params{
      llvm::Type::getInt8PtrTy(rCtxt),   // pCpuCtxt
      llvm::Type::getInt32Ty(rCtxt),    // Reg
      llvm::Type::getInt8PtrTy(rCtxt), // pVal
      llvm::Type::getInt32Ty(rCtxt)   // BitSize
    };
    static auto pReadRegisterFuncType = llvm::FunctionType::get(llvm::Type::getInt1Ty(rCtxt), Params, false);
    s_pReadRegisterFunc = llvm::Function::Create(pReadRegisterFuncType, llvm::GlobalValue::ExternalLinkage, "JitReadRegister", m_pCurMod);
    EE->addGlobalMapping(s_pReadRegisterFunc, (void*)JitReadRegister);
  }

  // Initialize WriteRegister function type
  {
    static std::vector<llvm::Type*> Params{
      llvm::Type::getInt8PtrTy(rCtxt),   // pCpuCtxt
      llvm::Type::getInt32Ty(rCtxt),    // Reg
      llvm::Type::getInt8PtrTy(rCtxt), // pVal
      llvm::Type::getInt32Ty(rCtxt)   // BitSize
    };
    static auto pWriteRegisterFuncType = llvm::FunctionType::get(llvm::Type::getInt8Ty(rCtxt), Params, false);
    s_pWriteRegisterFunc = llvm::Function::Create(pWriteRegisterFuncType, llvm::GlobalValue::ExternalLinkage, "JitWriteRegister", m_pCurMod);
    EE->addGlobalMapping(s_pWriteRegisterFunc, (void*)JitWriteRegister);
  }

  // Initialize TranslateMemory function type
  {
    static std::vector<llvm::Type*> Params{
      llvm::Type::getInt8PtrTy(rCtxt),    // pCpuCtxt
      llvm::Type::getInt16Ty(rCtxt),     // Base
      llvm::Type::getInt64Ty(rCtxt),    // Offset
    };
    static auto pTranslateAddressFuncType = llvm::FunctionType::get(llvm::Type::getInt64Ty(rCtxt), Params, false);
    s_pTranslateAddressFunc = llvm::Function::Create(pTranslateAddressFuncType, llvm::GlobalValue::ExternalLinkage, "JitTranslateAddress", m_pCurMod);
    EE->addGlobalMapping(s_pTranslateAddressFunc, (void*)JitTranslateAddress);
  }

  // Initialize GetMemory function type
  {
    static std::vector<llvm::Type*> Params{
      llvm::Type::getInt8PtrTy(rCtxt),   // pCpuCtxt
      llvm::Type::getInt8PtrTy(rCtxt),  // pMemCtxt
      llvm::Type::getInt16Ty(rCtxt),   // Base
      llvm::Type::getInt64Ty(rCtxt),  // Offset
      llvm::Type::getInt32Ty(rCtxt), // AccessBitSize
    };
    static auto pGetMemoryFuncType = llvm::FunctionType::get(llvm::Type::getInt8PtrTy(rCtxt), Params, false);
    s_pGetMemoryFunc = llvm::Function::Create(pGetMemoryFuncType, llvm::GlobalValue::ExternalLinkage, "JitGetMemory", m_pCurMod);
    EE->addGlobalMapping(s_pGetMemoryFunc, (void*)JitGetMemory);
  }

  // Initialize CallInstructionHook function type
  {
    static std::vector<llvm::Type*> Params{
      llvm::Type::getInt8PtrTy(rCtxt) // pEmul
    };
    static auto pCallInstructionHookType = llvm::FunctionType::get(llvm::Type::getVoidTy(rCtxt), Params, false);
    s_pCallInstructionHookFunc = llvm::Function::Create(pCallInstructionHookType, llvm::GlobalValue::ExternalLinkage, "JitCallInstructionHook", m_pCurMod);
    EE->addGlobalMapping(s_pCallInstructionHookFunc, (void*)JitCallInstructionHook);
  }

  // Initialize HandleHook function type
  {
    static std::vector<llvm::Type*> Params{
      llvm::Type::getInt8PtrTy(rCtxt),  // pEmul
      llvm::Type::getInt16Ty(rCtxt),   // Base
      llvm::Type::getInt64Ty(rCtxt),  // Offset
      llvm::Type::getInt32Ty(rCtxt), // HookType
    };
    static auto pHandleHookType = llvm::FunctionType::get(llvm::Type::getVoidTy(rCtxt), Params, false);
    s_pHandleHookFunc = llvm::Function::Create(pHandleHookType, llvm::GlobalValue::ExternalLinkage, "JitHandleHook", m_pCurMod);
    EE->addGlobalMapping(s_pHandleHookFunc, (void*)JitHandleHook);
  }

  m_Modules.push_back(m_pCurMod);
}

LlvmEmulator::EventListener::EventListener(void)
{
}

LlvmEmulator::EventListener::~EventListener(void)
{
}

void LlvmEmulator::EventListener::NotifyObjectEmitted(llvm::object::ObjectFile const& rObj, llvm::RuntimeDyld::LoadedObjectInfo const& rLdObjInfo)
{
  Log::Write("emul_llvm").Level(LogDebug) << "object emitted" << LogEnd;
}

void LlvmEmulator::EventListener::NotifyFreeingObject(llvm::object::ObjectFile const& rObj)
{
  Log::Write("emul_llvm").Level(LogDebug) << "object freed" << LogEnd;
}

LlvmEmulator::LlvmExpressionVisitor::LlvmExpressionVisitor(
  Emulator* pEmul,
  HookAddressHashMap const& rHooks,
  CpuContext* pCpuCtxt, MemoryContext* pMemCtxt, std::unordered_map<std::string, llvm::Value*>& rVars,
  llvm::IRBuilder<>& rBuilder,
  llvm::Value* pCpuCtxtObjParam, llvm::Value* pMemCtxtObjParam)
  : m_pEmul(pEmul)
  , m_rHooks(rHooks)
  , m_pCpuCtxt(pCpuCtxt), m_pMemCtxt(pMemCtxt), m_rVars(rVars)
  , m_rBuilder(rBuilder)
  , m_NrOfValueToRead(), m_State(Unknown)
  , m_pCpuCtxtObjParam(pCpuCtxtObjParam), m_pMemCtxtObjParam(pMemCtxtObjParam)
{
}

LlvmEmulator::LlvmExpressionVisitor::~LlvmExpressionVisitor(void)
{
  while (!m_ValueStack.empty())
  {
    auto pCurVal = m_ValueStack.top();
    m_ValueStack.pop();
    Log::Write("emul_llvm") << "leaked value: " << pCurVal->getName().str() << LogEnd;
  }
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitSystem(SystemExpression::SPType spSysExpr)
{
  auto const& rSysName = spSysExpr->GetName();

  if (rSysName == "dump_insn")
  {
    m_rBuilder.CreateCall(s_pCallInstructionHookFunc, _MakePointer(8, m_pEmul));
    return spSysExpr;
  }
  if (rSysName == "check_exec_hook")
  {
    Address CurAddr = spSysExpr->GetAddress();
    m_rBuilder.CreateCall4(
      s_pHandleHookFunc, _MakePointer(8, m_pEmul),
      _MakeInteger(IntType(16, CurAddr.GetBase())), _MakeInteger(IntType(64, CurAddr.GetOffset())),
      _MakeInteger(IntType(32, Emulator::HookOnExecute)));
    return spSysExpr;
  }

  Log::Write("emul_llvm").Level(LogWarning) << "unhandled system expression: " << rSysName << LogEnd;
  return spSysExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitBind(BindExpression::SPType spBindExpr)
{
  for (auto spExpr : spBindExpr->GetBoundExpressions())
  {
    if (spExpr->Visit(this) == nullptr)
      return nullptr;
  }
  return spBindExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitTernaryCondition(TernaryConditionExpression::SPType spTernExpr)
{
  Log::Write("emul_llvm").Level(LogError) << "ternary cond not supported" << LogEnd;
  return nullptr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitIfElseCondition(IfElseConditionExpression::SPType spIfElseExpr)
{
  // _Emit test and ref
  State OldState = m_State;
  m_State = Read;
  auto spRefExpr = spIfElseExpr->GetReferenceExpression()->Visit(this);
  auto spTestExpr = spIfElseExpr->GetTestExpression()->Visit(this);
  auto pCondVal = _EmitComparison(spIfElseExpr->GetType());
  m_State = OldState;

  auto pBbOrig = m_rBuilder.GetInsertBlock();
  auto& rCtxt = llvm::getGlobalContext();
  auto pFunc = pBbOrig->getParent();

  auto pBbThen = llvm::BasicBlock::Create(rCtxt, "then", pFunc);
  auto pBbElse = llvm::BasicBlock::Create(rCtxt, "else", pFunc);
  auto pBbMerg = llvm::BasicBlock::Create(rCtxt, "merged", pFunc);

  // Emit the then branch
  auto spThenExpr = spIfElseExpr->GetThenExpression();
  m_rBuilder.SetInsertPoint(pBbThen);
  OldState = m_State;
  m_State = Unknown;
  if (spThenExpr->Visit(this) == nullptr)
    return nullptr;
  m_State = OldState;
  m_rBuilder.CreateBr(pBbMerg);

  // Emit the else branch
  auto spElseExpr = spIfElseExpr->GetElseExpression();
  m_rBuilder.SetInsertPoint(pBbElse);
  OldState = m_State;
  m_State = Unknown;
  if (spElseExpr != nullptr)
    if (spElseExpr->Visit(this) == nullptr)
      return nullptr;
  m_State = OldState;
  m_rBuilder.CreateBr(pBbMerg);

  // Emit the condition
  m_rBuilder.SetInsertPoint(pBbOrig);
  m_rBuilder.CreateCondBr(pCondVal, pBbThen, pBbElse);

  m_rBuilder.SetInsertPoint(pBbMerg);
  return spIfElseExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitWhileCondition(WhileConditionExpression::SPType spWhileExpr)
{
  return nullptr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitAssignment(AssignmentExpression::SPType spAssignExpr)
{
  if (auto spDstVecId = expr_cast<VectorIdentifierExpression>(spAssignExpr->GetDestinationExpression()))
    m_NrOfValueToRead = spDstVecId->GetVector().size();
  else
    m_NrOfValueToRead = 0;

  State OldState = m_State;

  m_State = Read;
  auto spSrc = spAssignExpr->GetSourceExpression()->Visit(this);
  m_State = Write;
  auto spDst = spAssignExpr->GetDestinationExpression()->Visit(this);
  m_State = OldState;

  if (spDst == nullptr || spSrc == nullptr)
    return nullptr;

  return spAssignExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitUnaryOperation(UnaryOperationExpression::SPType spUnOpExpr)
{
  auto spExpr = spUnOpExpr->GetExpression()->Visit(this);

  if (spExpr == nullptr)
    return nullptr;

  if (m_ValueStack.size() < 1)
    return nullptr;
  auto pVal = m_ValueStack.top();
  m_ValueStack.pop();

  llvm::Value* pUnOpVal = nullptr;

  switch (spUnOpExpr->GetOperation())
  {
  default:
    return nullptr;

    case OperationExpression::OpNot:
      pUnOpVal = m_rBuilder.CreateNot(pVal);
      break;

    case OperationExpression::OpNeg:
      pUnOpVal = m_rBuilder.CreateNeg(pVal);
      break;

    case OperationExpression::OpSwap:
      pUnOpVal = _CallIntrinsic(llvm::Intrinsic::bswap, {}, { pVal });
      break;

    case OperationExpression::OpBsf:
      pUnOpVal = _CallIntrinsic(llvm::Intrinsic::cttz, {}, { pVal });
      break;

    case OperationExpression::OpBsr:
      pUnOpVal = _CallIntrinsic(llvm::Intrinsic::ctlz, {}, { pVal });
      break;
  }

  if (pUnOpVal == nullptr)
    return nullptr;
  m_ValueStack.push(pUnOpVal);
  return spUnOpExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitBinaryOperation(BinaryOperationExpression::SPType spBinOpExpr)
{
  //if (spBinOpExpr->GetOperation() == OperationExpression::OpXchg)
  //{
  //  State OldState = m_State;
  //  m_State = Read;
  //  auto spReadLeft = spBinOpExpr->GetLeftExpression()->Visit(this);
  //  auto spReadRight = spBinOpExpr->GetRightExpression()->Visit(this);
  //  m_State = Write;
  //  auto spWriteLeft = spBinOpExpr->GetLeftExpression()->Visit(this);
  //  auto spWriteRight = spBinOpExpr->GetRightExpression()->Visit(this);
  //  m_State = OldState;
  //  return spBinOpExpr;
  //}

  if (spBinOpExpr->GetOperation() == OperationExpression::OpXchg)
  {
    State OldState = m_State;
    m_State = Read;

    auto spReadLeft = spBinOpExpr->GetLeftExpression()->Visit(this);
    auto pLeftVal = m_ValueStack.top();
    m_ValueStack.pop();
    auto pLeftAlloca = m_rBuilder.CreateAlloca(_BitSizeToLlvmType(spReadLeft->GetBitSize()), nullptr, "xchg_tmp_left");
    m_rBuilder.CreateStore(pLeftVal, pLeftAlloca);
    auto pCopyLeftVal = m_rBuilder.CreateLoad(pLeftAlloca);
    m_ValueStack.push(pCopyLeftVal);

    auto spReadRight = spBinOpExpr->GetRightExpression()->Visit(this);
    auto pRightVal = m_ValueStack.top();
    m_ValueStack.pop();
    auto pRightAlloca = m_rBuilder.CreateAlloca(_BitSizeToLlvmType(spReadRight->GetBitSize()), nullptr, "xchg_tmp_right");
    m_rBuilder.CreateStore(pRightVal, pRightAlloca);
    auto pCopyRightVal = m_rBuilder.CreateLoad(pRightAlloca);
    m_ValueStack.push(pCopyRightVal);

    m_State = Write;
    auto spWriteLeft = spBinOpExpr->GetLeftExpression()->Visit(this);
    auto spWriteRight = spBinOpExpr->GetRightExpression()->Visit(this);
    m_State = OldState;
    return spBinOpExpr;
  }

  auto spLeft = spBinOpExpr->GetLeftExpression()->Visit(this);
  auto spRight = spBinOpExpr->GetRightExpression()->Visit(this);

  if (spLeft == nullptr || spRight == nullptr)
    return nullptr;

  if (m_ValueStack.size() < 2)
    return nullptr;

  auto RightVal = m_ValueStack.top();
  m_ValueStack.pop();
  auto LeftVal = m_ValueStack.top();
  m_ValueStack.pop();

  llvm::Value* pBinOpVal = nullptr;

  switch (spBinOpExpr->GetOperation())
  {
  default:
    return nullptr;

  case OperationExpression::OpAnd:
    pBinOpVal = m_rBuilder.CreateAnd(LeftVal, RightVal, "and");
    break;

  case OperationExpression::OpOr:
    pBinOpVal = m_rBuilder.CreateOr(LeftVal, RightVal, "or");
    break;

  case OperationExpression::OpXor:
    pBinOpVal = m_rBuilder.CreateXor(LeftVal, RightVal, "xor");
    break;

  case OperationExpression::OpLls:
    pBinOpVal = m_rBuilder.CreateShl(LeftVal, RightVal, "lls");
    break;

  case OperationExpression::OpLrs:
    pBinOpVal = m_rBuilder.CreateLShr(LeftVal, RightVal, "lrs");
    break;

  case OperationExpression::OpArs:
    pBinOpVal = m_rBuilder.CreateAShr(LeftVal, RightVal, "ars");
    break;

  case OperationExpression::OpRol:
    // TODO(KS):
    break;

  case OperationExpression::OpRor:
    // TODO(KS):
    break;

  case OperationExpression::OpAdd:
    pBinOpVal = m_rBuilder.CreateAdd(LeftVal, RightVal, "add");
    break;

  case OperationExpression::OpSub:
    pBinOpVal = m_rBuilder.CreateSub(LeftVal, RightVal, "sub");
    break;

  case OperationExpression::OpMul:
    pBinOpVal = m_rBuilder.CreateMul(LeftVal, RightVal, "mul");
    break;

  case OperationExpression::OpSDiv:
    pBinOpVal = m_rBuilder.CreateSDiv(LeftVal, RightVal, "sdiv");
    break;

  case OperationExpression::OpUDiv:
    pBinOpVal = m_rBuilder.CreateUDiv(LeftVal, RightVal, "udiv");
    break;

  case OperationExpression::OpSMod:
    pBinOpVal = m_rBuilder.CreateSRem(LeftVal, RightVal, "smod");
    break;

  case OperationExpression::OpUMod:
    pBinOpVal = m_rBuilder.CreateURem(LeftVal, RightVal, "rmod");
    break;

  case OperationExpression::OpSext:
    pBinOpVal = m_rBuilder.CreateSExt(LeftVal, _BitSizeToLlvmType(spRight->GetBitSize()), "sext");
    break;

  case OperationExpression::OpZext:
    pBinOpVal = m_rBuilder.CreateZExt(LeftVal, _BitSizeToLlvmType(spRight->GetBitSize()), "zext");
    break;

  case OperationExpression::OpBcast:
    if (spLeft->GetBitSize() == spRight->GetBitSize())
      pBinOpVal = LeftVal;
    else
      pBinOpVal = m_rBuilder.CreateZExtOrTrunc(LeftVal, _BitSizeToLlvmType(spRight->GetBitSize()), "bcast");
    break;

  case OperationExpression::OpInsertBits:
    break;

  case OperationExpression::OpExtractBits:
    break;
  }

  if (pBinOpVal == nullptr)
    return nullptr;
  m_ValueStack.push(pBinOpVal);
  return spBinOpExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitConstant(ConstantExpression::SPType spConstExpr)
{
  if (m_State != Read)
  {
    Log::Write("emul_llvm").Level(LogError) << "constant can only be read" << LogEnd;
    return nullptr;
  }
  auto pConstVal = _MakeInteger(spConstExpr->GetConstant());
  if (pConstVal == nullptr)
    return nullptr;
  m_ValueStack.push(pConstVal);
  return spConstExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitIdentifier(IdentifierExpression::SPType spIdExpr)
{
  switch (m_State)
  {
  case Read:
  {
    auto pRegVal = _EmitReadRegister(spIdExpr->GetId(), *spIdExpr->GetCpuInformation());
    if (pRegVal == nullptr)
      return nullptr;
    m_ValueStack.push(pRegVal);
    break;
  }

  case Write:
  {
    if (m_ValueStack.empty())
      return nullptr;
    auto pRegVal = m_ValueStack.top();
    m_ValueStack.pop();
    if (!_EmitWriteRegister(spIdExpr->GetId(), *spIdExpr->GetCpuInformation(), pRegVal))
      return nullptr;
    break;
  }

  default:
    return nullptr;
  }
  return spIdExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitVectorIdentifier(VectorIdentifierExpression::SPType spVecIdExpr)
{
  auto const* pCpuInfo = spVecIdExpr->GetCpuInformation();
  switch (m_State)
  {
  case Read:
  {
    auto VecId = spVecIdExpr->GetVector();
    for (auto Id : VecId)
    {
      auto pRegVal = _EmitReadRegister(Id, *spVecIdExpr->GetCpuInformation());
      if (pRegVal == nullptr)
        return nullptr;
      m_ValueStack.push(pRegVal);
    }
    break;
  }

  case Write:
  {
    auto VecId = spVecIdExpr->GetVector();
    for (auto Id : VecId)
    {
      if (m_ValueStack.empty())
        return nullptr;
      auto pRegVal = m_ValueStack.top();
      m_ValueStack.pop();
      if (!_EmitWriteRegister(Id, *spVecIdExpr->GetCpuInformation(), pRegVal))
        return nullptr;
    }
    break;
  }

  default:
    return nullptr;
  }

  return spVecIdExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitTrackedIdentifier(TrackedIdentifierExpression::SPType spTrkIdExpr)
{
  switch (m_State)
  {
  case Read:
  {
    auto pRegVal = _EmitReadRegister(spTrkIdExpr->GetId(), *spTrkIdExpr->GetCpuInformation());
    if (pRegVal == nullptr)
      return nullptr;
    m_ValueStack.push(pRegVal);
    break;
  }

  case Write:
  {
    if (m_ValueStack.empty())
      return nullptr;
    auto pRegVal = m_ValueStack.top();
    m_ValueStack.pop();
    if (!_EmitWriteRegister(spTrkIdExpr->GetId(), *spTrkIdExpr->GetCpuInformation(), pRegVal))
      return nullptr;
    break;
  }

  default:
    return nullptr;
  }
  return spTrkIdExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitVariable(VariableExpression::SPType spVarExpr)
{
  switch (m_State)
  {
  case Unknown:
  {
    switch (spVarExpr->GetAction())
    {
    case VariableExpression::Alloc:
      m_rVars[spVarExpr->GetName()] = m_rBuilder.CreateAlloca(_BitSizeToLlvmType(spVarExpr->GetBitSize()), nullptr, spVarExpr->GetName());
      break;

    case VariableExpression::Free:
      m_rVars.erase(spVarExpr->GetName());
      break;

    default:
      Log::Write("emul_llvm").Level(LogError) << "unknown variable action" << LogEnd;
      return nullptr;
    }
    break;
  }

  case Read:
    if (spVarExpr->GetAction() == VariableExpression::Use)
    {
      auto itVar = m_rVars.find(spVarExpr->GetName());
      if (itVar == std::end(m_rVars))
        return nullptr;
      m_ValueStack.push(m_rBuilder.CreateLoad(itVar->second, "read_var"));
      break;
    }
    else
    {
      Log::Write("emul_llvm").Level(LogError) << "invalid state for variable reading" << LogEnd;
      return nullptr;
    }

  case Write:
    if (spVarExpr->GetAction() == VariableExpression::Use)
    {
      auto itVar = m_rVars.find(spVarExpr->GetName());
      if (itVar == std::end(m_rVars))
        return nullptr;
      if (m_ValueStack.empty())
        return nullptr;
      auto pVal = m_ValueStack.top();
      m_ValueStack.pop();
      m_rBuilder.CreateStore(pVal, itVar->second);
      break;
    }
    else
    {
      Log::Write("emul_llvm").Level(LogError) << "invalid state for variable writing" << LogEnd;
      return nullptr;
    }

  default:
    return nullptr;
  }

  return spVarExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitMemory(MemoryExpression::SPType spMemExpr)
{
  State OldState = m_State;
  m_State = Read;

  auto spOffsetExpr = spMemExpr->GetOffsetExpression();
  auto spBaseExpr = spMemExpr->GetBaseExpression();

  if (spBaseExpr == nullptr)
    m_ValueStack.push(_MakeInteger(IntType(16, 0x0)));
  else if (spBaseExpr->Visit(this) == nullptr)
    return nullptr;

  if (spOffsetExpr->Visit(this) == nullptr)
    return nullptr;
  m_State = OldState;

  if (m_ValueStack.size() < 2)
  {
    Log::Write("emul_llvm").Level(LogError) << "no value for address" << LogEnd;
    return nullptr;
  }

  auto pOffVal = m_rBuilder.CreateZExtOrBitCast(m_ValueStack.top(), _BitSizeToLlvmType(64));
  m_ValueStack.pop();
  auto pBaseVal = m_ValueStack.top();
  m_ValueStack.pop();
  auto AccBitSize = spMemExpr->GetAccessSizeInBit();
  auto pAccBitSizeVal = _MakeInteger(IntType(32, AccBitSize));


  llvm::Value* pPtrVal = nullptr;
  if (m_State != Read || spMemExpr->IsDereferencable())
  {
    auto pRawMemVal = m_rBuilder.CreateCall5(s_pGetMemoryFunc, m_pCpuCtxtObjParam, m_pMemCtxtObjParam, pBaseVal, pOffVal, pAccBitSizeVal, "get_memory");
    pPtrVal = m_rBuilder.CreateBitCast(pRawMemVal, llvm::Type::getIntNPtrTy(llvm::getGlobalContext(), AccBitSize));
  }

  switch (m_State)
  {
  default:
    Log::Write("emul_llvm").Level(LogError) << "unknown state for address" << LogEnd;
    return nullptr;

  case Read:
  {
    if (!spMemExpr->IsDereferencable())
    {
      auto pTransAddrRes = m_rBuilder.CreateCall3(s_pTranslateAddressFunc, m_pCpuCtxtObjParam, pBaseVal, pOffVal, "translate_memory");
      auto pTransAddrResCast = m_rBuilder.CreateTruncOrBitCast(pTransAddrRes, _BitSizeToLlvmType(AccBitSize));
      m_ValueStack.push(pTransAddrResCast);
      break;
    }

    if (m_NrOfValueToRead == 0)
    {
      auto pVal = m_rBuilder.CreateLoad(pPtrVal, "read_mem");
      m_ValueStack.push(pVal);
    }
    while (m_NrOfValueToRead != 0)
    {
      auto pVal = m_rBuilder.CreateLoad(pPtrVal, "read_mem");
      m_ValueStack.push(pVal);
      // FIXME(KS):
      //m_rBuilder.CreateAdd(pPtrVal, _MakeInteger(IntType(AccBitSize, AccBitSize / 8)), "inc_ptr");
      --m_NrOfValueToRead;
    }
    break;
  }

  case Write:
  {
    if (m_ValueStack.empty())
    {
      Log::Write("emul_llvm").Level(LogError) << "no value for address writing" << LogEnd;
      return nullptr;
    }

    //  // NOTE: Trying to write an non-deferencable address is like
    //  // changing its offset.
    //  if (!spMemExpr->IsDereferencable())
    //  {
    //    auto spOffsetExpr = spMemExpr->GetOffsetExpression()->Visit(this);
    //    if (spOffsetExpr == nullptr)
    //      return nullptr;
    //    break;
    //  }

    do
    {
      auto pCurVal = m_ValueStack.top();
      m_ValueStack.pop();
      m_rBuilder.CreateStore(pCurVal, pPtrVal);
      // FIXME(KS):
      //m_rBuilder.CreateAdd(pPtrVal, _MakeInteger(IntType(AccBitSize, AccBitSize / 8)), "inc_ptr");
    } while (!m_ValueStack.empty());
    break;
  }
  }

  return spMemExpr;
}

Expression::SPType LlvmEmulator::LlvmExpressionVisitor::VisitSymbolic(SymbolicExpression::SPType spSymExpr)
{
  return nullptr;
}

llvm::Value* LlvmEmulator::LlvmExpressionVisitor::_MakeInteger(IntType const& rInt) const
{
  // TODO(KS): Handle integer larger than 64-bit
  if (rInt.GetBitSize() > 64)
  {
    Log::Write("emul_llvm").Level(LogError) << "unsupported int size " << rInt.GetBitSize() << LogEnd;
    return nullptr;
  }
  return llvm::ConstantInt::get(llvm::getGlobalContext(), llvm::APInt(rInt.GetBitSize(), rInt.ConvertTo<u64>()));
}

llvm::Value* LlvmEmulator::LlvmExpressionVisitor::_MakePointer(u32 Bits, void* pPointer, s32 Offset) const
{
  //src: http://llvm.1065342.n5.nabble.com/Creating-Pointer-Constants-td31886.html
  auto pConstInt = llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvm::getGlobalContext()), reinterpret_cast<u64>(pPointer));
  auto pPtr = llvm::ConstantExpr::getIntToPtr(pConstInt, llvm::PointerType::getIntNPtrTy(llvm::getGlobalContext(), Bits));

  if (Offset == 0x0)
    return pPtr;

  return m_rBuilder.CreateGEP(pPtr, _MakeInteger(IntType(Offset)));
}

llvm::Value* LlvmEmulator::LlvmExpressionVisitor::_MakePointer(u32 Bits, llvm::Value* pPointerValue, s32 Offset) const
{
  if (Offset != 0x0)
  {
    //src: http://llvm.1065342.n5.nabble.com/Creating-Pointer-Constants-td31886.html
    pPointerValue = m_rBuilder.CreateGEP(pPointerValue, _MakeInteger(IntType(Offset)));
  }

  return m_rBuilder.CreateBitCast(pPointerValue, llvm::PointerType::getIntNPtrTy(llvm::getGlobalContext(), Bits));
}

llvm::Type* LlvmEmulator::LlvmExpressionVisitor::_IntTypeToLlvmType(IntType const& rInt) const
{
  return _BitSizeToLlvmType(rInt.GetBitSize());
}

llvm::Type* LlvmEmulator::LlvmExpressionVisitor::_BitSizeToLlvmType(u16 BitSize) const
{
  return llvm::Type::getIntNTy(llvm::getGlobalContext(), BitSize);
}

llvm::Value* LlvmEmulator::LlvmExpressionVisitor::_CallIntrinsic(
  llvm::Intrinsic::ID IntrId,
  std::vector<llvm::Type*> const& rTypes,
  std::vector<llvm::Value*> const& rArgs) const
{
  auto pModule   = m_rBuilder.GetInsertBlock()->getParent()->getParent();
  auto pIntrFunc = llvm::Intrinsic::getDeclaration(pModule, IntrId);
  auto pCallIntr = m_rBuilder.CreateCall(pIntrFunc, rArgs);
  return pCallIntr;
}

llvm::Value* LlvmEmulator::LlvmExpressionVisitor::_EmitComparison(u8 CondOp)
{
  if (m_ValueStack.size() < 2)
  {
    Log::Write("emul_llvm").Level(LogError) << "no enough values to do comparison" << LogEnd;
    return false;
  }

  auto TestVal = m_ValueStack.top();
  m_ValueStack.pop();
  auto RefVal = m_ValueStack.top();
  m_ValueStack.pop();

  llvm::Value* pCmpVal = nullptr;

  switch (CondOp)
  {
  case ConditionExpression::CondEq:
    pCmpVal = m_rBuilder.CreateICmpEQ(TestVal, RefVal);
    break;

  case ConditionExpression::CondNe:
    pCmpVal = m_rBuilder.CreateICmpNE(TestVal, RefVal);
    break;

  case ConditionExpression::CondUgt:
    pCmpVal = m_rBuilder.CreateICmpUGT(TestVal, RefVal);
    break;

  case ConditionExpression::CondUge:
    pCmpVal = m_rBuilder.CreateICmpUGE(TestVal, RefVal);
    break;

  case ConditionExpression::CondUlt:
    pCmpVal = m_rBuilder.CreateICmpULT(TestVal, RefVal);
    break;

  case ConditionExpression::CondUle:
    pCmpVal = m_rBuilder.CreateICmpULE(TestVal, RefVal);
    break;

  case ConditionExpression::CondSgt:
    pCmpVal = m_rBuilder.CreateICmpSGT(TestVal, RefVal);
    break;

  case ConditionExpression::CondSge:
    pCmpVal = m_rBuilder.CreateICmpSGE(TestVal, RefVal);
    break;

  case ConditionExpression::CondSlt:
    pCmpVal = m_rBuilder.CreateICmpSLT(TestVal, RefVal);
    break;

  case ConditionExpression::CondSle:
    pCmpVal = m_rBuilder.CreateICmpSLE(TestVal, RefVal);
    break;

  default:
    Log::Write("emul_llvm") << "unknown comparison" << LogEnd;
    return nullptr;
  }

  return pCmpVal;
}

llvm::Value* LlvmEmulator::LlvmExpressionVisitor::_EmitReadRegister(u32 Reg, CpuInformation const& rCpuInfo)
{
  auto RegName = llvm::StringRef(rCpuInfo.ConvertIdentifierToName(Reg));

  // We need the register bit size to declare its type
  auto RegBitSize = rCpuInfo.GetSizeOfRegisterInBit(Reg);

  // Make values for function parameters
  auto pRegVal = _MakeInteger(IntType(32, Reg));
  auto pBitSize = _MakeInteger(IntType(32, RegBitSize));

  // Allocate the result on the stack
  auto pRegAlloca = m_rBuilder.CreateAlloca(_BitSizeToLlvmType(RegBitSize), nullptr, RegName + "_alloc");
  auto pRegPtrVal = m_rBuilder.CreateBitCast(pRegAlloca, llvm::Type::getIntNPtrTy(llvm::getGlobalContext(), 8));

  // Call ReadRegister wrapper
  auto pCallVal = m_rBuilder.CreateCall4(s_pReadRegisterFunc, m_pCpuCtxtObjParam, pRegVal, pRegPtrVal, pBitSize, RegName + "_read");

  // Return the result
  auto pRegResVal = m_rBuilder.CreateLoad(pRegAlloca, RegName + "_val");
  return pRegResVal;
}

bool LlvmEmulator::LlvmExpressionVisitor::_EmitWriteRegister(u32 Reg, CpuInformation const& rCpuInfo, llvm::Value* pVal)
{
  auto RegName = llvm::StringRef(rCpuInfo.ConvertIdentifierToName(Reg));

  // We need the register bit size to declare its type
  auto RegBitSize = rCpuInfo.GetSizeOfRegisterInBit(Reg);

  // Make values for function parameters
  auto pRegVal = _MakeInteger(IntType(32, Reg));
  auto pBitSize = _MakeInteger(IntType(32, RegBitSize));

  // Allocate the new value on the stack
  auto pRegAlloca = m_rBuilder.CreateAlloca(_BitSizeToLlvmType(RegBitSize), nullptr, RegName + "_alloc");
  auto pStoreReg = m_rBuilder.CreateStore(pVal, pRegAlloca);
  auto pRegPtrVal = m_rBuilder.CreateBitCast(pRegAlloca, llvm::Type::getIntNPtrTy(llvm::getGlobalContext(), 8), RegName + "_ptr");

  // Call WriteRegister wrapper
  auto pCallVal = m_rBuilder.CreateCall4(s_pWriteRegisterFunc, m_pCpuCtxtObjParam, pRegVal, pRegPtrVal, pBitSize, RegName + "_write");

  return true;
}