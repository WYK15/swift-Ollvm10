//===-- SwiftLanguageRuntimeDynamicTypeResolution.cpp ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SwiftLanguageRuntimeImpl.h"
#include "lldb/Target/SwiftLanguageRuntime.h"

#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ProcessStructReader.h"
#include "lldb/Target/Target.h"
#include "swift/AST/Types.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/Reflection/ReflectionContext.h"
#include "swift/Reflection/TypeRefBuilder.h"
#include "swift/Remote/MemoryReader.h"
#include "swift/RemoteAST/RemoteAST.h"

using namespace lldb;
using namespace lldb_private;

namespace lldb_private {
swift::Type GetSwiftType(CompilerType type) {
  auto *ts = type.GetTypeSystem();
  if (auto *tr = llvm::dyn_cast_or_null<TypeSystemSwiftTypeRef>(ts))
    return tr->GetSwiftType(type);
  if (auto *ast = llvm::dyn_cast_or_null<SwiftASTContext>(ts))
    return ast->GetSwiftType(type);
  return {};
}

swift::CanType GetCanonicalSwiftType(CompilerType type) {
  swift::Type swift_type = nullptr;
  auto *ts = type.GetTypeSystem();
  if (auto *tr = llvm::dyn_cast_or_null<TypeSystemSwiftTypeRef>(ts))
    swift_type = tr->GetSwiftType(type);
  if (auto *ast = llvm::dyn_cast_or_null<SwiftASTContext>(ts))
    swift_type = ast->GetSwiftType(type);
  return swift_type ? swift_type->getCanonicalType() : swift::CanType();
}

static lldb::addr_t
MaskMaybeBridgedPointer(Process &process, lldb::addr_t addr,
                        lldb::addr_t *masked_bits = nullptr) {
  const ArchSpec &arch_spec(process.GetTarget().GetArchitecture());
  ArchSpec::Core core_kind = arch_spec.GetCore();
  bool is_arm = false;
  bool is_intel = false;
  bool is_s390x = false;
  bool is_32 = false;
  bool is_64 = false;
  if (core_kind == ArchSpec::Core::eCore_arm_arm64) {
    is_arm = is_64 = true;
  } else if (core_kind >= ArchSpec::Core::kCore_arm_first &&
             core_kind <= ArchSpec::Core::kCore_arm_last) {
    is_arm = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_64_first &&
             core_kind <= ArchSpec::Core::kCore_x86_64_last) {
    is_intel = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_32_first &&
             core_kind <= ArchSpec::Core::kCore_x86_32_last) {
    is_intel = true;
  } else if (core_kind == ArchSpec::Core::eCore_s390x_generic) {
    is_s390x = true;
  } else {
    // this is a really random CPU core to be running on - just get out fast
    return addr;
  }

  switch (arch_spec.GetAddressByteSize()) {
  case 4:
    is_32 = true;
    break;
  case 8:
    is_64 = true;
    break;
  default:
    // this is a really random pointer size to be running on - just get out fast
    return addr;
  }

  lldb::addr_t mask = 0;

  if (is_arm && is_64)
    mask = SWIFT_ABI_ARM64_SWIFT_SPARE_BITS_MASK;

  if (is_arm && is_32)
    mask = SWIFT_ABI_ARM_SWIFT_SPARE_BITS_MASK;

  if (is_intel && is_64)
    mask = SWIFT_ABI_X86_64_SWIFT_SPARE_BITS_MASK;

  if (is_intel && is_32)
    mask = SWIFT_ABI_I386_SWIFT_SPARE_BITS_MASK;

  if (is_s390x && is_64)
    mask = SWIFT_ABI_S390X_SWIFT_SPARE_BITS_MASK;

  if (masked_bits)
    *masked_bits = addr & mask;
  return addr & ~mask;
}

lldb::addr_t
SwiftLanguageRuntime::MaskMaybeBridgedPointer(lldb::addr_t addr,
                                              lldb::addr_t *masked_bits) {
  return m_process ? ::MaskMaybeBridgedPointer(*m_process, addr, masked_bits)
                   : addr;
}

lldb::addr_t SwiftLanguageRuntime::MaybeMaskNonTrivialReferencePointer(
    lldb::addr_t addr,
    SwiftASTContext::NonTriviallyManagedReferenceStrategy strategy) {

  if (addr == 0)
    return addr;

  AppleObjCRuntime *objc_runtime = GetObjCRuntime();

  if (objc_runtime) {
    // tagged pointers don't perform any masking
    if (objc_runtime->IsTaggedPointer(addr))
      return addr;
  }

  if (!m_process)
    return addr;
  const ArchSpec &arch_spec(m_process->GetTarget().GetArchitecture());
  ArchSpec::Core core_kind = arch_spec.GetCore();
  bool is_arm = false;
  bool is_intel = false;
  bool is_32 = false;
  bool is_64 = false;
  if (core_kind == ArchSpec::Core::eCore_arm_arm64) {
    is_arm = is_64 = true;
  } else if (core_kind >= ArchSpec::Core::kCore_arm_first &&
             core_kind <= ArchSpec::Core::kCore_arm_last) {
    is_arm = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_64_first &&
             core_kind <= ArchSpec::Core::kCore_x86_64_last) {
    is_intel = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_32_first &&
             core_kind <= ArchSpec::Core::kCore_x86_32_last) {
    is_intel = true;
  } else {
    // this is a really random CPU core to be running on - just get out fast
    return addr;
  }

  switch (arch_spec.GetAddressByteSize()) {
  case 4:
    is_32 = true;
    break;
  case 8:
    is_64 = true;
    break;
  default:
    // this is a really random pointer size to be running on - just get out fast
    return addr;
  }

  lldb::addr_t mask = 0;

  if (strategy ==
      SwiftASTContext::NonTriviallyManagedReferenceStrategy::eWeak) {
    bool is_indirect = true;

    // On non-objc platforms, the weak reference pointer always pointed to a
    // runtime structure.
    // For ObjC platforms, the masked value determines whether it is indirect.

    uint32_t value = 0;

    if (objc_runtime) {

      if (is_intel) {
        if (is_64) {
          mask = SWIFT_ABI_X86_64_OBJC_WEAK_REFERENCE_MARKER_MASK;
          value = SWIFT_ABI_X86_64_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        } else {
          mask = SWIFT_ABI_I386_OBJC_WEAK_REFERENCE_MARKER_MASK;
          value = SWIFT_ABI_I386_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        }
      } else if (is_arm) {
        if (is_64) {
          mask = SWIFT_ABI_ARM64_OBJC_WEAK_REFERENCE_MARKER_MASK;
          value = SWIFT_ABI_ARM64_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        } else {
          mask = SWIFT_ABI_ARM_OBJC_WEAK_REFERENCE_MARKER_MASK;
          value = SWIFT_ABI_ARM_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        }
      }
    } else {
      // This name is a little confusing. The "DEFAULT" marking in System.h
      // is supposed to mean: the value for non-ObjC platforms.  So
      // DEFAULT_OBJC here actually means "non-ObjC".
      mask = SWIFT_ABI_DEFAULT_OBJC_WEAK_REFERENCE_MARKER_MASK;
      value = SWIFT_ABI_DEFAULT_OBJC_WEAK_REFERENCE_MARKER_VALUE;
    }

    is_indirect = ((addr & mask) == value);

    if (!is_indirect)
      return addr;

    // The masked value of address is a pointer to the runtime structure.
    // The first field of the structure is the actual pointer.
    Process *process = GetProcess();
    Status error;

    lldb::addr_t masked_addr = addr & ~mask;
    lldb::addr_t isa_addr = process->ReadPointerFromMemory(masked_addr, error);
    if (error.Fail()) {
      LLDB_LOG(
          GetLogIfAnyCategoriesSet(LIBLLDB_LOG_EXPRESSIONS | LIBLLDB_LOG_TYPES),
          "Couldn't deref masked pointer");
      return addr;
    }
    return isa_addr;

  } else {
    if (is_arm && is_64)
      mask = SWIFT_ABI_ARM64_OBJC_NUM_RESERVED_LOW_BITS;
    else if (is_intel && is_64)
      mask = SWIFT_ABI_X86_64_OBJC_NUM_RESERVED_LOW_BITS;
    else
      mask = SWIFT_ABI_DEFAULT_OBJC_NUM_RESERVED_LOW_BITS;

    mask = (1 << mask) | (1 << (mask + 1));

    return addr & ~mask;
  }

  return addr;
}

const CompilerType &SwiftLanguageRuntimeImpl::GetBoxMetadataType() {
  if (m_box_metadata_type.IsValid())
    return m_box_metadata_type;

  static ConstString g_type_name("__lldb_autogen_boxmetadata");
  const bool is_packed = false;
  if (TypeSystemClang *ast_ctx =
          TypeSystemClang::GetScratch(m_process.GetTarget())) {
    CompilerType voidstar =
        ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
    CompilerType uint32 = ast_ctx->GetIntTypeFromBitSize(32, false);

    m_box_metadata_type = ast_ctx->GetOrCreateStructForIdentifier(
        g_type_name, {{"kind", voidstar}, {"offset", uint32}}, is_packed);
  }

  return m_box_metadata_type;
}

class LLDBMemoryReader : public swift::remote::MemoryReader {
public:
  LLDBMemoryReader(Process &p, size_t max_read_amount = INT32_MAX)
      : m_process(p) {
    m_max_read_amount = max_read_amount;
  }

