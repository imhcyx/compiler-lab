//===--- Sema.cpp - AST Builder and Semantic Analysis Implementation ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the actions class which performs semantic analysis and
// builds an AST out of a parse stream.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaInternal.h"
#include "TargetAttributesSema.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/PartialDiagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/CXXFieldCollector.h"
#include "clang/Sema/DelayedDiagnostic.h"
#include "clang/Sema/ExternalSemaSource.h"
#include "clang/Sema/MultiplexExternalSemaSource.h"
#include "clang/Sema/ObjCMethodList.h"
#include "clang/Sema/PrettyDeclStackTrace.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/SemaConsumer.h"
#include "clang/Sema/TemplateDeduction.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/CrashRecoveryContext.h"
using namespace clang;
using namespace sema;

PrintingPolicy Sema::getPrintingPolicy(const ASTContext &Context,
                                       const Preprocessor &PP) {
  PrintingPolicy Policy = Context.getPrintingPolicy();
  Policy.Bool = Context.getLangOpts().Bool;
  if (!Policy.Bool) {
    if (const MacroInfo *
          BoolMacro = PP.getMacroInfo(&Context.Idents.get("bool"))) {
      Policy.Bool = BoolMacro->isObjectLike() &&
        BoolMacro->getNumTokens() == 1 &&
        BoolMacro->getReplacementToken(0).is(tok::kw__Bool);
    }
  }

  return Policy;
}

void Sema::ActOnTranslationUnitScope(Scope *S) {
  TUScope = S;
  PushDeclContext(S, Context.getTranslationUnitDecl());

  VAListTagName = PP.getIdentifierInfo("__va_list_tag");
}

Sema::Sema(Preprocessor &pp, ASTContext &ctxt, ASTConsumer &consumer,
           TranslationUnitKind TUKind,
           CodeCompleteConsumer *CodeCompleter)
  : TheTargetAttributesSema(0), ExternalSource(0),
    isMultiplexExternalSource(false), FPFeatures(pp.getLangOpts()),
    LangOpts(pp.getLangOpts()), PP(pp), Context(ctxt), Consumer(consumer),
    Diags(PP.getDiagnostics()), SourceMgr(PP.getSourceManager()),
    CollectStats(false), CodeCompleter(CodeCompleter),
    CurContext(0), OriginalLexicalContext(0),
    PackContext(0), ElementWise(false), MSStructPragmaOn(false), VisContext(0),
    IsBuildingRecoveryCallExpr(false),
    ExprNeedsCleanups(false), LateTemplateParser(0), OpaqueParser(0),
    IdResolver(pp), StdInitializerList(0), CXXTypeInfoDecl(0), MSVCGuidDecl(0),
    NSNumberDecl(0),
    NSStringDecl(0), StringWithUTF8StringMethod(0),
    NSArrayDecl(0), ArrayWithObjectsMethod(0),
    NSDictionaryDecl(0), DictionaryWithObjectsMethod(0),
    GlobalNewDeleteDeclared(false),
    TUKind(TUKind),
    NumSFINAEErrors(0), InFunctionDeclarator(0),
    AccessCheckingSFINAE(false), InNonInstantiationSFINAEContext(false),
    NonInstantiationEntries(0), ArgumentPackSubstitutionIndex(-1),
    CurrentInstantiationScope(0), TyposCorrected(0),
    AnalysisWarnings(*this), CurScope(0), Ident_super(0)
{
  TUScope = 0;

  LoadedExternalKnownNamespaces = false;
  for (unsigned I = 0; I != NSAPI::NumNSNumberLiteralMethods; ++I)
    NSNumberLiteralMethods[I] = 0;

  if (getLangOpts().ObjC1)
    NSAPIObj.reset(new NSAPI(Context));

  if (getLangOpts().CPlusPlus)
    FieldCollector.reset(new CXXFieldCollector());

  // Tell diagnostics how to render things from the AST library.
  PP.getDiagnostics().SetArgToStringFn(&FormatASTNodeDiagnosticArgument,
                                       &Context);

  ExprEvalContexts.push_back(
        ExpressionEvaluationContextRecord(PotentiallyEvaluated, 0,
                                          false, 0, false));

  FunctionScopes.push_back(new FunctionScopeInfo(Diags));
}

void Sema::Initialize() {
  // Tell the AST consumer about this Sema object.
  Consumer.Initialize(Context);

  // FIXME: Isn't this redundant with the initialization above?
  if (SemaConsumer *SC = dyn_cast<SemaConsumer>(&Consumer))
    SC->InitializeSema(*this);

  // Tell the external Sema source about this Sema object.
  if (ExternalSemaSource *ExternalSema
      = dyn_cast_or_null<ExternalSemaSource>(Context.getExternalSource()))
    ExternalSema->InitializeSema(*this);

  // Initialize predefined 128-bit integer types, if needed.
  if (PP.getTargetInfo().hasInt128Type()) {
    // If either of the 128-bit integer types are unavailable to name lookup,
    // define them now.
    DeclarationName Int128 = &Context.Idents.get("__int128_t");
    if (IdResolver.begin(Int128) == IdResolver.end())
      PushOnScopeChains(Context.getInt128Decl(), TUScope);

    DeclarationName UInt128 = &Context.Idents.get("__uint128_t");
    if (IdResolver.begin(UInt128) == IdResolver.end())
      PushOnScopeChains(Context.getUInt128Decl(), TUScope);
  }


  // Initialize predefined Objective-C types:
  if (PP.getLangOpts().ObjC1) {
    // If 'SEL' does not yet refer to any declarations, make it refer to the
    // predefined 'SEL'.
    DeclarationName SEL = &Context.Idents.get("SEL");
    if (IdResolver.begin(SEL) == IdResolver.end())
      PushOnScopeChains(Context.getObjCSelDecl(), TUScope);

    // If 'id' does not yet refer to any declarations, make it refer to the
    // predefined 'id'.
    DeclarationName Id = &Context.Idents.get("id");
    if (IdResolver.begin(Id) == IdResolver.end())
      PushOnScopeChains(Context.getObjCIdDecl(), TUScope);

    // Create the built-in typedef for 'Class'.
    DeclarationName Class = &Context.Idents.get("Class");
    if (IdResolver.begin(Class) == IdResolver.end())
      PushOnScopeChains(Context.getObjCClassDecl(), TUScope);

    // Create the built-in forward declaratino for 'Protocol'.
    DeclarationName Protocol = &Context.Idents.get("Protocol");
    if (IdResolver.begin(Protocol) == IdResolver.end())
      PushOnScopeChains(Context.getObjCProtocolDecl(), TUScope);
  }

  DeclarationName BuiltinVaList = &Context.Idents.get("__builtin_va_list");
  if (IdResolver.begin(BuiltinVaList) == IdResolver.end())
    PushOnScopeChains(Context.getBuiltinVaListDecl(), TUScope);
}

