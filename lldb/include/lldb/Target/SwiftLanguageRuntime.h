//===-- SwiftLanguageRuntime.h ----------------------------------*- C++ -*-===//
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

#ifndef liblldb_SwiftLanguageRuntime_h_
#define liblldb_SwiftLanguageRuntime_h_

// C Includes
// C++ Includes
#include <mutex>
#include <tuple>
#include <vector>
// Other libraries and framework includes
// Project includes
#include "Plugins/LanguageRuntime/ObjC/AppleObjCRuntime/AppleObjCRuntimeV2.h"
#include "lldb/Breakpoint/BreakpointPrecondition.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/Symbol/SwiftASTContext.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/lldb-private.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"

namespace swift {
namespace remote {
class MemoryReader;
class RemoteAddress;
} // namespace remote

template <typename T> struct External;
template <unsigned PointerSize> struct RuntimeTarget;

namespace reflection {
template <typename T> class ReflectionContext;
class TypeInfo;
} // namespace reflection

namespace remoteAST {
class RemoteASTContext;
}
enum class MetadataKind : uint32_t;
class TypeBase;
} // namespace swift

namespace lldb_private {

/// Statically cast a CompilerType to a Swift type.
swift::Type GetSwiftType(CompilerType type);
/// Statically cast a CompilerType to a Swift type and get its canonical form.
swift::CanType GetCanonicalSwiftType(CompilerType type);

class SwiftLanguageRuntimeStub;
class SwiftLanguageRuntimeImpl;

class SwiftLanguageRuntime : public LanguageRuntime {
protected:
  SwiftLanguageRuntime(Process *process);
  /// The private implementation object, either a stub or a full
  /// runtime.
  ///
  /// TODO: Instead of using these pImpl objects, it would be more
  ///   elegant to have CreateInstance return the right object,
  ///   unfortunately Process wants to cache the returned language
  ///   runtimes and doesn't call CreateInstance() ever again.
  std::unique_ptr<SwiftLanguageRuntimeStub> m_stub;
  std::unique_ptr<SwiftLanguageRuntimeImpl> m_impl;

public:
  class MetadataPromise;
  typedef std::shared_ptr<MetadataPromise> MetadataPromiseSP;

  static char ID;

  bool isA(const void *ClassID) const override {
    return ClassID == &ID || LanguageRuntime::isA(ClassID);
  }

  /// Static Functions.
  /// \{
  static void Initialize();
  static void Terminate();

  static lldb_private::LanguageRuntime *
  CreateInstance(Process *process, lldb::LanguageType language);

  static lldb_private::ConstString GetPluginNameStatic();

  static bool classof(const LanguageRuntime *runtime) {
    return runtime->isA(&ID);
  }

  static SwiftLanguageRuntime *Get(Process *process) {
    return process ? llvm::cast_or_null<SwiftLanguageRuntime>(
                         process->GetLanguageRuntime(lldb::eLanguageTypeSwift))
                   : nullptr;
  }

  static SwiftLanguageRuntime *Get(lldb::ProcessSP process_sp) {
    return SwiftLanguageRuntime::Get(process_sp.get());
  }
  /// \}

  /// PluginInterface protocol.
  lldb_private::ConstString GetPluginName() override;
  uint32_t GetPluginVersion() override;

  bool GetObjectDescription(Stream &str, Value &value,
                            ExecutionContextScope *exe_scope) override {
    // This is only interesting to do with a ValueObject for Swift.
    return false;
   }

  lldb::LanguageType GetLanguageType() const override {
    return lldb::eLanguageTypeSwift;
  }

  void ModulesDidLoad(const ModuleList &module_list) override;

  /// Mangling support.
  /// \{
  /// Use these passthrough functions rather than calling into Swift directly,
  /// since some day we may want to support more than one swift variant.
  static bool IsSwiftMangledName(const char *name);

  static std::string DemangleSymbolAsString(llvm::StringRef symbol,
                                            bool simplified = false,
                                            const SymbolContext *sc = nullptr);

  class MethodName {
  public:
    enum Type {
      eTypeInvalid,
      eTypeUnknownMethod,
      eTypeClassMethod,
      eTypeInstanceMethod,
      eTypeOperator,
      eTypeConstructor,
      eTypeDestructor,
      eTypeAllocator,
      eTypeDeallocator
    };

