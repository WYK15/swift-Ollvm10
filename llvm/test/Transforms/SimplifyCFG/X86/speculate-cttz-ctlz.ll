; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -simplifycfg -mtriple=x86_64-unknown-unknown -mattr=+bmi < %s | FileCheck %s --check-prefix=ALL --check-prefix=BMI
; RUN: opt -S -simplifycfg -mtriple=x86_64-unknown-unknown -mattr=+lzcnt < %s | FileCheck %s --check-prefix=ALL --check-prefix=LZCNT
; RUN: opt -S -simplifycfg -mtriple=x86_64-unknown-unknown < %s | FileCheck %s --check-prefix=ALL --check-prefix=GENERIC


define i64 @test1(i64 %A) {
; BMI-LABEL: @test1(
; BMI-NEXT:  entry:
; BMI-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[A:%.*]], 0
; BMI-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.ctlz.i64(i64 [[A]], i1 true)
; BMI-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i64 64, i64 [[TMP0]]
; BMI-NEXT:    ret i64 [[COND]]
;
; LZCNT-LABEL: @test1(
; LZCNT-NEXT:  entry:
; LZCNT-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[A:%.*]], 0
; LZCNT-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.ctlz.i64(i64 [[A]], i1 true)
; LZCNT-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[TOBOOL]], i64 64, i64 [[TMP0]]
; LZCNT-NEXT:    ret i64 [[SPEC_SELECT]]
;
; GENERIC-LABEL: @test1(
; GENERIC-NEXT:  entry:
; GENERIC-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[A:%.*]], 0
; GENERIC-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.ctlz.i64(i64 [[A]], i1 true)
; GENERIC-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i64 64, i64 [[TMP0]]
; GENERIC-NEXT:    ret i64 [[COND]]
;
entry:
  %tobool = icmp eq i64 %A, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i64 @llvm.ctlz.i64(i64 %A, i1 true)
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i64 [ %0, %cond.true ], [ 64, %entry ]
  ret i64 %cond
}

define i32 @test2(i32 %A) {
; BMI-LABEL: @test2(
; BMI-NEXT:  entry:
; BMI-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[A:%.*]], 0
; BMI-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.ctlz.i32(i32 [[A]], i1 true)
; BMI-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i32 32, i32 [[TMP0]]
; BMI-NEXT:    ret i32 [[COND]]
;
; LZCNT-LABEL: @test2(
; LZCNT-NEXT:  entry:
; LZCNT-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[A:%.*]], 0
; LZCNT-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.ctlz.i32(i32 [[A]], i1 true)
; LZCNT-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[TOBOOL]], i32 32, i32 [[TMP0]]
; LZCNT-NEXT:    ret i32 [[SPEC_SELECT]]
;
; GENERIC-LABEL: @test2(
; GENERIC-NEXT:  entry:
; GENERIC-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[A:%.*]], 0
; GENERIC-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.ctlz.i32(i32 [[A]], i1 true)
; GENERIC-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i32 32, i32 [[TMP0]]
; GENERIC-NEXT:    ret i32 [[COND]]
;
entry:
  %tobool = icmp eq i32 %A, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i32 @llvm.ctlz.i32(i32 %A, i1 true)
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i32 [ %0, %cond.true ], [ 32, %entry ]
  ret i32 %cond
}