Sema::~Sema() {
  if (PackContext) FreePackedContext();
  if (VisContext) FreeVisContext();
  delete TheTargetAttributesSema;
  MSStructPragmaOn = false;
  // Kill all the active scopes.
  for (unsigned I = 1, E = FunctionScopes.size(); I != E; ++I)
    delete FunctionScopes[I];
  if (FunctionScopes.size() == 1)
    delete FunctionScopes[0];

  // Tell the SemaConsumer to forget about us; we're going out of scope.
  if (SemaConsumer *SC = dyn_cast<SemaConsumer>(&Consumer))
    SC->ForgetSema();

  // Detach from the external Sema source.
  if (ExternalSemaSource *ExternalSema
        = dyn_cast_or_null<ExternalSemaSource>(Context.getExternalSource()))
    ExternalSema->ForgetSema();

  // If Sema's ExternalSource is the multiplexer - we own it.
  if (isMultiplexExternalSource)
    delete ExternalSource;
}

/// makeUnavailableInSystemHeader - There is an error in the current
/// context.  If we're still in a system header, and we can plausibly
/// make the relevant declaration unavailable instead of erroring, do
/// so and return true.
bool Sema::makeUnavailableInSystemHeader(SourceLocation loc,
                                         StringRef msg) {
  // If we're not in a function, it's an error.
  FunctionDecl *fn = dyn_cast<FunctionDecl>(CurContext);
  if (!fn) return false;

  // If we're in template instantiation, it's an error.
  if (!ActiveTemplateInstantiations.empty())
    return false;

  // If that function's not in a system header, it's an error.
  if (!Context.getSourceManager().isInSystemHeader(loc))
    return false;

  // If the function is already unavailable, it's not an error.
  if (fn->hasAttr<UnavailableAttr>()) return true;

  fn->addAttr(new (Context) UnavailableAttr(loc, Context, msg));
  return true;
}

ASTMutationListener *Sema::getASTMutationListener() const {
  return getASTConsumer().GetASTMutationListener();
}

///\brief Registers an external source. If an external source already exists,
/// creates a multiplex external source and appends to it.
///
///\param[in] E - A non-null external sema source.
///
void Sema::addExternalSource(ExternalSemaSource *E) {
  assert(E && "Cannot use with NULL ptr");

  if (!ExternalSource) {
    ExternalSource = E;
    return;
  }

  if (isMultiplexExternalSource)
    static_cast<MultiplexExternalSemaSource*>(ExternalSource)->addSource(*E);
  else {
    ExternalSource = new MultiplexExternalSemaSource(*ExternalSource, *E);
    isMultiplexExternalSource = true;
  }
}

/// \brief Print out statistics about the semantic analysis.
void Sema::PrintStats() const {
  llvm::errs() << "\n*** Semantic Analysis Stats:\n";
  llvm::errs() << NumSFINAEErrors << " SFINAE diagnostics trapped.\n";

  BumpAlloc.PrintStats();
  AnalysisWarnings.PrintStats();
}

/// ImpCastExprToType - If Expr is not of type 'Type', insert an implicit cast.
/// If there is already an implicit cast, merge into the existing one.
/// The result is of the given category.
ExprResult Sema::ImpCastExprToType(Expr *E, QualType Ty,
                                   CastKind Kind, ExprValueKind VK,
                                   const CXXCastPath *BasePath,
                                   CheckedConversionKind CCK) {
#ifndef NDEBUG
  if (VK == VK_RValue && !E->isRValue()) {
    switch (Kind) {
    default:
      assert(0 && "can't implicitly cast lvalue to rvalue with this cast kind");
    case CK_LValueToRValue:
    case CK_ArrayToPointerDecay:
    case CK_FunctionToPointerDecay:
    case CK_ToVoid:
      break;
    }
  }
  assert((VK == VK_RValue || !E->isRValue()) && "can't cast rvalue to lvalue");
#endif

  QualType ExprTy = Context.getCanonicalType(E->getType());
  QualType TypeTy = Context.getCanonicalType(Ty);

  if (ExprTy == TypeTy)
    return Owned(E);

  if (getLangOpts().ObjCAutoRefCount)
    CheckObjCARCConversion(SourceRange(), Ty, E, CCK);

  // If this is a derived-to-base cast to a through a virtual base, we
  // need a vtable.
  if (Kind == CK_DerivedToBase &&
      BasePathInvolvesVirtualBase(*BasePath)) {
    QualType T = E->getType();
    if (const PointerType *Pointer = T->getAs<PointerType>())
      T = Pointer->getPointeeType();
    if (const RecordType *RecordTy = T->getAs<RecordType>())
      MarkVTableUsed(E->getLocStart(),
                     cast<CXXRecordDecl>(RecordTy->getDecl()));
  }

  if (ImplicitCastExpr *ImpCast = dyn_cast<ImplicitCastExpr>(E)) {
    if (ImpCast->getCastKind() == Kind && (!BasePath || BasePath->empty())) {
      ImpCast->setType(Ty);
      ImpCast->setValueKind(VK);
      return Owned(E);
    }
  }

  return Owned(ImplicitCastExpr::Create(Context, Ty, Kind, E, BasePath, VK));
}

/// ScalarTypeToBooleanCastKind - Returns the cast kind corresponding
/// to the conversion from scalar type ScalarTy to the Boolean type.
CastKind Sema::ScalarTypeToBooleanCastKind(QualType ScalarTy) {
  switch (ScalarTy->getScalarTypeKind()) {
  case Type::STK_Bool: return CK_NoOp;
  case Type::STK_CPointer: return CK_PointerToBoolean;
  case Type::STK_BlockPointer: return CK_PointerToBoolean;
  case Type::STK_ObjCObjectPointer: return CK_PointerToBoolean;
  case Type::STK_MemberPointer: return CK_MemberPointerToBoolean;
  case Type::STK_Integral: return CK_IntegralToBoolean;
  case Type::STK_Floating: return CK_FloatingToBoolean;
  case Type::STK_IntegralComplex: return CK_IntegralComplexToBoolean;
  case Type::STK_FloatingComplex: return CK_FloatingComplexToBoolean;
  }
  return CK_Invalid;
}