    MethodName() = default;
    MethodName(ConstString s, bool do_parse = false) : m_full(s) {
      if (do_parse)
        Parse();
    }

    void Clear();

    bool IsValid() const {
      if (m_parse_error)
        return false;
      if (m_type == eTypeInvalid)
        return false;
      return (bool)m_full;
    }

    Type GetType() const { return m_type; }
    ConstString GetFullName() const { return m_full; }
    llvm::StringRef GetBasename();

    static bool ExtractFunctionBasenameFromMangled(ConstString mangled,
                                                   ConstString &basename,
                                                   bool &is_method);

  protected:
    void Parse();

    ConstString m_full; ///< Full name:    "foo.bar.baz : <A : AProtocol>
                        ///< (foo.bar.metatype)(x : Swift.Int64) -> A"
    llvm::StringRef m_basename;      ///< Basename:     "baz"
    llvm::StringRef m_context;       ///< Decl context: "foo.bar"
    llvm::StringRef m_metatype_ref;  ///< Meta type:    "(foo.bar.metatype)"
    llvm::StringRef m_template_args; ///< Generic args: "<A: AProtocol>
    llvm::StringRef m_arguments;     ///< Arguments:    "(x : Swift.Int64)"
    llvm::StringRef m_qualifiers;    ///< Qualifiers:   "const"
    llvm::StringRef m_return_type;   ///< Return type:  "A"
    Type m_type = eTypeInvalid;
    bool m_parsed = false;
    bool m_parse_error = false;
  };
  /// \}

  bool GetDynamicTypeAndAddress(ValueObject &in_value,
                                lldb::DynamicValueType use_dynamic,
                                TypeAndOrName &class_type_or_name,
                                Address &address,
                                Value::ValueType &value_type) override;
  TypeAndOrName FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                 ValueObject &static_value) override;
  lldb::BreakpointResolverSP CreateExceptionResolver(Breakpoint *bkpt,
                                                     bool catch_bp,
                                                     bool throw_bp) override;
  bool CouldHaveDynamicValue(ValueObject &in_value) override;
  bool GetObjectDescription(Stream &str, ValueObject &object) override;
  CompilerType GetConcreteType(ExecutionContextScope *exe_scope,
                               ConstString abstract_type_name) override;

  /// A proxy object to support lazy binding of Archetypes.
  class MetadataPromise {
    friend class SwiftLanguageRuntimeImpl;

    MetadataPromise(ValueObject &, SwiftLanguageRuntimeImpl &, lldb::addr_t);

    lldb::ValueObjectSP m_for_object_sp;
    SwiftLanguageRuntimeImpl &m_swift_runtime;
    lldb::addr_t m_metadata_location;
    llvm::Optional<swift::MetadataKind> m_metadata_kind;
    llvm::Optional<CompilerType> m_compiler_type;

  public:
    CompilerType FulfillTypePromise(Status *error = nullptr);
  };

  MetadataPromiseSP GetMetadataPromise(lldb::addr_t addr,
                                       ValueObject &for_object);
  /// Build the artificial type metadata variable name for \p swift_type.
  static bool GetAbstractTypeName(StreamString &name, swift::Type swift_type);

  /// A pair of depth and index.
  using ArchetypePath = std::pair<uint64_t, uint64_t>;
  /// Populate a map with the names of all archetypes in a function's generic
  /// context.
  static void GetGenericParameterNamesForFunction(
      const SymbolContext &sc,
      llvm::DenseMap<ArchetypePath, llvm::StringRef> &dict);

  CompilerType DoArchetypeBindingForType(StackFrame &stack_frame,
                                         CompilerType base_type);

  bool IsStoredInlineInBuffer(CompilerType type) override;

  /// Retrieve the offset of the named member variable within an instance
  /// of the given type.
  ///
  /// \param instance_type
  llvm::Optional<uint64_t> GetMemberVariableOffset(CompilerType instance_type,
                                                   ValueObject *instance,
                                                   ConstString member_name,
                                                   Status *error = nullptr);

  /// Ask Remote Mirrors for the size of a Swift type.
  llvm::Optional<uint64_t> GetBitSize(CompilerType type);

  /// Ask Remote mirrors for the stride of a Swift type.
  llvm::Optional<uint64_t> GetByteStride(CompilerType type);

