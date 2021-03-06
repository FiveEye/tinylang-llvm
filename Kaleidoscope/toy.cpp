#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "llvm/Analysis/Passes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;

// Lexer

enum Token {
  tok_eof = -1,
  tok_def = -2, tok_extern = -3,
  tok_identifier = -4, tok_number = -5
};

static std::string IdentifierStr;
static double NumVal;

static int gettok() {
  static int LastChar = ' ';
  while(isspace(LastChar)) LastChar = getchar();

  if(isalpha(LastChar)) {
    IdentifierStr = LastChar;
    while(isalnum(LastChar = getchar())) IdentifierStr += LastChar;
    if(IdentifierStr == "def") return tok_def;
    if(IdentifierStr == "extern") return tok_extern;
    return tok_identifier;
  }

  if(isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = getchar();
    } while(isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), 0);
    return tok_number;
  }

  if(LastChar == '#') {
    do LastChar = getchar();
    while(LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if(LastChar != EOF) return gettok();
  }

  if(LastChar == EOF) return tok_eof;

  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}

// AST

namespace {

class ExprAST {
public:
  virtual ~ExprAST() {}
  virtual Value *Codegen() = 0;
};

class NumberExprAST : public ExprAST {
  double Val;
public:
  NumberExprAST(double val) : Val(val) {}
  virtual Value *Codegen();
};

class VariableExprAST : public ExprAST {
  std::string Name;
public:
  VariableExprAST(const std::string &name) : Name(name) {}
  virtual Value *Codegen();
};

class BinaryExprAST : public ExprAST {
  char Op;
  ExprAST *LHS, *RHS;
public:
  BinaryExprAST(char op, ExprAST *lhs, ExprAST *rhs) : Op(op), LHS(lhs), RHS(rhs) {}
  virtual Value *Codegen();
};

class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<ExprAST*> Args;
public:
  CallExprAST(const std::string &callee, std::vector<ExprAST*> &args) : Callee(callee), Args(args) {}
  virtual Value *Codegen();
};

class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
public:
  PrototypeAST(const std::string &name, const std::vector<std::string> &args) : Name(name), Args(args) {}
  Function *Codegen();
};

class FunctionAST {
  PrototypeAST *Proto;
  ExprAST *Body;
public:
  FunctionAST(PrototypeAST *proto, ExprAST *body) : Proto(proto), Body(body) {}
  Function *Codegen();
};

}

// Parser

static int CurTok;
static int getNextToken() {
  return CurTok = gettok();
}

static std::map<char, int> BinopPrecedence;

static int GetTokPrecedence() {
  if(!isascii(CurTok)) return -1;

  int TokPrec = BinopPrecedence[CurTok];
  if(TokPrec <= 0) return -1;
  return TokPrec;
}

ExprAST *Error(const char *Str) { fprintf(stderr, "Error: %s\n", Str); return 0; }
PrototypeAST *ErrorP(const char *Str) { Error(Str); return 0; }
FunctionAST *ErrorF(const char *Str) { Error(Str); return 0; }
Value *ErrorV(const char *Str) { Error(Str); return 0; }


static ExprAST *ParseExpression();

static ExprAST *ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;

  getNextToken();

  if(CurTok != '(') return new VariableExprAST(IdName);

  getNextToken();

  std::vector<ExprAST*> Args;
  if(CurTok != ')') {
    while(1) {
      ExprAST *Arg = ParseExpression();
      if(!Arg) return 0;
      Args.push_back(Arg);
      if(CurTok == ')') break;
      if(CurTok != ',') return Error("Expected ')' or ',' in argument list");
      getNextToken();
    }
  }
  getNextToken();
  return new CallExprAST(IdName, Args);
}

static ExprAST *ParseNumberExpr() {
  ExprAST *ret = new NumberExprAST(NumVal);
  getNextToken();
  return ret;
}

static ExprAST *ParseParenExpr() {
  getNextToken();
  ExprAST *V = ParseExpression();
  if(!V) return 0;
  if(CurTok != ')') return Error("expected ')'");
  getNextToken();
  return V;
}