/// \brief Used to prune the decls of Sema's UnusedFileScopedDecls vector.
static bool ShouldRemoveFromUnused(Sema *SemaRef, const DeclaratorDecl *D) {
  if (D->getMostRecentDecl()->isUsed())
    return true;

  if (D->hasExternalLinkage())
    return true;

  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    // UnusedFileScopedDecls stores the first declaration.
    // The declaration may have become definition so check again.
    const FunctionDecl *DeclToCheck;
    if (FD->hasBody(DeclToCheck))
      return !SemaRef->ShouldWarnIfUnusedFileScopedDecl(DeclToCheck);

    // Later redecls may add new information resulting in not having to warn,
    // so check again.
    DeclToCheck = FD->getMostRecentDecl();
    if (DeclToCheck != FD)
      return !SemaRef->ShouldWarnIfUnusedFileScopedDecl(DeclToCheck);
  }

  if (const VarDecl *VD = dyn_cast<VarDecl>(D)) {
    // UnusedFileScopedDecls stores the first declaration.
    // The declaration may have become definition so check again.
    const VarDecl *DeclToCheck = VD->getDefinition();
    if (DeclToCheck)
      return !SemaRef->ShouldWarnIfUnusedFileScopedDecl(DeclToCheck);

    // Later redecls may add new information resulting in not having to warn,
    // so check again.
    DeclToCheck = VD->getMostRecentDecl();
    if (DeclToCheck != VD)
      return !SemaRef->ShouldWarnIfUnusedFileScopedDecl(DeclToCheck);
  }

  return false;
}

namespace {
  struct SortUndefinedButUsed {
    const SourceManager &SM;
    explicit SortUndefinedButUsed(SourceManager &SM) : SM(SM) {}

    bool operator()(const std::pair<NamedDecl *, SourceLocation> &l,
                    const std::pair<NamedDecl *, SourceLocation> &r) const {
      if (l.second.isValid() && !r.second.isValid())
        return true;
      if (!l.second.isValid() && r.second.isValid())
        return false;
      if (l.second != r.second)
        return SM.isBeforeInTranslationUnit(l.second, r.second);
      return SM.isBeforeInTranslationUnit(l.first->getLocation(),
                                          r.first->getLocation());
    }
  };
}

/// Obtains a sorted list of functions that are undefined but ODR-used.
void Sema::getUndefinedButUsed(
    SmallVectorImpl<std::pair<NamedDecl *, SourceLocation> > &Undefined) {
  for (llvm::DenseMap<NamedDecl *, SourceLocation>::iterator
         I = UndefinedButUsed.begin(), E = UndefinedButUsed.end();
       I != E; ++I) {
    NamedDecl *ND = I->first;

    // Ignore attributes that have become invalid.
    if (ND->isInvalidDecl()) continue;

    // __attribute__((weakref)) is basically a definition.
    if (ND->hasAttr<WeakRefAttr>()) continue;

    if (FunctionDecl *FD = dyn_cast<FunctionDecl>(ND)) {
      if (FD->isDefined())
        continue;
      if (FD->hasExternalLinkage() &&
          !FD->getMostRecentDecl()->isInlined())
        continue;
    } else {
      if (cast<VarDecl>(ND)->hasDefinition() != VarDecl::DeclarationOnly)
        continue;
      if (ND->hasExternalLinkage())
        continue;
    }

    Undefined.push_back(std::make_pair(ND, I->second));
  }

  // Sort (in order of use site) so that we're not dependent on the iteration
  // order through an llvm::DenseMap.
  std::sort(Undefined.begin(), Undefined.end(),
            SortUndefinedButUsed(Context.getSourceManager()));
}

/// checkUndefinedButUsed - Check for undefined objects with internal linkage
/// or that are inline.
static void checkUndefinedButUsed(Sema &S) {
  if (S.UndefinedButUsed.empty()) return;

  // Collect all the still-undefined entities with internal linkage.
  SmallVector<std::pair<NamedDecl *, SourceLocation>, 16> Undefined;
  S.getUndefinedButUsed(Undefined);
  if (Undefined.empty()) return;

  for (SmallVectorImpl<std::pair<NamedDecl *, SourceLocation> >::iterator
         I = Undefined.begin(), E = Undefined.end(); I != E; ++I) {
    NamedDecl *ND = I->first;

    if (ND->getLinkage() != ExternalLinkage) {
      S.Diag(ND->getLocation(), diag::warn_undefined_internal)
        << isa<VarDecl>(ND) << ND;
    } else {
      assert(cast<FunctionDecl>(ND)->getMostRecentDecl()->isInlined() &&
             "used object requires definition but isn't inline or internal?");
      S.Diag(ND->getLocation(), diag::warn_undefined_inline) << ND;
    }
    if (I->second.isValid())
      S.Diag(I->second, diag::note_used_here);
  }
}

void Sema::LoadExternalWeakUndeclaredIdentifiers() {
  if (!ExternalSource)
    return;

  SmallVector<std::pair<IdentifierInfo *, WeakInfo>, 4> WeakIDs;
  ExternalSource->ReadWeakUndeclaredIdentifiers(WeakIDs);
  for (unsigned I = 0, N = WeakIDs.size(); I != N; ++I) {
    llvm::DenseMap<IdentifierInfo*,WeakInfo>::iterator Pos
      = WeakUndeclaredIdentifiers.find(WeakIDs[I].first);
    if (Pos != WeakUndeclaredIdentifiers.end())
      continue;

    WeakUndeclaredIdentifiers.insert(WeakIDs[I]);
  }
}


typedef llvm::DenseMap<const CXXRecordDecl*, bool> RecordCompleteMap;

/// \brief Returns true, if all methods and nested classes of the given
/// CXXRecordDecl are defined in this translation unit.
///
/// Should only be called from ActOnEndOfTranslationUnit so that all
/// definitions are actually read.
static bool MethodsAndNestedClassesComplete(const CXXRecordDecl *RD,
                                            RecordCompleteMap &MNCComplete) {
  RecordCompleteMap::iterator Cache = MNCComplete.find(RD);
  if (Cache != MNCComplete.end())
    return Cache->second;
  if (!RD->isCompleteDefinition())
    return false;
  bool Complete = true;
  for (DeclContext::decl_iterator I = RD->decls_begin(),
                                  E = RD->decls_end();
       I != E && Complete; ++I) {
    if (const CXXMethodDecl *M = dyn_cast<CXXMethodDecl>(*I))
      Complete = M->isDefined() || (M->isPure() && !isa<CXXDestructorDecl>(M));
    else if (const FunctionTemplateDecl *F = dyn_cast<FunctionTemplateDecl>(*I))
      Complete = F->getTemplatedDecl()->isDefined();
    else if (const CXXRecordDecl *R = dyn_cast<CXXRecordDecl>(*I)) {
      if (R->isInjectedClassName())
        continue;
      if (R->hasDefinition())
        Complete = MethodsAndNestedClassesComplete(R->getDefinition(),
                                                   MNCComplete);
      else
        Complete = false;
    }
  }
  MNCComplete[RD] = Complete;
  return Complete;
}