  virtual ~LLDBMemoryReader() = default;

  bool queryDataLayout(DataLayoutQueryType type, void *inBuffer,
                       void *outBuffer) override {
    switch (type) {
    case DLQ_GetPointerSize: {
      auto result = static_cast<uint8_t *>(outBuffer);
      *result = m_process.GetAddressByteSize();
      return true;
    }
    case DLQ_GetSizeSize: {
      auto result = static_cast<uint8_t *>(outBuffer);
      *result = m_process.GetAddressByteSize(); // FIXME: sizeof(size_t)
      return true;
    }
    }

    return false;
  }

  swift::remote::RemoteAddress
  getSymbolAddress(const std::string &name) override {
    lldbassert(!name.empty());
    if (name.empty())
      return swift::remote::RemoteAddress(nullptr);

    LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
             "[MemoryReader] asked to retrieve the address of symbol {0}",
             name);

    ConstString name_cs(name.c_str(), name.size());
    SymbolContextList sc_list;
    m_process.GetTarget().GetImages().FindSymbolsWithNameAndType(
        name_cs, lldb::eSymbolTypeAny, sc_list);
    if (!sc_list.GetSize()) {
      LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
               "[MemoryReader] symbol resolution failed {0}", name);
      return swift::remote::RemoteAddress(nullptr);
    }

    SymbolContext sym_ctx;
    // Remove undefined symbols from the list.
    size_t num_sc_matches = sc_list.GetSize();
    if (num_sc_matches > 1) {
      SymbolContextList tmp_sc_list(sc_list);
      sc_list.Clear();
      for (size_t idx = 0; idx < num_sc_matches; idx++) {
        tmp_sc_list.GetContextAtIndex(idx, sym_ctx);
        if (sym_ctx.symbol &&
            sym_ctx.symbol->GetType() != lldb::eSymbolTypeUndefined) {
          sc_list.Append(sym_ctx);
        }
      }
    }
    if (sc_list.GetSize() == 1 && sc_list.GetContextAtIndex(0, sym_ctx)) {
      if (sym_ctx.symbol) {
        auto load_addr = sym_ctx.symbol->GetLoadAddress(&m_process.GetTarget());
        LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
                 "[MemoryReader] symbol resolved to 0x%" PRIx64, load_addr);
        return swift::remote::RemoteAddress(load_addr);
      }
    }

    // Empty list, resolution failed.
    if (sc_list.GetSize() == 0) {
      LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
               "[MemoryReader] symbol resoution failed {0}", name);
      return swift::remote::RemoteAddress(nullptr);
    }

    // If there's a single symbol, then we're golden. If there's more than
    // a symbol, then just make sure all of them agree on the value.
    Status error;
    auto sym = sc_list.GetContextAtIndex(0, sym_ctx);
    auto load_addr = sym_ctx.symbol->GetLoadAddress(&m_process.GetTarget());
    uint64_t sym_value = m_process.GetTarget().ReadUnsignedIntegerFromMemory(
        load_addr, false, m_process.GetAddressByteSize(), 0, error);
    for (unsigned i = 1; i < sc_list.GetSize(); ++i) {
      auto other_sym = sc_list.GetContextAtIndex(i, sym_ctx);
      auto other_load_addr =
          sym_ctx.symbol->GetLoadAddress(&m_process.GetTarget());
      uint64_t other_sym_value =
          m_process.GetTarget().ReadUnsignedIntegerFromMemory(
              load_addr, false, m_process.GetAddressByteSize(), 0, error);
      if (sym_value != other_sym_value) {
        LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
                 "[MemoryReader] symbol resoution failed {0}", name);
        return swift::remote::RemoteAddress(nullptr);
      }
    }
    LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
             "[MemoryReader] symbol resolved to {0}", load_addr);
    return swift::remote::RemoteAddress(load_addr);
  }

  bool readBytes(swift::remote::RemoteAddress address, uint8_t *dest,
                 uint64_t size) override {
    if (m_local_buffer) {
      auto addr = address.getAddressData();
      if (addr >= m_local_buffer &&
          addr + size <= m_local_buffer + m_local_buffer_size) {
        // If this crashes, the assumptions stated in
        // GetDynamicTypeAndAddress_Protocol() most likely no longer
        // hold.
        memcpy(dest, (void *)addr, size);
        return true;
      }
    }

    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf("[MemoryReader] asked to read %" PRIu64
                  " bytes at address 0x%" PRIx64,
                  size, address.getAddressData());

    if (size > m_max_read_amount) {
      if (log)
        log->Printf("[MemoryReader] memory read exceeds maximum allowed size");
      return false;
    }

    Target &target(m_process.GetTarget());
    Address addr(address.getAddressData());
    Status error;
    if (size > target.ReadMemory(addr, false, dest, size, error)) {
      if (log)
        log->Printf(
            "[MemoryReader] memory read returned fewer bytes than asked for");
      return false;
    }
    if (error.Fail()) {
      if (log)
        log->Printf("[MemoryReader] memory read returned error: %s",
                    error.AsCString());
      return false;
    }

    if (log && log->GetVerbose()) {
      StreamString stream;
      for (uint64_t i = 0; i < size; i++) {
        stream.PutHex8(dest[i]);
        stream.PutChar(' ');
      }
      log->Printf("[MemoryReader] memory read returned data: %s",
                  stream.GetData());
    }

    return true;
  }

  bool readString(swift::remote::RemoteAddress address,
                  std::string &dest) override {
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf(
          "[MemoryReader] asked to read string data at address 0x%" PRIx64,
          address.getAddressData());

    uint32_t read_size = 50 * 1024;
    std::vector<char> storage(read_size, 0);
    Target &target(m_process.GetTarget());
    Address addr(address.getAddressData());
    Status error;
    target.ReadCStringFromMemory(addr, &storage[0], storage.size(), error);
    if (error.Success()) {
      dest.assign(&storage[0]);
      if (log)
        log->Printf("[MemoryReader] memory read returned data: %s",
                    dest.c_str());
      return true;
    } else {
      if (log)
        log->Printf("[MemoryReader] memory read returned error: %s",
                    error.AsCString());
      return false;
    }
  }

  void pushLocalBuffer(uint64_t local_buffer, uint64_t local_buffer_size) {
    lldbassert(!m_local_buffer);
    m_local_buffer = local_buffer;
    m_local_buffer_size = local_buffer_size;
  }

  void popLocalBuffer() {
    lldbassert(m_local_buffer);
    m_local_buffer = 0;
    m_local_buffer_size = 0;
  }

private:
  Process &m_process;
  size_t m_max_read_amount;

  uint64_t m_local_buffer = 0;
  uint64_t m_local_buffer_size = 0;
};

std::shared_ptr<swift::remote::MemoryReader>
SwiftLanguageRuntimeImpl::GetMemoryReader() {
  if (!m_memory_reader_sp)
    m_memory_reader_sp.reset(new LLDBMemoryReader(m_process));

  return m_memory_reader_sp;
}

void SwiftLanguageRuntimeImpl::PushLocalBuffer(uint64_t local_buffer,
                                               uint64_t local_buffer_size) {
  ((LLDBMemoryReader *)GetMemoryReader().get())
      ->pushLocalBuffer(local_buffer, local_buffer_size);
}

