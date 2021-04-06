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

#ifndef liblldb_SwiftLanguageRuntimeImpl_h_
#define liblldb_SwiftLanguageRuntimeImpl_h_

#include "lldb/Target/SwiftLanguageRuntime.h"

namespace lldb_private {
class Process;

/// A full LLDB language runtime backed by the Swift runtime library
/// in the process.
class SwiftLanguageRuntimeImpl {
  Process &m_process;

public:
  SwiftLanguageRuntimeImpl(Process &process);
  static lldb::BreakpointPreconditionSP
  GetBreakpointExceptionPrecondition(lldb::LanguageType language,
                                     bool throw_bp);

  class SwiftExceptionPrecondition : public BreakpointPrecondition {
  public:
    SwiftExceptionPrecondition();

    virtual ~SwiftExceptionPrecondition() {}

    bool EvaluatePrecondition(StoppointCallbackContext &context) override;
    void GetDescription(Stream &stream, lldb::DescriptionLevel level) override;
    Status ConfigurePrecondition(Args &args) override;

  protected:
    void AddTypeName(const char *type_name);
    void AddEnumSpec(const char *enum_name, const char *element_name);

  private:
    std::unordered_set<std::string> m_type_names;
    std::unordered_map<std::string, std::vector<std::string>> m_enum_spec;
  };

  void ModulesDidLoad(const ModuleList &module_list);

  bool GetObjectDescription(Stream &str, ValueObject &object);

  SwiftExceptionPrecondition *GetExceptionPrecondition();

  /// This call should return true if it could set the name and/or the type.
  bool GetDynamicTypeAndAddress(ValueObject &in_value,
                                lldb::DynamicValueType use_dynamic,
                                TypeAndOrName &class_type_or_name,
                                Address &address, Value::ValueType &value_type);

  TypeAndOrName FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                 ValueObject &static_value);

  /// Ask Remote Mirrors for the type info about a Swift type.
  const swift::reflection::TypeInfo *GetTypeInfo(CompilerType type);
  bool IsStoredInlineInBuffer(CompilerType type);

  /// Ask Remote Mirrors for the size of a Swift type.
  llvm::Optional<uint64_t> GetBitSize(CompilerType type);

  /// Ask Remote mirrors for the stride of a Swift type.
  llvm::Optional<uint64_t> GetByteStride(CompilerType type);

  /// Ask Remote mirrors for the alignment of a Swift type.
  llvm::Optional<size_t> GetBitAlignment(CompilerType type);

  SwiftLanguageRuntime::MetadataPromiseSP
  GetMetadataPromise(lldb::addr_t addr, ValueObject &for_object);
  llvm::Optional<uint64_t> GetMemberVariableOffset(CompilerType instance_type,
                                                   ValueObject *instance,
                                                   ConstString member_name,
                                                   Status *error);

  CompilerType DoArchetypeBindingForType(StackFrame &stack_frame,
                                         CompilerType base_type);

  CompilerType GetConcreteType(ExecutionContextScope *exe_scope,
                               ConstString abstract_type_name);

  /// Retrieve the remote AST context for the given Swift AST context.
  swift::remoteAST::RemoteASTContext &
  GetRemoteASTContext(SwiftASTContext &swift_ast_ctx);

  /// Release the RemoteASTContext associated with the given swift::ASTContext.
  /// Note that a RemoteASTContext must be destroyed before its associated
  /// swift::ASTContext is destroyed.
  void ReleaseAssociatedRemoteASTContext(swift::ASTContext *ctx);

  void AddToLibraryNegativeCache(llvm::StringRef library_name);
  bool IsInLibraryNegativeCache(llvm::StringRef library_name);
  void WillStartExecutingUserExpression(bool runs_in_playground_or_repl);
  void DidFinishExecutingUserExpression(bool runs_in_playground_or_repl);
  bool IsValidErrorValue(ValueObject &in_value);

  ConstString GetErrorBackstopName();
  ConstString GetStandardLibraryName();
  ConstString GetStandardLibraryBaseName();

  lldb::SyntheticChildrenSP
  GetBridgedSyntheticChildProvider(ValueObject &valobj);

  bool IsABIStable();

protected:
  // Classes that inherit from SwiftLanguageRuntime can see and modify these
  Value::ValueType GetValueType(Value::ValueType static_value_type,
                                CompilerType static_type,
                                CompilerType dynamic_type,
                                bool is_indirect_enum_case);

  bool GetDynamicTypeAndAddress_Class(
      ValueObject &in_value, SwiftASTContextForExpressions &scratch_ctx,
      lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
      Address &address);

  bool GetDynamicTypeAndAddress_Protocol(
      ValueObject &in_value, CompilerType protocol_type,
      SwiftASTContextForExpressions &scratch_ctx,
      lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
      Address &address);