define signext i16 @test3(i16 signext %A) {
; BMI-LABEL: @test3(
; BMI-NEXT:  entry:
; BMI-NEXT:    [[TOBOOL:%.*]] = icmp eq i16 [[A:%.*]], 0
; BMI-NEXT:    [[TMP0:%.*]] = tail call i16 @llvm.ctlz.i16(i16 [[A]], i1 true)
; BMI-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 16, i16 [[TMP0]]
; BMI-NEXT:    ret i16 [[COND]]
;
; LZCNT-LABEL: @test3(
; LZCNT-NEXT:  entry:
; LZCNT-NEXT:    [[TOBOOL:%.*]] = icmp eq i16 [[A:%.*]], 0
; LZCNT-NEXT:    [[TMP0:%.*]] = tail call i16 @llvm.ctlz.i16(i16 [[A]], i1 true)
; LZCNT-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[TOBOOL]], i16 16, i16 [[TMP0]]
; LZCNT-NEXT:    ret i16 [[SPEC_SELECT]]
;
; GENERIC-LABEL: @test3(
; GENERIC-NEXT:  entry:
; GENERIC-NEXT:    [[TOBOOL:%.*]] = icmp eq i16 [[A:%.*]], 0
; GENERIC-NEXT:    [[TMP0:%.*]] = tail call i16 @llvm.ctlz.i16(i16 [[A]], i1 true)
; GENERIC-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 16, i16 [[TMP0]]
; GENERIC-NEXT:    ret i16 [[COND]]
;
entry:
  %tobool = icmp eq i16 %A, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i16 @llvm.ctlz.i16(i16 %A, i1 true)
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i16 [ %0, %cond.true ], [ 16, %entry ]
  ret i16 %cond
}


define i64 @test1b(i64 %A) {
; BMI-LABEL: @test1b(
; BMI-NEXT:  entry:
; BMI-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[A:%.*]], 0
; BMI-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.cttz.i64(i64 [[A]], i1 true)
; BMI-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[TOBOOL]], i64 64, i64 [[TMP0]]
; BMI-NEXT:    ret i64 [[SPEC_SELECT]]
;
; LZCNT-LABEL: @test1b(
; LZCNT-NEXT:  entry:
; LZCNT-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[A:%.*]], 0
; LZCNT-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.cttz.i64(i64 [[A]], i1 true)
; LZCNT-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i64 64, i64 [[TMP0]]
; LZCNT-NEXT:    ret i64 [[COND]]
;
; GENERIC-LABEL: @test1b(
; GENERIC-NEXT:  entry:
; GENERIC-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[A:%.*]], 0
; GENERIC-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.cttz.i64(i64 [[A]], i1 true)
; GENERIC-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i64 64, i64 [[TMP0]]
; GENERIC-NEXT:    ret i64 [[COND]]
;
entry:
  %tobool = icmp eq i64 %A, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i64 @llvm.cttz.i64(i64 %A, i1 true)
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i64 [ %0, %cond.true ], [ 64, %entry ]
  ret i64 %cond
}


define i32 @test2b(i32 %A) {
; BMI-LABEL: @test2b(
; BMI-NEXT:  entry:
; BMI-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[A:%.*]], 0
; BMI-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.cttz.i32(i32 [[A]], i1 true)
; BMI-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[TOBOOL]], i32 32, i32 [[TMP0]]
; BMI-NEXT:    ret i32 [[SPEC_SELECT]]
;
; LZCNT-LABEL: @test2b(
; LZCNT-NEXT:  entry:
; LZCNT-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[A:%.*]], 0
; LZCNT-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.cttz.i32(i32 [[A]], i1 true)
; LZCNT-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i32 32, i32 [[TMP0]]
; LZCNT-NEXT:    ret i32 [[COND]]
;
; GENERIC-LABEL: @test2b(
; GENERIC-NEXT:  entry:
; GENERIC-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[A:%.*]], 0
; GENERIC-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.cttz.i32(i32 [[A]], i1 true)
; GENERIC-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i32 32, i32 [[TMP0]]
; GENERIC-NEXT:    ret i32 [[COND]]
;
entry:
  %tobool = icmp eq i32 %A, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i32 @llvm.cttz.i32(i32 %A, i1 true)
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i32 [ %0, %cond.true ], [ 32, %entry ]
  ret i32 %cond
}