void SwiftLanguageRuntimeImpl::PopLocalBuffer() {
  ((LLDBMemoryReader *)GetMemoryReader().get())->popLocalBuffer();
}

SwiftLanguageRuntime::MetadataPromise::MetadataPromise(
    ValueObject &for_object, SwiftLanguageRuntimeImpl &runtime,
    lldb::addr_t location)
    : m_for_object_sp(for_object.GetSP()), m_swift_runtime(runtime),
      m_metadata_location(location) {}

CompilerType
SwiftLanguageRuntime::MetadataPromise::FulfillTypePromise(Status *error) {
  if (error)
    error->Clear();

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  if (log)
    log->Printf("[MetadataPromise] asked to fulfill type promise at location "
                "0x%" PRIx64,
                m_metadata_location);

  if (m_compiler_type.hasValue())
    return m_compiler_type.getValue();

  auto swift_ast_ctx = m_for_object_sp->GetScratchSwiftASTContext();
  if (!swift_ast_ctx) {
    error->SetErrorString("couldn't get Swift scratch context");
    return CompilerType();
  }
  auto &remote_ast = m_swift_runtime.GetRemoteASTContext(*swift_ast_ctx);
  swift::remoteAST::Result<swift::Type> result =
      remote_ast.getTypeForRemoteTypeMetadata(
          swift::remote::RemoteAddress(m_metadata_location));

  if (result) {
    m_compiler_type = {swift_ast_ctx.get(), result.getValue().getPointer()};
    if (log)
      log->Printf("[MetadataPromise] result is type %s",
                  m_compiler_type->GetTypeName().AsCString());
    return m_compiler_type.getValue();
  } else {
    const auto &failure = result.getFailure();
    if (error)
      error->SetErrorStringWithFormat("error in resolving type: %s",
                                      failure.render().c_str());
    if (log)
      log->Printf("[MetadataPromise] failure: %s", failure.render().c_str());
    return (m_compiler_type = CompilerType()).getValue();
  }
}

SwiftLanguageRuntime::MetadataPromiseSP
SwiftLanguageRuntimeImpl::GetMetadataPromise(lldb::addr_t addr,
                                             ValueObject &for_object) {
  auto swift_ast_ctx = for_object.GetScratchSwiftASTContext();
  if (!swift_ast_ctx || swift_ast_ctx->HasFatalErrors())
    return nullptr;

  if (addr == 0 || addr == LLDB_INVALID_ADDRESS)
    return nullptr;

  auto key = std::make_pair(swift_ast_ctx->GetASTContext(), addr);
  auto iter = m_promises_map.find(key);
  if (iter != m_promises_map.end())
    return iter->second;

  SwiftLanguageRuntime::MetadataPromiseSP promise_sp(
      new SwiftLanguageRuntime::MetadataPromise(for_object, *this, addr));
  m_promises_map.insert({key, promise_sp});
  return promise_sp;
}

swift::remoteAST::RemoteASTContext &
SwiftLanguageRuntimeImpl::GetRemoteASTContext(SwiftASTContext &swift_ast_ctx) {
  // If we already have a remote AST context for this AST context,
  // return it.
  auto known = m_remote_ast_contexts.find(swift_ast_ctx.GetASTContext());
  if (known != m_remote_ast_contexts.end())
    return *known->second;

  // Initialize a new remote AST context.
  (void)GetReflectionContext();
  auto remote_ast_up = std::make_unique<swift::remoteAST::RemoteASTContext>(
      *swift_ast_ctx.GetASTContext(), GetMemoryReader());
  auto &remote_ast = *remote_ast_up;
  m_remote_ast_contexts.insert(
      {swift_ast_ctx.GetASTContext(), std::move(remote_ast_up)});
  return remote_ast;
}

void SwiftLanguageRuntimeImpl::ReleaseAssociatedRemoteASTContext(
    swift::ASTContext *ctx) {
  m_remote_ast_contexts.erase(ctx);
}

namespace {
class ASTVerifier : public swift::ASTWalker {
  bool hasMissingPatterns = false;

  bool walkToDeclPre(swift::Decl *D) override {
    if (auto *PBD = llvm::dyn_cast<swift::PatternBindingDecl>(D)) {
      if (PBD->getPatternList().empty()) {
        hasMissingPatterns = true;
        return false;
      }
    }
    return true;
  }

public:
  /// Detect (one form of) incomplete types. These may appear if
  /// member variables have Clang-imported types that couldn't be
  /// resolved.
  static bool Verify(swift::Decl *D) {
    if (!D)
      return false;

    ASTVerifier verifier;
    D->walk(verifier);
    return !verifier.hasMissingPatterns;
  }
};

} // namespace

llvm::Optional<uint64_t> SwiftLanguageRuntimeImpl::GetMemberVariableOffset(
    CompilerType instance_type, ValueObject *instance, ConstString member_name,
    Status *error) {
  if (!instance_type.IsValid())
    return llvm::None;

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  // Using the module context for RemoteAST is cheaper bit only safe
  // when there is no dynamic type resolution involved.
  if (auto *ts = llvm::dyn_cast_or_null<TypeSystemSwiftTypeRef>(
          instance_type.GetTypeSystem()))
    instance_type = ts->ReconstructType(instance_type);

  auto *module_ctx =
      llvm::dyn_cast_or_null<SwiftASTContext>(instance_type.GetTypeSystem());
  if (!module_ctx || module_ctx->HasFatalErrors())
    return llvm::None;

  llvm::Optional<SwiftASTContextReader> scratch_ctx;
  if (instance) {
    if (SwiftASTContextReader reader = instance->GetScratchSwiftASTContext())
      scratch_ctx = reader;
    else
      return llvm::None;
  }

  auto *remote_ast = &GetRemoteASTContext(*module_ctx);

  if (log)
    log->Printf(
        "[GetMemberVariableOffset] asked to resolve offset for member %s",
        member_name.AsCString());

  // Check whether we've already cached this offset.
  swift::TypeBase *swift_type =
      GetCanonicalSwiftType(instance_type).getPointer();

  // Perform the cache lookup.
  MemberID key{swift_type, member_name.GetCString()};
  auto it = m_member_offsets.find(key);
  if (it != m_member_offsets.end())
    return it->second;

  // Dig out metadata describing the type, if it's easy to find.
  // FIXME: the Remote AST library should make this easier.
  swift::remote::RemoteAddress optmeta(nullptr);
  const swift::TypeKind type_kind = swift_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass: {
    if (log)
      log->Printf("[MemberVariableOffsetResolver] type is a class - trying to "
                  "get metadata for valueobject %s",
                  (instance ? instance->GetName().AsCString() : "<null>"));
    if (instance) {
      lldb::addr_t pointer = instance->GetPointerValue();
      if (!pointer || pointer == LLDB_INVALID_ADDRESS)
        break;
      swift::remote::RemoteAddress address(pointer);
      if (auto metadata = remote_ast->getHeapMetadataForObject(address))
        optmeta = metadata.getValue();
    }
    if (log)
      log->Printf("[MemberVariableOffsetResolver] optmeta = 0x%" PRIx64,
                  optmeta.getAddressData());
    break;
  }

  default:
    // Bind generic parameters if necessary.
    if (instance && swift_type->hasTypeParameter())
      if (auto *frame = instance->GetExecutionContextRef().GetFrameSP().get())
        if (auto bound = DoArchetypeBindingForType(*frame, instance_type)) {
          if (log)
            log->Printf(
                "[MemberVariableOffsetResolver] resolved non-class type = %s",
                bound.GetTypeName().AsCString());

          swift_type = GetCanonicalSwiftType(bound).getPointer();
          MemberID key{swift_type, member_name.GetCString()};
          auto it = m_member_offsets.find(key);
          if (it != m_member_offsets.end())
            return it->second;

          assert(bound.GetTypeSystem() == scratch_ctx->get());
          remote_ast = &GetRemoteASTContext(*scratch_ctx->get());
        }
  }

  // Try to determine whether it is safe to use RemoteAST.  RemoteAST
  // is faster than RemoteMirrors, but can't do dynamic types (checked
  // inside RemoteAST) or incomplete types (checked here).
  bool safe_to_use_remote_ast = true;
  if (swift::Decl *type_decl = swift_type->getNominalOrBoundGenericNominal())
    safe_to_use_remote_ast &= ASTVerifier::Verify(type_decl);

  // Use RemoteAST to determine the member offset.
  if (safe_to_use_remote_ast) {
    swift::remoteAST::Result<uint64_t> result = remote_ast->getOffsetOfMember(
        swift_type, optmeta, member_name.GetStringRef());
    if (result) {
      if (log)
        log->Printf(
            "[MemberVariableOffsetResolver] offset discovered = %" PRIu64,
            (uint64_t)result.getValue());

      // Cache this result.
      MemberID key{swift_type, member_name.GetCString()};
      m_member_offsets.insert({key, result.getValue()});
      return result.getValue();
    }

    const auto &failure = result.getFailure();
    if (error)
      error->SetErrorStringWithFormat("error in resolving type offset: %s",
                                      failure.render().c_str());
    if (log)
      log->Printf("[MemberVariableOffsetResolver] failure: %s",
                  failure.render().c_str());
  }

  // Try remote mirrors.
  const swift::reflection::TypeInfo *type_info = GetTypeInfo(instance_type);
  if (!type_info)
    return llvm::None;
  auto record_type_info =
      llvm::dyn_cast<swift::reflection::RecordTypeInfo>(type_info);
  if (record_type_info) {
    // Handle tuples.
    if (record_type_info->getRecordKind() ==
        swift::reflection::RecordKind::Tuple) {
      unsigned tuple_idx;
      if (member_name.GetStringRef().getAsInteger(10, tuple_idx) ||
          tuple_idx >= record_type_info->getNumFields()) {
        if (error)
          error->SetErrorString("tuple index out of bounds");
        return llvm::None;
      }
      return record_type_info->getFields()[tuple_idx].Offset;
    }

    // Handle other record types.
    for (auto &field : record_type_info->getFields()) {
      if (ConstString(field.Name) == member_name)
        return field.Offset;
    }
  }

  lldb::addr_t pointer = instance->GetPointerValue();
  auto *reflection_ctx = GetReflectionContext();
  if (!reflection_ctx)
    return llvm::None;

  auto class_instance_type_info = reflection_ctx->getInstanceTypeInfo(pointer);
  if (class_instance_type_info) {
    auto class_type_info = llvm::dyn_cast<swift::reflection::RecordTypeInfo>(
        class_instance_type_info);
    if (class_type_info) {
      for (auto &field : class_type_info->getFields()) {
        if (ConstString(field.Name) == member_name)
          return field.Offset;
      }
    }
  }
  return llvm::None;
}