  bool GetDynamicTypeAndAddress_Value(ValueObject &in_value,
                                      CompilerType &bound_type,
                                      lldb::DynamicValueType use_dynamic,
                                      TypeAndOrName &class_type_or_name,
                                      Address &address);

  bool GetDynamicTypeAndAddress_IndirectEnumCase(
      ValueObject &in_value, lldb::DynamicValueType use_dynamic,
      TypeAndOrName &class_type_or_name, Address &address);

  bool GetDynamicTypeAndAddress_ClangType(ValueObject &in_value,
                                          lldb::DynamicValueType use_dynamic,
                                          TypeAndOrName &class_type_or_name,
                                          Address &address,
                                          Value::ValueType &value_type);

  SwiftLanguageRuntime::MetadataPromiseSP
  GetPromiseForTypeNameAndFrame(const char *type_name, StackFrame *frame);

  const CompilerType &GetBoxMetadataType();

  std::shared_ptr<swift::remote::MemoryReader> GetMemoryReader();

  void PushLocalBuffer(uint64_t local_buffer, uint64_t local_buffer_size);

  void PopLocalBuffer();

  /// We have to load swift dependent libraries by hand, but if they
  /// are missing, we shouldn't keep trying.
  llvm::StringSet<> m_library_negative_cache;
  std::mutex m_negative_cache_mutex;

  std::shared_ptr<swift::remote::MemoryReader> m_memory_reader_sp;

  llvm::DenseMap<std::pair<swift::ASTContext *, lldb::addr_t>,
                 SwiftLanguageRuntime::MetadataPromiseSP>
      m_promises_map;

  llvm::DenseMap<swift::ASTContext *,
                 std::unique_ptr<swift::remoteAST::RemoteASTContext>>
      m_remote_ast_contexts;

  /// Uses ConstStrings as keys to avoid storing the strings twice.
  llvm::DenseMap<const char *, lldb::SyntheticChildrenSP>
      m_bridged_synthetics_map;

  /// Cached member variable offsets.
  using MemberID = std::pair<const swift::TypeBase *, const char *>;
  llvm::DenseMap<MemberID, uint64_t> m_member_offsets;

  CompilerType m_box_metadata_type;

private:
  using NativeReflectionContext = swift::reflection::ReflectionContext<
      swift::External<swift::RuntimeTarget<sizeof(uintptr_t)>>>;

  /// Don't call these directly.
  /// \{
  /// There is a global variable \p _swift_classIsSwiftMask that is
  /// used to communicate with the Swift language runtime. It needs to
  /// be initialized by us, but could in theory also be written to by
  /// the runtime.
  void SetupABIBit();
  void SetupExclusivity();
  void SetupReflection();
  void SetupSwiftError();
  /// \}

  /// Whether \p SetupReflection() has been run.
  bool m_initialized_reflection_ctx = false;

  /// Lazily initialize and return \p m_dynamic_exclusivity_flag_addr.
  llvm::Optional<lldb::addr_t> GetDynamicExclusivityFlagAddr();

  /// Lazily initialize the reflection context. Return \p nullptr on failure.
  NativeReflectionContext *GetReflectionContext();

  /// Lazily initialize and return \p m_SwiftNativeNSErrorISA.
  llvm::Optional<lldb::addr_t> GetSwiftNativeNSErrorISA();

  /// These members are used to track and toggle the state of the "dynamic
  /// exclusivity enforcement flag" in the swift runtime. This flag is set to
  /// true when an LLDB expression starts running, and reset to its original
  /// state after that expression (and any other concurrently running
  /// expressions) terminates.
  /// \{
  std::mutex m_active_user_expr_mutex;
  uint32_t m_active_user_expr_count = 0;

  bool m_original_dynamic_exclusivity_flag_state = false;
  llvm::Optional<lldb::addr_t> m_dynamic_exclusivity_flag_addr;
  /// \}

  /// Reflection context.
  /// \{
  std::unique_ptr<NativeReflectionContext> m_reflection_ctx;

  /// Record modules added through ModulesDidLoad, which are to be
  /// added to the reflection context once it's being initialized.
  ModuleList m_modules_to_add;
  std::recursive_mutex m_add_module_mutex;

  /// Add the image to the reflection context.
  /// \return true on success.
  bool AddModuleToReflectionContext(const lldb::ModuleSP &module_sp);
  /// \}

  /// Swift native NSError isa.
  llvm::Optional<lldb::addr_t> m_SwiftNativeNSErrorISA;

  DISALLOW_COPY_AND_ASSIGN(SwiftLanguageRuntimeImpl);
};

} // namespace lldb_private
#endif