/// \brief Returns true, if the given CXXRecordDecl is fully defined in this
/// translation unit, i.e. all methods are defined or pure virtual and all
/// friends, friend functions and nested classes are fully defined in this
/// translation unit.
///
/// Should only be called from ActOnEndOfTranslationUnit so that all
/// definitions are actually read.
static bool IsRecordFullyDefined(const CXXRecordDecl *RD,
                                 RecordCompleteMap &RecordsComplete,
                                 RecordCompleteMap &MNCComplete) {
  RecordCompleteMap::iterator Cache = RecordsComplete.find(RD);
  if (Cache != RecordsComplete.end())
    return Cache->second;
  bool Complete = MethodsAndNestedClassesComplete(RD, MNCComplete);
  for (CXXRecordDecl::friend_iterator I = RD->friend_begin(),
                                      E = RD->friend_end();
       I != E && Complete; ++I) {
    // Check if friend classes and methods are complete.
    if (TypeSourceInfo *TSI = (*I)->getFriendType()) {
      // Friend classes are available as the TypeSourceInfo of the FriendDecl.
      if (CXXRecordDecl *FriendD = TSI->getType()->getAsCXXRecordDecl())
        Complete = MethodsAndNestedClassesComplete(FriendD, MNCComplete);
      else
        Complete = false;
    } else {
      // Friend functions are available through the NamedDecl of FriendDecl.
      if (const FunctionDecl *FD =
          dyn_cast<FunctionDecl>((*I)->getFriendDecl()))
        Complete = FD->isDefined();
      else
        // This is a template friend, give up.
        Complete = false;
    }
  }
  RecordsComplete[RD] = Complete;
  return Complete;
}