bool SwiftLanguageRuntime::IsSelf(Variable &variable) {
  // A variable is self if its name if "self", and it's either a
  // function argument or a local variable and it's scope is a
  // constructor. These checks are sorted from cheap to expensive.
  if (variable.GetUnqualifiedName().GetStringRef() != "self")
    return false;

  if (variable.GetScope() == lldb::eValueTypeVariableArgument)
    return true;

  if (variable.GetScope() != lldb::eValueTypeVariableLocal)
    return false;

  SymbolContextScope *sym_ctx_scope = variable.GetSymbolContextScope();
  if (!sym_ctx_scope)
    return false;
  Function *function = sym_ctx_scope->CalculateSymbolContextFunction();
  if (!function)
    return false;
  StringRef func_name = function->GetMangled().GetMangledName().GetStringRef();
  swift::Demangle::Context demangle_ctx;
  swift::Demangle::NodePointer node_ptr =
      demangle_ctx.demangleSymbolAsNode(func_name);
  if (!node_ptr)
    return false;
  if (node_ptr->getKind() != swift::Demangle::Node::Kind::Global)
    return false;
  if (node_ptr->getNumChildren() != 1)
    return false;
  node_ptr = node_ptr->getFirstChild();
  return node_ptr->getKind() == swift::Demangle::Node::Kind::Constructor ||
    node_ptr->getKind() == swift::Demangle::Node::Kind::Allocator;
}

/// Determine whether the scratch SwiftASTContext has been locked.
static bool IsScratchContextLocked(Target &target) {
  if (target.GetSwiftScratchContextLock().try_lock()) {
    target.GetSwiftScratchContextLock().unlock();
    return false;
  }
  return true;
}

/// Determine whether the scratch SwiftASTContext has been locked.
static bool IsScratchContextLocked(TargetSP target) {
  return target ? IsScratchContextLocked(*target) : true;
}

bool SwiftLanguageRuntimeImpl::GetDynamicTypeAndAddress_Class(
    ValueObject &in_value, SwiftASTContextForExpressions &scratch_ctx,
    lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
    Address &address) {
  AddressType address_type;
  lldb::addr_t class_metadata_ptr = in_value.GetPointerValue(&address_type);
  if (class_metadata_ptr == LLDB_INVALID_ADDRESS || class_metadata_ptr == 0)
    return false;
  address.SetRawAddress(class_metadata_ptr);

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  auto &remote_ast = GetRemoteASTContext(scratch_ctx);
  swift::remote::RemoteAddress instance_address(class_metadata_ptr);
  auto metadata_address = remote_ast.getHeapMetadataForObject(instance_address);
  if (!metadata_address) {
    if (log) {
      log->Printf("could not read heap metadata for object at %lu: %s\n",
                  class_metadata_ptr,
                  metadata_address.getFailure().render().c_str());
    }

    return false;
  }

  auto instance_type =
      remote_ast.getTypeForRemoteTypeMetadata(metadata_address.getValue(),
                                              /*skipArtificial=*/true);
  if (!instance_type) {
    if (log) {
      log->Printf("could not get type metadata from address %" PRIu64 " : %s\n",
                  metadata_address.getValue().getAddressData(),
                  instance_type.getFailure().render().c_str());
    }
    return false;
  }

  // The read lock must have been acquired by the caller.
  class_type_or_name.SetCompilerType(
      {&scratch_ctx, instance_type.getValue().getPointer()});
  return true;
}

bool SwiftLanguageRuntimeImpl::IsValidErrorValue(ValueObject &in_value) {
  CompilerType var_type = in_value.GetStaticValue()->GetCompilerType();
  SwiftASTContext::ProtocolInfo protocol_info;
  if (!SwiftASTContext::GetProtocolTypeInfo(var_type, protocol_info))
    return false;
  if (!protocol_info.m_is_errortype)
    return false;

  unsigned index = SwiftASTContext::ProtocolInfo::error_instance_index;
  ValueObjectSP instance_type_sp(
      in_value.GetStaticValue()->GetChildAtIndex(index, true));
  if (!instance_type_sp)
    return false;
  lldb::addr_t metadata_location = instance_type_sp->GetValueAsUnsigned(0);
  if (metadata_location == 0 || metadata_location == LLDB_INVALID_ADDRESS)
    return false;

  if (auto swift_native_nserror_isa = GetSwiftNativeNSErrorISA()) {
    if (auto objc_runtime = SwiftLanguageRuntime::GetObjCRuntime(m_process)) {
      if (auto descriptor =
              objc_runtime->GetClassDescriptor(*instance_type_sp)) {
        if (descriptor->GetISA() != *swift_native_nserror_isa) {
          // not a __SwiftNativeNSError - but statically typed as ErrorType
          // return true here
          return true;
        }
      }
    }
  }

  if (SwiftLanguageRuntime::GetObjCRuntime(m_process)) {
    // this is a swift native error but it can be bridged to ObjC
    // so it needs to be layout compatible

    size_t ptr_size = m_process.GetAddressByteSize();
    size_t metadata_offset =
        ptr_size + 4 + (ptr_size == 8 ? 4 : 0);        // CFRuntimeBase
    metadata_offset += ptr_size + ptr_size + ptr_size; // CFIndex + 2*CFRef

    metadata_location += metadata_offset;
    Status error;
    lldb::addr_t metadata_ptr_value =
        m_process.ReadPointerFromMemory(metadata_location, error);
    if (metadata_ptr_value == 0 || metadata_ptr_value == LLDB_INVALID_ADDRESS ||
        error.Fail())
      return false;
  } else {
    // this is a swift native error and it has no way to be bridged to ObjC
    // so it adopts a more compact layout

    Status error;

    size_t ptr_size = m_process.GetAddressByteSize();
    size_t metadata_offset = 2 * ptr_size;
    metadata_location += metadata_offset;
    lldb::addr_t metadata_ptr_value =
        m_process.ReadPointerFromMemory(metadata_location, error);
    if (metadata_ptr_value == 0 || metadata_ptr_value == LLDB_INVALID_ADDRESS ||
        error.Fail())
      return false;
  }

  return true;
}

