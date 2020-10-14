//
// Created by Vladimir on 20.06.20.
//

#include "ApiMatchHandler.h"

#include <string>
#include <sstream>
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Inclusions/IncludeStyle.h"
#include "Globals.h"
#include "Utils.h"

using namespace clang;

std::string ApiMatchHandler::getFunctionIdentifier(const CallExpr *CallExpression) {

    const FunctionDecl *FnDeclaration = CallExpression->getDirectCallee();

    //abort if invalid call
    if (FnDeclaration == nullptr)
        return nullptr;

    IdentifierInfo *II = FnDeclaration->getIdentifier();

    if (II == nullptr) {
        return nullptr;
    }

    return II->getName();
}

bool ApiMatchHandler::replaceIdentifier(const CallExpr *CallExpression, const std::string &ApiName,
                                        const std::string &NewIdentifier) {
    return this->ASTRewriter->ReplaceText(CallExpression->getBeginLoc(), ApiName.length(), NewIdentifier);
}

bool ApiMatchHandler::handleCallExpr(const CallExpr *CallExpression, clang::ASTContext *const pContext) {

    std::string Identifier = getFunctionIdentifier(CallExpression);

    if(shouldReplaceWithSyscall(Identifier)) {


        rewriteApiToSyscall(CallExpression, pContext, Identifier);
        return true;
    }

    std::string Replacement = Utils::translateStringToIdentifier(Identifier);

    if (!addGetProcAddress(CallExpression, pContext, Replacement, Identifier))
        return false;

    return replaceIdentifier(CallExpression, Identifier, Replacement);
}

void ApiMatchHandler::run(const MatchResult &Result) {

    llvm::outs() << "Found " << _ApiName << "\n";

    const auto *CallExpression = Result.Nodes.getNodeAs<clang::CallExpr>("callExpr");
    handleCallExpr(CallExpression, Result.Context);
}


bool ApiMatchHandler::addGetProcAddress(const clang::CallExpr *pCallExpression, clang::ASTContext *const pContext,
                                        const std::string &NewIdentifier, std::string &ApiName) {

    SourceRange EnclosingFunctionRange = findInjectionSpot(pContext, clang::ast_type_traits::DynTypedNode(),
                                                           *pCallExpression, 0);

    std::stringstream Result;

    // add function prototype if not already added
    //if(std::find(TypedefAdded.begin(), TypedefAdded.end(), pCallExpression->getDirectCallee()) == TypedefAdded.end()) {

        Result << "\t" << _TypeDef << "\n";
    //}

    // add LoadLibrary with obfuscated strings
    std::string LoadLibraryVariable = Utils::translateStringToIdentifier(_Library);
    std::string LoadLibraryString = Utils::generateVariableDeclaration(LoadLibraryVariable, _Library);
    std::string LoadLibraryHandleIdentifier = Utils::translateStringToIdentifier("hHandle_"+_Library);
    Result << "\t" << LoadLibraryString << std::endl;
    Result << "\tHANDLE " << LoadLibraryHandleIdentifier << " = LoadLibrary(" << LoadLibraryVariable << ");\n";

    // add GetProcAddress with obfuscated string: TypeDef NewIdentifier = (TypeDef) GetProcAddress(handleIdentifier, ApiName)
    std::string ApiNameIdentifier = Utils::translateStringToIdentifier(ApiName);
    std::string ApiNameDecl = Utils::generateVariableDeclaration(ApiNameIdentifier, ApiName);
    Result << "\t" << ApiNameDecl << "\n";
    Result << "\t_"<< ApiName << " " << NewIdentifier << " = (_" << ApiName << ") GetProcAddress("
           << LoadLibraryHandleIdentifier << ", " << ApiNameIdentifier << ");\n";

    TypedefAdded.push_back(pCallExpression->getDirectCallee());

    // add everything at the beginning of the function.
    return !(ASTRewriter->InsertText(EnclosingFunctionRange.getBegin(), Result.str()));
}

SourceRange
ApiMatchHandler::findInjectionSpot(clang::ASTContext *const Context, clang::ast_type_traits::DynTypedNode Parent,
                                   const clang::CallExpr &Literal, uint64_t Iterations) {

    if (Iterations > Globs::CLIMB_PARENTS_MAX_ITER)
        throw std::runtime_error("Reached max iterations when trying to find a function declaration");

    ASTContext::DynTypedNodeList parents = Context->getParents(Literal);;

    if (Iterations > 0) {
        parents = Context->getParents(Parent);
    }

    for (const auto &parent : parents) {

        StringRef ParentNodeKind = parent.getNodeKind().asStringRef();

        if (ParentNodeKind.find("FunctionDecl") != std::string::npos) {
            auto FunDecl = parent.get<clang::FunctionDecl>();
            auto *Statement = FunDecl->getBody();
            auto *FirstChild = *Statement->child_begin();
            return {FirstChild->getBeginLoc(), FunDecl->getEndLoc()};
        }

        return findInjectionSpot(Context, parent, Literal, ++Iterations);
    }
}

static std::vector<std::string> GetArgs(const CallExpr *CallExpression) {
    std::vector<std::string> Args;
    clang::LangOptions LangOpts;
    LangOpts.CPlusPlus = true;
    clang::PrintingPolicy Policy(LangOpts);

    for (int i = 0; i < CallExpression->getNumArgs(); i++) {

        std::string TypeS;
        llvm::raw_string_ostream s(TypeS);
        CallExpression->getArg(i)->printPretty(s, 0, Policy);
        Args.push_back(s.str());
    }

    return Args;
}
bool ApiMatchHandler::shouldReplaceWithSyscall(std::string ApiName) {

    return std::find(Syscalls.begin(), Syscalls.end(), ApiName) != Syscalls.end();
}