/// ActOnEndOfTranslationUnit - This is called at the very end of the
/// translation unit when EOF is reached and all but the top-level scope is
/// popped.
void Sema::ActOnEndOfTranslationUnit() {
  assert(DelayedDiagnostics.getCurrentPool() == NULL
         && "reached end of translation unit with a pool attached?");

  // If code completion is enabled, don't perform any end-of-translation-unit
  // work.
  if (PP.isCodeCompletionEnabled())
    return;

  // Only complete translation units define vtables and perform implicit
  // instantiations.
  if (TUKind == TU_Complete) {
    DiagnoseUseOfUnimplementedSelectors();

    // If any dynamic classes have their key function defined within
    // this translation unit, then those vtables are considered "used" and must
    // be emitted.
    for (DynamicClassesType::iterator I = DynamicClasses.begin(ExternalSource),
                                      E = DynamicClasses.end();
         I != E; ++I) {
      assert(!(*I)->isDependentType() &&
             "Should not see dependent types here!");
      if (const CXXMethodDecl *KeyFunction = Context.getCurrentKeyFunction(*I)) {
        const FunctionDecl *Definition = 0;
        if (KeyFunction->hasBody(Definition))
          MarkVTableUsed(Definition->getLocation(), *I, true);
      }
    }

    // If DefinedUsedVTables ends up marking any virtual member functions it
    // might lead to more pending template instantiations, which we then need
    // to instantiate.
    DefineUsedVTables();

    // C++: Perform implicit template instantiations.
    //
    // FIXME: When we perform these implicit instantiations, we do not
    // carefully keep track of the point of instantiation (C++ [temp.point]).
    // This means that name lookup that occurs within the template
    // instantiation will always happen at the end of the translation unit,
    // so it will find some names that should not be found. Although this is
    // common behavior for C++ compilers, it is technically wrong. In the
    // future, we either need to be able to filter the results of name lookup
    // or we need to perform template instantiations earlier.
    PerformPendingInstantiations();
  }

  // Remove file scoped decls that turned out to be used.
  UnusedFileScopedDecls.erase(
      std::remove_if(UnusedFileScopedDecls.begin(0, true),
                     UnusedFileScopedDecls.end(),
                     std::bind1st(std::ptr_fun(ShouldRemoveFromUnused), this)),
      UnusedFileScopedDecls.end());

  if (TUKind == TU_Prefix) {
    // Translation unit prefixes don't need any of the checking below.
    TUScope = 0;
    return;
  }

  // Check for #pragma weak identifiers that were never declared
  // FIXME: This will cause diagnostics to be emitted in a non-determinstic
  // order!  Iterating over a densemap like this is bad.
  LoadExternalWeakUndeclaredIdentifiers();
  for (llvm::DenseMap<IdentifierInfo*,WeakInfo>::iterator
       I = WeakUndeclaredIdentifiers.begin(),
       E = WeakUndeclaredIdentifiers.end(); I != E; ++I) {
    if (I->second.getUsed()) continue;

    Diag(I->second.getLocation(), diag::warn_weak_identifier_undeclared)
      << I->first;
  }

  if (LangOpts.CPlusPlus11 &&
      Diags.getDiagnosticLevel(diag::warn_delegating_ctor_cycle,
                               SourceLocation())
        != DiagnosticsEngine::Ignored)
    CheckDelegatingCtorCycles();

  if (TUKind == TU_Module) {
    // If we are building a module, resolve all of the exported declarations
    // now.
    if (Module *CurrentModule = PP.getCurrentModule()) {
      ModuleMap &ModMap = PP.getHeaderSearchInfo().getModuleMap();

      SmallVector<Module *, 2> Stack;
      Stack.push_back(CurrentModule);
      while (!Stack.empty()) {
        Module *Mod = Stack.back();
        Stack.pop_back();

        // Resolve the exported declarations and conflicts.
        // FIXME: Actually complain, once we figure out how to teach the
        // diagnostic client to deal with complaints in the module map at this
        // point.
        ModMap.resolveExports(Mod, /*Complain=*/false);
        ModMap.resolveConflicts(Mod, /*Complain=*/false);

        // Queue the submodules, so their exports will also be resolved.
        for (Module::submodule_iterator Sub = Mod->submodule_begin(),
                                     SubEnd = Mod->submodule_end();
             Sub != SubEnd; ++Sub) {
          Stack.push_back(*Sub);
        }
      }
    }

    // Modules don't need any of the checking below.
    TUScope = 0;
    return;
  }

  // C99 6.9.2p2:
  //   A declaration of an identifier for an object that has file
  //   scope without an initializer, and without a storage-class
  //   specifier or with the storage-class specifier static,
  //   constitutes a tentative definition. If a translation unit
  //   contains one or more tentative definitions for an identifier,
  //   and the translation unit contains no external definition for
  //   that identifier, then the behavior is exactly as if the
  //   translation unit contains a file scope declaration of that
  //   identifier, with the composite type as of the end of the
  //   translation unit, with an initializer equal to 0.
  llvm::SmallSet<VarDecl *, 32> Seen;
  for (TentativeDefinitionsType::iterator
            T = TentativeDefinitions.begin(ExternalSource),
         TEnd = TentativeDefinitions.end();
       T != TEnd; ++T)
  {
    VarDecl *VD = (*T)->getActingDefinition();

    // If the tentative definition was completed, getActingDefinition() returns
    // null. If we've already seen this variable before, insert()'s second
    // return value is false.
    if (VD == 0 || VD->isInvalidDecl() || !Seen.insert(VD))
      continue;

    if (const IncompleteArrayType *ArrayT
        = Context.getAsIncompleteArrayType(VD->getType())) {
      if (RequireCompleteType(VD->getLocation(),
                              ArrayT->getElementType(),
                              diag::err_tentative_def_incomplete_type_arr)) {
        VD->setInvalidDecl();
        continue;
      }

      // Set the length of the array to 1 (C99 6.9.2p5).
      Diag(VD->getLocation(), diag::warn_tentative_incomplete_array);
      llvm::APInt One(Context.getTypeSize(Context.getSizeType()), true);
      QualType T = Context.getConstantArrayType(ArrayT->getElementType(),
                                                One, ArrayType::Normal, 0);
      VD->setType(T);
    } else if (RequireCompleteType(VD->getLocation(), VD->getType(),
                                   diag::err_tentative_def_incomplete_type))
      VD->setInvalidDecl();

    CheckCompleteVariableDeclaration(VD);

    // Notify the consumer that we've completed a tentative definition.
    if (!VD->isInvalidDecl())
      Consumer.CompleteTentativeDefinition(VD);

  }

  // If there were errors, disable 'unused' warnings since they will mostly be
  // noise.
  if (!Diags.hasErrorOccurred()) {
    // Output warning for unused file scoped decls.
    for (UnusedFileScopedDeclsType::iterator
           I = UnusedFileScopedDecls.begin(ExternalSource),
           E = UnusedFileScopedDecls.end(); I != E; ++I) {
      if (ShouldRemoveFromUnused(this, *I))
        continue;

      if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(*I)) {
        const FunctionDecl *DiagD;
        if (!FD->hasBody(DiagD))
          DiagD = FD;
        if (DiagD->isDeleted())
          continue; // Deleted functions are supposed to be unused.
        if (DiagD->isReferenced()) {
          if (isa<CXXMethodDecl>(DiagD))
            Diag(DiagD->getLocation(), diag::warn_unneeded_member_function)
                  << DiagD->getDeclName();
          else {
            if (FD->getStorageClass() == SC_Static &&
                !FD->isInlineSpecified() &&
                !SourceMgr.isFromMainFile(
                   SourceMgr.getExpansionLoc(FD->getLocation())))
              Diag(DiagD->getLocation(), diag::warn_unneeded_static_internal_decl)
                << DiagD->getDeclName();
            else
              Diag(DiagD->getLocation(), diag::warn_unneeded_internal_decl)
                   << /*function*/0 << DiagD->getDeclName();
          }
        } else {
          Diag(DiagD->getLocation(),
               isa<CXXMethodDecl>(DiagD) ? diag::warn_unused_member_function
                                         : diag::warn_unused_function)
                << DiagD->getDeclName();
        }
      } else {
        const VarDecl *DiagD = cast<VarDecl>(*I)->getDefinition();
        if (!DiagD)
          DiagD = cast<VarDecl>(*I);
        if (DiagD->isReferenced()) {
          Diag(DiagD->getLocation(), diag::warn_unneeded_internal_decl)
                << /*variable*/1 << DiagD->getDeclName();
        } else if (getSourceManager().isFromMainFile(DiagD->getLocation())) {
          // If the declaration is in a header which is included into multiple
          // TUs, it will declare one variable per TU, and one of the other
          // variables may be used. So, only warn if the declaration is in the
          // main file.
          Diag(DiagD->getLocation(), diag::warn_unused_variable)
              << DiagD->getDeclName();
        }
      }
    }

    if (ExternalSource)
      ExternalSource->ReadUndefinedButUsed(UndefinedButUsed);
    checkUndefinedButUsed(*this);
  }

  if (Diags.getDiagnosticLevel(diag::warn_unused_private_field,
                               SourceLocation())
        != DiagnosticsEngine::Ignored) {
    RecordCompleteMap RecordsComplete;
    RecordCompleteMap MNCComplete;
    for (NamedDeclSetType::iterator I = UnusedPrivateFields.begin(),
         E = UnusedPrivateFields.end(); I != E; ++I) {
      const NamedDecl *D = *I;
      const CXXRecordDecl *RD = dyn_cast<CXXRecordDecl>(D->getDeclContext());
      if (RD && !RD->isUnion() &&
          IsRecordFullyDefined(RD, RecordsComplete, MNCComplete)) {
        Diag(D->getLocation(), diag::warn_unused_private_field)
              << D->getDeclName();
      }
    }
  }

  // Check we've noticed that we're no longer parsing the initializer for every
  // variable. If we miss cases, then at best we have a performance issue and
  // at worst a rejects-valid bug.
  assert(ParsingInitForAutoVars.empty() &&
         "Didn't unmark var as having its initializer parsed");

  TUScope = 0;
}


//===----------------------------------------------------------------------===//
// Helper functions.
//===----------------------------------------------------------------------===//

DeclContext *Sema::getFunctionLevelDeclContext() {
  DeclContext *DC = CurContext;

  while (true) {
    if (isa<BlockDecl>(DC) || isa<EnumDecl>(DC) || isa<CapturedDecl>(DC)) {
      DC = DC->getParent();
    } else if (isa<CXXMethodDecl>(DC) &&
               cast<CXXMethodDecl>(DC)->getOverloadedOperator() == OO_Call &&
               cast<CXXRecordDecl>(DC->getParent())->isLambda()) {
      DC = DC->getParent()->getParent();
    }
    else break;
  }

  return DC;
}

/// getCurFunctionDecl - If inside of a function body, this returns a pointer
/// to the function decl for the function being parsed.  If we're currently
/// in a 'block', this returns the containing context.
FunctionDecl *Sema::getCurFunctionDecl() {
  DeclContext *DC = getFunctionLevelDeclContext();
  return dyn_cast<FunctionDecl>(DC);
}