bool SwiftLanguageRuntimeImpl::GetDynamicTypeAndAddress_Protocol(
    ValueObject &in_value, CompilerType protocol_type,
    SwiftASTContextForExpressions &scratch_ctx,
    lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
    Address &address) {
  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  auto &target = m_process.GetTarget();
  assert(IsScratchContextLocked(target) &&
         "Swift scratch context not locked ahead");
  auto &remote_ast = GetRemoteASTContext(scratch_ctx);

  lldb::addr_t existential_address;
  bool use_local_buffer = false;

  if (in_value.GetValueType() == eValueTypeConstResult &&
      in_value.GetValue().GetValueType() ==
          lldb_private::Value::eValueTypeHostAddress) {
    if (log)
      log->Printf("existential value is a const result");

    // We have a locally materialized value that is a host address;
    // register it with MemoryReader so it does not treat it as a load
    // address.  Note that this assumes that any address at that host
    // address is also a load address. If this assumption breaks there
    // will be a crash in readBytes().
    existential_address = in_value.GetValue().GetScalar().ULongLong();
    use_local_buffer = true;
  } else {
    existential_address = in_value.GetAddressOf();
  }

  if (log)
    log->Printf("existential address is 0x%llx", existential_address);

  if (!existential_address || existential_address == LLDB_INVALID_ADDRESS)
    return false;

  if (use_local_buffer)
    PushLocalBuffer(existential_address, in_value.GetByteSize());

  swift::remote::RemoteAddress remote_existential(existential_address);
  auto result = remote_ast.getDynamicTypeAndAddressForExistential(
      remote_existential, GetSwiftType(protocol_type));

  if (use_local_buffer)
    PopLocalBuffer();

  if (!result.isSuccess()) {
    if (log)
      log->Printf("RemoteAST failed to get dynamic type of existential");
    return false;
  }

  auto type_and_address = result.getValue();
  class_type_or_name.SetCompilerType(
      ToCompilerType(type_and_address.InstanceType));
  address.SetRawAddress(type_and_address.PayloadAddress.getAddressData());
  return true;
}

SwiftLanguageRuntime::MetadataPromiseSP
SwiftLanguageRuntimeImpl::GetPromiseForTypeNameAndFrame(const char *type_name,
                                                        StackFrame *frame) {
  if (!frame || !type_name || !type_name[0])
    return nullptr;

  StreamString type_metadata_ptr_var_name;
  type_metadata_ptr_var_name.Printf("$%s", type_name);
  VariableList *var_list = frame->GetVariableList(false);
  if (!var_list)
    return nullptr;

  VariableSP var_sp(var_list->FindVariable(
      ConstString(type_metadata_ptr_var_name.GetData())));
  if (!var_sp)
    return nullptr;

  ValueObjectSP metadata_ptr_var_sp(
      frame->GetValueObjectForFrameVariable(var_sp, lldb::eNoDynamicValues));
  if (!metadata_ptr_var_sp ||
      metadata_ptr_var_sp->UpdateValueIfNeeded() == false)
    return nullptr;

  lldb::addr_t metadata_location(metadata_ptr_var_sp->GetValueAsUnsigned(0));
  if (metadata_location == 0 || metadata_location == LLDB_INVALID_ADDRESS)
    return nullptr;

  return GetMetadataPromise(metadata_location, *metadata_ptr_var_sp);
}

CompilerType
SwiftLanguageRuntimeImpl::DoArchetypeBindingForType(StackFrame &stack_frame,
                                                    CompilerType base_type) {
  auto sc = stack_frame.GetSymbolContext(lldb::eSymbolContextEverything);
  Status error;
  // A failing Clang import in a module context permanently damages
  // that module context.  Binding archetypes can trigger an import of
  // another module, so switch to a scratch context where such an
  // operation is safe.
  auto &target = m_process.GetTarget();
  assert(IsScratchContextLocked(target) &&
         "Swift scratch context not locked ahead of archetype binding");
  auto scratch_ctx = target.GetScratchSwiftASTContext(error, stack_frame);
  if (!scratch_ctx)
    return base_type;
  base_type = scratch_ctx->ImportType(base_type, error);

  if (base_type.GetTypeInfo() & lldb::eTypeIsSwift) {
    swift::Type target_swift_type(GetSwiftType(base_type));
    if (target_swift_type->hasArchetype())
      target_swift_type = target_swift_type->mapTypeOutOfContext().getPointer();

    // FIXME: This is wrong, but it doesn't actually matter right now since
    // all conformances are always visible
    auto *module_decl = scratch_ctx->GetASTContext()->getStdlibModule();

    // Replace opaque types with their underlying types when possible.
    swift::Mangle::ASTMangler mangler(true);

    // Rewrite all dynamic self types to their static self types.
    target_swift_type =
        target_swift_type.transform([](swift::Type type) -> swift::Type {
          if (auto *dynamic_self =
                  llvm::dyn_cast<swift::DynamicSelfType>(type.getPointer()))
            return dynamic_self->getSelfType();
          return type;
        });

    while (target_swift_type->hasOpaqueArchetype()) {
      auto old_type = target_swift_type;
      target_swift_type = target_swift_type.subst(
          [&](swift::SubstitutableType *type) -> swift::Type {
            auto opaque_type =
                llvm::dyn_cast<swift::OpaqueTypeArchetypeType>(type);
            if (!opaque_type)
              return type;

            // Try to find the symbol for the opaque type descriptor in the
            // process.
            auto mangled_name = ConstString(
                mangler.mangleOpaqueTypeDescriptor(opaque_type->getDecl()));

            SymbolContextList found;
            target.GetImages().FindSymbolsWithNameAndType(
                mangled_name, eSymbolTypeData, found);

            if (found.GetSize() == 0)
              return type;

            swift::Type result_type;

            for (unsigned i = 0, e = found.GetSize(); i < e; ++i) {
              SymbolContext found_sc;
              if (!found.GetContextAtIndex(i, found_sc))
                continue;

              // See if the symbol has an address.
              if (!found_sc.symbol)
                continue;

              auto addr = found_sc.symbol->GetAddress().GetLoadAddress(&target);
              if (!addr || addr == LLDB_INVALID_ADDRESS)
                continue;

              // Ask RemoteAST to get the underlying type out of the descriptor.
              auto &remote_ast = GetRemoteASTContext(*scratch_ctx);
              auto underlying_type_result =
                  remote_ast.getUnderlyingTypeForOpaqueType(
                      swift::remote::RemoteAddress(addr),
                      opaque_type->getSubstitutions(),
                      opaque_type->getOrdinal());

              if (!underlying_type_result)
                continue;

              // If we haven't yet gotten an underlying type, use this as our
              // possible result.
              if (!result_type) {
                result_type = underlying_type_result.getValue();
              }
              // If we have two possibilities, they should match.
              else if (!result_type->isEqual(
                           underlying_type_result.getValue())) {
                return type;
              }
            }

            if (!result_type)
              return type;

            return result_type;
          },
          swift::LookUpConformanceInModule(module_decl),
          swift::SubstFlags::DesugarMemberTypes |
              swift::SubstFlags::SubstituteOpaqueArchetypes);

      // Stop if we've reached a fixpoint where we can't further resolve opaque
      // types.
      if (old_type->isEqual(target_swift_type))
        break;
    }

    target_swift_type = target_swift_type.subst(
        [this, &stack_frame,
         &scratch_ctx](swift::SubstitutableType *type) -> swift::Type {
          StreamString type_name;
          if (!SwiftLanguageRuntime::GetAbstractTypeName(type_name, type))
            return type;
          CompilerType concrete_type = this->GetConcreteType(
              &stack_frame, ConstString(type_name.GetString()));
          Status import_error;
          CompilerType target_concrete_type =
              scratch_ctx->ImportType(concrete_type, import_error);

          if (target_concrete_type.IsValid())
            return swift::Type(GetSwiftType(target_concrete_type));

          return type;
        },
        swift::LookUpConformanceInModule(module_decl),
        swift::SubstFlags::DesugarMemberTypes);
    assert(target_swift_type);

    return ToCompilerType({target_swift_type.getPointer()});
  }
  return base_type;
}