/*
 * 1. Rename API to random identifier
 * 2. Adapt parameters
 * 3. Handle If conditions since the return value is different
 * 4.
 */
void ApiMatchHandler::rewriteApiToSyscall(const clang::CallExpr *pExpr, clang::ASTContext *const pContext,
                                          std::string ApiName) {

    if (ApiName == "WriteProcessMemory") {

        llvm::outs() << "[*] Found WriteProcessMemory\n";

        std::vector<std::string> FunctionArgs = GetArgs(pExpr);

        std::ostringstream params(std::ostringstream::out);
        //	reussite = ((fZwWriteVirtualMemory(handleProcess, (VOID*)adresseBase, adresseSource, longueur, &dwBytesWrite) != 0) && (dwBytesWrite == longueur));

        params << "("
               << FunctionArgs.at(0) << ", "
               << FunctionArgs.at(1) << ", (PVOID)("
               << FunctionArgs.at(2) << "), (ULONG)("
               << FunctionArgs.at(3) << "), (PULONG)("
               << FunctionArgs.at(4) << "))";
        std::string Replacement = params.str();

        SourceRange Range(pExpr->getBeginLoc(), pExpr->getEndLoc());
        std::string Suffix = "";

        if(isInsideIfCondition(pExpr, pContext)) {
            llvm::outs() << "CompountStmt > IfStmt\n";

            Suffix = "==ERROR_SUCCESS";
        }

        llvm::outs() << "Replacing with " << Replacement + Suffix << "\n";

        SourceRange EnclosingFunctionRange = findInjectionSpot(pContext, clang::ast_type_traits::DynTypedNode(),
                                                               *pExpr, 0);

        // remember line number + function name
        bool invalid;
        auto toto = ASTRewriter->getSourceMgr().getLineNumber(ASTRewriter->getSourceMgr().getMainFileID(),  EnclosingFunctionRange.getBegin().getRawEncoding(), &invalid);
        auto index = std::pair<int, std::string>(toto, _ApiName);

        bool AlreadyInitialized = Globs::TypeDefsInserted.count(index) != 0;

        auto FunctionPointerIdentifier = std::string();

        if(AlreadyInitialized) {
            FunctionPointerIdentifier = Globs::TypeDefsInserted.at(index);
        } else {
            FunctionPointerIdentifier = Utils::translateStringToIdentifier(_ApiName);
        }

        ASTRewriter->ReplaceText(Range, FunctionPointerIdentifier + params.str() + Suffix);

        if(Globs::TypeDefsInserted.count(index) != 0) {
            return;
        }

        // remember that the fn pointer was already initialised
        Globs::TypeDefsInserted.insert({index, FunctionPointerIdentifier});

        /*
         *
            void *memWriteVirtualMemory = get_shellcode_buffer("ZwWriteVirtualMemory");
            _ZwWriteVirtualMemory fZwWriteVirtualMemory =(_ZwWriteVirtualMemory)(memWriteVirtualMemory);
         */
        std::ostringstream InitStream(std::ostringstream::out);

        auto MemoryBufferIdentifier = Utils::translateStringToIdentifier(_ApiName);

        InitStream <<_TypeDef << "\n";
        InitStream << "\tvoid *" << MemoryBufferIdentifier << " = get_shellcode_buffer(\"Zw" << _ApiName << "\");\n";
        InitStream << "\t_" << _ApiName << " " << FunctionPointerIdentifier << " =(_" << _ApiName << ")("
                       << MemoryBufferIdentifier << ");\n\n";

        ASTRewriter->InsertText(EnclosingFunctionRange.getBegin(), InitStream.str(), false, true);
        return;
    }
}

std::vector<std::string>
ApiMatchHandler::getParents(const Expr& pExpr, clang::ast_type_traits::DynTypedNode Node,
                             clang::ASTContext *const Context, std::vector<std::string> &CurrentParents,
                             uint64_t Iterations) {

    if (Iterations > Globs::CLIMB_PARENTS_MAX_ITER) {
        return CurrentParents;
    }

    ASTContext::DynTypedNodeList parents = Context->getParents(pExpr);

    if (Iterations > 0) {
        parents = Context->getParents(Node);
    }

    for (const auto &parent : parents) {

        StringRef ParentNodeKind = parent.getNodeKind().asStringRef();

        if (ParentNodeKind.find("Cast") != std::string::npos) {

            return getParents(pExpr, parent, Context, CurrentParents, ++Iterations);
        }

        CurrentParents.push_back(ParentNodeKind);
        return getParents(pExpr, parent, Context, CurrentParents, ++Iterations);
    }

    return CurrentParents;
}

bool ApiMatchHandler::isInsideIfCondition(const clang::CallExpr *pExpr, clang::ASTContext *const pContext) {

    std::vector<std::string> Parents;
    getParents(*pExpr, clang::ast_type_traits::DynTypedNode(), pContext, Parents, 0);

    for(auto& parent : Parents){
        llvm::outs() << "Parent is : " << parent << "\n";
    }
    auto it = std::find(Parents.begin(), Parents.end(), "IfStmt");
    if(it != Parents.end()) {
        // WriteProcessMemory call is located within an If statement. Now we should check if it's the
        // in the If condition or the If Body.

        auto CompoundStmtIt = std::find(Parents.begin(), Parents.end(), "CompoundStmt");

        if( CompoundStmtIt == Parents.end()){
            return true;
        }

        if(CompoundStmtIt > it)  {
            // WriteProcessMememory is inside a CompoundStmt, inside an IfStmt
            return true;
        }
    }

    return false;
}
