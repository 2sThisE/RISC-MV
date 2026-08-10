; Hand-authored canonical fixture until the host Clang target is available.
target datalayout = "e-p:64:64-i8:8-i16:16-i32:32-i64:64-f32:32-f64:64-v128:128-a:0:64-n8:16:32:64-S128"
target triple = "cvm64-unknown-none"

define i64 @add(i64 %left, i64 %right) {
entry:
  %sum = add i64 %left, %right
  ret i64 %sum
}

define i64 @checked_add(i64 %left, i64 %right) {
entry:
  %sum = call i64 @add(i64 %left, i64 %right)
  %valid = icmp eq i64 %sum, 42
  br i1 %valid, label %success, label %failure

success:
  ret i64 %sum

failure:
  ret i64 0
}