bool SwiftLanguageRuntime::GetAbstractTypeName(StreamString &name,
                                               swift::Type swift_type) {
  auto *generic_type_param = swift_type->getAs<swift::GenericTypeParamType>();
  if (!generic_type_param)
    return false;

  name.Printf(u8"\u03C4_%d_%d", generic_type_param->getDepth(),
              generic_type_param->getIndex());
  return true;
}

bool SwiftLanguageRuntimeImpl::GetDynamicTypeAndAddress_Value(
    ValueObject &in_value, CompilerType &bound_type,
    lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
    Address &address) {
  class_type_or_name.SetCompilerType(bound_type);

  llvm::Optional<uint64_t> size = bound_type.GetByteSize(
      in_value.GetExecutionContextRef().GetFrameSP().get());
  if (!size)
    return false;
  lldb::addr_t val_address = in_value.GetAddressOf(true, nullptr);
  if (*size && (!val_address || val_address == LLDB_INVALID_ADDRESS))
    return false;

  address.SetLoadAddress(val_address, in_value.GetTargetSP().get());
  return true;
}

bool SwiftLanguageRuntimeImpl::GetDynamicTypeAndAddress_IndirectEnumCase(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &address) {
  static ConstString g_offset("offset");

  DataExtractor data;
  Status error;
  if (!(in_value.GetParent() && in_value.GetParent()->GetData(data, error) &&
        error.Success()))
    return false;

  bool has_payload;
  bool is_indirect;
  CompilerType payload_type;
  if (!SwiftASTContext::GetSelectedEnumCase(
          in_value.GetParent()->GetCompilerType(), data, nullptr, &has_payload,
          &payload_type, &is_indirect))
    return false;

  if (has_payload && is_indirect && payload_type)
    class_type_or_name.SetCompilerType(payload_type);

  lldb::addr_t box_addr = in_value.GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
  if (box_addr == LLDB_INVALID_ADDRESS)
    return false;

  box_addr = MaskMaybeBridgedPointer(m_process, box_addr);
  lldb::addr_t box_location = m_process.ReadPointerFromMemory(box_addr, error);
  if (box_location == LLDB_INVALID_ADDRESS)
    return false;

  box_location = MaskMaybeBridgedPointer(m_process, box_location);
  ProcessStructReader reader(&m_process, box_location, GetBoxMetadataType());
  uint32_t offset = reader.GetField<uint32_t>(g_offset);
  lldb::addr_t box_value = box_addr + offset;

  // try to read one byte at the box value
  m_process.ReadUnsignedIntegerFromMemory(box_value, 1, 0, error);
  if (error.Fail()) // and if that fails, then we're off in no man's land
    return false;

  Flags type_info(payload_type.GetTypeInfo());
  if (type_info.AllSet(eTypeIsSwift | eTypeIsClass)) {
    lldb::addr_t old_box_value = box_value;
    box_value = m_process.ReadPointerFromMemory(box_value, error);
    if (box_value == LLDB_INVALID_ADDRESS)
      return false;

    DataExtractor data(&box_value, m_process.GetAddressByteSize(),
                       m_process.GetByteOrder(),
                       m_process.GetAddressByteSize());
    ValueObjectSP valobj_sp(ValueObject::CreateValueObjectFromData(
        "_", data, m_process, payload_type));
    if (!valobj_sp)
      return false;

    Value::ValueType value_type;
    if (!GetDynamicTypeAndAddress(*valobj_sp, use_dynamic, class_type_or_name,
                                  address, value_type))
      return false;

    address.SetRawAddress(old_box_value);
    return true;
  } else if (type_info.AllSet(eTypeIsSwift | eTypeIsProtocol)) {
    SwiftASTContext::ProtocolInfo protocol_info;
    if (!SwiftASTContext::GetProtocolTypeInfo(payload_type, protocol_info))
      return false;
    auto ptr_size = m_process.GetAddressByteSize();
    std::vector<uint8_t> buffer(ptr_size * protocol_info.m_num_storage_words,
                                0);
    for (uint32_t idx = 0; idx < protocol_info.m_num_storage_words; idx++) {
      lldb::addr_t word = m_process.ReadUnsignedIntegerFromMemory(
          box_value + idx * ptr_size, ptr_size, 0, error);
      if (error.Fail())
        return false;
      memcpy(&buffer[idx * ptr_size], &word, ptr_size);
    }
    DataExtractor data(&buffer[0], buffer.size(), m_process.GetByteOrder(),
                       m_process.GetAddressByteSize());
    ValueObjectSP valobj_sp(ValueObject::CreateValueObjectFromData(
        "_", data, m_process, payload_type));
    if (!valobj_sp)
      return false;

    Value::ValueType value_type;
    if (!GetDynamicTypeAndAddress(*valobj_sp, use_dynamic, class_type_or_name,
                                  address, value_type))
      return false;

    address.SetRawAddress(box_value);
    return true;
  } else {
    // This is most likely a statically known type.
    address.SetLoadAddress(box_value, &m_process.GetTarget());
    return true;
  }
}

// Dynamic type resolution tends to want to generate scalar data - but there are
// caveats
// Per original comment here
// "Our address is the location of the dynamic type stored in memory.  It isn't
// a load address,
//  because we aren't pointing to the LOCATION that stores the pointer to us,
//  we're pointing to us..."
// See inlined comments for exceptions to this general rule.
Value::ValueType SwiftLanguageRuntimeImpl::GetValueType(
    Value::ValueType static_value_type, CompilerType static_type,
    CompilerType dynamic_type, bool is_indirect_enum_case) {
  Flags static_type_flags(static_type.GetTypeInfo());
  Flags dynamic_type_flags(dynamic_type.GetTypeInfo());

  if (dynamic_type_flags.AllSet(eTypeIsSwift)) {
    // for a protocol object where does the dynamic data live if the target
    // object is a struct? (for a class, it's easy)
    if (static_type_flags.AllSet(eTypeIsSwift | eTypeIsProtocol) &&
        dynamic_type_flags.AnySet(eTypeIsStructUnion | eTypeIsEnumeration)) {
      if (auto *ts = llvm::dyn_cast_or_null<TypeSystemSwiftTypeRef>(
              static_type.GetTypeSystem()))
        static_type = ts->ReconstructType(static_type);
      SwiftASTContext *swift_ast_ctx =
          llvm::dyn_cast_or_null<SwiftASTContext>(static_type.GetTypeSystem());
      if (!swift_ast_ctx)
        return {};
      if (swift_ast_ctx->IsErrorType(static_type.GetOpaqueQualType())) {
        // ErrorType values are always a pointer
        return Value::eValueTypeLoadAddress;
      }

      if (auto *ts = llvm::dyn_cast_or_null<TypeSystemSwift>(
              dynamic_type.GetTypeSystem()))
        switch (ts->GetAllocationStrategy(dynamic_type.GetOpaqueQualType())) {
        case SwiftASTContext::TypeAllocationStrategy::eDynamic:
        case SwiftASTContext::TypeAllocationStrategy::eUnknown:
          break;
        case SwiftASTContext::TypeAllocationStrategy::eInline: // inline data;
                                                               // same as the
                                                               // static data
          return static_value_type;
        case SwiftASTContext::TypeAllocationStrategy::ePointer: // pointed-to;
                                                                // in the target
          return Value::eValueTypeLoadAddress;
        }
    }
    if (static_type_flags.AllSet(eTypeIsSwift | eTypeIsGenericTypeParam)) {
      // if I am handling a non-pointer Swift type obtained from an archetype,
      // then the runtime vends the location
      // of the object, not the object per se (since the object is not a pointer
      // itself, this is way easier to achieve)
      // hence, it's a load address, not a scalar containing a pointer as for
      // ObjC classes
      if (dynamic_type_flags.AllClear(eTypeIsPointer | eTypeIsReference |
                                      eTypeInstanceIsPointer))
        return Value::eValueTypeLoadAddress;
    }

    if (static_type_flags.AllSet(eTypeIsSwift | eTypeIsPointer) &&
        static_type_flags.AllClear(eTypeIsGenericTypeParam)) {
      // FIXME: This branch is not covered by any testcases in the test suite.
      if (is_indirect_enum_case || static_type_flags.AllClear(eTypeIsBuiltIn))
        return Value::eValueTypeLoadAddress;
    }
  }

  // Enabling this makes the inout_variables test hang.
  //  return Value::eValueTypeScalar;
  if (static_type_flags.AllSet(eTypeIsSwift) &&
      dynamic_type_flags.AllSet(eTypeIsSwift) &&
      dynamic_type_flags.AllClear(eTypeIsPointer | eTypeInstanceIsPointer))
    return static_value_type;
  else
    return Value::eValueTypeScalar;
}