ObjCMethodDecl *Sema::getCurMethodDecl() {
  DeclContext *DC = getFunctionLevelDeclContext();
  return dyn_cast<ObjCMethodDecl>(DC);
}

NamedDecl *Sema::getCurFunctionOrMethodDecl() {
  DeclContext *DC = getFunctionLevelDeclContext();
  if (isa<ObjCMethodDecl>(DC) || isa<FunctionDecl>(DC))
    return cast<NamedDecl>(DC);
  return 0;
}

void Sema::EmitCurrentDiagnostic(unsigned DiagID) {
  // FIXME: It doesn't make sense to me that DiagID is an incoming argument here
  // and yet we also use the current diag ID on the DiagnosticsEngine. This has
  // been made more painfully obvious by the refactor that introduced this
  // function, but it is possible that the incoming argument can be
  // eliminnated. If it truly cannot be (for example, there is some reentrancy
  // issue I am not seeing yet), then there should at least be a clarifying
  // comment somewhere.
  if (Optional<TemplateDeductionInfo*> Info = isSFINAEContext()) {
    switch (DiagnosticIDs::getDiagnosticSFINAEResponse(
              Diags.getCurrentDiagID())) {
    case DiagnosticIDs::SFINAE_Report:
      // We'll report the diagnostic below.
      break;

    case DiagnosticIDs::SFINAE_SubstitutionFailure:
      // Count this failure so that we know that template argument deduction
      // has failed.
      ++NumSFINAEErrors;

      // Make a copy of this suppressed diagnostic and store it with the
      // template-deduction information.
      if (*Info && !(*Info)->hasSFINAEDiagnostic()) {
        Diagnostic DiagInfo(&Diags);
        (*Info)->addSFINAEDiagnostic(DiagInfo.getLocation(),
                       PartialDiagnostic(DiagInfo, Context.getDiagAllocator()));
      }

      Diags.setLastDiagnosticIgnored();
      Diags.Clear();
      return;

    case DiagnosticIDs::SFINAE_AccessControl: {
      // Per C++ Core Issue 1170, access control is part of SFINAE.
      // Additionally, the AccessCheckingSFINAE flag can be used to temporarily
      // make access control a part of SFINAE for the purposes of checking
      // type traits.
      if (!AccessCheckingSFINAE && !getLangOpts().CPlusPlus11)
        break;

      SourceLocation Loc = Diags.getCurrentDiagLoc();

      // Suppress this diagnostic.
      ++NumSFINAEErrors;

      // Make a copy of this suppressed diagnostic and store it with the
      // template-deduction information.
      if (*Info && !(*Info)->hasSFINAEDiagnostic()) {
        Diagnostic DiagInfo(&Diags);
        (*Info)->addSFINAEDiagnostic(DiagInfo.getLocation(),
                       PartialDiagnostic(DiagInfo, Context.getDiagAllocator()));
      }

      Diags.setLastDiagnosticIgnored();
      Diags.Clear();

      // Now the diagnostic state is clear, produce a C++98 compatibility
      // warning.
      Diag(Loc, diag::warn_cxx98_compat_sfinae_access_control);

      // The last diagnostic which Sema produced was ignored. Suppress any
      // notes attached to it.
      Diags.setLastDiagnosticIgnored();
      return;
    }

    case DiagnosticIDs::SFINAE_Suppress:
      // Make a copy of this suppressed diagnostic and store it with the
      // template-deduction information;
      if (*Info) {
        Diagnostic DiagInfo(&Diags);
        (*Info)->addSuppressedDiagnostic(DiagInfo.getLocation(),
                       PartialDiagnostic(DiagInfo, Context.getDiagAllocator()));
      }

      // Suppress this diagnostic.
      Diags.setLastDiagnosticIgnored();
      Diags.Clear();
      return;
    }
  }

  // Set up the context's printing policy based on our current state.
  Context.setPrintingPolicy(getPrintingPolicy());

  // Emit the diagnostic.
  if (!Diags.EmitCurrentDiagnostic())
    return;

  // If this is not a note, and we're in a template instantiation
  // that is different from the last template instantiation where
  // we emitted an error, print a template instantiation
  // backtrace.
  if (!DiagnosticIDs::isBuiltinNote(DiagID) &&
      !ActiveTemplateInstantiations.empty() &&
      ActiveTemplateInstantiations.back()
        != LastTemplateInstantiationErrorContext) {
    PrintInstantiationStack();
    LastTemplateInstantiationErrorContext = ActiveTemplateInstantiations.back();
  }
}

Sema::SemaDiagnosticBuilder
Sema::Diag(SourceLocation Loc, const PartialDiagnostic& PD) {
  SemaDiagnosticBuilder Builder(Diag(Loc, PD.getDiagID()));
  PD.Emit(Builder);

  return Builder;
}

/// \brief Looks through the macro-expansion chain for the given
/// location, looking for a macro expansion with the given name.
/// If one is found, returns true and sets the location to that
/// expansion loc.
bool Sema::findMacroSpelling(SourceLocation &locref, StringRef name) {
  SourceLocation loc = locref;
  if (!loc.isMacroID()) return false;

  // There's no good way right now to look at the intermediate
  // expansions, so just jump to the expansion location.
  loc = getSourceManager().getExpansionLoc(loc);

  // If that's written with the name, stop here.
  SmallVector<char, 16> buffer;
  if (getPreprocessor().getSpelling(loc, buffer) == name) {
    locref = loc;
    return true;
  }
  return false;
}

/// \brief Determines the active Scope associated with the given declaration
/// context.
///
/// This routine maps a declaration context to the active Scope object that
/// represents that declaration context in the parser. It is typically used
/// from "scope-less" code (e.g., template instantiation, lazy creation of
/// declarations) that injects a name for name-lookup purposes and, therefore,
/// must update the Scope.
///
/// \returns The scope corresponding to the given declaraion context, or NULL
/// if no such scope is open.
Scope *Sema::getScopeForContext(DeclContext *Ctx) {

  if (!Ctx)
    return 0;

  Ctx = Ctx->getPrimaryContext();
  for (Scope *S = getCurScope(); S; S = S->getParent()) {
    // Ignore scopes that cannot have declarations. This is important for
    // out-of-line definitions of static class members.
    if (S->getFlags() & (Scope::DeclScope | Scope::TemplateParamScope))
      if (DeclContext *Entity = static_cast<DeclContext *> (S->getEntity()))
        if (Ctx == Entity->getPrimaryContext())
          return S;
  }

  return 0;
}

/// \brief Enter a new function scope
void Sema::PushFunctionScope() {
  if (FunctionScopes.size() == 1) {
    // Use the "top" function scope rather than having to allocate
    // memory for a new scope.
    FunctionScopes.back()->Clear();
    FunctionScopes.push_back(FunctionScopes.back());
    return;
  }

  FunctionScopes.push_back(new FunctionScopeInfo(getDiagnostics()));
}