static ExprAST *ParsePrimary() {
  switch(CurTok) {
  case tok_identifier: return ParseIdentifierExpr();
  case tok_number: return ParseNumberExpr();
  case '(': return ParseParenExpr();
  default: return Error("unknown token when expecting an expression");
  }
}

static ExprAST *ParseBinOpRHS(int ExprPrec, ExprAST *LHS) {
  while(1) {
    int TokPrec = GetTokPrecedence();
    if(TokPrec < ExprPrec) return LHS;

    int BinOp = CurTok;
    getNextToken();

    ExprAST *RHS = ParsePrimary();
    if(!RHS) return 0;

    int NextPrec = GetTokPrecedence();
    if(TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, RHS);
      if(!RHS) return 0;
    }

    LHS = new BinaryExprAST(BinOp, LHS, RHS);
  }
}

static ExprAST *ParseExpression() {
  ExprAST *LHS = ParsePrimary();
  if(!LHS) return 0;
  return ParseBinOpRHS(0, LHS);
}

static PrototypeAST *ParsePrototype() {
  if(CurTok != tok_identifier) return ErrorP("Expected function name in prototype");

  std::string FnName = IdentifierStr;

  getNextToken();

  if(CurTok != '(') return ErrorP("Expected '(' in prototype");

  std::vector<std::string> ArgNames;

  while(getNextToken() == tok_identifier) {
    ArgNames.push_back(IdentifierStr);
  }
  if(CurTok != ')') return ErrorP("Expected ')' in prototype");

  getNextToken();

  return new PrototypeAST(FnName, ArgNames);
}

static FunctionAST *ParseDefinition() {
  getNextToken();
  PrototypeAST *Proto = ParsePrototype();
  if(Proto == 0) return 0;

  if(ExprAST *E = ParseExpression()) {
    return new FunctionAST(Proto, E);
  }
  return 0;
}

static FunctionAST *ParseTopLevelExpr() {
  if(ExprAST *E = ParseExpression()) {
    PrototypeAST *Proto = new PrototypeAST("", std::vector<std::string>());
    return new FunctionAST(Proto, E);
  }
  return 0;
}

static PrototypeAST *ParseExtern() {
  getNextToken();
  return ParsePrototype();
}

// MCJIT helper

std::string GenerateUniqueName(const char *root) {
  static int i = 0;
  char s[16];
  sprintf(s, "%s%d", root, i++);
  std::string S = s;
  return S;
}

std::string MakeLegalFunctionName(std::string Name) {
  std::string NewName;
  if(!Name.length()) {
    return GenerateUniqueName("anon_func_");
  }
  NewName = Name;
  if(NewName.find_first_of("0123456789") == 0) {
    NewName.insert(0, 1, 'n');
  }

  std::string legal_elements =
    "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  size_t pos;
  while((pos = NewName.find_first_not_of(legal_elements)) != std::string::npos) {
    char old_c = NewName.at(pos);
    char new_str[16];
    sprintf(new_str, "%d", (int)old_c);
    NewName = NewName.replace(pos, 1, new_str);
  }

  return NewName;
}

class MCJITHelper {
public:
  MCJITHelper(LLVMContext &C) : Context(C), OpenModule(NULL) {}
  ~MCJITHelper();

  Function *getFunction(const std::string FnName);
  Module *getModuleForNewFunction();
  void *getPointerToFunction(Function *F);
  void *getSymbolAddress(const std::string &Name);
  void dump();

private:
  typedef std::vector<Module *> ModuleVector;
  typedef std::vector<ExecutionEngine *> EngineVector;

  LLVMContext &Context;
  Module *OpenModule;
  ModuleVector Modules;
  EngineVector Engines;
};

class HelpingMemoryManager : public SectionMemoryManager {
  HelpingMemoryManager(const HelpingMemoryManager &) = delete;
  void operator=(const HelpingMemoryManager &) = delete;

public:
  HelpingMemoryManager(MCJITHelper *Helper) : MasterHelper(Helper) {}
  virtual ~HelpingMemoryManager() {}

  virtual uint64_t getSymbolAddress(const std::string &Name) override;

private:
  MCJITHelper *MasterHelper;

};