bool SwiftLanguageRuntimeImpl::GetDynamicTypeAndAddress_ClangType(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &address,
    Value::ValueType &value_type) {
  AppleObjCRuntime *objc_runtime =
      SwiftLanguageRuntime::GetObjCRuntime(m_process);
  if (!objc_runtime)
    return false;

  // This is a Clang type, which means it must have been an
  // Objective-C protocol. Protocols are not represented in DWARF and
  // LLDB's ObjC runtime implementation doesn't know how to deal with
  // them either.  Use the Objective-C runtime to perform dynamic type
  // resolution first, and then map the dynamic Objective-C type back
  // into Swift.
  TypeAndOrName dyn_class_type_or_name = class_type_or_name;
  if (!objc_runtime->GetDynamicTypeAndAddress(
          in_value, use_dynamic, dyn_class_type_or_name, address, value_type))
    return false;

  StringRef dyn_name = dyn_class_type_or_name.GetName().GetStringRef();
  // If this is an Objective-C runtime value, skip; this is handled elsewhere.
  if (swift::Demangle::isOldFunctionTypeMangling(dyn_name) ||
      dyn_name.startswith("__NS"))
    return false;

  std::string remangled;
  {
    // Create a mangle tree for __C.dyn_name?.
    using namespace swift::Demangle;
    NodeFactory factory;
    NodePointer global = factory.createNode(Node::Kind::Global);
    NodePointer tm = factory.createNode(Node::Kind::TypeMangling);
    global->addChild(tm, factory);
    NodePointer bge = factory.createNode(Node::Kind::BoundGenericEnum);
    tm->addChild(bge, factory);
    NodePointer ety = factory.createNode(Node::Kind::Type);
    bge->addChild(ety, factory);
    NodePointer e = factory.createNode(Node::Kind::Enum);
    e->addChild(factory.createNode(Node::Kind::Module, "Swift"), factory);
    e->addChild(factory.createNode(Node::Kind::Identifier, "Optional"),
                factory);
    ety->addChild(e, factory);
    NodePointer list = factory.createNode(Node::Kind::TypeList);
    bge->addChild(list, factory);
    NodePointer cty = factory.createNode(Node::Kind::Type);
    list->addChild(cty, factory);
    NodePointer c = factory.createNode(Node::Kind::Class);
    c->addChild(factory.createNode(Node::Kind::Module, "__C"), factory);
    c->addChild(factory.createNode(Node::Kind::Identifier, dyn_name), factory);
    cty->addChild(c, factory);

    remangled = mangleNode(global);
  }

  // Import the remangled dynamic name into the scratch context.
  assert(IsScratchContextLocked(in_value.GetTargetSP()) &&
         "Swift scratch context not locked ahead of dynamic type resolution");
  auto scratch_ctx = in_value.GetScratchSwiftASTContext();
  if (!scratch_ctx)
    return false;
  CompilerType swift_type =
      scratch_ctx->GetTypeFromMangledTypename(ConstString(remangled));

  // Roll back the ObjC dynamic type resolution.
  if (!swift_type)
    return false;
  class_type_or_name = dyn_class_type_or_name;
  class_type_or_name.SetCompilerType(swift_type);
  value_type = GetValueType(in_value.GetValue().GetValueType(),
                            in_value.GetCompilerType(),
                            class_type_or_name.GetCompilerType(), false);
  return true;
}

static bool IsIndirectEnumCase(ValueObject &valobj) {
  return (valobj.GetLanguageFlags() &
          SwiftASTContext::LanguageFlags::eIsIndirectEnumCase) ==
         SwiftASTContext::LanguageFlags::eIsIndirectEnumCase;
}

static bool CouldHaveDynamicValue(ValueObject &in_value) {
  if (IsIndirectEnumCase(in_value))
    return true;
  CompilerType var_type(in_value.GetCompilerType());
  Flags var_type_flags(var_type.GetTypeInfo());
  if (var_type_flags.AllSet(eTypeIsSwift | eTypeInstanceIsPointer)) {
    // Swift class instances are actually pointers, but base class instances
    // are inlined at offset 0 in the class data. If we just let base classes
    // be dynamic, it would cause an infinite recursion. So we would usually
    // disable it.
    return !in_value.IsBaseClass();
  }
  return var_type.IsPossibleDynamicType(nullptr, false, false);
}

bool SwiftLanguageRuntimeImpl::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &address,
    Value::ValueType &value_type) {
  class_type_or_name.Clear();
  if (use_dynamic == lldb::eNoDynamicValues)
    return false;

  // Try to import a Clang type into Swift.
  if (in_value.GetObjectRuntimeLanguage() == eLanguageTypeObjC)
    return GetDynamicTypeAndAddress_ClangType(
        in_value, use_dynamic, class_type_or_name, address, value_type);

  if (!CouldHaveDynamicValue(in_value))
    return false;

  // Dynamic type resolution in RemoteAST might pull in other Swift modules, so
  // use the scratch context where such operations are legal and safe.
  assert(IsScratchContextLocked(in_value.GetTargetSP()) &&
         "Swift scratch context not locked ahead of dynamic type resolution");
  auto scratch_ctx = in_value.GetScratchSwiftASTContext();
  if (!scratch_ctx)
    return false;

  auto retry_once = [&]() {
    // Retry exactly once using the per-module fallback scratch context.
    auto &target = m_process.GetTarget();
    if (!target.UseScratchTypesystemPerModule()) {
      Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
      if (log)
        log->Printf("Dynamic type resolution detected fatal errors in "
                    "shared Swift state. Falling back to per-module "
                    "scratch context.\n");
      target.SetUseScratchTypesystemPerModule(true);
      return GetDynamicTypeAndAddress(in_value, use_dynamic, class_type_or_name,
                                      address, value_type);
    }
    return false;
  };

  if (scratch_ctx->HasFatalErrors())
    return retry_once();

  // Import the type into the scratch context. Any form of dynamic
  // type resolution may trigger a cross-module import.
  CompilerType val_type(in_value.GetCompilerType());
  Flags type_info(val_type.GetTypeInfo());
  if (!type_info.AnySet(eTypeIsSwift))
    return false;

  bool success = false;
  bool is_indirect_enum_case = IsIndirectEnumCase(in_value);
  // Type kinds with metadata don't need archetype binding.
  if (is_indirect_enum_case)
    // ..._IndirectEnumCase() recurses, no need to bind archetypes.
    success = GetDynamicTypeAndAddress_IndirectEnumCase(
        in_value, use_dynamic, class_type_or_name, address);
  else if (type_info.AnySet(eTypeIsClass) ||
           type_info.AllSet(eTypeIsBuiltIn | eTypeIsPointer | eTypeHasValue))
    success = GetDynamicTypeAndAddress_Class(
        in_value, *scratch_ctx, use_dynamic, class_type_or_name, address);
  else if (type_info.AnySet(eTypeIsProtocol))
    success = GetDynamicTypeAndAddress_Protocol(in_value, val_type,
                                                *scratch_ctx, use_dynamic,
                                                class_type_or_name, address);
  else {
    // Perform archetype binding in the scratch context.
    StackFrameSP frame = in_value.GetExecutionContextRef().GetFrameSP();
    if (!frame)
      return false;

    CompilerType bound_type = DoArchetypeBindingForType(*frame.get(), val_type);
    if (!bound_type)
      return false;

    Flags subst_type_info(bound_type.GetTypeInfo());
    if (subst_type_info.AnySet(eTypeIsClass)) {
      success = GetDynamicTypeAndAddress_Class(
          in_value, *scratch_ctx, use_dynamic, class_type_or_name, address);
    } else if (subst_type_info.AnySet(eTypeIsProtocol)) {
      success = GetDynamicTypeAndAddress_Protocol(in_value, bound_type,
                                                  *scratch_ctx, use_dynamic,
                                                  class_type_or_name, address);
    } else {
      success = GetDynamicTypeAndAddress_Value(
          in_value, bound_type, use_dynamic, class_type_or_name, address);
    }
  }

  if (success)
    value_type = GetValueType(
        in_value.GetValue().GetValueType(), in_value.GetCompilerType(),
        class_type_or_name.GetCompilerType(), is_indirect_enum_case);
  else if (scratch_ctx->HasFatalErrors())
    return retry_once();
  return success;
}