void Sema::PushBlockScope(Scope *BlockScope, BlockDecl *Block) {
  FunctionScopes.push_back(new BlockScopeInfo(getDiagnostics(),
                                              BlockScope, Block));
}

void Sema::PushLambdaScope(CXXRecordDecl *Lambda,
                           CXXMethodDecl *CallOperator) {
  FunctionScopes.push_back(new LambdaScopeInfo(getDiagnostics(), Lambda,
                                               CallOperator));
}

void Sema::PopFunctionScopeInfo(const AnalysisBasedWarnings::Policy *WP,
                                const Decl *D, const BlockExpr *blkExpr) {
  FunctionScopeInfo *Scope = FunctionScopes.pop_back_val();
  assert(!FunctionScopes.empty() && "mismatched push/pop!");

  // Issue any analysis-based warnings.
  if (WP && D)
    AnalysisWarnings.IssueWarnings(*WP, Scope, D, blkExpr);
  else {
    for (SmallVectorImpl<sema::PossiblyUnreachableDiag>::iterator
         i = Scope->PossiblyUnreachableDiags.begin(),
         e = Scope->PossiblyUnreachableDiags.end();
         i != e; ++i) {
      const sema::PossiblyUnreachableDiag &D = *i;
      Diag(D.Loc, D.PD);
    }
  }

  if (FunctionScopes.back() != Scope) {
    delete Scope;
  }
}

void Sema::PushCompoundScope() {
  getCurFunction()->CompoundScopes.push_back(CompoundScopeInfo());
}

void Sema::PopCompoundScope() {
  FunctionScopeInfo *CurFunction = getCurFunction();
  assert(!CurFunction->CompoundScopes.empty() && "mismatched push/pop");

  CurFunction->CompoundScopes.pop_back();
}

/// \brief Determine whether any errors occurred within this function/method/
/// block.
bool Sema::hasAnyUnrecoverableErrorsInThisFunction() const {
  return getCurFunction()->ErrorTrap.hasUnrecoverableErrorOccurred();
}

BlockScopeInfo *Sema::getCurBlock() {
  if (FunctionScopes.empty())
    return 0;

  return dyn_cast<BlockScopeInfo>(FunctionScopes.back());
}

LambdaScopeInfo *Sema::getCurLambda() {
  if (FunctionScopes.empty())
    return 0;

  return dyn_cast<LambdaScopeInfo>(FunctionScopes.back());
}

void Sema::ActOnComment(SourceRange Comment) {
  if (!LangOpts.RetainCommentsFromSystemHeaders &&
      SourceMgr.isInSystemHeader(Comment.getBegin()))
    return;
  RawComment RC(SourceMgr, Comment, false,
                LangOpts.CommentOpts.ParseAllComments);
  if (RC.isAlmostTrailingComment()) {
    SourceRange MagicMarkerRange(Comment.getBegin(),
                                 Comment.getBegin().getLocWithOffset(3));
    StringRef MagicMarkerText;
    switch (RC.getKind()) {
    case RawComment::RCK_OrdinaryBCPL:
      MagicMarkerText = "///<";
      break;
    case RawComment::RCK_OrdinaryC:
      MagicMarkerText = "/**<";
      break;
    default:
      llvm_unreachable("if this is an almost Doxygen comment, "
                       "it should be ordinary");
    }
    Diag(Comment.getBegin(), diag::warn_not_a_doxygen_trailing_member_comment) <<
      FixItHint::CreateReplacement(MagicMarkerRange, MagicMarkerText);
  }
  Context.addComment(RC);
}

// Pin this vtable to this file.
ExternalSemaSource::~ExternalSemaSource() {}

void ExternalSemaSource::ReadMethodPool(Selector Sel) { }

void ExternalSemaSource::ReadKnownNamespaces(
                           SmallVectorImpl<NamespaceDecl *> &Namespaces) {
}

void ExternalSemaSource::ReadUndefinedButUsed(
                       llvm::DenseMap<NamedDecl *, SourceLocation> &Undefined) {
}

void PrettyDeclStackTraceEntry::print(raw_ostream &OS) const {
  SourceLocation Loc = this->Loc;
  if (!Loc.isValid() && TheDecl) Loc = TheDecl->getLocation();
  if (Loc.isValid()) {
    Loc.print(OS, S.getSourceManager());
    OS << ": ";
  }
  OS << Message;

  if (TheDecl && isa<NamedDecl>(TheDecl)) {
    std::string Name = cast<NamedDecl>(TheDecl)->getNameAsString();
    if (!Name.empty())
      OS << " '" << Name << '\'';
  }

  OS << '\n';
}

/// \brief Figure out if an expression could be turned into a call.
///
/// Use this when trying to recover from an error where the programmer may have
/// written just the name of a function instead of actually calling it.
///
/// \param E - The expression to examine.
/// \param ZeroArgCallReturnTy - If the expression can be turned into a call
///  with no arguments, this parameter is set to the type returned by such a
///  call; otherwise, it is set to an empty QualType.
/// \param OverloadSet - If the expression is an overloaded function
///  name, this parameter is populated with the decls of the various overloads.
bool Sema::isExprCallable(const Expr &E, QualType &ZeroArgCallReturnTy,
                          UnresolvedSetImpl &OverloadSet) {
  ZeroArgCallReturnTy = QualType();
  OverloadSet.clear();

  if (E.getType() == Context.OverloadTy) {
    OverloadExpr::FindResult FR = OverloadExpr::find(const_cast<Expr*>(&E));
    const OverloadExpr *Overloads = FR.Expression;

    for (OverloadExpr::decls_iterator it = Overloads->decls_begin(),
         DeclsEnd = Overloads->decls_end(); it != DeclsEnd; ++it) {
      OverloadSet.addDecl(*it);

      // Check whether the function is a non-template which takes no
      // arguments.
      if (const FunctionDecl *OverloadDecl
            = dyn_cast<FunctionDecl>((*it)->getUnderlyingDecl())) {
        if (OverloadDecl->getMinRequiredArguments() == 0)
          ZeroArgCallReturnTy = OverloadDecl->getResultType();
      }
    }

    // Ignore overloads that are pointer-to-member constants.
    if (FR.HasFormOfMemberPointer)
      return false;

    return true;
  }

  if (const DeclRefExpr *DeclRef = dyn_cast<DeclRefExpr>(E.IgnoreParens())) {
    if (const FunctionDecl *Fun = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
      if (Fun->getMinRequiredArguments() == 0)
        ZeroArgCallReturnTy = Fun->getResultType();
      return true;
    }
  }

  // We don't have an expression that's convenient to get a FunctionDecl from,
  // but we can at least check if the type is "function of 0 arguments".
  QualType ExprTy = E.getType();
  const FunctionType *FunTy = NULL;
  QualType PointeeTy = ExprTy->getPointeeType();
  if (!PointeeTy.isNull())
    FunTy = PointeeTy->getAs<FunctionType>();
  if (!FunTy)
    FunTy = ExprTy->getAs<FunctionType>();
  if (!FunTy && ExprTy == Context.BoundMemberTy) {
    // Look for the bound-member type.  If it's still overloaded, give up,
    // although we probably should have fallen into the OverloadExpr case above
    // if we actually have an overloaded bound member.
    QualType BoundMemberTy = Expr::findBoundMemberType(&E);
    if (!BoundMemberTy.isNull())
      FunTy = BoundMemberTy->castAs<FunctionType>();
  }

  if (const FunctionProtoType *FPT =
      dyn_cast_or_null<FunctionProtoType>(FunTy)) {
    if (FPT->getNumArgs() == 0)
      ZeroArgCallReturnTy = FunTy->getResultType();
    return true;
  }
  return false;
}