uint64_t HelpingMemoryManager::getSymbolAddress(const std::string &Name) {
  uint64_t pfn = RTDyldMemoryManager::getSymbolAddress(Name);
  if(pfn) return pfn;

  pfn = (uint64_t)MasterHelper->getSymbolAddress(Name);
  if(!pfn) report_fatal_error("Program used extern function '" + Name + "' which could not be resolved!");

  return pfn;
}

MCJITHelper::~MCJITHelper() {
  if(OpenModule) delete OpenModule;
  EngineVector::iterator begin = Engines.begin();
  EngineVector::iterator end = Engines.end();
  EngineVector::iterator it;
  for(it = begin; it != end; ++it) delete *it;

}

Function *MCJITHelper::getFunction(const std::string FnName) {
  ModuleVector::iterator begin = Modules.begin();
  ModuleVector::iterator end = Modules.end();
  ModuleVector::iterator it;
  for(it = begin; it != end; ++it) {
    Function *F = (*it)->getFunction(FnName);
    if(F) {
      if(*it == OpenModule) return F;

      assert(OpenModule != NULL);

      Function *PF = OpenModule->getFunction(FnName);
      if(PF && !PF->empty()) {
        ErrorF("redefinition of function across modules");
        return 0;
      }

      if(!PF) PF = Function::Create(F->getFunctionType(), Function::ExternalLinkage, FnName, OpenModule);

      return PF;

    }
  }
  return NULL;
}

Module *MCJITHelper::getModuleForNewFunction() {
  if(OpenModule) return OpenModule;

  std::string ModName = GenerateUniqueName("mcjit_module_");
  Module *M = new Module(ModName, Context);
  Modules.push_back(M);
  OpenModule = M;
  return M;
}

void *MCJITHelper::getPointerToFunction(Function *F) {
  EngineVector::iterator begin = Engines.begin();
  EngineVector::iterator end = Engines.end();
  for(auto it = begin; it != end; ++it) {
    void *P = (*it)->getPointerToFunction(F);
    if(P) return P;
  }

  if(OpenModule) {
    std::string ErrStr;
    ExecutionEngine *NewEngine =
      EngineBuilder(OpenModule)
        .setErrorStr(&ErrStr)
        .setMCJITMemoryManager(
          new HelpingMemoryManager(this))
        .create();
    if(!NewEngine) {
      fprintf(stderr, "Could not create ExecutionEngine: %s\n", ErrStr.c_str());
      exit(1);
    }

    auto *FPM = new legacy::FunctionPassManager(OpenModule);

    OpenModule->setDataLayout(NewEngine->getDataLayout());
    FPM->add(createBasicAliasAnalysisPass());
    FPM->add(createPromoteMemoryToRegisterPass());
    FPM->add(createInstructionCombiningPass());
    FPM->add(createReassociatePass());
    FPM->add(createGVNPass());
    FPM->add(createCFGSimplificationPass());
    FPM->doInitialization();

    Module::iterator it;
    Module::iterator end = OpenModule->end();
    for(it = OpenModule->begin(); it != end; ++it) {
      FPM->run(*it);
    }

    delete FPM;

    OpenModule = NULL;
    Engines.push_back(NewEngine);
    NewEngine->finalizeObject();
    return NewEngine->getPointerToFunction(F);
  }
  return NULL;
}

void *MCJITHelper::getSymbolAddress(const std::string &Name) {
  EngineVector::iterator begin = Engines.begin();
  EngineVector::iterator end = Engines.end();
  EngineVector::iterator it;
  for(it = begin; it != end; ++it) {
    uint64_t FAddr = (*it)->getFunctionAddress(Name);
    if(FAddr) return (void *)FAddr;
  }
  return NULL;
}

void MCJITHelper::dump() {
  for(auto it = Modules.begin(); it != Modules.end(); ++it) {
      (*it)->dump();
  }
}

// Code Generation

//static Module *TheModule;
static MCJITHelper *JITHelper;
static IRBuilder<> Builder(getGlobalContext());
static std::map<std::string, Value*> NamedValues;

Value *NumberExprAST::Codegen() {
  return ConstantFP::get(getGlobalContext(), APFloat(Val));
}


Value *VariableExprAST::Codegen() {
  Value *V = NamedValues[Name];
  return V ? V : ErrorV("Unknown variable name");
}