TypeAndOrName SwiftLanguageRuntimeImpl::FixUpDynamicType(
    const TypeAndOrName &type_and_or_name, ValueObject &static_value) {
  CompilerType static_type = static_value.GetCompilerType();
  CompilerType dynamic_type = type_and_or_name.GetCompilerType();
  // The logic in this function only applies to static/dynamic Swift types.
  if (llvm::isa<TypeSystemClang>(static_type.GetTypeSystem()))
    return type_and_or_name;

  bool should_be_made_into_ref = false;
  bool should_be_made_into_ptr = false;
  Flags type_flags = static_type.GetTypeInfo();
  Flags type_andor_name_flags = dynamic_type.GetTypeInfo();

  // if the static type is a pointer or reference, so should the
  // dynamic type caveat: if the static type is a Swift class
  // instance, the dynamic type could either be a Swift type (no need
  // to change anything), or an ObjC type in which case it needs to be
  // made into a pointer
  if (type_flags.AnySet(eTypeIsPointer))
    should_be_made_into_ptr =
        (type_flags.AllClear(eTypeIsGenericTypeParam | eTypeIsBuiltIn) &&
         !IsIndirectEnumCase(static_value));
  else if (type_flags.AnySet(eTypeInstanceIsPointer))
    should_be_made_into_ptr = !type_andor_name_flags.AllSet(eTypeIsSwift);
  else if (type_flags.AnySet(eTypeIsReference))
    should_be_made_into_ref = true;
  else if (type_flags.AllSet(eTypeIsSwift | eTypeIsProtocol))
    should_be_made_into_ptr =
        dynamic_type.IsRuntimeGeneratedType() && !dynamic_type.IsPointerType();

  if (type_and_or_name.HasType()) {
    // The type will always be the type of the dynamic object.  If our
    // parent's type was a pointer, then our type should be a pointer
    // to the type of the dynamic object.  If a reference, then the
    // original type should be okay...
    CompilerType corrected_type = dynamic_type;
    if (should_be_made_into_ptr)
      corrected_type = dynamic_type.GetPointerType();
    else if (should_be_made_into_ref)
      corrected_type = dynamic_type.GetLValueReferenceType();
    TypeAndOrName result = type_and_or_name;
    result.SetCompilerType(corrected_type);
    return result;
  }
  return type_and_or_name;
}

bool SwiftLanguageRuntime::IsTaggedPointer(lldb::addr_t addr,
                                           CompilerType type) {
  if (!m_process)
    return false;
  swift::CanType swift_can_type = GetCanonicalSwiftType(type);
  if (!swift_can_type)
    return false;
  switch (swift_can_type->getKind()) {
  case swift::TypeKind::UnownedStorage: {
    Target &target = m_process->GetTarget();
    llvm::Triple triple = target.GetArchitecture().GetTriple();
    // On Darwin the Swift runtime stores unowned references to
    // Objective-C objects as a pointer to a struct that has the
    // actual object pointer at offset zero. The least significant bit
    // of the reference pointer indicates whether the reference refers
    // to an Objective-C or Swift object.
    //
    // This is a property of the Swift runtime(!). In the future it
    // may be necessary to check for the version of the Swift runtime
    // (or indirectly by looking at the version of the remote
    // operating system) to determine how to interpret references.
    if (triple.isOSDarwin())
      // Check whether this is a reference to an Objective-C object.
      if ((addr & 1) == 1)
        return true;
  }
  default:
    break;
  }
  return false;
}

std::pair<lldb::addr_t, bool>
SwiftLanguageRuntime::FixupPointerValue(lldb::addr_t addr, CompilerType type) {
  if (!m_process)
    return {addr, false};

  // Check for an unowned Darwin Objective-C reference.
  if (IsTaggedPointer(addr, type)) {
    // Clear the discriminator bit to get at the pointer to Objective-C object.
    bool needs_deref = true;
    return {addr & ~1ULL, needs_deref};
  }

  // Adjust the pointer to strip away the spare bits.
  Target &target = m_process->GetTarget();
  llvm::Triple triple = target.GetArchitecture().GetTriple();
  switch (triple.getArch()) {
  case llvm::Triple::ArchType::aarch64:
    return {addr & ~SWIFT_ABI_ARM64_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::arm:
    return {addr & ~SWIFT_ABI_ARM_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::x86:
    return {addr & ~SWIFT_ABI_I386_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::x86_64:
    return {addr & ~SWIFT_ABI_X86_64_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::systemz:
    return {addr & ~SWIFT_ABI_S390X_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::ppc64le:
    return {addr & ~SWIFT_ABI_POWERPC64_SWIFT_SPARE_BITS_MASK, false};
  default:
    break;
  }
  return {addr, false};
}

/// This allows a language runtime to adjust references depending on the type.
lldb::addr_t SwiftLanguageRuntime::FixupAddress(lldb::addr_t addr,
                                                CompilerType type,
                                                Status &error) {
  if (!m_process)
    return addr;

  swift::CanType swift_can_type = GetCanonicalSwiftType(type);
  switch (swift_can_type->getKind()) {
  case swift::TypeKind::UnownedStorage: {
    // Peek into the reference to see whether it needs an extra deref.
    // If yes, return the fixed-up address we just read.
    Target &target = m_process->GetTarget();
    size_t ptr_size = m_process->GetAddressByteSize();
    lldb::addr_t refd_addr = LLDB_INVALID_ADDRESS;
    target.ReadMemory(addr, false, &refd_addr, ptr_size, error);
    if (error.Success()) {
      bool extra_deref;
      std::tie(refd_addr, extra_deref) = FixupPointerValue(refd_addr, type);
      if (extra_deref)
        return refd_addr;
    }
  }
  default:
    break;
  }
  return addr;
}

const swift::reflection::TypeInfo *
SwiftLanguageRuntimeImpl::GetTypeInfo(CompilerType type) {
  auto *reflection_ctx = GetReflectionContext();
  if (!reflection_ctx)
    return nullptr;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  CompilerType can_type = ToCompilerType(swift_can_type);
  ConstString mangled_name(can_type.GetMangledTypeName());
  StringRef mangled_no_prefix =
      swift::Demangle::dropSwiftManglingPrefix(mangled_name.GetStringRef());
  swift::Demangle::Demangler Dem;
  auto demangled = Dem.demangleType(mangled_no_prefix);
  auto *type_ref = swift::Demangle::decodeMangledType(
      reflection_ctx->getBuilder(), demangled);
  if (!type_ref)
    return nullptr;
  return reflection_ctx->getBuilder().getTypeConverter().getTypeInfo(type_ref);
}

bool SwiftLanguageRuntimeImpl::IsStoredInlineInBuffer(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->isBitwiseTakable() && type_info->getSize() <= 24;
  return true;
}

llvm::Optional<uint64_t>
SwiftLanguageRuntimeImpl::GetBitSize(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->getSize() * 8;
  return {};
}

llvm::Optional<uint64_t>
SwiftLanguageRuntimeImpl::GetByteStride(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->getStride();
  return {};
}

llvm::Optional<size_t>
SwiftLanguageRuntimeImpl::GetBitAlignment(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->getAlignment();
  return {};
}

bool SwiftLanguageRuntime::IsWhitelistedRuntimeValue(ConstString name) {
  return name.GetStringRef() == "self";
}

bool SwiftLanguageRuntime::CouldHaveDynamicValue(ValueObject &in_value) {
  return ::CouldHaveDynamicValue(in_value);
}

CompilerType
SwiftLanguageRuntimeImpl::GetConcreteType(ExecutionContextScope *exe_scope,
                                          ConstString abstract_type_name) {
  if (!exe_scope)
    return CompilerType();

  StackFrame *frame(exe_scope->CalculateStackFrame().get());
  if (!frame)
    return CompilerType();

  SwiftLanguageRuntime::MetadataPromiseSP promise_sp(
      GetPromiseForTypeNameAndFrame(abstract_type_name.GetCString(), frame));
  if (!promise_sp)
    return CompilerType();

  return promise_sp->FulfillTypePromise();
}

} // namespace lldb_private