  /// Ask Remote mirrors for the alignment of a Swift type.
  llvm::Optional<size_t> GetBitAlignment(CompilerType type);

  /// Release the RemoteASTContext associated with the given swift::ASTContext.
  /// Note that a RemoteASTContext must be destroyed before its associated
  /// swift::ASTContext is destroyed.
  void ReleaseAssociatedRemoteASTContext(swift::ASTContext *ctx);

  void AddToLibraryNegativeCache(llvm::StringRef library_name);
  bool IsInLibraryNegativeCache(llvm::StringRef library_name);

  // Swift uses a few known-unused bits in ObjC pointers
  // to record useful-for-bridging information
  // This API's task is to return such pointer+info aggregates
  // back to a pure pointer
  lldb::addr_t MaskMaybeBridgedPointer(lldb::addr_t, lldb::addr_t * = nullptr);

  /// Swift uses a few known-unused bits in weak,unowned,unmanaged
  /// references to record useful runtime information.  This API's
  /// task is to strip those bits if necessary and return a pure
  /// pointer (or a tagged pointer).
  lldb::addr_t MaybeMaskNonTrivialReferencePointer(
      lldb::addr_t,
      SwiftASTContext::NonTriviallyManagedReferenceStrategy strategy);

  /// \return true if this is a Swift tagged pointer (as opposed to an
  /// Objective-C tagged pointer).
  bool IsTaggedPointer(lldb::addr_t addr, CompilerType type);
  std::pair<lldb::addr_t, bool> FixupPointerValue(lldb::addr_t addr,
                                                  CompilerType type) override;
  lldb::addr_t FixupAddress(lldb::addr_t addr, CompilerType type,
                            Status &error) override;

  lldb::ThreadPlanSP GetStepThroughTrampolinePlan(Thread &thread,
                                                  bool stop_others) override;
  /// If you are at the initial instruction of the frame passed in,
  /// then this will examine the call arguments, and if any of them is
  /// a function pointer, this will push the address of the function
  /// into addresses.  If debug_only is true, then it will only push
  /// function pointers that are in user code.
  void FindFunctionPointersInCall(StackFrame &frame,
                                  std::vector<Address> &addresses,
                                  bool debug_only = true,
                                  bool resolve_thunks = true) override;

  /// Error value handling.
  /// \{
  static lldb::ValueObjectSP CalculateErrorValue(lldb::StackFrameSP frame_sp,
                                                 ConstString name);

  lldb::ValueObjectSP CalculateErrorValueObjectFromValue(Value &value,
                                                         ConstString name,
                                                         bool persistent);

  llvm::Optional<Value>
  GetErrorReturnLocationAfterReturn(lldb::StackFrameSP frame_sp);

  llvm::Optional<Value>
  GetErrorReturnLocationBeforeReturn(lldb::StackFrameSP frame_sp,
                                     bool &need_to_check_after_return);

  static void RegisterGlobalError(Target &target, ConstString name,
                                  lldb::addr_t addr);

  // Provide a quick and yet somewhat reasonable guess as to whether
  // this ValueObject represents something that validly conforms
  // to the magic ErrorType protocol.
  bool IsValidErrorValue(ValueObject &in_value);
  /// \}

  static const char *GetErrorBackstopName();
  ConstString GetStandardLibraryName();
  static const char *GetStandardLibraryBaseName();
  static bool IsSwiftClassName(const char *name);
  /// Determines wether \c variable is the "self" object.
  static bool IsSelf(Variable &variable);
  bool IsWhitelistedRuntimeValue(ConstString name) override;

  lldb::SyntheticChildrenSP
  GetBridgedSyntheticChildProvider(ValueObject &valobj);

  /// Expression Callbacks.
  /// \{
  void WillStartExecutingUserExpression(bool);
  void DidFinishExecutingUserExpression(bool);
  /// \}

  bool IsABIStable();

  DISALLOW_COPY_AND_ASSIGN(SwiftLanguageRuntime);

  static AppleObjCRuntimeV2 *GetObjCRuntime(lldb_private::Process &process);
protected:
  bool GetTargetOfPartialApply(SymbolContext &curr_sc, ConstString &apply_name,
                               SymbolContext &sc);
  AppleObjCRuntimeV2 *GetObjCRuntime();
};

} // namespace lldb_private

#endif // liblldb_SwiftLanguageRuntime_h_