Value *BinaryExprAST::Codegen() {
  Value *L = LHS->Codegen();
  Value *R = RHS->Codegen();
  if(L == 0 || R == 0) return 0;
  switch(Op) {
  case '+' : return Builder.CreateFAdd(L, R, "addtmp");
  case '-' : return Builder.CreateFSub(L, R, "subtmp");
  case '*' : return Builder.CreateFMul(L, R, "multmp");
  case '<' :
    L = Builder.CreateFCmpULT(L, R, "cmptmp");
    return Builder.CreateUIToFP(L, Type::getDoubleTy(getGlobalContext()), "booltmp");
  default : return ErrorV("invalid binary operator");
  }
}

Value *CallExprAST::Codegen() {
  Function *CalleeF = JITHelper->getFunction(Callee);
  if(CalleeF == 0) return ErrorV("Unknown function referenced");

  if(CalleeF->arg_size() != Args.size()) return ErrorV("Incorrect # arguments passed");

  std::vector<Value*> ArgsV;
  for(unsigned int i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->Codegen());
    if(ArgsV.back() == 0) return 0;
  }

  return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *PrototypeAST::Codegen() {
  std::vector<Type*> Doubles(Args.size(), Type::getDoubleTy(getGlobalContext()));
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(getGlobalContext()), Doubles, false);
  std::string FnName = MakeLegalFunctionName(Name);
  Module *M = JITHelper->getModuleForNewFunction();
  Function *F = Function::Create(FT, Function::ExternalLinkage, FnName, M);

  if(F->getName() != FnName) {
    F->eraseFromParent();
    F = JITHelper->getFunction(Name);

    if(!F->empty()) {
      ErrorF("redefinition of function");
      return 0;
    }

    if(F->arg_size() != Args.size()) {
      ErrorF("redefinition of function with different # args");
    }
  }

  unsigned Idx = 0;
  for(Function::arg_iterator AI = F->arg_begin(); Idx != Args.size(); ++AI, ++Idx) {
    AI->setName(Args[Idx]);
    NamedValues[Args[Idx]] = AI;
  }

  return F;
}

Function *FunctionAST::Codegen() {
  NamedValues.clear();

  Function *TheFunction = Proto->Codegen();
  if(TheFunction == 0) return 0;

  BasicBlock *BB = BasicBlock::Create(getGlobalContext(), "entry", TheFunction);
  Builder.SetInsertPoint(BB);

  if(Value *RetVal = Body->Codegen()) {
    Builder.CreateRet(RetVal);
    verifyFunction(*TheFunction);
    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return 0;
}


// Top-Level parsing

static void HandleDefinition() {
  if(FunctionAST *F = ParseDefinition()) {
    if(Function *LF = F->Codegen()) {
      fprintf(stderr, "Read a function definition: ");
      LF->dump();
    }
  } else {
    getNextToken();
  }
}

static void HandleExtern() {
  if(PrototypeAST *P = ParseExtern()) {
    if(Function *F = P->Codegen()) {
      fprintf(stderr, "Read an extern: ");
      F->dump();
    }
  } else {
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  if(FunctionAST *F = ParseTopLevelExpr()) {
    if(Function *LF = F->Codegen()) {
      void *FPtr = JITHelper->getPointerToFunction(LF);
      double (*FP)() = (double (*)())(intptr_t)FPtr;
      fprintf(stderr, "Evaluated to %f\n", FP());
    }
  } else {
    getNextToken();
  }
}

static void MainLoop() {
  while(1) {
    fprintf(stderr, "ready> ");
    switch(CurTok) {
    case tok_eof: return;
    case ';': getNextToken(); break;
    case tok_def: HandleDefinition(); break;
    case tok_extern: HandleExtern(); break;
    default: HandleTopLevelExpression(); break;
    }
  }
}

//Lib

extern "C"
double putchard(double x) {
  putchar((char)x);
  return 0;
}

// Main


int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  LLVMContext &Context = getGlobalContext();
  JITHelper = new MCJITHelper(Context);

  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 30;
  BinopPrecedence['*'] = 40;

  fprintf(stderr, "ready> ");
  getNextToken();

  MainLoop();

  JITHelper->dump();

  //while(1) printf("%d\n", gettok());
  return 0;
}