define signext i16 @test3b(i16 signext %A) {
; BMI-LABEL: @test3b(
; BMI-NEXT:  entry:
; BMI-NEXT:    [[TOBOOL:%.*]] = icmp eq i16 [[A:%.*]], 0
; BMI-NEXT:    [[TMP0:%.*]] = tail call i16 @llvm.cttz.i16(i16 [[A]], i1 true)
; BMI-NEXT:    [[SPEC_SELECT:%.*]] = select i1 [[TOBOOL]], i16 16, i16 [[TMP0]]
; BMI-NEXT:    ret i16 [[SPEC_SELECT]]
;
; LZCNT-LABEL: @test3b(
; LZCNT-NEXT:  entry:
; LZCNT-NEXT:    [[TOBOOL:%.*]] = icmp eq i16 [[A:%.*]], 0
; LZCNT-NEXT:    [[TMP0:%.*]] = tail call i16 @llvm.cttz.i16(i16 [[A]], i1 true)
; LZCNT-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 16, i16 [[TMP0]]
; LZCNT-NEXT:    ret i16 [[COND]]
;
; GENERIC-LABEL: @test3b(
; GENERIC-NEXT:  entry:
; GENERIC-NEXT:    [[TOBOOL:%.*]] = icmp eq i16 [[A:%.*]], 0
; GENERIC-NEXT:    [[TMP0:%.*]] = tail call i16 @llvm.cttz.i16(i16 [[A]], i1 true)
; GENERIC-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 16, i16 [[TMP0]]
; GENERIC-NEXT:    ret i16 [[COND]]
;
entry:
  %tobool = icmp eq i16 %A, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i16 @llvm.cttz.i16(i16 %A, i1 true)
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i16 [ %0, %cond.true ], [ 16, %entry ]
  ret i16 %cond
}

; The following tests verify that calls to cttz/ctlz are speculated even if
; basic block %cond.true has an extra zero extend/truncate which is "free"
; for the target.

define i64 @test1e(i32 %x) {
; ALL-LABEL: @test1e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.cttz.i32(i32 [[X]], i1 true)
; ALL-NEXT:    [[PHITMP2:%.*]] = zext i32 [[TMP0]] to i64
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i64 32, i64 [[PHITMP2]]
; ALL-NEXT:    ret i64 [[COND]]
;
entry:
  %tobool = icmp eq i32 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i32 @llvm.cttz.i32(i32 %x, i1 true)
  %phitmp2 = zext i32 %0 to i64
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i64 [ %phitmp2, %cond.true ], [ 32, %entry ]
  ret i64 %cond
}

define i32 @test2e(i64 %x) {
; ALL-LABEL: @test2e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.cttz.i64(i64 [[X]], i1 true)
; ALL-NEXT:    [[CAST:%.*]] = trunc i64 [[TMP0]] to i32
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i32 64, i32 [[CAST]]
; ALL-NEXT:    ret i32 [[COND]]
;
entry:
  %tobool = icmp eq i64 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i64 @llvm.cttz.i64(i64 %x, i1 true)
  %cast = trunc i64 %0 to i32
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i32 [ %cast, %cond.true ], [ 64, %entry ]
  ret i32 %cond
}

define i64 @test3e(i32 %x) {
; ALL-LABEL: @test3e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.ctlz.i32(i32 [[X]], i1 true)
; ALL-NEXT:    [[PHITMP2:%.*]] = zext i32 [[TMP0]] to i64
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i64 32, i64 [[PHITMP2]]
; ALL-NEXT:    ret i64 [[COND]]
;
entry:
  %tobool = icmp eq i32 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i32 @llvm.ctlz.i32(i32 %x, i1 true)
  %phitmp2 = zext i32 %0 to i64
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i64 [ %phitmp2, %cond.true ], [ 32, %entry ]
  ret i64 %cond
}

define i32 @test4e(i64 %x) {
; ALL-LABEL: @test4e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.ctlz.i64(i64 [[X]], i1 true)
; ALL-NEXT:    [[CAST:%.*]] = trunc i64 [[TMP0]] to i32
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i32 64, i32 [[CAST]]
; ALL-NEXT:    ret i32 [[COND]]
;
entry:
  %tobool = icmp eq i64 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i64 @llvm.ctlz.i64(i64 %x, i1 true)
  %cast = trunc i64 %0 to i32
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i32 [ %cast, %cond.true ], [ 64, %entry ]
  ret i32 %cond
}

define i16 @test5e(i64 %x) {
; ALL-LABEL: @test5e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.ctlz.i64(i64 [[X]], i1 true)
; ALL-NEXT:    [[CAST:%.*]] = trunc i64 [[TMP0]] to i16
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 64, i16 [[CAST]]
; ALL-NEXT:    ret i16 [[COND]]
;
entry:
  %tobool = icmp eq i64 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i64 @llvm.ctlz.i64(i64 %x, i1 true)
  %cast = trunc i64 %0 to i16
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i16 [ %cast, %cond.true ], [ 64, %entry ]
  ret i16 %cond
}

define i16 @test6e(i32 %x) {
; ALL-LABEL: @test6e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.ctlz.i32(i32 [[X]], i1 true)
; ALL-NEXT:    [[CAST:%.*]] = trunc i32 [[TMP0]] to i16
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 32, i16 [[CAST]]
; ALL-NEXT:    ret i16 [[COND]]
;
entry:
  %tobool = icmp eq i32 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i32 @llvm.ctlz.i32(i32 %x, i1 true)
  %cast = trunc i32 %0 to i16
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i16 [ %cast, %cond.true ], [ 32, %entry ]
  ret i16 %cond
}

define i16 @test7e(i64 %x) {
; ALL-LABEL: @test7e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i64 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i64 @llvm.cttz.i64(i64 [[X]], i1 true)
; ALL-NEXT:    [[CAST:%.*]] = trunc i64 [[TMP0]] to i16
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 64, i16 [[CAST]]
; ALL-NEXT:    ret i16 [[COND]]
;
entry:
  %tobool = icmp eq i64 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i64 @llvm.cttz.i64(i64 %x, i1 true)
  %cast = trunc i64 %0 to i16
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i16 [ %cast, %cond.true ], [ 64, %entry ]
  ret i16 %cond
}

define i16 @test8e(i32 %x) {
; ALL-LABEL: @test8e(
; ALL-NEXT:  entry:
; ALL-NEXT:    [[TOBOOL:%.*]] = icmp eq i32 [[X:%.*]], 0
; ALL-NEXT:    [[TMP0:%.*]] = tail call i32 @llvm.cttz.i32(i32 [[X]], i1 true)
; ALL-NEXT:    [[CAST:%.*]] = trunc i32 [[TMP0]] to i16
; ALL-NEXT:    [[COND:%.*]] = select i1 [[TOBOOL]], i16 32, i16 [[CAST]]
; ALL-NEXT:    ret i16 [[COND]]
;
entry:
  %tobool = icmp eq i32 %x, 0
  br i1 %tobool, label %cond.end, label %cond.true

cond.true:                                        ; preds = %entry
  %0 = tail call i32 @llvm.cttz.i32(i32 %x, i1 true)
  %cast = trunc i32 %0 to i16
  br label %cond.end

cond.end:                                         ; preds = %entry, %cond.true
  %cond = phi i16 [ %cast, %cond.true ], [ 32, %entry ]
  ret i16 %cond
}


declare i64 @llvm.ctlz.i64(i64, i1)
declare i32 @llvm.ctlz.i32(i32, i1)
declare i16 @llvm.ctlz.i16(i16, i1)
declare i64 @llvm.cttz.i64(i64, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare i16 @llvm.cttz.i16(i16, i1)