/// \brief Give notes for a set of overloads.
///
/// A companion to isExprCallable. In cases when the name that the programmer
/// wrote was an overloaded function, we may be able to make some guesses about
/// plausible overloads based on their return types; such guesses can be handed
/// off to this method to be emitted as notes.
///
/// \param Overloads - The overloads to note.
/// \param FinalNoteLoc - If we've suppressed printing some overloads due to
///  -fshow-overloads=best, this is the location to attach to the note about too
///  many candidates. Typically this will be the location of the original
///  ill-formed expression.
static void noteOverloads(Sema &S, const UnresolvedSetImpl &Overloads,
                          const SourceLocation FinalNoteLoc) {
  int ShownOverloads = 0;
  int SuppressedOverloads = 0;
  for (UnresolvedSetImpl::iterator It = Overloads.begin(),
       DeclsEnd = Overloads.end(); It != DeclsEnd; ++It) {
    // FIXME: Magic number for max shown overloads stolen from
    // OverloadCandidateSet::NoteCandidates.
    if (ShownOverloads >= 4 && S.Diags.getShowOverloads() == Ovl_Best) {
      ++SuppressedOverloads;
      continue;
    }

    NamedDecl *Fn = (*It)->getUnderlyingDecl();
    S.Diag(Fn->getLocation(), diag::note_possible_target_of_call);
    ++ShownOverloads;
  }

  if (SuppressedOverloads)
    S.Diag(FinalNoteLoc, diag::note_ovl_too_many_candidates)
      << SuppressedOverloads;
}

static void notePlausibleOverloads(Sema &S, SourceLocation Loc,
                                   const UnresolvedSetImpl &Overloads,
                                   bool (*IsPlausibleResult)(QualType)) {
  if (!IsPlausibleResult)
    return noteOverloads(S, Overloads, Loc);

  UnresolvedSet<2> PlausibleOverloads;
  for (OverloadExpr::decls_iterator It = Overloads.begin(),
         DeclsEnd = Overloads.end(); It != DeclsEnd; ++It) {
    const FunctionDecl *OverloadDecl = cast<FunctionDecl>(*It);
    QualType OverloadResultTy = OverloadDecl->getResultType();
    if (IsPlausibleResult(OverloadResultTy))
      PlausibleOverloads.addDecl(It.getDecl());
  }
  noteOverloads(S, PlausibleOverloads, Loc);
}

/// Determine whether the given expression can be called by just
/// putting parentheses after it.  Notably, expressions with unary
/// operators can't be because the unary operator will start parsing
/// outside the call.
static bool IsCallableWithAppend(Expr *E) {
  E = E->IgnoreImplicit();
  return (!isa<CStyleCastExpr>(E) &&
          !isa<UnaryOperator>(E) &&
          !isa<BinaryOperator>(E) &&
          !isa<CXXOperatorCallExpr>(E));
}

bool Sema::tryToRecoverWithCall(ExprResult &E, const PartialDiagnostic &PD,
                                bool ForceComplain,
                                bool (*IsPlausibleResult)(QualType)) {
  SourceLocation Loc = E.get()->getExprLoc();
  SourceRange Range = E.get()->getSourceRange();

  QualType ZeroArgCallTy;
  UnresolvedSet<4> Overloads;
  if (isExprCallable(*E.get(), ZeroArgCallTy, Overloads) &&
      !ZeroArgCallTy.isNull() &&
      (!IsPlausibleResult || IsPlausibleResult(ZeroArgCallTy))) {
    // At this point, we know E is potentially callable with 0
    // arguments and that it returns something of a reasonable type,
    // so we can emit a fixit and carry on pretending that E was
    // actually a CallExpr.
    SourceLocation ParenInsertionLoc =
      PP.getLocForEndOfToken(Range.getEnd());
    Diag(Loc, PD)
      << /*zero-arg*/ 1 << Range
      << (IsCallableWithAppend(E.get())
          ? FixItHint::CreateInsertion(ParenInsertionLoc, "()")
          : FixItHint());
    notePlausibleOverloads(*this, Loc, Overloads, IsPlausibleResult);

    // FIXME: Try this before emitting the fixit, and suppress diagnostics
    // while doing so.
    E = ActOnCallExpr(0, E.take(), ParenInsertionLoc,
                      None, ParenInsertionLoc.getLocWithOffset(1));
    return true;
  }

  if (!ForceComplain) return false;

  Diag(Loc, PD) << /*not zero-arg*/ 0 << Range;
  notePlausibleOverloads(*this, Loc, Overloads, IsPlausibleResult);
  E = ExprError();
  return true;
}

IdentifierInfo *Sema::getSuperIdentifier() const {
  if (!Ident_super)
    Ident_super = &Context.Idents.get("super");
  return Ident_super;
}

void Sema::PushCapturedRegionScope(Scope *S, CapturedDecl *CD, RecordDecl *RD,
                                   CapturedRegionKind K) {
  CapturingScopeInfo *CSI = new CapturedRegionScopeInfo(getDiagnostics(), S, CD, RD,
                                                        CD->getContextParam(), K);
  CSI->ReturnType = Context.VoidTy;
  FunctionScopes.push_back(CSI);
}

CapturedRegionScopeInfo *Sema::getCurCapturedRegion() {
  if (FunctionScopes.empty())
    return 0;

  return dyn_cast<CapturedRegionScopeInfo>(FunctionScopes.back());
}
