; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-unknown | FileCheck %s

define i32 @testmsws(float %x) {
; CHECK-LABEL: testmsws:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    callq llroundf
; CHECK-NEXT:    # kill: def $eax killed $eax killed $rax
; CHECK-NEXT:    popq %rcx
; CHECK-NEXT:    .cfi_def_cfa_offset 8
; CHECK-NEXT:    retq
entry:
  %0 = tail call i64 @llvm.llround.f32(float %x)
  %conv = trunc i64 %0 to i32
  ret i32 %conv
}

define i64 @testmsxs(float %x) {
; CHECK-LABEL: testmsxs:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    jmp llroundf # TAILCALL
entry:
  %0 = tail call i64 @llvm.llround.f32(float %x)
  ret i64 %0
}

define i32 @testmswd(double %x) {
; CHECK-LABEL: testmswd:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    pushq %rax
; CHECK-NEXT:    .cfi_def_cfa_offset 16
; CHECK-NEXT:    callq llround
; CHECK-NEXT:    # kill: def $eax killed $eax killed $rax
; CHECK-NEXT:    popq %rcx
; CHECK-NEXT:    .cfi_def_cfa_offset 8
; CHECK-NEXT:    retq
entry:
  %0 = tail call i64 @llvm.llround.f64(double %x)
  %conv = trunc i64 %0 to i32
  ret i32 %conv
}

define i64 @testmsxd(double %x) {
; CHECK-LABEL: testmsxd:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    jmp llround # TAILCALL
entry:
  %0 = tail call i64 @llvm.llround.f64(double %x)
  ret i64 %0
}

define i32 @testmswl(x86_fp80 %x) {
; CHECK-LABEL: testmswl:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    subq $24, %rsp
; CHECK-NEXT:    .cfi_def_cfa_offset 32
; CHECK-NEXT:    fldt {{[0-9]+}}(%rsp)
; CHECK-NEXT:    fstpt (%rsp)
; CHECK-NEXT:    callq llroundl
; CHECK-NEXT:    # kill: def $eax killed $eax killed $rax
; CHECK-NEXT:    addq $24, %rsp
; CHECK-NEXT:    .cfi_def_cfa_offset 8
; CHECK-NEXT:    retq
entry:
  %0 = tail call i64 @llvm.llround.f80(x86_fp80 %x)
  %conv = trunc i64 %0 to i32
  ret i32 %conv
}

define i64 @testmsll(x86_fp80 %x) {
; CHECK-LABEL: testmsll:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    jmp llroundl # TAILCALL
entry:
  %0 = tail call i64 @llvm.llround.f80(x86_fp80 %x)
  ret i64 %0
}

declare i64 @llvm.llround.f32(float) nounwind readnone
declare i64 @llvm.llround.f64(double) nounwind readnone
declare i64 @llvm.llround.f80(x86_fp80) nounwind readnone
