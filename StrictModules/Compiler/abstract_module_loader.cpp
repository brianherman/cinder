// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include "StrictModules/Compiler/abstract_module_loader.h"
#include <cstring>
#include <filesystem>
#include "StrictModules/Objects/objects.h"
#include "StrictModules/analyzer.h"
#include "StrictModules/parser_util.h"
namespace strictmod::compiler {

using strictmod::objects::ModuleType;
using strictmod::objects::StrictModuleObject;

static const char* kFileSuffixNames[] = {".py", ".pys", ".pyi"};

const char* getFileSuffixKindName(FileSuffixKind kind) {
  return kFileSuffixNames[static_cast<int>(kind)];
}

const std::string ModuleLoader::kArenaNewErrorMsg =
    "failed to allocate memory in PyArena";

ModuleKind getModuleKindFromStmts(const asdl_seq* seq) {
  Py_ssize_t n = asdl_seq_LEN(seq);
  bool seenDocStr = false;
  for (int _i = 0; _i < n; _i++) {
    stmt_ty stmt = reinterpret_cast<stmt_ty>(asdl_seq_GET(seq, _i));
    switch (stmt->kind) {
      case Import_kind: {
        auto importNames = stmt->v.Import.names;
        if (asdl_seq_LEN(importNames) == 1) {
          alias_ty alias =
              reinterpret_cast<alias_ty>(asdl_seq_GET(importNames, 0));
          if (alias->asname) {
            return ModuleKind::kNonStrict;
          }
          const char* aliasNameStr = PyUnicode_AsUTF8(alias->name);
          const char* strictFlag = "__strict__";
          const char* staticFlag = "__static__";
          if (strncmp(aliasNameStr, staticFlag, strlen(staticFlag)) == 0) {
            return ModuleKind::kStatic;
          } else if (
              strncmp(aliasNameStr, strictFlag, strlen(strictFlag)) == 0) {
            return ModuleKind::kStrict;
          } else {
            return ModuleKind::kNonStrict;
          }
        }
      }
      case Expr_kind: {
        if (!seenDocStr) {
          auto expr = stmt->v.Expr.value;
          if (expr->kind == Constant_kind &&
              PyUnicode_Check(expr->v.Constant.value)) {
            seenDocStr = true;
            continue;
          }
        }
        return ModuleKind::kNonStrict;
      }
      case ImportFrom_kind: {
        auto importFromStmt = stmt->v.ImportFrom;
        const char* modName = PyUnicode_AsUTF8(importFromStmt.module);
        const char* futureFlag = "__future__";
        // skip future imports
        if (strncmp(modName, futureFlag, strlen(futureFlag)) == 0) {
          continue;
        }
        // encountered an import, not strict
        return ModuleKind::kNonStrict;
      }
      default:
        return ModuleKind::kNonStrict;
    }
  }
  return ModuleKind::kNonStrict;
}

ModuleKind getModuleKind(const mod_ty ast) {
  switch (ast->kind) {
    case Module_kind:
      return getModuleKindFromStmts(ast->v.Module.body);
    case Interactive_kind:
    case Expression_kind:
    case FunctionType_kind:
    case Suite_kind:
    default:
      return ModuleKind::kNonStrict;
  }
  return ModuleKind::kNonStrict;
}

AnalyzedModule* ModuleLoader::loadModule(const char* modName) {
  return loadModule(std::string(modName));
}

AnalyzedModule* ModuleLoader::loadModule(const std::string& modName) {
  char delimiter = '.';
  auto end = modName.find(delimiter);
  while (end != std::string::npos) {
    auto mod = loadSingleModule(modName.substr(0, end));
    if (!mod) {
      return nullptr;
    }
    end = modName.find(delimiter, end);
  }
  return loadSingleModule(modName);
}

AnalyzedModule* ModuleLoader::loadSingleModule(const std::string& modName) {
  // look for py source code
  auto modInfo = findModule(modName, FileSuffixKind::kPythonFile);
  if (!modInfo) {
    return nullptr;
  }
  return analyze(std::move(modInfo));
}

bool ModuleLoader::setImportPath(std::vector<std::string> importPath) {
  importPath_ = std::move(importPath);
  return true;
}

AnalyzedModule* ModuleLoader::loadModuleFromSource(
    const std::string& source,
    const std::string& name,
    const std::string& filename,
    std::vector<std::string> searchLocations) {
  auto readResult = readFromSource(source.c_str(), filename.c_str(), arena_);
  if (readResult) {
    AstAndSymbols& result = readResult.value();
    auto modinfo = std::make_unique<ModuleInfo>(
        std::move(name),
        std::move(filename),
        result.ast,
        result.futureAnnotations,
        std::move(result.symbols),
        std::move(searchLocations));
    return analyze(std::move(modinfo));
  }
  return nullptr;
}

std::unique_ptr<ModuleInfo> ModuleLoader::findModule(
    const std::string& modName,
    const std::vector<std::string>& searchLocations,
    FileSuffixKind suffixKind) {
  // replace module separator '.' with file path separator
  size_t pos = 0;
  std::string modPathStr(modName);
  while ((pos = modPathStr.find('.', pos)) != std::string::npos) {
    modPathStr.replace(pos, 1, 1, std::filesystem::path::preferred_separator);
    pos += 1;
  }

  const char* suffix = getFileSuffixKindName(suffixKind);
  for (const std::string& importPath : searchLocations) {
    std::filesystem::path modPath = importPath;
    modPath /= modPathStr;
    modPath += suffix;
    const char* modPathCstr = modPath.c_str();
    auto readResult = readFromFile(modPathCstr, arena_);
    if (readResult) {
      AstAndSymbols& result = readResult.value();
      return std::make_unique<ModuleInfo>(
          std::move(modName),
          modPath.string(),
          result.ast,
          result.futureAnnotations,
          std::move(result.symbols));
    }
  }
  return nullptr;
}

std::unique_ptr<ModuleInfo> ModuleLoader::findModule(
    const std::string& modName,
    FileSuffixKind suffixKind) {
  return findModule(modName, importPath_, suffixKind);
}

AnalyzedModule* ModuleLoader::analyze(std::unique_ptr<ModuleInfo> modInfo) {
  const mod_ty ast = modInfo->getAst();
  const std::string& name = modInfo->getModName();

  // Following python semantics, publish the module before ast visits
  auto errorSink = errorSinkFactory_();
  BaseErrorSink* errorSinkBorrowed = errorSink.get();
  AnalyzedModule* analyzedModule =
      new AnalyzedModule(getModuleKind(ast), std::move(errorSink));
  modules_[name] = std::unique_ptr<AnalyzedModule>(analyzedModule);
  if (!analyzedModule->isStrict() &&
      !(forceStrict_ && forceStrict_.value()(name, modInfo->getFilename()))) {
    return analyzedModule;
  }
  // Run ast visits
  auto globalScope = std::shared_ptr(objects::getBuiltinsDict());
  auto mod =
      StrictModuleObject::makeStrictModule(ModuleType(), name, globalScope);
  analyzedModule->setModuleValue(mod);

  Analyzer analyzer(
      ast,
      this,
      Symtable(modInfo->passSymtable()),
      globalScope,
      errorSinkBorrowed,
      modInfo->getFilename(),
      "<module>",
      mod);
  analyzer.analyze();

  return analyzedModule;
}

} // namespace strictmod::compiler
